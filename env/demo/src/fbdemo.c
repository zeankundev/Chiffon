/* fbdemo: draws on the framebuffer through /dev/fb0 (Linux-style ioctls + mmap).
 *   fbdemo      draw a test pattern, bounce a box for a few seconds, flip pages, restore the console
 *   fbdemo -w   leave the pattern up until Enter is pressed
 * Needs a framebuffer: `make run-gfx`. */
#include "libchiffon.h"

static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (v < 0) { chiffon_puts("-"); v = -v; }
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = '0' + v % 10;
    chiffon_puts(b + i);
}

static unsigned *fbp;          /* mapped pixels, 32bpp */
static unsigned stride;        /* in pixels */
static unsigned W, H;

static unsigned gradient(unsigned x, unsigned y) { return ((x * 255 / W) << 16) | ((y * 255 / H) << 8) | 0x60; }
static void rect(int x0, int y0, int w, int h, unsigned c) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) fbp[y * stride + x] = c;
}
static void restore(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) fbp[y * stride + x] = gradient(x, y);
}

int main(int argc, char **argv, char **envp) {
    int wait = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'w';
    int fd = chiffon_open("/dev/fb0", O_RDWR, 0);
    if (fd < 0) { chiffon_puts("fbdemo: no /dev/fb0 (is there a display? try `make run-gfx`)\n"); return 1; }
    struct chi_fb_var_screeninfo v; struct chi_fb_fix_screeninfo f;
    if (chiffon_ioctl(fd, CHI_FBIOGET_VSCREENINFO, &v) || chiffon_ioctl(fd, CHI_FBIOGET_FSCREENINFO, &f)) { chiffon_puts("fbdemo: ioctl failed\n"); return 1; }
    chiffon_puts("fb0: "); put_num(v.xres); chiffon_puts("x"); put_num(v.yres); chiffon_puts(" "); put_num(v.bits_per_pixel);
    chiffon_puts("bpp, pitch "); put_num(f.line_length); chiffon_puts(", virtual height "); put_num(v.yres_virtual); chiffon_puts("\n");
    if (v.bits_per_pixel != 32) { chiffon_puts("fbdemo: needs a 32bpp framebuffer\n"); return 1; }

    W = v.xres; H = v.yres; stride = f.line_length / 4;
    long m = chiffon_mmap(0, (size_t)f.line_length * v.yres_virtual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m < 0 && m > -4096) { chiffon_puts("fbdemo: mmap failed: "); put_num(m); chiffon_puts("\n"); return 1; }
    fbp = (unsigned *)m;

    int con = chiffon_open("/dev/console", O_RDWR, 0);
    if (con >= 0) chiffon_ioctl(con, CHI_KDSETMODE, (void *)(long)CHI_KD_GRAPHICS);     /* console stops drawing */

    restore(0, 0, W, H);
    static const unsigned bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    for (int i = 0; i < 8; i++) rect(i * (W / 8), 0, W / 8, 64, bars[i]);

    int x = 20, y = 100, dx = 7, dy = 5, s = 48;
    for (int frame = 0; frame < (wait ? 60 : 150); frame++) {
        restore(x, y, s, s);
        x += dx; y += dy;
        if (x < 0 || x + s > (int)W) { dx = -dx; x += 2 * dx; }
        if (y < 70 || y + s > (int)H) { dy = -dy; y += 2 * dy; }
        rect(x, y, s, s, 0xFFA000);
        rect(x + 8, y + 8, s - 16, s - 16, 0x202020);
        chiffon_sleep_ms(20);
    }

    if (v.yres_virtual >= 2 * v.yres) {                    /* page flip: paint page 2, show it, flip back */
        for (unsigned yy = 0; yy < H; yy++) for (unsigned xx = 0; xx < W; xx++) fbp[(H + yy) * stride + xx] = ((xx / 32 + yy / 32) & 1) ? 0x2040C0 : 0xE0E0E0;
        v.yoffset = H;
        long r = chiffon_ioctl(fd, CHI_FBIOPAN_DISPLAY, &v);
        chiffon_puts(r ? "pan failed\n" : "flipped to page 2\n");
        chiffon_sleep_ms(wait ? 1500 : 700);
        v.yoffset = 0; chiffon_ioctl(fd, CHI_FBIOPAN_DISPLAY, &v);
    }

    if (wait) { chiffon_puts("pattern up, press Enter\n"); char b[8]; chiffon_read(0, b, sizeof b); }
    chiffon_munmap(fbp, (size_t)f.line_length * v.yres_virtual);
    if (con >= 0) chiffon_ioctl(con, CHI_KDSETMODE, (void *)(long)CHI_KD_TEXT);
    chiffon_puts("fbdemo: done\n");
    return 0;
}
