/* Framebuffer text console: a vt_display that blits 8x16 glyphs into the primary framebuffer, and
 * a console that feeds kernel/tty output into the terminal emulator. */
#include "kernel.h"

static struct fb_device *fb;
static uint32_t pal[16];
static int xoff, yoff;
/* pixel rectangle painted since the last flush: the device is told about this, and only this */
static int dirty, dx0, dy0, dx1, dy1;

static void mark(int x0, int y0, int x1, int y1) {       /* inclusive-exclusive pixel rectangle */
    if (!dirty) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; dirty = 1; return; }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

static void draw(int x, int y, uint16_t cell) {
    unsigned ch = cell & 0xFF;
    const uint8_t *g = font8x16_data[(ch < FONT_FIRST || ch - FONT_FIRST >= FONT_NGLYPH) ? 0 : ch - FONT_FIRST];
    uint32_t f = pal[(cell >> 8) & 15], b = pal[(cell >> 12) & 15];
    mark(xoff + x * FONT_W, yoff + y * FONT_H, xoff + (x + 1) * FONT_W, yoff + (y + 1) * FONT_H);
    uint8_t *row = fb->info.virt + (size_t)(yoff + y * FONT_H) * fb->info.pitch + (size_t)(xoff + x * FONT_W) * (fb->info.bpp / 8);
    for (int r = 0; r < FONT_H; r++, row += fb->info.pitch) {
        unsigned bits = g[r];
        if (fb->info.bpp == 32) {
            uint32_t *p = (uint32_t *)row;
            for (int k = 0; k < FONT_W; k++) p[k] = (bits & (0x80 >> k)) ? f : b;
        } else if (fb->info.bpp == 24) {
            for (int k = 0; k < FONT_W; k++) {
                uint32_t px = (bits & (0x80 >> k)) ? f : b;
                row[3 * k + 0] = (uint8_t)px;
                row[3 * k + 1] = (uint8_t)(px >> 8);
                row[3 * k + 2] = (uint8_t)(px >> 16);
            }
        } else {
            uint16_t *p = (uint16_t *)row;
            for (int k = 0; k < FONT_W; k++) p[k] = (bits & (0x80 >> k)) ? (uint16_t)f : (uint16_t)b;
        }
    }
}
static void redraw_all(void) { fb_fill(fb, 0, 0, fb->info.width, fb->info.height, 0); }
static struct vt_display display = { "fbcon", 0, 0, draw, redraw_all };
static void fbcon_write(int c) { vt_write(0, c); }     /* kernel messages land on /dev/tty1 */

/* Paint the changed cells, then refresh the pixels that changed (for SVGA-style devices). */
static void fbcon_flush(void) {
    vt_flush();
    if (!dirty) return;
    dirty = 0;
    fb_damage(fb, dx0, dy0, dx1 - dx0, dy1 - dy0);
}
static struct console fbcon = { "fbcon", fbcon_write, fbcon_flush, 0, 1 };

void fbcon_attach(struct fb_device *d) {
    const char *opt = cmdline_get("fbcon");
    if (opt && (opt[0] == '0' || !kstrcasecmp(opt, "off"))) return;
    if (d->info.bpp != 32 && d->info.bpp != 24 && d->info.bpp != 16) return;
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
