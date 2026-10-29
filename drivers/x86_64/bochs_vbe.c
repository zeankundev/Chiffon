/* Bochs "dispi" VBE display: QEMU's default `-vga std` (and VirtualBox's VBoxVGA).  A real driver:
 * it programs the resolution through the dispi registers, enables the linear framebuffer found in
 * PCI BAR0, and reserves a second page of video memory when it fits so programs can page-flip with
 * FBIOPAN_DISPLAY.  Resolution: `video=WIDTHxHEIGHT` on the kernel command line, default 1024x768. */
#include "kernel.h"
#include "drivers/x86io.h"
#include "drivers/x86_platform.h"

#define DISPI_IDX 0x1CE
#define DISPI_DAT 0x1CF
enum { D_ID, D_XRES, D_YRES, D_BPP, D_ENABLE, D_BANK, D_VIRT_W, D_VIRT_H, D_XOFF, D_YOFF, D_VRAM64K };
#define DISPI_ENABLED 0x01
#define DISPI_LFB     0x40

static struct pci_dev pdev;
static struct fb_device fbd;

static void wr(int i, uint16_t v) { outw(DISPI_IDX, i); outw(DISPI_DAT, v); }
static uint16_t rd(int i) { outw(DISPI_IDX, i); return inw(DISPI_DAT); }

static int probe(void) {
    if (pci_find(0x1234, 0x1111, &pdev) && pci_find(0x80EE, 0xBEEF, &pdev)) return -1;
    return rd(D_ID) >= 0xB0C0 ? 0 : -1;
}

static int pan(struct fb_device *d, uint32_t x, uint32_t y) { wr(D_XOFF, x); wr(D_YOFF, y); return 0; }

static int init(void) {
    uint32_t w = 1024, h = 768;
    const char *v = cmdline_get("video");
    if (v) {
        uint32_t a = 0, b = 0;
        while (*v >= '0' && *v <= '9') a = a * 10 + (*v++ - '0');
        if (*v == 'x') { v++; while (*v >= '0' && *v <= '9') b = b * 10 + (*v++ - '0'); }
        if (a >= 320 && a <= 4096 && b >= 200 && b <= 4096) { w = a; h = b; }
    }
    uint32_t cmd = pci_read32(pdev.bus, pdev.dev, pdev.fn, 4);
    pci_write32(pdev.bus, pdev.dev, pdev.fn, 4, cmd | 2);          /* memory space on */
    uint32_t bar = pci_bar(&pdev, 0);
    if (!bar) return -1;
    uint32_t vram = (uint32_t)rd(D_VRAM64K) * 65536u;
    if (vram < w * h * 4) { w = 1024; h = 768; }                    /* not enough memory for the requested mode */
    if (vram < w * h * 4) return -1;
    uint32_t vh = vram >= 2 * w * h * 4 ? 2 * h : h;                /* second page for flipping, if it fits */

    wr(D_ENABLE, 0);
    wr(D_BPP, 32); wr(D_XRES, w); wr(D_YRES, h);
    wr(D_BANK, 0); wr(D_VIRT_W, w); wr(D_VIRT_H, vh); wr(D_XOFF, 0); wr(D_YOFF, 0);
    wr(D_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (rd(D_XRES) != w || rd(D_YRES) != h) return -1;              /* device refused the mode */

    fbd.name = "bochs-vbe";
    fbd.pan = pan;
    fbd.info = (struct fb_info){ .phys = bar, .width = w, .height = h, .pitch = w * 4, .bpp = 32, .vheight = vh,
                                 .roff = 16, .rlen = 8, .goff = 8, .glen = 8, .boff = 0, .blen = 8, .toff = 24, .tlen = 0 };
    return fb_register(&fbd);
}
static struct kdriver drv = { "bochs-vbe", "QEMU/Bochs VGA (dispi) framebuffer", DRV_DISPLAY, 20, probe, init, 0, 0 };
KDRIVER(drv);
