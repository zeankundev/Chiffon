/* Read-only FAT32 and exFAT volumes, mounted as a directory tree (kernel/fat.c).  The volume is read
 * through any sector reader, so the same code serves removable media and tests. */
#pragma once
#include <stdint.h>

/* read nsect 512-byte sectors starting at absolute sector lba into buf; 0 = success */
typedef int (*fat_read_fn)(void *ctx, uint64_t lba, uint32_t nsect, void *buf);

int  fat_mount(const char *dir, fat_read_fn rd, void *ctx, uint64_t nsect, void **handle);   /* 0 = mounted */
void fat_unmount(void *handle);
