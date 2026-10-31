/* BCM2837 VideoCore Mailbox & Linear Framebuffer Driver for Chiffon */
#include "kernel.h"
#include "fb.h"

#define BCM2837_PERIPH_BASE 0x3F000000UL
#define MBOX_BASE           (BCM2837_PERIPH_BASE + 0xB880)

#define MBOX_READ           ((volatile uint32_t *)P2V(MBOX_BASE + 0x00))
#define MBOX_STATUS         ((volatile uint32_t *)P2V(MBOX_BASE + 0x18))
#define MBOX_WRITE          ((volatile uint32_t *)P2V(MBOX_BASE + 0x20))

#define MBOX_RESPONSE       0x80000000
#define MBOX_FULL           0x80000000
#define MBOX_EMPTY          0x40000000

#define MBOX_CH_PROP        8

/* Mailbox buffer must be 16-byte aligned */
static uint32_t __attribute__((aligned(16))) mbox[36];
static struct fb_device rpi_fb_dev;

static int mbox_call(uint8_t ch) {
    uint32_t r = (uint32_t)(V2P((uintptr_t)mbox) & ~0xFUL) | (ch & 0xF);
    /* Flush memory before notifying VideoCore */
    __asm__ volatile("dsb sy; isb" ::: "memory");
    while (*MBOX_STATUS & MBOX_FULL) { }
    *MBOX_WRITE = r;
    while (1) {
        while (*MBOX_STATUS & MBOX_EMPTY) { }
        if (*MBOX_READ == r) {
            __asm__ volatile("dsb sy; isb" ::: "memory");
            return mbox[1] == MBOX_RESPONSE ? 0 : -1;
        }
    }
}

static int rpi_fb_probe(void) {
    return 0; /* Always attempt to initialize if driver is compiled in */
}

static int rpi_fb_init(void) {
    uint32_t width = 1024, height = 768, bpp = 32;
    const char *v = cmdline_get("video");
    if (v) {
        uint32_t w = 0, h = 0;
        const char *p = v;
        while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
        if (*p == 'x' || *p == 'X') {
            p++;
            while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
        }
        if (w >= 320 && h >= 240) {
            width = w;
            height = h;
        }
    }

    mbox[0] = 35 * 4;
    mbox[1] = 0; // request code

    mbox[2] = 0x48003; // set phys width/height
    mbox[3] = 8;
    mbox[4] = 8;
    mbox[5] = width;
    mbox[6] = height;

    mbox[7] = 0x48004; // set virt width/height
    mbox[8] = 8;
    mbox[9] = 8;
    mbox[10] = width;
    mbox[11] = height;

    mbox[12] = 0x48009; // set virt offset
    mbox[13] = 8;
    mbox[14] = 8;
    mbox[15] = 0;
    mbox[16] = 0;

    mbox[17] = 0x48005; // set depth
    mbox[18] = 4;
    mbox[19] = 4;
    mbox[20] = bpp;

    mbox[21] = 0x48006; // set pixel order (1 = RGB)
    mbox[22] = 4;
    mbox[23] = 4;
    mbox[24] = 1;

    mbox[25] = 0x40001; // allocate buffer
    mbox[26] = 8;
    mbox[27] = 8;
    mbox[28] = 4096; // alignment
    mbox[29] = 0;    // return: fb address

    mbox[30] = 0x40008; // get pitch
    mbox[31] = 4;
    mbox[32] = 4;
    mbox[33] = 0;

    mbox[34] = 0; // end tag

    if (mbox_call(MBOX_CH_PROP) != 0 || mbox[20] != bpp || mbox[28] == 0) {
        kprintf("rpi_fb: mailbox call failed or unsupported resolution %ux%u@%u\n", width, height, bpp);
        return -1;
    }

    uint32_t fb_bus = mbox[28];
    uint32_t fb_phys = fb_bus & 0x3FFFFFFF; // Convert VC bus address to ARM physical address
    uint32_t fb_size = mbox[29];
    uint32_t pitch = mbox[33];
    uint32_t actual_w = mbox[5];
    uint32_t actual_h = mbox[6];

    rpi_fb_dev.name = "rpi-fb";
    rpi_fb_dev.info.phys = fb_phys;
    rpi_fb_dev.info.virt = (uint8_t *)P2V(fb_phys);
    rpi_fb_dev.info.width = actual_w;
    rpi_fb_dev.info.height = actual_h;
    rpi_fb_dev.info.vheight = actual_h;
    rpi_fb_dev.info.pitch = pitch;
    rpi_fb_dev.info.bpp = bpp;
    rpi_fb_dev.info.size = fb_size ? fb_size : (pitch * actual_h);
    rpi_fb_dev.info.xoff = 0;
    rpi_fb_dev.info.yoff = 0;
    rpi_fb_dev.info.roff = 16; rpi_fb_dev.info.rlen = 8;
    rpi_fb_dev.info.goff = 8;  rpi_fb_dev.info.glen = 8;
    rpi_fb_dev.info.boff = 0;  rpi_fb_dev.info.blen = 8;

    fb_register(&rpi_fb_dev);
    fbcon_attach(&rpi_fb_dev);

    driver_note("rpi-fb", "BCM2837 VideoCore Framebuffer Display", DRV_DISPLAY);
    kprintf("rpi_fb: %ux%u (%ubpp, pitch %u) at phys 0x%lx\n", actual_w, actual_h, bpp, pitch, (unsigned long)fb_phys);
    return 0;
}

static struct kdriver rpi_fb_driver = {
    .name = "rpi_fb",
    .desc = "Raspberry Pi VideoCore Framebuffer Driver",
    .class = DRV_DISPLAY,
    .prio = 10,
    .probe = rpi_fb_probe,
    .init = rpi_fb_init,
};
KDRIVER(rpi_fb_driver);
