#include "kernel.h"

static const struct fs_driver *drivers[] = { &ext_driver, 0 };

/* Boards without a writable disk keep this default and persistence stays off. */
int __attribute__((weak)) board_blk_write(uint64_t lba, const void *buf, uint32_t nsect) {
    (void)lba; (void)buf; (void)nsect;
    return -1;
}

static int try_volume(uint64_t base, const char *what) {
    for (int i = 0; drivers[i]; i++) {
        if (drivers[i]->probe(base)) continue;
        kprintf("[block] found volume %s: %s filesystem\n", what, drivers[i]->name);
        int r = drivers[i]->mount(base);
        if (r == 0) {
            vfs_mark_base();
            persist_attach_fs(drivers[i], base);
        }
        return r;
    }
    return -ENOENT;
}

int blk_mount_root(int part) {
    if (board_blk_init()) return -ENODEV;
    kprintf("[block] mounting disk with %lu sectors (%lu MB)\n", (unsigned long)board_blk_sectors(), (unsigned long)(board_blk_sectors() >> 11));
    int r = -ENOENT;
    if (part == 0 && (r = try_volume(0, "whole disk")) == 0) return 0;
    uint8_t mbr[512];
    if (board_blk_read(0, mbr, 1) || mbr[510] != 0x55 || mbr[511] != 0xAA) return r;
    for (int i = 0; i < 4; i++) {
        if (part && part != i + 1) continue;
        const uint8_t *e = mbr + 446 + i * 16;
        uint32_t lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
        if (!e[4] || !lba) continue;
        char what[24]; ksnprintf(what, sizeof what, "partition %d", i + 1);
        if ((r = try_volume(lba, what)) == 0) return 0;
    }
    return r;
}