/* virtio-blk over MMIO transport (legacy v1 and modern v2), polled, one queue of 8 descriptors.
 * State is file-local: include from ONE board.c.  aarch64 barriers; adapt DSB() for other CPUs. */
#pragma once
#include <stdint.h>
#include "arch.h"
#define VQN 8
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
static uint8_t vq_mem[2 * 4096] __attribute__((aligned(4096)));
static struct { uint32_t type, res; uint64_t sector; } vreq_hdr;
static volatile uint8_t vreq_status;
static volatile uint32_t *vio;
static uint64_t vio_sectors;
static uint16_t vavail_idx, vlast_used;
static int vio_ready;
#define DSB() __asm__ volatile("dsb sy" ::: "memory")
static inline uint32_t vr(uint32_t off) { return vio[off / 4]; }
static inline void vw(uint32_t off, uint32_t v) { vio[off / 4] = v; }

/* Is there a virtio block device at this MMIO base?  (caller must have mapped it) */
static int vblk_present(uintptr_t base) {
    volatile uint32_t *b = (volatile uint32_t *)P2V(base);
    return b[0] == 0x74726976 && b[2] == 2;
}
static int vblk_init_at(uintptr_t base) {
    if (vio_ready) return 0;
    if (!vblk_present(base)) return -1;
    vio = (volatile uint32_t *)P2V(base);
    uint32_t ver = vr(4);
    vw(0x70, 0); vw(0x70, 1); vw(0x70, 3);
    uint64_t q = V2P((uintptr_t)vq_mem);
    if (ver == 1) {
        vw(0x20, 0); vw(0x28, 4096); vw(0x30, 0);
        if (vr(0x34) < VQN) return -1;
        vw(0x38, VQN); vw(0x3c, 4096); vw(0x40, q >> 12);
    } else {
        vw(0x24, 0); vw(0x20, 0); vw(0x24, 1); vw(0x20, 1);            /* VIRTIO_F_VERSION_1 */
        vw(0x70, 0xB);
        if (!(vr(0x70) & 8)) return -1;
        vw(0x30, 0);
        if (vr(0x34) < VQN) return -1;
        vw(0x38, VQN);
        uint64_t avail = q + 128, used = q + 4096;
        vw(0x80, (uint32_t)q);     vw(0x84, q >> 32);
        vw(0x90, (uint32_t)avail); vw(0x94, avail >> 32);
        vw(0xA0, (uint32_t)used);  vw(0xA4, used >> 32);
        vw(0x44, 1);
    }
    vw(0x70, vr(0x70) | 4);
    vio_sectors = vr(0x100) | (uint64_t)vr(0x104) << 32;
    vio_ready = vio_sectors != 0;
    return vio_ready ? 0 : -1;
}
static int vblk_request(uint64_t lba, void *buf, uint32_t nsect) {
    volatile struct vdesc *d = (volatile struct vdesc *)vq_mem;
    volatile uint16_t *avail = (volatile uint16_t *)(vq_mem + 128);
    volatile uint16_t *used = (volatile uint16_t *)(vq_mem + 4096);
    vreq_hdr.type = 0; vreq_hdr.res = 0; vreq_hdr.sector = lba;
    vreq_status = 0xFF;
    d[0].addr = V2P((uintptr_t)&vreq_hdr); d[0].len = 16; d[0].flags = 1; d[0].next = 1;
    d[1].addr = V2P((uintptr_t)buf); d[1].len = nsect * 512; d[1].flags = 1 | 2; d[1].next = 2;
    d[2].addr = V2P((uintptr_t)&vreq_status); d[2].len = 1; d[2].flags = 2; d[2].next = 0;
    avail[2 + (vavail_idx % VQN)] = 0;
    DSB();
    vavail_idx++;
    avail[1] = vavail_idx;
    DSB();
    vw(0x50, 0);
    for (long spin = 0; used[1] == vlast_used; spin++) { DSB(); if (spin > 200000000L) return -1; }
    vlast_used++;
    DSB();
    return vreq_status == 0 ? 0 : -1;
}
static int vblk_read(uint64_t lba, void *buf, uint32_t n) {
    if (!vio_ready || lba + n > vio_sectors) return -1;
    uint8_t *b = buf;
    while (n) {
        uint32_t c = n > 64 ? 64 : n;
        if (vblk_request(lba, b, c)) return -1;
        lba += c; n -= c; b += c * 512;
    }
    return 0;
}
