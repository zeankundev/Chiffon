/* Linear framebuffer handed over by a Multiboot loader (GRUB with `set gfxpayload=...`).  The mode
 * is whatever the loader set; this driver only adopts it.  Preferred over bochs-vbe so that real
 * hardware booted through GRUB gets a display without a chipset-specific driver. */
#include "kernel.h"
#include "drivers/x86_platform.h"

static struct { int valid; uint64_t addr; uint32_t pitch, width, height; uint8_t bpp, type, rpos, rsize, gpos, gsize, bpos, bsize; } boot;
static struct fb_device fbd;

void mb_fb_boot_info(const uint32_t *m) {
    if (!(m[0] & (1u << 12))) return;                    /* framebuffer info present */
    const uint8_t *b = (const uint8_t *)m;
    boot.addr = *(const uint64_t *)(b + 88);
    boot.pitch = m[24]; boot.width = m[25]; boot.height = m[26];
    boot.bpp = b[108]; boot.type = b[109];
    boot.rpos = b[110]; boot.rsize = b[111]; boot.gpos = b[112]; boot.gsize = b[113]; boot.bpos = b[114]; boot.bsize = b[115];
    boot.valid = 1;
}

static int probe(void) {
    if (!boot.valid || boot.type != 1 || (boot.bpp != 32 && boot.bpp != 16)) return -1;   /* direct RGB only */
    return boot.addr < 0x100000000ULL ? 0 : -1;
}
static int init(void) {
    fbd.name = "multiboot-fb";
    fbd.info = (struct fb_info){ .phys = boot.addr, .width = boot.width, .height = boot.height, .pitch = boot.pitch, .bpp = boot.bpp,
                                 .roff = boot.rpos, .rlen = boot.rsize, .goff = boot.gpos, .glen = boot.gsize, .boff = boot.bpos, .blen = boot.bsize };
    return fb_register(&fbd);
}
static struct kdriver drv = { "multiboot-fb", "bootloader-provided framebuffer", DRV_DISPLAY, 10, probe, init, 0, 0 };
KDRIVER(drv);
