/* 16550 UART on x86 I/O ports (COM1 = 0x3F8) */
#pragma once
#include "x86io.h"
static inline void ns16550_io_init(uint16_t base) {
    outb(base + 1, 0); outb(base + 3, 0x80); outb(base, 1); outb(base + 1, 0);   /* 115200 */
    outb(base + 3, 3); outb(base + 2, 0xC7); outb(base + 4, 0x0B);
}
static inline void ns16550_io_putc(uint16_t base, int c) { while (!(inb(base + 5) & 0x20)) { } outb(base, c); }
static inline int ns16550_io_getc(uint16_t base) { return (inb(base + 5) & 1) ? inb(base) : -1; }
