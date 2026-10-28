/* ARM GICv2 (distributor + CPU interface), single core */
#pragma once
#include <stdint.h>
#include "arch.h"
static inline volatile uint32_t *gic_regs(uintptr_t base) { return (volatile uint32_t *)P2V(base); }
static inline void gicv2_init(uintptr_t dist, uintptr_t cpu, unsigned intid) {
    volatile uint32_t *d = gic_regs(dist), *c = gic_regs(cpu);
    d[0] = 0;
    d[(0x100 + 4 * (intid / 32)) / 4] = 1u << (intid % 32);      /* ISENABLERn */
    d[0] = 1;
    c[1] = 0xFF;                                                 /* PMR */
    c[0] = 1;
}
static inline uint32_t gicv2_ack(uintptr_t cpu) { return gic_regs(cpu)[0xC / 4]; }       /* IAR */
static inline void gicv2_eoi(uintptr_t cpu, uint32_t iar) { gic_regs(cpu)[0x10 / 4] = iar; }
