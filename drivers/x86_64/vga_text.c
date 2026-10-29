/* Legacy VGA text mode (80x25 at 0xB8000): the "minimal VGA" fallback for machines where no
 * graphics framebuffer driver took over.  A VGA text cell is exactly a vt cell, so this backend is
 * a single store per character. */
#include "kernel.h"
#include "drivers/x86io.h"

#define VGA_MEM ((volatile uint16_t *)P2V(0xB8000))
#define COLS 80
#define ROWS 25

static void draw(int x, int y, uint16_t cell) { VGA_MEM[y * COLS + x] = cell; }
static struct vt_display display = { "vga-text", COLS, ROWS, draw, 0 };
static struct console con = { "vga-text", vt_putc, vt_flush, 0 };

static int probe(void) {
    if (fb_primary()) return -1;                          /* a graphics display already owns the screen */
    volatile uint16_t *m = VGA_MEM;
    uint16_t keep = m[0];
    m[0] = 0xA55A;
    int ok = m[0] == 0xA55A;
    m[0] = keep;
    return ok ? 0 : -1;
}
static int init(void) {
    outb(0x3D4, 0x0A); outb(0x3D5, 0x20);                 /* hide the hardware cursor (vt draws its own) */
    inb(0x3DA); outb(0x3C0, 0x30); uint8_t mode = inb(0x3C1); outb(0x3C0, mode & ~0x08); /* bit 7 = bright background, not blink */
    if (vt_attach(&display)) return -1;
    console_register(&con);
    return 0;
}
static struct kdriver drv = { "vga-text", "VGA text mode 80x25 console", DRV_DISPLAY, 90, probe, init, 0, 0 };
KDRIVER(drv);
