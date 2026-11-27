/* Tiny flattened-device-tree reader (header-only). */
#pragma once
#include <stdint.h>
#include "arch.h"
#include "kernel.h"

struct fdt_node {
    const char *name; int depth;
    const uint8_t *compat; uint32_t compat_len;      /* NUL-separated strings */
    const uint8_t *reg; uint32_t reg_len;
    const uint8_t *irq; uint32_t irq_len;            /* big-endian cells */
    const char *method, *bootargs, *devtype;
    const char *parent_name;
    uint32_t addr_cells, size_cells, parent_addr_cells, parent_size_cells;
    int disabled, has_ranges, has_size;
    uint32_t ranges_len;
    const uint8_t *initrd_start, *initrd_end;
    uint32_t initrd_start_len, initrd_end_len;
};
struct fdt {
    const uint8_t *st, *st_end, *strs, *strs_end, *reserve, *reserve_end;
    uint32_t size;
};

static inline uint32_t fdt_be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static inline uint64_t fdt_be64(const void *p) { return (uint64_t)fdt_be32(p) << 32 | fdt_be32((const uint8_t *)p + 4); }
static inline int fdt_open(struct fdt *f, uintptr_t dtb_pa) {
    const uintptr_t limit = 1UL << 32;                  /* initial AArch64 direct map */
    if (!dtb_pa || dtb_pa > limit - 40) return -1;
    const uint8_t *h = P2V(dtb_pa);
    if (fdt_be32(h) != 0xd00dfeed || fdt_be32(h + 20) < 17 || fdt_be32(h + 24) > 17)
        return -1;
    uint32_t size = fdt_be32(h + 4), st = fdt_be32(h + 8), strs = fdt_be32(h + 12);
    uint32_t rsv = fdt_be32(h + 16), strs_len = fdt_be32(h + 32), st_len = fdt_be32(h + 36);
    if (size < 40 || size > limit - dtb_pa || (st & 3) || (rsv & 7) ||
        st < 40 || st > size || st_len > size - st ||
        strs < 40 || strs > size || strs_len > size - strs ||
        rsv < 40 || rsv >= st || rsv >= strs ||
        (st < strs + strs_len && strs < st + st_len))
        return -1;
    f->size = size;
    f->st = h + st;
    f->st_end = h + st + st_len;
    f->strs = h + strs;
    f->strs_end = h + strs + strs_len;
    f->reserve = h + rsv;
    f->reserve_end = h + (st < strs ? st : strs);
    return 0;
}

static inline const uint8_t *fdt_nul(const uint8_t *p, const uint8_t *end) {
    for (; p < end; p++)
        if (!*p) return p;
    return 0;
}

/* Calls cb for every node (after its properties are known), children before parents. */
static inline int fdt_walk(const struct fdt *f, void (*cb)(const struct fdt_node *, void *), void *ctx) {
    struct fdt_node stack[10];
    int saw_child[10];
    int depth = 0, seen_root = 0;
    for (const uint8_t *p = f->st; f->st_end - p >= 4;) {
        uint32_t tok = fdt_be32(p); p += 4;
        if (tok == 1) {
            if (depth == (int)(sizeof stack / sizeof stack[0]) || (!depth && seen_root))
                return -1;
            const uint8_t *nul = fdt_nul(p, f->st_end);
            if (!nul || (!depth && p != nul) || (depth && p == nul)) return -1;
            size_t bytes = ((size_t)(nul - p) + 4) & ~3UL;
            if (bytes > (size_t)(f->st_end - p)) return -1;
            struct fdt_node *n = &stack[depth];
            memset(n, 0, sizeof *n);
            saw_child[depth] = 0;
            n->name = (const char *)p;
            n->depth = depth + 1;
            n->addr_cells = 2;
            n->size_cells = 1;
            if (depth) {
                saw_child[depth - 1] = 1;
                const struct fdt_node *parent = &stack[depth - 1];
                n->parent_name = parent->name;
                n->parent_addr_cells = parent->addr_cells;
                n->parent_size_cells = parent->size_cells;
                n->disabled = parent->disabled;
            }
            p += bytes;
            depth++;
            seen_root = 1;
        } else if (tok == 2) {
            if (!depth) return -1;
            cb(&stack[--depth], ctx);
        } else if (tok == 3) {
            if (!depth || saw_child[depth - 1] || f->st_end - p < 8) return -1;
            uint32_t len = fdt_be32(p), off = fdt_be32(p + 4);
            const uint8_t *data = p + 8;
            size_t bytes = ((size_t)len + 3) & ~3UL;
            if (bytes > (size_t)(f->st_end - data) || off >= (size_t)(f->strs_end - f->strs) ||
                !fdt_nul(f->strs + off, f->strs_end))
                return -1;
            const char *pn = (const char *)f->strs + off;
            struct fdt_node *n = &stack[depth - 1];
            if (!strcmp(pn, "compatible")) { n->compat = data; n->compat_len = len; }
            else if (!strcmp(pn, "reg")) { n->reg = data; n->reg_len = len; }
            else if (!strcmp(pn, "interrupts")) { n->irq = data; n->irq_len = len; }
            else if (!strcmp(pn, "ranges")) { n->has_ranges = 1; n->ranges_len = len; }
            else if (!strcmp(pn, "size")) n->has_size = 1;
            else if (!strcmp(pn, "linux,initrd-start")) { n->initrd_start = data; n->initrd_start_len = len; }
            else if (!strcmp(pn, "linux,initrd-end")) { n->initrd_end = data; n->initrd_end_len = len; }
            else if (!strcmp(pn, "#address-cells") || !strcmp(pn, "#size-cells")) {
                if (len != 4) return -1;
                if (!strcmp(pn, "#address-cells")) n->addr_cells = fdt_be32(data);
                else n->size_cells = fdt_be32(data);
            } else if (!strcmp(pn, "method") || !strcmp(pn, "bootargs") ||
                       !strcmp(pn, "device_type") || !strcmp(pn, "status")) {
                if (!fdt_nul(data, data + len)) return -1;
                const char *value = (const char *)data;
                if (!strcmp(pn, "method")) n->method = value;
                else if (!strcmp(pn, "bootargs")) n->bootargs = value;
                else if (!strcmp(pn, "device_type")) n->devtype = value;
                else if (strcmp(value, "okay") && strcmp(value, "ok")) n->disabled = 1;
            }
            p = data + bytes;
        } else if (tok == 9) return !depth && seen_root ? 0 : -1;
        else if (tok != 4) return -1;
    }
    return -1;
}
/* does any compatible string equal s? */
static inline int fdt_compat(const struct fdt_node *n, const char *s) {
    for (uint32_t i = 0; i < n->compat_len;) {
        const char *c = (const char *)n->compat + i;
        const uint8_t *nul = fdt_nul(n->compat + i, n->compat + n->compat_len);
        if (!nul) return 0;
        if (!strcmp(c, s)) return 1;
        i = (uint32_t)(nul - n->compat) + 1;
    }
    return 0;
}
