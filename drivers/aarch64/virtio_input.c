/* virtio-input keyboard over virtio-mmio (QEMU `-device virtio-keyboard-device`).
 *
 * This is how a keyboard reaches the guest on the aarch64 `virt` machine: there is no PS/2 or USB
 * stack, and QEMU forwards everything typed into its window / VNC session / host keyboard to the
 * virtio keyboard.  The driver keeps an event queue full of 8-byte buffers and polls the used ring
 * from the timer tick (100 Hz, queue depth 16 -- no interrupt needed).  Key codes are Linux evdev
 * codes; 1..88 coincide with scancode set 1, the rest are mapped to extended keys. */
#include "kernel.h"

#define VIRTIO_BASE 0x0a000000UL
#define VIRTIO_STEP 0x200
#define N 16
#define DSB() __asm__ volatile("dsb sy" ::: "memory")

struct ev { uint16_t type, code; uint32_t value; };
static uint8_t vq[2 * 4096] __attribute__((aligned(4096)));    /* descriptors+avail in page 0, used ring in page 1 */
static struct ev events[N] __attribute__((aligned(16)));
static volatile uint32_t *dev;
static uint16_t avail_idx, last_used;

static inline uint32_t rd(uint32_t off) { return dev[off / 4]; }
static inline void wr(uint32_t off, uint32_t v) { dev[off / 4] = v; }

static int probe(void) {
    if (arch_map_device(VIRTIO_BASE, 32 * VIRTIO_STEP)) return -1;
    for (int i = 0; i < 32; i++) {
        volatile uint32_t *b = (volatile uint32_t *)P2V(VIRTIO_BASE + i * VIRTIO_STEP);
        if (b[0] == 0x74726976 && b[2] == 18) { dev = b; return 0; }      /* "virt", device id 18 = input */
    }
    return -1;                                                            /* QEMU started without a virtio keyboard */
}

static void post(int id) {
    volatile uint16_t *avail = (volatile uint16_t *)(vq + N * 16);
    avail[2 + (avail_idx % N)] = id;
    DSB();
    avail[1] = ++avail_idx;
    DSB();
    wr(0x50, 0);                                                          /* notify queue 0 */
}

static int init(void) {
    uint32_t ver = rd(4);
    wr(0x70, 0); wr(0x70, 1); wr(0x70, 3);                                /* reset, ACKNOWLEDGE, DRIVER */
    uint64_t q = V2P((uintptr_t)vq);
    if (ver == 1) {                                                       /* legacy */
        wr(0x20, 0); wr(0x28, 4096); wr(0x30, 0);
        if (rd(0x34) < N) return -1;
        wr(0x38, N); wr(0x3c, 4096); wr(0x40, q >> 12);
    } else {                                                              /* modern */
        wr(0x24, 0); wr(0x20, 0); wr(0x24, 1); wr(0x20, 1);               /* VIRTIO_F_VERSION_1 */
        wr(0x70, 0xB);
        if (!(rd(0x70) & 8)) return -1;
        wr(0x30, 0);
        if (rd(0x34) < N) return -1;
        wr(0x38, N);
        uint64_t avail = q + N * 16, used = q + 4096;
        wr(0x80, (uint32_t)q);     wr(0x84, q >> 32);
        wr(0x90, (uint32_t)avail); wr(0x94, avail >> 32);
        wr(0xA0, (uint32_t)used);  wr(0xA4, used >> 32);
        wr(0x44, 1);
    }
    volatile struct { uint64_t addr; uint32_t len; uint16_t flags, next; } *d = (void *)vq;
    for (int i = 0; i < N; i++) {                                         /* N device-writable 8-byte buffers */
        d[i].addr = V2P((uintptr_t)&events[i]); d[i].len = sizeof(struct ev); d[i].flags = 2; d[i].next = 0;
    }
    wr(0x70, rd(0x70) | 4);                                               /* DRIVER_OK */
    for (int i = 0; i < N; i++) post(i);
    return 0;
}

/* Linux evdev code -> (scancode-set-1 code, extended) for keys outside 1..88 */
static int ext_key(unsigned code, unsigned char *sc) {
    switch (code) {
    case 96: *sc = 0x1C; return 1;   case 97: *sc = 0x1D; return 1;   case 98: *sc = 0x35; return 1;
    case 100: *sc = 0x38; return 1;  case 102: *sc = 0x47; return 1;  case 103: *sc = 0x48; return 1;
    case 104: *sc = 0x49; return 1;  case 105: *sc = 0x4B; return 1;  case 106: *sc = 0x4D; return 1;
    case 107: *sc = 0x4F; return 1;  case 108: *sc = 0x50; return 1;  case 109: *sc = 0x51; return 1;
    case 110: *sc = 0x52; return 1;  case 111: *sc = 0x53; return 1;
    }
    return 0;
}

static void poll(void) {
    volatile uint16_t *used = (volatile uint16_t *)(vq + 4096);
    while (last_used != used[1]) {
        DSB();
        volatile uint32_t *e = (volatile uint32_t *)(vq + 4096 + 4 + 8 * (last_used % N));
        uint32_t id = e[0] % N;
        struct ev ev = events[id];
        if (ev.type == 1) {                                               /* EV_KEY: value 0 up, 1 down, 2 repeat */
            unsigned char sc;
            if (ev.code >= 1 && ev.code <= 88) kbd_scancode(ev.code, ev.value == 0, 0);
            else if (ext_key(ev.code, &sc)) kbd_scancode(sc, ev.value == 0, 1);
        }
        last_used++;
        post(id);                                                         /* hand the buffer back */
    }
    if (rd(0x60)) wr(0x64, rd(0x60));                                     /* ack the (unused) interrupt line */
}

static struct kdriver drv = { "virtio-kbd", "virtio-input keyboard (virtio-mmio, polled)", DRV_INPUT, 10, probe, init, poll, 0 };
KDRIVER(drv);
