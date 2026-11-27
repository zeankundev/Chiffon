#pragma once
#include "kernel.h"

#define BOOTMEM_RANGES 32
#define BOOTMEM_LIMIT (1UL << 32)  /* both QEMU targets map 4 GiB during boot */

struct boot_range { uintptr_t start, end; };
struct bootmem {
    struct boot_range ram[BOOTMEM_RANGES], reserved[BOOTMEM_RANGES];
    struct boot_range usable[BOOTMEM_RANGES];
    int nram, nreserved, nusable;
};

static inline void bootmem_add(struct boot_range *ranges, int *count,
                               uintptr_t start, uintptr_t size) {
    if (!size) return;
    if (size > UINTPTR_MAX - start) panic("mem: boot range overflows at %lx", start);
    if (*count == BOOTMEM_RANGES) panic("mem: too many boot ranges");
    ranges[(*count)++] = (struct boot_range){start, start + size};
}

static inline void bootmem_emit(struct bootmem *m, uintptr_t start, uintptr_t end) {
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (end > start)
        bootmem_add(m->usable, &m->nusable, start, end - start);
}

/* Firmware reservations can overlap each other. Usable banks must be disjoint. */
static inline void bootmem_prepare(struct bootmem *m, uintptr_t limit) {
    for (int i = 0; i < m->nram; i++) {
        struct boot_range r = m->ram[i];
        for (int j = 0; j < i; j++)
            if (r.start < m->ram[j].end && m->ram[j].start < r.end)
                panic("mem: overlapping RAM banks");

        if (r.end > limit) {
            uintptr_t skipped = r.end - (r.start > limit ? r.start : limit);
            kprintf("mem: ignoring %lu KiB beyond boot RAM limit %lx\n", skipped >> 10, limit);
            r.end = limit;
        }
        while (r.start < r.end) {
            uintptr_t next = r.end, skip = r.start;
            for (int j = 0; j < m->nreserved; j++) {
                struct boot_range q = m->reserved[j];
                if (q.end <= r.start) continue;
                if (q.start <= r.start) {
                    if (q.end > skip) skip = q.end;
                } else if (q.start < next) {
                    next = q.start;
                }
            }
            if (skip > r.start) {
                r.start = skip;
            } else {
                bootmem_emit(m, r.start, next);
                r.start = next;
            }
        }
    }
    if (!m->nusable) panic("mem: no usable boot RAM");

    /* Put the largest contiguous range first so a small fragment cannot starve the heap. */
    for (int i = 1; i < m->nusable; i++) {
        struct boot_range first = m->usable[0], r = m->usable[i];
        if (r.end - r.start > first.end - first.start) {
            m->usable[i] = first;
            m->usable[0] = r;
        }
    }
}

static inline int bootmem_get(const struct bootmem *m, int index, uintptr_t *start, uintptr_t *end) {
    if (index < 0 || index >= m->nusable) return -1;
    *start = m->usable[index].start;
    *end = m->usable[index].end;
    return 0;
}
