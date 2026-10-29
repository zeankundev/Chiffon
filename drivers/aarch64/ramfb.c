/* QEMU `ramfb` display for the aarch64 `virt` machine.  The device has no registers of its own: the
 * guest allocates a framebuffer in RAM and tells QEMU where it is by writing a small config struct
 * to the fw_cfg file "etc/ramfb" through fw_cfg's DMA interface.  Resolution: `video=WxH`
 * (default 1024x768).  Run QEMU with `-device ramfb` (see `make run-gfx`). */
#include "kernel.h"

#define FWCFG_BASE 0x09020000UL
#define FW_SIG 0x0000
#define FW_ID 0x0001
#define FW_DIR 0x0019
#define DMA_ERROR 1
#define DMA_SELECT 8
#define DMA_WRITE 16

static inline uint16_t bs16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t bs32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bs64(uint64_t v) { return __builtin_bswap64(v); }

static volatile uint8_t *fw(void) { return (volatile uint8_t *)P2V(FWCFG_BASE); }
static void select_item(uint16_t s) { *(volatile uint16_t *)(fw() + 8) = bs16(s); }
static void read_bytes(void *dst, size_t n) { uint8_t *d = dst; while (n--) *d++ = *fw(); }

struct dma_access { uint32_t control, length; uint64_t address; } __attribute__((packed, aligned(16)));
struct ramfb_cfg { uint64_t addr; uint32_t fourcc, flags, width, height, stride; } __attribute__((packed, aligned(16)));
static struct dma_access dma;
static struct ramfb_cfg cfg;
static struct fb_device fbd;
static int ramfb_sel = -1;

static int probe(void) {
    if (arch_map_device(FWCFG_BASE, 0x20)) return -1;
    char sig[4]; uint32_t feat = 0;
    select_item(FW_SIG); read_bytes(sig, 4);
    if (memcmp(sig, "QEMU", 4)) return -1;
    select_item(FW_ID); read_bytes(&feat, 4);
    if (!(feat & 2)) return -1;                                   /* needs the DMA interface */
    uint32_t count;
    select_item(FW_DIR); read_bytes(&count, 4);
    count = bs32(count);
    for (uint32_t i = 0; i < count && i < 256; i++) {
        struct { uint32_t size; uint16_t sel, res; char name[56]; } e;
        read_bytes(&e, sizeof e);
        if (!strcmp(e.name, "etc/ramfb")) { ramfb_sel = bs16(e.sel); return 0; }
    }
    return -1;                                                    /* QEMU started without -device ramfb */
}

static int init(void) {
    uint32_t w = 1024, h = 768;
    const char *v = cmdline_get("video");
    if (v) {
        uint32_t a = 0, b = 0;
        while (*v >= '0' && *v <= '9') a = a * 10 + (*v++ - '0');
        if (*v == 'x') { v++; while (*v >= '0' && *v <= '9') b = b * 10 + (*v++ - '0'); }
        if (a >= 320 && a <= 2048 && b >= 200 && b <= 2048) { w = a; h = b; }
    }
    size_t bytes = (size_t)w * h * 4;
    uint8_t *mem = kmalloc(bytes + PAGE_SIZE);
    if (!mem) return -1;
    mem = (uint8_t *)(((uintptr_t)mem + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(mem, 0, bytes);

    cfg.addr = bs64(V2P((uintptr_t)mem));
    cfg.fourcc = bs32(0x34325258);                                /* 'XR24' = XRGB8888 */
    cfg.width = bs32(w); cfg.height = bs32(h); cfg.stride = bs32(w * 4);
    dma.control = bs32((uint32_t)ramfb_sel << 16 | DMA_SELECT | DMA_WRITE);
    dma.length = bs32(28);                                       /* the fw_cfg file is exactly 28 bytes (not sizeof: aligned) */
    dma.address = bs64(V2P((uintptr_t)&cfg));
    uint64_t da = bs64(V2P((uintptr_t)&dma));
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(fw() + 0x10) = (uint32_t)(da);         /* big-endian high word first ... */
    *(volatile uint32_t *)(fw() + 0x14) = (uint32_t)(da >> 32);   /* ... low word triggers the transfer */
    for (long spin = 0; (*(volatile uint32_t *)&dma.control) && spin < 10000000; spin++) { }
    if (dma.control) return -1;

    fbd.name = "ramfb";
    fbd.info = (struct fb_info){ .phys = V2P((uintptr_t)mem), .virt = mem, .width = w, .height = h, .pitch = w * 4, .bpp = 32,
                                 .roff = 16, .rlen = 8, .goff = 8, .glen = 8, .boff = 0, .blen = 8 };
    return fb_register(&fbd);
}
static struct kdriver drv = { "ramfb", "QEMU ramfb framebuffer (fw_cfg)", DRV_DISPLAY, 20, probe, init, 0, 0 };
KDRIVER(drv);
