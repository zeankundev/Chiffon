/* Framebuffer core: registration, drawing helpers, /dev/fb0 backend and the "fb" / "console" IPC
 * services.  Hardware drivers (drivers/<arch>/) only have to describe their linear framebuffer. */
#include "kernel.h"

static struct fb_device *fbs, *primary;

struct fb_device *fb_primary(void) { return primary; }

uint32_t fb_pack(const struct fb_info *i, uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return ((r >> (8 - i->rlen)) << i->roff) | ((g >> (8 - i->glen)) << i->goff) | ((b >> (8 - i->blen)) << i->boff);
}

void fb_fill(struct fb_device *d, int x, int y, int w, int h, uint32_t rgb) {
    struct fb_info *i = &d->info;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)i->width) w = i->width - x;
    if (y + h > (int)i->height) h = i->height - y;
    if (w <= 0 || h <= 0) return;
    uint32_t px = fb_pack(i, rgb);
    for (int row = 0; row < h; row++) {
        uint8_t *line = i->virt + (size_t)(i->yoff + y + row) * i->pitch + (size_t)x * (i->bpp / 8);
        if (i->bpp == 32) for (int k = 0; k < w; k++) ((uint32_t *)line)[k] = px;
        else for (int k = 0; k < w; k++) ((uint16_t *)line)[k] = (uint16_t)px;
    }
}

/* ---- kernel IPC service: "fb" ---- */
static int fb_service(const struct chi_ipc_msg *q, struct chi_ipc_msg *r) {
    struct fb_device *d = primary;
    const chi_u32 *in = (const chi_u32 *)q->data;
    chi_u32 *out = (chi_u32 *)r->data;
    if (!d) return -ENODEV;
    struct fb_info *i = &d->info;
    switch (q->tag) {
    case FB_OP_INFO:
        out[0] = i->width; out[1] = i->height; out[2] = i->pitch; out[3] = i->bpp;
        out[4] = i->width; out[5] = i->vheight; out[6] = i->xoff; out[7] = i->yoff; out[8] = i->size;
        r->len = 36; return 0;
    case FB_OP_FILL:
        if (q->len < 20) return -EINVAL;
        fb_fill(d, (int)in[0], (int)in[1], (int)in[2], (int)in[3], in[4]);
        return 0;
    case FB_OP_PAN:
        if (q->len < 8) return -EINVAL;
        if (!d->pan || in[1] + i->height > i->vheight || in[0] + i->width > i->width) return -EINVAL;
        if (d->pan(d, in[0], in[1])) return -EIO;
        i->xoff = in[0]; i->yoff = in[1];
        return 0;
    }
    return -ENOSYS;
}

/* ---- kernel IPC service: "console" ---- */
static int console_service(const struct chi_ipc_msg *q, struct chi_ipc_msg *r) {
    chi_u32 *out = (chi_u32 *)r->data;
    switch (q->tag) {
    case CON_OP_WRITE:
        for (chi_u32 k = 0; k < q->len && k < IPC_DATA_MAX; k++) console_putc(q->data[k]);
        return 0;
    case CON_OP_SIZE: {
        int c = 80, rw = 24;
        vt_size(&c, &rw);
        out[0] = c; out[1] = rw; r->len = 8; return 0; }
    case CON_OP_CLEAR: vt_clear(); return 0;
    case CON_OP_GRAPHICS:
        if (q->len < 4) return -EINVAL;
        vt_set_graphics(((const chi_u32 *)q->data)[0]);
        return 0;
    }
    return -ENOSYS;
}

int fb_register(struct fb_device *d) {
    struct fb_info *i = &d->info;
    if ((i->bpp != 32 && i->bpp != 16) || !i->width || !i->height || !i->pitch) return -EINVAL;
    if (!i->vheight) i->vheight = i->height;
    if (!i->size) i->size = i->pitch * i->vheight;
    if (arch_map_device(i->phys, i->size)) return -ENODEV;
    if (!i->virt) i->virt = P2V(i->phys);
    d->next = fbs; fbs = d;
    kprintf("fb: %s %ux%u %ubpp pitch %u at %lx\n", d->name, i->width, i->height, i->bpp, i->pitch, (unsigned long)i->phys);
    if (!primary) {
        primary = d;
        ipc_register_service(IPC_SVC_FB, fb_service);
        fbcon_attach(d);
    }
    return 0;
}

/* ---- /dev/fb0 ---- */
ssize_t fb_read(uint64_t off, void *buf, size_t n) {
    if (!primary) return -ENODEV;
    if (off >= primary->info.size) return 0;
    if (n > primary->info.size - off) n = primary->info.size - off;
    memcpy(buf, primary->info.virt + off, n);
    return n;
}
ssize_t fb_write(uint64_t off, const void *buf, size_t n) {
    if (!primary) return -ENODEV;
    if (off >= primary->info.size) return -28;
    if (n > primary->info.size - off) n = primary->info.size - off;
    memcpy(primary->info.virt + off, buf, n);
    return n;
}

static void fill_var(struct chi_fb_var_screeninfo *v) {
    struct fb_info *i = &primary->info;
    memset(v, 0, sizeof *v);
    v->xres = v->xres_virtual = i->width; v->yres = i->height; v->yres_virtual = i->vheight;
    v->xoffset = i->xoff; v->yoffset = i->yoff; v->bits_per_pixel = i->bpp;
    v->red.offset = i->roff; v->red.length = i->rlen;
    v->green.offset = i->goff; v->green.length = i->glen;
    v->blue.offset = i->boff; v->blue.length = i->blen;
    v->transp.offset = i->toff; v->transp.length = i->tlen;
    v->height = v->width = (chi_u32)-1;                      /* physical size unknown */
}

long fb_ioctl(unsigned long cmd, uintptr_t arg) {
    if (!primary) return -ENODEV;
    struct fb_info *i = &primary->info;
    switch (cmd) {
    case CHI_FBIOGET_VSCREENINFO: {
        if (!uok(arg, sizeof(struct chi_fb_var_screeninfo))) return -EFAULT;
        struct chi_fb_var_screeninfo v; fill_var(&v);
        memcpy((void *)arg, &v, sizeof v);
        return 0; }
    case CHI_FBIOPUT_VSCREENINFO:
    case CHI_FBIOPAN_DISPLAY: {                              /* modes are fixed; only panning can change */
        if (!uok(arg, sizeof(struct chi_fb_var_screeninfo))) return -EFAULT;
        struct chi_fb_var_screeninfo v; memcpy(&v, (void *)arg, sizeof v);
        if (cmd == CHI_FBIOPUT_VSCREENINFO && (v.xres != i->width || v.yres != i->height || v.bits_per_pixel != i->bpp)) return -EINVAL;
        if (v.xoffset == i->xoff && v.yoffset == i->yoff) return 0;
        if (!primary->pan || v.xoffset != 0 || v.yoffset + i->height > i->vheight) return -EINVAL;
        if (primary->pan(primary, v.xoffset, v.yoffset)) return -EIO;
        i->xoff = v.xoffset; i->yoff = v.yoffset;
        return 0; }
    case CHI_FBIOGET_FSCREENINFO: {
        if (!uok(arg, sizeof(struct chi_fb_fix_screeninfo))) return -EFAULT;
        struct chi_fb_fix_screeninfo f; memset(&f, 0, sizeof f);
        strlcpy(f.id, primary->name, sizeof f.id);
        f.smem_start = i->phys; f.smem_len = i->size; f.type = 0; f.visual = 2;       /* PACKED_PIXELS, TRUECOLOR */
        f.ypanstep = primary->pan ? 1 : 0; f.line_length = i->pitch;
        memcpy((void *)arg, &f, sizeof f);
        return 0; }
    case CHI_FBIOBLANK: return 0;
    }
    return -ENOTTY;
}

size_t fb_dump(char *buf, size_t n) {
    if (!primary) return ksnprintf(buf, n, "no framebuffer\n");
    struct fb_info *i = &primary->info;
    return ksnprintf(buf, n, "driver: %s\nmode: %ux%u %ubpp pitch %u virtual-height %u\nphys: 0x%lx size %u\nconsole: %s\n",
                     primary->name, i->width, i->height, i->bpp, i->pitch, i->vheight, (unsigned long)i->phys, i->size,
                     vt_graphics() ? "graphics (owned by a program)" : "text");
}

void console_services_init(void) { ipc_register_service(IPC_SVC_CONSOLE, console_service); }
