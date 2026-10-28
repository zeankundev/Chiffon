/* ARM PL011 UART (MMIO), polled */
#pragma once
#include <stdint.h>
#include "arch.h"
static inline volatile uint32_t *pl011_regs(uintptr_t base) { return (volatile uint32_t *)P2V(base); }
static inline void pl011_putc(uintptr_t base, int c) { volatile uint32_t *r = pl011_regs(base); while (r[0x18 / 4] & 0x20) { } r[0] = (uint32_t)c; }
static inline int pl011_getc(uintptr_t base) { volatile uint32_t *r = pl011_regs(base); return (r[0x18 / 4] & 0x10) ? -1 : (int)(r[0] & 0xFF); }
