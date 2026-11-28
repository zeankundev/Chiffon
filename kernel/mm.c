#include "kernel.h"

/* Physical frame allocator.  Usable RAM is a list of ranges: the primary range given to pmm_init()
 * and any number added with pmm_add_range() (external RAM such as PSRAM, or a second memory node).
 * Every frame of every range has one bit in one global bitmap, so the allocator treats them alike.
 * CHI_MAXFRAMES bounds the total; the default (4 GiB) keeps the old behaviour. */
#ifndef CHI_MAXFRAMES
#define CHI_MAXFRAMES (1u << 20)
#endif
#define MAXFRAMES CHI_MAXFRAMES
#define MAXPRANGES 8
static uint32_t bm[(MAXFRAMES + 31) / 32];
struct prange { uintptr_t base; size_t first, n; };
static struct prange pr[MAXPRANGES];
static int npr;
static size_t nframes, freec, hint;

static inline void bset(size_t i) { bm[i >> 5] |= 1u << (i & 31); }
static inline void bclr(size_t i) { bm[i >> 5] &= ~(1u << (i & 31)); }
static inline int  btst(size_t i) { return bm[i >> 5] >> (i & 31) & 1; }

static uintptr_t frame_pa(size_t i) {
    for (int r = 0; r < npr; r++)
        if (i >= pr[r].first && i - pr[r].first < pr[r].n)
            return pr[r].base + ((uintptr_t)(i - pr[r].first) << 12);
    return 0;
}
static long frame_index(uintptr_t pa) {
    for (int r = 0; r < npr; r++)
        if (pa >= pr[r].base && (pa - pr[r].base) / PAGE_SIZE < pr[r].n)
            return (long)(pr[r].first + ((pa - pr[r].base) >> 12));
    return -1;
}

/* Add a range of RAM (page-aligned by this call).  Returns the number of frames added. */
size_t pmm_add_range(uintptr_t start, uintptr_t end) {
    if (start == 0 || end <= start || start > UINTPTR_MAX - (PAGE_SIZE - 1)) {
        kprintf("mem: rejecting invalid PMM range %lx..%lx\n", start, end);
        return 0;
    }
    start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end &= ~(PAGE_SIZE - 1);
    if (end <= start) return 0;
    for (int r = 0; r < npr; r++)
        if (start < pr[r].base + pr[r].n * PAGE_SIZE && pr[r].base < end) {
            kprintf("mem: rejecting overlapping PMM range %lx..%lx\n", start, end);
            return 0;
        }
    if (npr == MAXPRANGES) {
        kprintf("mem: PMM range capacity reached, ignoring %lx..%lx\n", start, end);
        return 0;
    }
    size_t n = (end - start) / PAGE_SIZE;
    if (n > MAXFRAMES - nframes) {
        kprintf("mem: PMM frame capacity reached, ignoring %lu frames at %lx\n",
                (unsigned long)(n - (MAXFRAMES - nframes)), start + (MAXFRAMES - nframes) * PAGE_SIZE);
        n = MAXFRAMES - nframes;
    }
    if (!n) return 0;
    pr[npr].base = start; pr[npr].first = nframes; pr[npr].n = n; npr++;
    for (size_t i = nframes; i < nframes + n; i++) bclr(i);
    freec += n;
    nframes += n;
    return n;
}
void pmm_init(uintptr_t start, uintptr_t end) {
    for (size_t i = 0; i < sizeof bm / sizeof bm[0]; i++) bm[i] = ~0u;
    npr = 0; nframes = 0; freec = 0; hint = 0;
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
    if (pa & (PAGE_SIZE - 1)) {
        kprintf("mem: rejecting unaligned frame free %lx\n", pa);
        return;
    }
    long i = frame_index(pa);
    if (i < 0) {
        kprintf("mem: rejecting out-of-range frame free %lx\n", pa);
        return;
    }
    if (!btst((size_t)i)) {
        kprintf("mem: rejecting already free frame %lx\n", pa);
        return;
    }
    bclr((size_t)i); freec++;
}
size_t pmm_free_count(void) { return freec; }
size_t pmm_total_count(void) { return nframes; }

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

static struct hdr *heap_block(uint8_t *p, uint8_t *end) {
    struct hdr *h = (struct hdr *)p;
    size_t left = (size_t)(end - p);
    if (left < sizeof *h || h->free > 1 || h->size > left - sizeof *h ||
        (h->size < left - sizeof *h && (h->size & 15)))
        panic("heap: invalid block metadata");
    return h;
}

static struct hdr *heap_allocation(void *ptr, int *region) {
    for (int r = 0; r < nhreg; r++) {
        if ((uintptr_t)ptr < (uintptr_t)hreg[r].s || (uintptr_t)ptr >= (uintptr_t)hreg[r].e)
            continue;
        for (uint8_t *p = hreg[r].s; p < hreg[r].e;) {
            struct hdr *h = heap_block(p, hreg[r].e);
            if (h + 1 == ptr && !h->free) {
                *region = r;
                return h;
            }
            p += sizeof *h + h->size;
        }
    }
    panic("heap: invalid or already freed allocation");
}

void *kmalloc(size_t n) {
    if (n > (size_t)-1 - 15) return 0;
    n = (n + 15) & ~15UL;
    if (!n) n = 16;
    for (int r = 0; r < nhreg; r++)
        for (uint8_t *p = hreg[r].s; p < hreg[r].e; p += sizeof(struct hdr) + ((struct hdr *)p)->size) {
            struct hdr *h = heap_block(p, hreg[r].e);
            if (!h->free || h->size < n) continue;
            if (h->size - n >= sizeof *h + 32) {
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
    int r;
    struct hdr *allocation = heap_allocation(ptr, &r);
    allocation->free = 1;
    for (uint8_t *p = hreg[r].s; p < hreg[r].e;) {
        struct hdr *h = heap_block(p, hreg[r].e);
        uint8_t *nx = p + sizeof *h + h->size;
        if (h->free && nx < hreg[r].e && heap_block(nx, hreg[r].e)->free)
            h->size += sizeof *h + ((struct hdr *)nx)->size;
        else p = nx;
    }
}
void *krealloc(void *ptr, size_t n) {
    if (!ptr) return kmalloc(n);
    int region;
    size_t old = heap_allocation(ptr, &region)->size;
    if (old >= n) return ptr;
    void *q = kmalloc(n);
    if (!q) return 0;
    memcpy(q, ptr, old);
    kfree(ptr);
    return q;
}
