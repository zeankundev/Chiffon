/* virtio-net over virtio-mmio, polled.  Supports the legacy transport (version 1, which QEMU's
 * `virt` machine uses by default) and the modern transport (version 2).  QEMU: -device
 * virtio-net-device.  Two virtqueues (0 = receive, 1 = transmit), four descriptors each, one
 * 1536-byte buffer per descriptor.  Only the MAC feature is negotiated: no checksum or GSO offload.
 * The board finds the device: board_net_probe() calls vnet_probe() on each virtio-mmio slot. */
#include "kernel.h"
#include "net.h"

#define VNQ     4
#define VN_BUF  1536
#define VIRTIO_MAGIC 0x74726976
#define VIRTIO_ID_NET 1
#define VIRTIO_F_MAC (1u << 5)                      /* feature word 0, bit 5 */
#define VIRTIO_F_VERSION_1 (1u << 0)                /* feature word 1, bit 0 = bit 32 (modern only) */
#define DSB() __asm__ volatile("dsb sy" ::: "memory")

/* Queue memory.  The legacy layout is fixed by the device: descriptors, then the avail ring right
 * after them, then the used ring on the next 4 KiB boundary.  The modern layout puts the same three
 * parts at the same offsets, so one structure serves both transports. */
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vqueue {
    struct vdesc desc[VNQ];                                   /* offset 0    */
    uint16_t avail_flags, avail_idx, avail_ring[VNQ];         /* offset 64   */
    uint8_t pad[4096 - 64 - 4 - 2 * VNQ];
    uint16_t used_flags, used_idx;                            /* offset 4096 */
    struct { uint32_t id, len; } used_ring[VNQ];
} __attribute__((aligned(4096)));
static struct vqueue vq[2];
static uint8_t vbuf[2 * VNQ][VN_BUF] __attribute__((aligned(16)));

static volatile uint32_t *reg;
static uint32_t hdr_len = 10;                       /* virtio_net_hdr: 10 bytes legacy, 12 modern */
static uint16_t rx_av, rx_used, tx_av, tx_used;
static struct netdev nd;
static uint8_t nd_mac[6];

static inline uint32_t rd(uint32_t off) { return reg[off / 4]; }
static inline void wr(uint32_t off, uint32_t v) { reg[off / 4] = v; }

static int setup_queue(int qi, int modern) {
    wr(0x30, (uint32_t)qi);                         /* QueueSel */
    if (rd(0x34) < VNQ) return -1;                  /* QueueNumMax */
    wr(0x38, VNQ);                                  /* QueueNum */
    uint64_t q = V2P((uintptr_t)&vq[qi]);
    if (modern) {
        wr(0x80, (uint32_t)q);          wr(0x84, (uint32_t)(q >> 32));                   /* desc  */
        wr(0x90, (uint32_t)(q + 64));   wr(0x94, (uint32_t)((q + 64) >> 32));            /* avail */
        wr(0xA0, (uint32_t)(q + 4096)); wr(0xA4, (uint32_t)((q + 4096) >> 32));          /* used  */
        wr(0x44, 1);                                /* QueueReady */
    } else {
        wr(0x28, 4096);                             /* GuestPageSize */
        wr(0x3c, 4096);                             /* QueueAlign */
        wr(0x40, (uint32_t)(q >> 12));              /* QueuePFN */
    }
    return 0;
}

static int vn_send(const uint8_t *frame, size_t len) {
    if (len > VN_BUF - hdr_len) return -EINVAL;
    struct vqueue *q = &vq[1];
    uint8_t *b = vbuf[VNQ];                         /* transmit slot 0: one frame in flight */
    memset(b, 0, hdr_len);
    memcpy(b + hdr_len, frame, len);
    q->desc[0].addr = V2P((uintptr_t)b);
    q->desc[0].len = (uint32_t)(hdr_len + len);
    q->desc[0].flags = 0; q->desc[0].next = 0;
    q->avail_ring[tx_av % VNQ] = 0;
    DSB();
    tx_av++;
    q->avail_idx = tx_av;
    DSB();
    wr(0x50, 1);                                    /* QueueNotify */
    for (long spin = 0; q->used_idx == tx_used; spin++) {
        DSB();
        if (spin > 50000000L) return -EIO;
    }
    tx_used++;
    return 0;
}

static int vn_poll(void) {
    struct vqueue *q = &vq[0];
    int n = 0;
    DSB();
    while (rx_used != q->used_idx) {
        uint16_t pos = rx_used % VNQ;
        uint32_t id = q->used_ring[pos].id, len = q->used_ring[pos].len;
        if (id < VNQ && len > hdr_len && len <= VN_BUF) net_input(vbuf[id] + hdr_len, len - hdr_len);
        q->avail_ring[rx_av % VNQ] = (uint16_t)id;  /* hand the buffer back to the device */
        rx_av++;
        DSB();
        q->avail_idx = rx_av;
        rx_used++;
        n++;
    }
    if (n) { DSB(); wr(0x50, 0); }
    return n;
}

/* Probe one virtio-mmio slot (already inside the device window).  0 = a NIC was attached. */
int vnet_probe(uintptr_t mmio_pa) {
    reg = (volatile uint32_t *)P2V(mmio_pa);
    if (reg[0] != VIRTIO_MAGIC || reg[2] != VIRTIO_ID_NET) return -1;
    uint32_t ver = reg[1];
    if (ver != 1 && ver != 2) return -1;
    int modern = ver == 2;
    wr(0x70, 0); wr(0x70, 1); wr(0x70, 3);          /* reset, ACKNOWLEDGE, DRIVER */
    uint32_t f0;
    if (modern) {
        wr(0x14, 0); f0 = rd(0x10);
        wr(0x14, 1); uint32_t f1 = rd(0x10);
        if (!(f1 & VIRTIO_F_VERSION_1)) return -1;
        wr(0x24, 0); wr(0x20, f0 & VIRTIO_F_MAC);
        wr(0x24, 1); wr(0x20, VIRTIO_F_VERSION_1);
        wr(0x70, rd(0x70) | 8);                     /* FEATURES_OK */
        if (!(rd(0x70) & 8)) return -1;
        hdr_len = 12;
    } else {
        f0 = rd(0x10);
        wr(0x20, f0 & VIRTIO_F_MAC);                /* legacy: guest features at 0x20 */
        hdr_len = 10;
    }
    if (setup_queue(0, modern) || setup_queue(1, modern)) return -1;
    volatile uint8_t *cfg = (volatile uint8_t *)reg + 0x100;
    for (int i = 0; i < 6; i++) nd_mac[i] = (f0 & VIRTIO_F_MAC) ? cfg[i] : (uint8_t)(0x52 + i);
    for (int i = 0; i < VNQ; i++) {                 /* receive buffers: device-writable */
        vq[0].desc[i].addr = V2P((uintptr_t)vbuf[i]);
        vq[0].desc[i].len = VN_BUF;
        vq[0].desc[i].flags = 2;                    /* VIRTQ_DESC_F_WRITE */
        vq[0].desc[i].next = 0;
        vq[0].avail_ring[i] = (uint16_t)i;
    }
    rx_av = VNQ;
    DSB();
    vq[0].avail_idx = rx_av;
    DSB();
    wr(0x50, 0);
    wr(0x70, rd(0x70) | 4);                         /* DRIVER_OK */
    nd.name = modern ? "virtio-net (modern)" : "virtio-net (legacy)";
    memcpy(nd.mac, nd_mac, 6);
    nd.send = vn_send;
    nd.poll = vn_poll;
    return net_attach(&nd) == 0 ? 0 : -1;
}
