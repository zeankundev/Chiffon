/* Tiny flattened-device-tree reader (header-only).
 * Assumes #address-cells = #size-cells = 2 (true for QEMU virt and most 64-bit SoCs' root bus). */
#pragma once
#include <stdint.h>
#include "arch.h"
#include "kernel.h"

struct fdt_node {
    const char *name; int depth;
    const uint8_t *compat; uint32_t compat_len;      /* NUL-separated strings */
    const uint8_t *reg; uint32_t reg_len;            /* tuples of (addr64, size64) */
    const uint8_t *irq; uint32_t irq_len;            /* big-endian cells */
    const char *method, *bootargs, *devtype;
};
struct fdt { const uint8_t *st, *strs; };

static inline uint32_t fdt_be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static inline uint64_t fdt_be64(const void *p) { return (uint64_t)fdt_be32(p) << 32 | fdt_be32((const uint8_t *)p + 4); }
static inline int fdt_open(struct fdt *f, uintptr_t dtb_pa) {
    const uint8_t *h = P2V(dtb_pa);
    if (!dtb_pa || fdt_be32(h) != 0xd00dfeed) return -1;
    f->st = h + fdt_be32(h + 8); f->strs = h + fdt_be32(h + 12);
    return 0;
}
/* Calls cb for every node (after its properties are known), children before parents. */
static inline void fdt_walk(const struct fdt *f, void (*cb)(const struct fdt_node *, void *), void *ctx) {
    struct fdt_node stack[10]; int depth = 0;
    for (const uint8_t *p = f->st;;) {
        uint32_t tok = fdt_be32(p); p += 4;
        if (tok == 1) {
            const char *name = (const char *)p;
            p += (strlen(name) + 1 + 3) & ~3UL;
            if (depth < 10) { memset(&stack[depth], 0, sizeof stack[0]); stack[depth].name = name; stack[depth].depth = depth + 1; }
            depth++;
        } else if (tok == 2) {
            depth--;
            if (depth >= 0 && depth < 10) cb(&stack[depth], ctx);
            if (depth <= 0) break;
        } else if (tok == 3) {
            uint32_t len = fdt_be32(p), off = fdt_be32(p + 4);
            const uint8_t *data = p + 8;
            const char *pn = (const char *)f->strs + off;
            if (depth >= 1 && depth <= 10) {
                struct fdt_node *n = &stack[depth - 1];
                if (!strcmp(pn, "compatible")) { n->compat = data; n->compat_len = len; }
                else if (!strcmp(pn, "reg")) { n->reg = data; n->reg_len = len; }
                else if (!strcmp(pn, "interrupts")) { n->irq = data; n->irq_len = len; }
                else if (!strcmp(pn, "method")) n->method = (const char *)data;
                else if (!strcmp(pn, "bootargs")) n->bootargs = (const char *)data;
                else if (!strcmp(pn, "device_type")) n->devtype = (const char *)data;
            }
            p = data + ((len + 3) & ~3U);
        } else if (tok == 9) break;
        else if (tok != 4) break;
    }
}
/* does any compatible string equal s? */
static inline int fdt_compat(const struct fdt_node *n, const char *s) {
    for (uint32_t i = 0; i < n->compat_len;) {
        const char *c = (const char *)n->compat + i;
        if (!strcmp(c, s)) return 1;
        i += strlen(c) + 1;
    }
    return 0;
}
