/* VMware SVGA II display: VMware Workstation/Player, VirtualBox "VMSVGA" and "VBoxSVGA" (both present
 * as PCI 15AD:0405) and QEMU's `-vga vmware`.  Matched by PCI ID table, like a Linux pci_driver.
 *
 * The device has three PCI resources, used the way Linux's vmwgfx uses them:
 *   BAR0  I/O ports.  Index port at +0, value port at +1 (byte offsets; the register number goes in the
 *         index port and its contents in the value port).
 *   BAR1  video RAM.  The scanout framebuffer lives here, at SVGA_REG_FB_OFFSET from the base.
 *   BAR2  the command FIFO, a ring of 32-bit words the guest fills with drawing commands.
 *
 * Mode setting is plain register writes.  The device does not refresh the screen from video RAM by
 * itself: after the kernel paints a rectangle, an SVGA_CMD_UPDATE for that rectangle goes into the FIFO
 * and the device copies it to the display.  That is the flush hook below; without it the screen
 * stays black although the pixels are in memory. */
#include "kernel.h"
#include "drivers/x86io.h"
#include "drivers/x86_platform.h"

static const struct pci_id ids[] = { { 0x15AD, 0x0405 } };       /* VMware SVGA II / VirtualBox VMSVGA, VBoxSVGA */

/* register numbers, written to the index port */
enum {
    R_ID = 0, R_ENABLE, R_WIDTH, R_HEIGHT, R_MAX_WIDTH, R_MAX_HEIGHT, R_DEPTH, R_BITS_PER_PIXEL,
    R_PSEUDOCOLOR, R_RED_MASK, R_GREEN_MASK, R_BLUE_MASK, R_BYTES_PER_LINE, R_FB_START, R_FB_OFFSET,
    R_VRAM_SIZE, R_FB_SIZE, R_CAPABILITIES, R_MEM_START, R_MEM_SIZE, R_CONFIG_DONE, R_SYNC, R_BUSY,
};
#define SVGA_ID_2       0x90000002u
#define SVGA_CMD_UPDATE 1u
/* FIFO header, in 32-bit words: MIN, MAX, NEXT_CMD, STOP (byte offsets into the FIFO) */
#define F_MIN  0
#define F_MAX  1
#define F_NEXT 2
#define F_STOP 3
#define FIFO_HDR   0x1000u               /* room for the header and any extended FIFO registers */
#define FIFO_MAXB  (1u << 20)            /* ring size used, however large the device's FIFO is */
#define SPIN_LIMIT 10000000u             /* device too slow or stuck: stop waiting, never hang the kernel */

#define DEFAULT_W 1024
#define DEFAULT_H 768

static struct pci_dev pdev;
static uint16_t iobase;
static volatile uint32_t *fifo;          /* BAR2 (uncacheable), NULL when the device has no FIFO */
static uint32_t fifo_bytes;
static struct fb_device fbd;

static void svga_write(uint32_t reg, uint32_t val) {
    outl(iobase + 0, reg);               /* index port */
    outl(iobase + 1, val);               /* value port */
}
static uint32_t svga_read(uint32_t reg) {
    outl(iobase + 0, reg);
    return inl(iobase + 1);
}

/* Wait until the device has consumed the FIFO.  Bounded: a stuck device must not hang the kernel. */
static void fifo_drain(void) {
    for (uint32_t i = 0; i < SPIN_LIMIT && fifo[F_STOP] != fifo[F_NEXT]; i++)
        __asm__ volatile("pause");
}

/* Queue one SVGA_CMD_UPDATE and kick the device.  The FIFO is drained before every command, so when
 * it has to wrap the device is idle and both pointers can restart at FIFO_HDR together. */
static void svga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!fifo) return;
    const uint32_t cmd[5] = { SVGA_CMD_UPDATE, x, y, w, h };
    const uint32_t len = sizeof cmd;
    fifo_drain();
    uint32_t next = fifo[F_NEXT];
    if (next + len > fifo[F_MAX]) {
        fifo[F_STOP] = FIFO_HDR;
        next = FIFO_HDR;
    }
    for (uint32_t k = 0; k < 5; k++) fifo[(next / 4) + k] = cmd[k];
    __sync_synchronize();
    fifo[F_NEXT] = next + len;
    svga_write(R_SYNC, 1);
    fifo_drain();
}

static void svga_flush(struct fb_device *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)d;
    svga_update(x, y, w, h);
}

static int probe(void) {
    if (pci_find_any(ids, 1, &pdev)) return -1;
    pci_enable(&pdev, 1 | 2);                                    /* I/O space and memory space */
    iobase = (uint16_t)pci_bar(&pdev, 0);
    if (!iobase) return -1;
    svga_write(R_ID, SVGA_ID_2);                                 /* identify: the device echoes its ID */
    return svga_read(R_ID) == SVGA_ID_2 ? 0 : -1;
}

/* Pixel layout from the device's colour masks (e.g. 0xFF0000 = 8 red bits at bit 16), as Linux does. */
static void mask_field(uint32_t mask, uint8_t *off, uint8_t *len) {
    *off = 0; *len = 0;
    if (!mask) return;
    while (!(mask & 1)) { mask >>= 1; (*off)++; }
    while (mask & 1) { mask >>= 1; (*len)++; }
}

static int init(void) {
    uint32_t w, h;
    fb_cmdline_mode(&w, &h, DEFAULT_W, DEFAULT_H);
    uint32_t mw = svga_read(R_MAX_WIDTH), mh = svga_read(R_MAX_HEIGHT);
    if (mw && w > mw) w = mw;
    if (mh && h > mh) h = mh;

    uint32_t bar1 = pci_bar(&pdev, 1), bar2 = pci_bar(&pdev, 2);
    if (!bar1) return -1;

    svga_write(R_ENABLE, 0);                                     /* disable while the mode changes */
    svga_write(R_WIDTH, w);
    svga_write(R_HEIGHT, h);
    svga_write(R_BITS_PER_PIXEL, 32);
    svga_write(R_ENABLE, 1);

    uint32_t aw = svga_read(R_WIDTH), ah = svga_read(R_HEIGHT), abpp = svga_read(R_BITS_PER_PIXEL);
    uint32_t pitch = svga_read(R_BYTES_PER_LINE);
    uint32_t fb_off = svga_read(R_FB_OFFSET);
    uint32_t vram = svga_read(R_VRAM_SIZE);
    if (!aw || !ah || !pitch || (abpp != 32 && abpp != 16)) {
        kprintf("vmsvga: device refused %ux%u (got %ux%u %ubpp)\n", w, h, aw, ah, abpp);
        svga_write(R_ENABLE, 0);
        return -1;
    }
    if (vram && (uint64_t)fb_off + (uint64_t)pitch * ah > vram) {
        kprintf("vmsvga: %ux%u does not fit in %u KiB of video memory\n", aw, ah, vram >> 10);
        svga_write(R_ENABLE, 0);
        return -1;
    }

    /* the FIFO: the device reads commands from [MIN, NEXT) and reports how far it got in STOP */
    uint32_t fmem = svga_read(R_MEM_SIZE);
    if (bar2 && fmem > FIFO_HDR) {
        fifo_bytes = fmem < FIFO_MAXB ? fmem : FIFO_MAXB;
        arch_map_device(bar2, fifo_bytes);
        fifo = P2V(bar2);
        fifo[F_MIN] = FIFO_HDR;
        fifo[F_MAX] = fifo_bytes;
        fifo[F_NEXT] = FIFO_HDR;
        fifo[F_STOP] = FIFO_HDR;
        svga_write(R_CONFIG_DONE, 1);
    }

    uint8_t roff = 16, rlen = 8, goff = 8, glen = 8, boff = 0, blen = 8;
    uint32_t rm = svga_read(R_RED_MASK), gm = svga_read(R_GREEN_MASK), bm = svga_read(R_BLUE_MASK);
    if (rm && gm && bm) {
        mask_field(rm, &roff, &rlen);
        mask_field(gm, &goff, &glen);
        mask_field(bm, &boff, &blen);
    }

    fbd.name = "vmsvga";
    fbd.flush = fifo ? svga_flush : 0;                           /* no FIFO: the framebuffer is still usable */
    fbd.info = (struct fb_info){
        .phys = (uintptr_t)bar1 + fb_off, .width = aw, .height = ah, .pitch = pitch, .bpp = abpp,
        .vheight = ah, .roff = roff, .rlen = rlen, .goff = goff, .glen = glen, .boff = boff, .blen = blen,
    };
    kprintf("vmsvga: %ux%u %ubpp, video memory %u KiB, FIFO %s\n", aw, ah, abpp, vram >> 10, fifo ? "on" : "off");
    return fb_register(&fbd);
}

/* Priority 5 in the display class: a device that has a driver owns the screen, ahead of the
 * bootloader's framebuffer (multiboot-fb, prio 10), which is only the fallback. */
static struct kdriver drv = {
    "vmsvga", "VMware / VirtualBox SVGA II framebuffer (15AD:0405)",
    DRV_DISPLAY, 5, probe, init, 0, 0
};
KDRIVER(drv);
