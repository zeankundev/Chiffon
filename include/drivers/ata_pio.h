/* Legacy ATA PIO, primary channel master, LBA28.  State is file-local: include from ONE board.c. */
#pragma once
#include "x86io.h"
static uint64_t ata_nsect;
static int ata_ready;
static int ata_wait(int need_drq) {
    for (int i = 0; i < 2000000; i++) {
        uint8_t st = inb(0x1F7);
        if (st & 0x80) continue;
        if (st & 0x21) return -1;
        if (!need_drq || (st & 8)) return 0;
    }
    return -1;
}
static int ata_init(void) {
    if (ata_ready) return 0;
    if (inb(0x1F7) == 0xFF) return -1;
    outb(0x1F6, 0xA0); outb(0x1F7, 0xEC);
    if (inb(0x1F7) == 0) return -1;
    if (ata_wait(0) || inb(0x1F4) || inb(0x1F5)) return -1;
    if (ata_wait(1)) return -1;
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(0x1F0);
    ata_nsect = id[60] | (uint32_t)id[61] << 16;
    ata_ready = ata_nsect != 0;
    return ata_ready ? 0 : -1;
}
static int ata_read(uint64_t lba, void *buf, uint32_t n) {
    if (!ata_ready || lba + n > ata_nsect || lba + n > 0x0FFFFFFF) return -1;
    uint8_t *b = buf;
    while (n) {
        uint32_t c = n > 128 ? 128 : n;
        outb(0x1F6, 0xE0 | ((lba >> 24) & 0xF));
        outb(0x1F2, c); outb(0x1F3, lba); outb(0x1F4, lba >> 8); outb(0x1F5, lba >> 16);
        outb(0x1F7, 0x20);
        for (int i = 0; i < 4; i++) inb(0x3F6);
        for (uint32_t i = 0; i < c; i++) {
            if (ata_wait(1)) return -1;
            for (int w = 0; w < 256; w++) ((uint16_t *)b)[w] = inw(0x1F0);
            b += 512;
        }
        lba += c; n -= c;
    }
    return 0;
}
