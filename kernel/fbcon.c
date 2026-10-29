/* Framebuffer text console: a vt_display that blits 8x16 glyphs into the primary framebuffer, and
 * a console that feeds kernel/tty output into the terminal emulator. */
#include "kernel.h"

static struct fb_device *fb;
static uint32_t pal[16];
static int xoff, yoff;

static void draw(int x, int y, uint16_t cell) {
    unsigned ch = cell & 0xFF;
    const uint8_t *g = font8x16_data[(ch < FONT_FIRST || ch - FONT_FIRST >= FONT_NGLYPH) ? 0 : ch - FONT_FIRST];
    uint32_t f = pal[(cell >> 8) & 15], b = pal[(cell >> 12) & 15];
    uint8_t *row = fb->info.virt + (size_t)(yoff + y * FONT_H) * fb->info.pitch + (size_t)(xoff + x * FONT_W) * (fb->info.bpp / 8);
    for (int r = 0; r < FONT_H; r++, row += fb->info.pitch) {
        unsigned bits = g[r];
        if (fb->info.bpp == 32) {
            uint32_t *p = (uint32_t *)row;
            for (int k = 0; k < FONT_W; k++) p[k] = (bits & (0x80 >> k)) ? f : b;
        } else {
            uint16_t *p = (uint16_t *)row;
            for (int k = 0; k < FONT_W; k++) p[k] = (bits & (0x80 >> k)) ? (uint16_t)f : (uint16_t)b;
        }
    }
}
static void redraw_all(void) { fb_fill(fb, 0, 0, fb->info.width, fb->info.height, 0); }
static struct vt_display display = { "fbcon", 0, 0, draw, redraw_all };
static struct console fbcon = { "fbcon", vt_putc, vt_flush, 0 };

void fbcon_attach(struct fb_device *d) {
    const char *opt = cmdline_get("fbcon");
    if (opt && (opt[0] == '0' || !kstrcasecmp(opt, "off"))) return;
    if (d->info.bpp != 32 && d->info.bpp != 16) return;
    fb = d;
    for (int i = 0; i < 16; i++) pal[i] = fb_pack(&d->info, vt_palette[i]);
    display.cols = d->info.width / FONT_W; display.rows = d->info.height / FONT_H;
    xoff = (d->info.width - display.cols * FONT_W) / 2;
    yoff = (d->info.height - display.rows * FONT_H) / 2;
    if (display.cols > VT_MAXCOLS) { xoff += (display.cols - VT_MAXCOLS) * FONT_W / 2; display.cols = VT_MAXCOLS; }
    if (display.rows > VT_MAXROWS) { yoff += (display.rows - VT_MAXROWS) * FONT_H / 2; display.rows = VT_MAXROWS; }
    fb_fill(d, 0, 0, d->info.width, d->info.height, 0);
    if (vt_attach(&display)) return;
    console_register(&fbcon);                               /* replays the boot log onto the screen */
    kprintf("fbcon: %dx%d text on %ux%u %s\n", display.cols, display.rows, d->info.width, d->info.height, d->name);
}
