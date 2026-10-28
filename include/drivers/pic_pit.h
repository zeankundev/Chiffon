/* Legacy PC interrupt hardware: 8259 PIC (vectors 32..47) + 8254 PIT timer on IRQ0 */
#pragma once
#include "x86io.h"
static inline void pic_pit_init(unsigned hz) {
    outb(0x20, 0x11); outb(0xA0, 0x11); outb(0x21, 32); outb(0xA1, 40);
    outb(0x21, 4); outb(0xA1, 2); outb(0x21, 1); outb(0xA1, 1);
    outb(0x21, 0xFE); outb(0xA1, 0xFF);                       /* only IRQ0 unmasked */
    outb(0x43, 0x36); outb(0x40, (1193182 / hz) & 0xFF); outb(0x40, (1193182 / hz) >> 8);
}
static inline void pic_eoi(unsigned vector) { if (vector >= 40) outb(0xA0, 0x20); outb(0x20, 0x20); }
