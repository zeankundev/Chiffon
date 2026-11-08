#include "kernel.h"

/* Physical frame allocator.  One bit per 4 KiB frame of the usable RAM range, indexed from the
 * first usable frame (not from physical address 0), so a 1 MiB machine at 0x40000000 needs a
 * 32-byte bitmap instead of a 128 KiB one.  CHI_MAXFRAMES is the largest range supported; the
 * default (4 GiB) keeps the old behaviour.  PROFILE=tiny sets it to 4 MiB. */
#ifndef CHI_MAXFRAMES
#define CHI_MAXFRAMES (1u << 20)
#endif
#define MAXFRAMES CHI_MAXFRAMES
static uint32_t bm[MAXFRAMES / 32];
static uintptr_t pbase;
static size_t nframes, freec, hint, totalc;

static inline void bset(size_t i) { bm[i >> 5] |= 1u << (i & 31); }
static inline void bclr(size_t i) { bm[i >> 5] &= ~(1u << (i & 31)); }
static inline int  btst(size_t i) { return bm[i >> 5] >> (i & 31) & 1; }

void pmm_init(uintptr_t start, uintptr_t end) {
    for (size_t i = 0; i < MAXFRAMES / 32; i++) bm[i] = ~0u;
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (end <= start) return;
    if (end - start > (uintptr_t)MAXFRAMES * PAGE_SIZE) end = start + (uintptr_t)MAXFRAMES * PAGE_SIZE;
    pbase = start;
    nframes = (end - start) / PAGE_SIZE;
    for (size_t i = 0; i < nframes; i++) { bclr(i); freec++; totalc++; }
    hint = 0;
}
uintptr_t pmm_alloc(void) {
    for (size_t n = 0; n < nframes; n++) {
        size_t i = (hint + n) % nframes;
        if (!btst(i)) {
            bset(i); freec--; hint = i + 1;
            uintptr_t pa = pbase + ((uintptr_t)i << 12);
            memset(P2V(pa), 0, PAGE_SIZE);
            return pa;
        }
    }
    return 0;
}
void pmm_free(uintptr_t pa) {
    if (pa < pbase) return;
    size_t i = (pa - pbase) >> 12;
    if (i < nframes && btst(i)) { bclr(i); freec++; }
}
size_t pmm_free_count(void) { return freec; }
size_t pmm_total_count(void) { return totalc; }

struct hdr { size_t size, free; };
static uint8_t *hstart, *hend;

void heap_init(void *base, size_t size) {
    hstart = base; hend = hstart + size;
    struct hdr *h = (struct hdr *)hstart;
    h->size = size - sizeof *h; h->free = 1;
}
void *kmalloc(size_t n) {
    n = (n + 15) & ~15UL;
    if (!n) n = 16;
    for (uint8_t *p = hstart; p < hend; p += sizeof(struct hdr) + ((struct hdr *)p)->size) {
        struct hdr *h = (struct hdr *)p;
        if (!h->free || h->size < n) continue;
        if (h->size >= n + sizeof *h + 32) {
            struct hdr *r = (struct hdr *)(p + sizeof *h + n);
            r->size = h->size - n - sizeof *h; r->free = 1;
            h->size = n;
        }
        h->free = 0;
        return p + sizeof *h;
    }
    return 0;
}
void kfree(void *ptr) {
    if (!ptr) return;
    ((struct hdr *)((uint8_t *)ptr - sizeof(struct hdr)))->free = 1;
    for (uint8_t *p = hstart; p < hend;) {
        struct hdr *h = (struct hdr *)p;
        uint8_t *nx = p + sizeof *h + h->size;
        if (h->free && nx < hend && ((struct hdr *)nx)->free) h->size += sizeof *h + ((struct hdr *)nx)->size;
        else p = nx;
    }
}
void *krealloc(void *ptr, size_t n) {
    if (!ptr) return kmalloc(n);
    size_t old = ((struct hdr *)((uint8_t *)ptr - sizeof(struct hdr)))->size;
    if (old >= n) return ptr;
    void *q = kmalloc(n);
    if (!q) return 0;
    memcpy(q, ptr, old);
    kfree(ptr);
    return q;
}
