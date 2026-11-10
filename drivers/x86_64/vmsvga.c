/* VMware SVGA2 framebuffer driver.
 *
 * Covers VirtualBox "VMSVGA" and "VBoxSVGA" adapters (PCI 15AD:0405) as well
 * as VMware Workstation/Player (same PCI IDs, same register protocol).
 *
 * The SVGA2 device is controlled through a small set of indexed registers
 * reached via two I/O ports whose base address comes from PCI BAR0:
 *
 *   base + SVGA_INDEX_PORT (0)  – write the register index
 *   base + SVGA_VALUE_PORT (1)  – read/write the register value
 *
 * Bring-up sequence:
 *   1. Write SVGA_REG_ID with SVGA_ID_2 to identify the device.
 *   2. Set SVGA_REG_WIDTH / HEIGHT / BITS_PER_PIXEL.
 *   3. Enable by writing 1 to SVGA_REG_ENABLE.
 *   4. Read SVGA_REG_FB_START and SVGA_REG_BYTES_PER_LINE for the linear
 *      framebuffer address and pitch.
 *
 * The framebuffer lies in PCI BAR1 (memory-mapped); we do NOT need to program
 * the BAR ourselves – the firmware/BIOS already did that.  We only need to
 * read back the physical address.
 *
 * References: VMware SVGA Device Developer Kit, open-vm-tools, QEMU vmware_vga. */
#include "kernel.h"
#include "drivers/x86io.h"
#include "drivers/x86_platform.h"

/* PCI IDs */
#define VMSVGA_VENDOR 0x15AD
#define VMSVGA_DEVICE 0x0405

/* I/O register layout (offsets from BAR0 base, in 32-bit words / 4-byte steps) */
#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1

/* Register indices */
#define SVGA_REG_ID            0
#define SVGA_REG_ENABLE        1
#define SVGA_REG_WIDTH         2
#define SVGA_REG_HEIGHT        3
#define SVGA_REG_MAX_WIDTH     4
#define SVGA_REG_MAX_HEIGHT    5
#define SVGA_REG_DEPTH         6
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_PSEUDOCOLOR   8
#define SVGA_REG_RED_MASK      9
#define SVGA_REG_GREEN_MASK    10
#define SVGA_REG_BLUE_MASK     11
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START      13
#define SVGA_REG_FB_OFFSET     14
#define SVGA_REG_VRAM_SIZE     15
#define SVGA_REG_FB_SIZE       16
#define SVGA_REG_CAPABILITIES  17

/* Magic ID values */
#define SVGA_MAGIC   0x900000UL
#define SVGA_ID_2    (SVGA_MAGIC << 8 | 2)
#define SVGA_ID_1    (SVGA_MAGIC << 8 | 1)
#define SVGA_ID_0    (SVGA_MAGIC << 8 | 0)
#define SVGA_ID_INVALID 0xFFFFFFFFu

/* Default resolution if no kernel command-line override is given */
#define DEFAULT_W 1024
#define DEFAULT_H  768

static struct pci_dev pdev;
static uint16_t iobase;
static struct fb_device fbd;

static void svga_write(uint32_t reg, uint32_t val) {
    outl(iobase + SVGA_INDEX_PORT * 4, reg);
    outl(iobase + SVGA_VALUE_PORT * 4, val);
}
static uint32_t svga_read(uint32_t reg) {
    outl(iobase + SVGA_INDEX_PORT * 4, reg);
    return inl(iobase + SVGA_VALUE_PORT * 4);
}

static int probe(void) {
    if (pci_find(VMSVGA_VENDOR, VMSVGA_DEVICE, &pdev)) return -1;
    /* Enable I/O + memory space on the PCI device */
    uint32_t cmd = pci_read32(pdev.bus, pdev.dev, pdev.fn, 4);
    pci_write32(pdev.bus, pdev.dev, pdev.fn, 4, cmd | 3);
    /* BAR0 is the I/O register base */
    uint32_t bar0 = pci_bar(&pdev, 0);
    if (!bar0) return -1;
    iobase = (uint16_t)(bar0 & 0xFFFC);
    /* Identify the device */
    svga_write(SVGA_REG_ID, SVGA_ID_2);
    uint32_t id = svga_read(SVGA_REG_ID);
    if (id != SVGA_ID_2) {
        svga_write(SVGA_REG_ID, SVGA_ID_1);
        id = svga_read(SVGA_REG_ID);
        if (id != SVGA_ID_1) return -1;
    }
    return 0;
}

static int init(void) {
    /* Pick a resolution (kernel command line: video=WIDTHxHEIGHT) */
    uint32_t w = DEFAULT_W, h = DEFAULT_H;
    const char *v = cmdline_get("video");
    if (v) {
        uint32_t a = 0, b = 0;
        while (*v >= '0' && *v <= '9') a = a * 10 + (*v++ - '0');
        if (*v == 'x') { v++; while (*v >= '0' && *v <= '9') b = b * 10 + (*v++ - '0'); }
        if (a >= 320 && a <= 4096 && b >= 200 && b <= 4096) { w = a; h = b; }
    }
    /* Clamp to device maximum */
    uint32_t mw = svga_read(SVGA_REG_MAX_WIDTH);
    uint32_t mh = svga_read(SVGA_REG_MAX_HEIGHT);
    if (mw && w > mw) w = mw;
    if (mh && h > mh) h = mh;

    /* Program the mode and enable the device */
    svga_write(SVGA_REG_ENABLE,        0);         /* disable while changing mode */
    svga_write(SVGA_REG_WIDTH,         w);
    svga_write(SVGA_REG_HEIGHT,        h);
    svga_write(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_write(SVGA_REG_ENABLE,        1);

    uint32_t pitch  = svga_read(SVGA_REG_BYTES_PER_LINE);
    uint32_t fb_pa  = svga_read(SVGA_REG_FB_START);
    uint32_t fb_off = svga_read(SVGA_REG_FB_OFFSET);

    if (!fb_pa || !pitch) {
        kprintf("vmsvga: device did not provide a framebuffer address\n");
        svga_write(SVGA_REG_ENABLE, 0);
        return -1;
    }

    uint32_t actual_w = svga_read(SVGA_REG_WIDTH);
    uint32_t actual_h = svga_read(SVGA_REG_HEIGHT);
    uint32_t actual_bpp = svga_read(SVGA_REG_BITS_PER_PIXEL);
    if (actual_bpp != 32 && actual_bpp != 16) {
        kprintf("vmsvga: unsupported bpp %u\n", (unsigned)actual_bpp);
        svga_write(SVGA_REG_ENABLE, 0);
        return -1;
    }

    uint64_t phys = (uint64_t)fb_pa + fb_off;
    fbd.name = "vmsvga";
    fbd.info = (struct fb_info){
        .phys   = phys,
        .width  = actual_w,
        .height = actual_h,
        .pitch  = pitch,
        .bpp    = (uint8_t)actual_bpp,
        /* VMware SVGA2 in 32bpp always delivers xRGB (blue at bit 0) */
        .roff = 16, .rlen = 8,
        .goff =  8, .glen = 8,
        .boff =  0, .blen = 8,
    };
    return fb_register(&fbd);
}

/* Priority 15: after multiboot-fb (10) and bochs-vbe (20) but before vga-text (90).
 * We prefer the GRUB-provided framebuffer if GRUB managed to set gfxpayload, and
 * fall back to programming SVGA ourselves when it did not. */
static struct kdriver drv = {
    "vmsvga", "VMware/VirtualBox SVGA2 framebuffer",
    DRV_DISPLAY, 15, probe, init, 0, 0
};
KDRIVER(drv);
