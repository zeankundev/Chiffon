/* 16550 UART on x86 I/O ports (COM1 = 0x3F8).  The serial port is optional: a VM or machine
 * without it must still boot, so callers probe first and every transmit wait is bounded. */
#pragma once
#include "x86io.h"

#define NS16550_SPIN 100000u                 /* transmit-ready polls before giving up on a byte */

/* 1 if a 16550 answers at `base`: the scratch register (base+7) holds what is written to it.
 * An absent port reads back 0xFF or 0x00, which fails the test. */
static inline int ns16550_io_probe(uint16_t base) {
    if (inb(base + 5) == 0xFF) return 0;              /* nothing decodes the port: floating bus */
    outb(base + 7, 0xA5); uint8_t a = inb(base + 7);
    outb(base + 7, 0x5A); uint8_t b = inb(base + 7);
    return a == 0xA5 && b == 0x5A;
}
static inline void ns16550_io_init(uint16_t base) {
    outb(base + 1, 0); outb(base + 3, 0x80); outb(base, 1); outb(base + 1, 0);   /* 115200 */
    outb(base + 3, 3); outb(base + 2, 0xC7); outb(base + 4, 0x0B);
}
/* Waits at most NS16550_SPIN polls; a byte that cannot be sent is dropped, never blocks the kernel. */
static inline void ns16550_io_putc(uint16_t base, int c) {
    for (uint32_t i = 0; i < NS16550_SPIN && !(inb(base + 5) & 0x20); i++) { }
    if (inb(base + 5) & 0x20) outb(base, c);
}
static inline int ns16550_io_getc(uint16_t base) { return (inb(base + 5) & 1) ? inb(base) : -1; }
