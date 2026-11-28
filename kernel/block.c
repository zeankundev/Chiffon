#include "kernel.h"

static const struct fs_driver *drivers[] = { &ext_driver, 0 };

/* Boards without a writable disk keep this default and persistence stays off. */
int __attribute__((weak)) board_blk_write(uint64_t lba, const void *buf, uint32_t nsect) {
    (void)lba; (void)buf; (void)nsect;
    return -1;
}

static uint32_t mbr_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int try_volume(uint64_t base, uint64_t limit, uint64_t occupied_end, const char *what) {
    for (int i = 0; drivers[i]; i++) {
        uint64_t end, id;
        int r = drivers[i]->mount(base, limit, &end, &id);
        if (r == -ENOENT) continue;
        if (r == 0) {
            kprintf("[block] found volume %s: %s filesystem\n", what, drivers[i]->name);
            vfs_mark_base();
            persist_attach_fs(occupied_end > end ? occupied_end : end, id);
        }
        return r;
    }
    return -ENOENT;
}

int blk_mount_root(int part) {
    if (board_blk_init()) return -ENODEV;
    uint64_t sectors = board_blk_sectors();
    kprintf("[block] mounting disk with %lu sectors (%lu MB)\n", (unsigned long)sectors, (unsigned long)(sectors >> 11));
    uint8_t mbr[512];
    if (!sectors || board_blk_read(0, mbr, 1)) return -EIO;
    uint64_t starts[4] = {0}, ends[4] = {0}, occupied_end = 0;
    int entries = 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            uint32_t start = mbr_u32(e + 8), size = mbr_u32(e + 12);
            if (!e[4] && !start && !size) continue;
            /* GPT and extended partition layouts are not parsed; their occupied
             * extents cannot safely be inferred from primary entries alone. */
            if (!e[4] || !start || !size || e[4] == 0xee || e[4] == 0x05 ||
                e[4] == 0x0f || e[4] == 0x85 || start >= sectors ||
                (uint64_t)size > sectors - start) {
                kprintf("[block] invalid or unsupported partition table\n");
                return -EINVAL;
            }
            starts[i] = start; ends[i] = (uint64_t)start + size;
            for (int j = 0; j < i; j++) {
                if (ends[j] && starts[i] < ends[j] && starts[j] < ends[i]) {
                    kprintf("[block] overlapping partitions\n");
                    return -EINVAL;
                }
            }
            if (ends[i] > occupied_end) occupied_end = ends[i];
            entries++;
        }
    }
    if (!entries) return part ? -ENOENT : try_volume(0, sectors, 0, "whole disk");
    int r = -ENOENT;
    for (int i = 0; i < 4; i++) {
        if ((part && part != i + 1) || !ends[i]) continue;
        char what[24]; ksnprintf(what, sizeof what, "partition %d", i + 1);
        if ((r = try_volume(starts[i], ends[i], occupied_end, what)) == 0) return 0;
    }
    return r;
}
