/* USB device classes over xhci.c: HID boot-protocol keyboards and mice (keys reach the console and
 * /dev/input/event0 through kbd_scancode(), pointer motion through input_rel()/input_btn()), and
 * bulk-only mass storage, mounted read-only under /media/usbN when the VFS is up and unmounted when the
 * device is removed.  Descriptors are parsed by hand; only the interfaces this driver can use are configured. */
#include "kernel.h"
#include "fat.h"
#include "drivers/usb.h"

struct usbdev {
    int attached, port, speed, unit, unit_set;
    uint16_t vid, pid;
    uint8_t kbd_ep, kbd_if, mouse_ep, mouse_if, msc_in, msc_out;   /* endpoint addresses; 0 = none */
    uint8_t prev_kbd[8], prev_btn;
    int msc, mount_pending;
    void *vol;
    uint32_t tag, nblk, blksz;
};
static struct usbdev devs[USB_MAX_PORTS + 1];

static const char *speed_name(int s) {
    return s == USB_LS ? "low" : s == USB_FS ? "full" : s == USB_HS ? "high" : "super";
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int ctrl(int p, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *buf, uint16_t len) {
    struct usb_setup s = { rt, req, val, idx, len };
    return xhci_control(p, &s, buf);
}

/* ---- HID boot keyboard: USB usage (page 0x07) -> scancode set 1; bit 8 = E0 prefix ---- */
static const uint16_t hid_sc[256] = {
    [0x04]=0x1E, [0x05]=0x30, [0x06]=0x2E, [0x07]=0x20, [0x08]=0x12, [0x09]=0x21, [0x0A]=0x22, [0x0B]=0x23,
    [0x0C]=0x17, [0x0D]=0x24, [0x0E]=0x25, [0x0F]=0x26, [0x10]=0x32, [0x11]=0x31, [0x12]=0x18, [0x13]=0x19,
    [0x14]=0x10, [0x15]=0x13, [0x16]=0x1F, [0x17]=0x14, [0x18]=0x16, [0x19]=0x2F, [0x1A]=0x11, [0x1B]=0x2D,
    [0x1C]=0x15, [0x1D]=0x2C,
    [0x1E]=0x02, [0x1F]=0x03, [0x20]=0x04, [0x21]=0x05, [0x22]=0x06, [0x23]=0x07, [0x24]=0x08, [0x25]=0x09,
    [0x26]=0x0A, [0x27]=0x0B,
    [0x28]=0x1C, [0x29]=0x01, [0x2A]=0x0E, [0x2B]=0x0F, [0x2C]=0x39, [0x2D]=0x0C, [0x2E]=0x0D, [0x2F]=0x1A,
    [0x30]=0x1B, [0x31]=0x2B, [0x33]=0x27, [0x34]=0x28, [0x35]=0x29, [0x36]=0x33, [0x37]=0x34, [0x38]=0x35,
    [0x39]=0x3A,
    [0x3A]=0x3B, [0x3B]=0x3C, [0x3C]=0x3D, [0x3D]=0x3E, [0x3E]=0x3F, [0x3F]=0x40, [0x40]=0x41, [0x41]=0x42,
    [0x42]=0x43, [0x43]=0x44, [0x44]=0x57, [0x45]=0x58,
    [0x46]=0x137, [0x47]=0x46, [0x49]=0x152, [0x4A]=0x147, [0x4B]=0x149, [0x4C]=0x153, [0x4D]=0x14F,
    [0x4E]=0x151, [0x4F]=0x14D, [0x50]=0x14B, [0x51]=0x150, [0x52]=0x148,
    [0x53]=0x45, [0x54]=0x135, [0x55]=0x37, [0x56]=0x4A, [0x57]=0x4E, [0x58]=0x11C,
    [0x59]=0x4F, [0x5A]=0x50, [0x5B]=0x51, [0x5C]=0x4B, [0x5D]=0x4C, [0x5E]=0x4D, [0x5F]=0x47, [0x60]=0x48,
    [0x61]=0x49, [0x62]=0x52, [0x63]=0x53,
};
/* modifier byte bit -> scancode (bit 8 = E0 prefix) */
static const struct { uint8_t bit; uint16_t sc; } hid_mod[8] = {
    { 0x01, 0x1D }, { 0x02, 0x2A }, { 0x04, 0x38 }, { 0x08, 0x15B },
    { 0x10, 0x11D }, { 0x20, 0x36 }, { 0x40, 0x138 }, { 0x80, 0x15C },
};

static void kbd_key(uint16_t v, int release) { kbd_scancode((uint8_t)(v & 0xFF), release, (v >> 8) & 1); }
static int in_report(const uint8_t *r, uint8_t k) { for (int i = 2; i < 8; i++) if (r[i] == k) return 1; return 0; }

static void kbd_report(struct usbdev *d, const uint8_t *r, uint32_t len) {
    if (len < 8) return;
    const uint8_t *old = d->prev_kbd;
    for (int i = 0; i < 8; i++)
        if ((old[0] ^ r[0]) & hid_mod[i].bit) kbd_key(hid_mod[i].sc, !(r[0] & hid_mod[i].bit));
    for (int i = 2; i < 8; i++) {                        /* releases first, then presses */
        uint8_t k = old[i];
        if (k > 3 && !in_report(r, k)) kbd_key(hid_sc[k], 1);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k > 3 && !in_report(old, k)) kbd_key(hid_sc[k], 0);
    }
    memcpy(d->prev_kbd, r, 8);
}

static void mouse_report(struct usbdev *d, const uint8_t *r, uint32_t len) {
    if (len < 3) return;
    uint8_t changed = d->prev_btn ^ r[0];
    for (int b = 0; b < 3; b++)
        if (changed & (1u << b)) input_btn((unsigned)b, (r[0] >> b) & 1);
    input_rel(0, (int8_t)r[1]);
    input_rel(1, (int8_t)r[2]);
    d->prev_btn = r[0] & 7;
}

void usb_report(int p, uint8_t addr, const uint8_t *data, uint32_t len) {
    struct usbdev *d = &devs[p];
    if (!d->attached) return;
    if (addr == d->kbd_ep) kbd_report(d, data, len);
    else if (addr == d->mouse_ep) mouse_report(d, data, len);
}

/* ---- bulk-only transport for mass storage ---- */
static int bot(struct usbdev *d, const uint8_t *cdb, uint8_t cdblen, void *data, uint32_t dlen, int in) {
    uint8_t cbw[31];
    memset(cbw, 0, sizeof cbw);
    uint32_t tag = ++d->tag;
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    for (int i = 0; i < 4; i++) { cbw[4 + i] = (uint8_t)(tag >> (8 * i)); cbw[8 + i] = (uint8_t)(dlen >> (8 * i)); }
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdblen;
    memcpy(cbw + 15, cdb, cdblen);
    if (xhci_bulk(d->port, d->msc_out, cbw, sizeof cbw) != (int)sizeof cbw) return -EIO;
    if (dlen && xhci_bulk(d->port, in ? d->msc_in : d->msc_out, data, dlen) < 0) return -EIO;
    uint8_t csw[13];
    if (xhci_bulk(d->port, d->msc_in, csw, sizeof csw) != (int)sizeof csw) return -EIO;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' || csw[12] != 0) return -EIO;
    return 0;
}

/* sector reader for the FAT driver: READ(10) in runs of up to 64 blocks */
static int msc_rd(void *ctx, uint64_t lba, uint32_t n, void *buf) {
    struct usbdev *d = ctx;
    if (!d->attached || !d->msc || d->blksz != 512) return -EIO;
    uint8_t *out = buf;
    while (n) {
        uint32_t k = n > 64 ? 64 : n;
        uint8_t c[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                          0, (uint8_t)(k >> 8), (uint8_t)k, 0 };
        if (bot(d, c, 10, out, k * 512, 1)) return -EIO;
        out += k * 512; lba += k; n -= k;
    }
    return 0;
}

static void msc_init(struct usbdev *d) {
    for (int i = 0; i < 3; i++) {                        /* devices report "becoming ready" first */
        uint8_t c[6] = { 0 };
        if (!bot(d, c, 6, 0, 0, 0)) break;
    }
    uint8_t inq[36] = { 0 }, icdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (!bot(d, icdb, 6, inq, 36, 1)) {                 /* vendor (8) and product (16) strings, space padded */
        char vendor[9], product[17];
        for (int i = 0; i < 8; i++) vendor[i] = inq[8 + i] >= 32 && inq[8 + i] < 127 ? (char)inq[8 + i] : '?';
        for (int i = 0; i < 16; i++) product[i] = inq[16 + i] >= 32 && inq[16 + i] < 127 ? (char)inq[16 + i] : '?';
        vendor[8] = 0; product[16] = 0;
        kprintf("[usb] port %d: mass storage \"%s %s\"\n", d->port, vendor, product);
    }
    uint8_t cap[8] = { 0 }, ccdb[10] = { 0x25 };
    if (bot(d, ccdb, 10, cap, 8, 1)) { kprintf("[usb] port %d: READ CAPACITY failed\n", d->port); return; }
    d->nblk = (uint64_t)be32(cap) + 1;
    d->blksz = be32(cap + 4);
    if (d->blksz != 512) { kprintf("[usb] port %d: %u-byte blocks not supported\n", d->port, d->blksz); return; }
    d->msc = 1;
    d->mount_pending = 1;
    kprintf("[usb] port %d: %lu MiB of storage\n", d->port, (unsigned long)(d->nblk / 2048));
}

/* ---- enumeration ---- */
static int unit_alloc(void) {
    for (int u = 0; u < 8; u++) {
        int used = 0;
        for (int p = 1; p <= USB_MAX_PORTS; p++) if (devs[p].unit_set && devs[p].unit == u) used = 1;
        if (!used) return u;
    }
    return -1;
}

void usb_attach(int p, int speed) {
    struct usbdev *d = &devs[p];
    memset(d, 0, sizeof *d);
    d->port = p; d->speed = speed; d->attached = 1;
    uint8_t dd[18];
    if (ctrl(p, 0x80, 6, 0x0100, 0, dd, 8) != 8) { kprintf("[usb] port %d: no device descriptor\n", p); return; }
    uint16_t want = dd[7] ? dd[7] : 8;
    uint16_t def = speed == USB_SS ? 512 : speed == USB_HS ? 64 : 8;
    if (want != def && xhci_set_ep0_mps(p, want)) { kprintf("[usb] port %d: cannot set control packet size\n", p); return; }
    if (ctrl(p, 0x80, 6, 0x0100, 0, dd, 18) != 18) { kprintf("[usb] port %d: device descriptor failed\n", p); return; }
    d->vid = le16(dd + 8); d->pid = le16(dd + 10);
    kprintf("[usb] port %d: %s-speed device %04x:%04x\n", p, speed_name(speed), d->vid, d->pid);

    uint8_t hdr[9];
    if (ctrl(p, 0x80, 6, 0x0200, 0, hdr, 9) != 9) return;
    uint16_t total = le16(hdr + 2);
    if (total < 9 || total > 1024) return;
    uint8_t *cfg = kmalloc(total);
    if (!cfg) return;
    if (ctrl(p, 0x80, 6, 0x0200, 0, cfg, total) != total) { kfree(cfg); return; }

    struct usb_ep eps[8]; int n = 0;
    int curif = 0, have_kbd = 0, have_mouse = 0, have_msc = 0;
    for (uint32_t o = 0; o + 2 <= total;) {
        uint8_t l = cfg[o], t = cfg[o + 1];
        if (l < 2 || o + l > total) break;
        if (t == 4 && l >= 9) {                           /* interface: alternate setting 0 only */
            curif = 0;
            uint8_t cls = cfg[o + 5], sub = cfg[o + 6], pr = cfg[o + 7];
            if (cfg[o + 3] == 0) {
                if (cls == 3 && sub == 1 && pr == 1 && !have_kbd) { curif = 1; have_kbd = 1; d->kbd_if = cfg[o + 2]; }
                else if (cls == 3 && sub == 1 && pr == 2 && !have_mouse) { curif = 2; have_mouse = 1; d->mouse_if = cfg[o + 2]; }
                else if (cls == 8 && sub == 6 && pr == 0x50 && !have_msc) { curif = 3; have_msc = 1; }
            }
        } else if (t == 5 && l >= 7) {                    /* endpoint of the interface chosen above */
            uint8_t addr = cfg[o + 2], attr = cfg[o + 3] & 3;
            uint16_t mps = le16(cfg + o + 4);
            int use = 0;
            if (curif == 1 && attr == 3 && (addr & 0x80)) { d->kbd_ep = addr; use = 1; }
            else if (curif == 2 && attr == 3 && (addr & 0x80)) { d->mouse_ep = addr; use = 1; }
            else if (curif == 3 && attr == 2) { if (addr & 0x80) d->msc_in = addr; else d->msc_out = addr; use = 1; }
            if (use && n < 8) eps[n++] = (struct usb_ep){ addr, (uint8_t)attr, mps, cfg[o + 6] };
        }
        o += l;
    }
    if (!n) { kprintf("[usb] port %d: no keyboard, mouse or mass storage interface\n", p); kfree(cfg); return; }
    if (xhci_configure(p, eps, n)) { kprintf("[usb] port %d: configure failed\n", p); kfree(cfg); return; }
    if (ctrl(p, 0x00, 9, cfg[5], 0, 0, 0)) { kprintf("[usb] port %d: set configuration failed\n", p); kfree(cfg); return; }
    if (d->kbd_ep) {
        ctrl(p, 0x21, 0x0B, 0, d->kbd_if, 0, 0);          /* SET_PROTOCOL: boot protocol */
        ctrl(p, 0x21, 0x0A, 0, d->kbd_if, 0, 0);          /* SET_IDLE: report on change only */
        xhci_interrupt_start(p, d->kbd_ep);
        kprintf("[usb] port %d: keyboard\n", p);
    }
    if (d->mouse_ep) {
        ctrl(p, 0x21, 0x0B, 0, d->mouse_if, 0, 0);
        xhci_interrupt_start(p, d->mouse_ep);
        kprintf("[usb] port %d: mouse\n", p);
    }
    if (d->msc_in && d->msc_out) msc_init(d);
    kfree(cfg);
}

void usb_detach(int p) {
    struct usbdev *d = &devs[p];
    if (d->vol) { fat_unmount(d->vol); kprintf("[usb] port %d: /media/usb%d unmounted\n", p, d->unit); }
    memset(d, 0, sizeof *d);
}

void usb_poll(void) {
    if (!vfs_ready()) return;                             /* volumes wait until the namespace exists */
    for (int p = 1; p <= USB_MAX_PORTS; p++) {
        struct usbdev *d = &devs[p];
        if (!d->attached || !d->mount_pending) continue;
        d->mount_pending = 0;
        int u = unit_alloc();
        if (u < 0) { kprintf("[usb] port %d: no free /media slot\n", p); continue; }
        d->unit = u; d->unit_set = 1;
        char dir[24];
        ksnprintf(dir, sizeof dir, "/media/usb%d", u);
        void *h = 0;
        int r = fat_mount(dir, msc_rd, d, d->nblk, &h);
        if (r) { d->unit_set = 0; kprintf("[usb] port %d: no FAT32 or exFAT volume (%d)\n", p, r); }
        else d->vol = h;
    }
}
