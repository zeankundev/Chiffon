#include "kernel.h"

/* Physical frame allocator.  One bit per 4 KiB frame of the usable RAM range, indexed from the
 * first usable frame (not from physical address 0), so a 1 MiB machine at 0x40000000 needs a
 * 32-byte bitmap instead of a 128 KiB one.  CHI_MAXFRAMES is the largest range supported; the
 * default (4 GiB) keeps the old behaviour.  PROFILE=tiny sets it to 4 MiB. */
#ifndef CHI_MAXFRAMES
#define CHI_MAXFRAMES (1u << 20)
#endif
#define MAXFRAMES CHI_MAXFRAMES
/* Physical frame allocator.  Usable RAM is a list of ranges: the primary range given to pmm_init()
 * and any number added with pmm_add_range() (external RAM such as PSRAM, or a second memory node).
 * Every frame of every range has one bit in one global bitmap, so the allocator treats them alike.
 * CHI_MAXFRAMES bounds the total; the default (4 GiB) keeps the old behaviour. */
#ifndef CHI_MAXFRAMES
#define CHI_MAXFRAMES (1u << 20)
#endif
#define MAXFRAMES CHI_MAXFRAMES
#define MAXPRANGES 8
static uint32_t bm[MAXFRAMES / 32];
struct prange { uintptr_t base; size_t first, n; };
static struct prange pr[MAXPRANGES];
static int npr;
static size_t nframes, freec, hint, totalc;

static inline void bset(size_t i) { bm[i >> 5] |= 1u << (i & 31); }
static inline void bclr(size_t i) { bm[i >> 5] &= ~(1u << (i & 31)); }
static inline int  btst(size_t i) { return bm[i >> 5] >> (i & 31) & 1; }

static uintptr_t frame_pa(size_t i) {
    for (int r = 0; r < npr; r++)
        if (i < pr[r].first + pr[r].n) return pr[r].base + ((uintptr_t)(i - pr[r].first) << 12);
    return 0;
}
static long frame_index(uintptr_t pa) {
    for (int r = 0; r < npr; r++)
        if (pa >= pr[r].base && pa < pr[r].base + ((uintptr_t)pr[r].n << 12))
            return (long)(pr[r].first + ((pa - pr[r].base) >> 12));
    return -1;
}

/* Add a range of RAM (page-aligned by this call).  Returns the number of frames added. */
size_t pmm_add_range(uintptr_t start, uintptr_t end) {
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (end <= start || npr == MAXPRANGES) return 0;
    size_t n = (end - start) / PAGE_SIZE;
    if (n > MAXFRAMES - nframes) n = MAXFRAMES - nframes;
    if (!n) return 0;
    pr[npr].base = start; pr[npr].first = nframes; pr[npr].n = n; npr++;
    for (size_t i = nframes; i < nframes + n; i++) { bclr(i); freec++; totalc++; }
    nframes += n;
    return n;
}
void pmm_init(uintptr_t start, uintptr_t end) {
    for (size_t i = 0; i < MAXFRAMES / 32; i++) bm[i] = ~0u;
    npr = 0; nframes = 0; freec = 0; totalc = 0; hint = 0;
    pmm_add_range(start, end);
}
uintptr_t pmm_alloc(void) {
    for (size_t n = 0; n < nframes; n++) {
        size_t i = (hint + n) % nframes;
        if (!btst(i)) {
            bset(i); freec--; hint = i + 1;
            uintptr_t pa = frame_pa(i);
            memset(P2V(pa), 0, PAGE_SIZE);
            return pa;
        }
    }
    return 0;
}
void pmm_free(uintptr_t pa) {
    long i = frame_index(pa);
    if (i >= 0 && btst((size_t)i)) { bclr((size_t)i); freec++; }
}
size_t pmm_free_count(void) { return freec; }
size_t pmm_total_count(void) { return totalc; }

struct hdr { size_t size, free; };

/* Kernel heap: one or more independent regions, each a chain of blocks.  The first region is
 * given by heap_init(); heap_add_region() adds more (for example from external RAM). */
#define HEAP_REGIONS 4
static struct { uint8_t *s, *e; } hreg[HEAP_REGIONS];
static int nhreg;

void heap_add_region(void *base, size_t size) {
    if (nhreg == HEAP_REGIONS || size < 2 * sizeof(struct hdr)) return;
    hreg[nhreg].s = base; hreg[nhreg].e = (uint8_t *)base + size;
    struct hdr *h = base;
    h->size = size - sizeof *h; h->free = 1;
    nhreg++;
}
void heap_init(void *base, size_t size) { nhreg = 0; heap_add_region(base, size); }

void *kmalloc(size_t n) {
    n = (n + 15) & ~15UL;
    if (!n) n = 16;
    for (int r = 0; r < nhreg; r++)
        for (uint8_t *p = hreg[r].s; p < hreg[r].e; p += sizeof(struct hdr) + ((struct hdr *)p)->size) {
            struct hdr *h = (struct hdr *)p;
            if (!h->free || h->size < n) continue;
            if (h->size >= n + sizeof *h + 32) {
                struct hdr *nb = (struct hdr *)(p + sizeof *h + n);
                nb->size = h->size - n - sizeof *h; nb->free = 1;
                h->size = n;
            }
            h->free = 0;
            return p + sizeof *h;
        }
    return 0;
}
void kfree(void *ptr) {
    if (!ptr) return;
    uint8_t *q = (uint8_t *)ptr - sizeof(struct hdr);
    ((struct hdr *)q)->free = 1;
    for (int r = 0; r < nhreg; r++) {
        if (q < hreg[r].s || q >= hreg[r].e) continue;
        for (uint8_t *p = hreg[r].s; p < hreg[r].e;) {
            struct hdr *h = (struct hdr *)p;
            uint8_t *nx = p + sizeof *h + h->size;
            if (h->free && nx < hreg[r].e && ((struct hdr *)nx)->free) h->size += sizeof *h + ((struct hdr *)nx)->size;
            else p = nx;
        }
        return;
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
