/* PS/2 keyboard (i8042), scancode set 1 (the controller translates for us).  Key -> character
 * translation is shared with other keyboards: see kernel/kbd.c. */
#include "kernel.h"
#include "drivers/x86io.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64

static int ext;

static void drain(void) {
    for (int n = 0; n < 64; n++) {
        uint8_t st = inb(KBD_STAT);
        if (!(st & 1)) return;
        uint8_t sc = inb(KBD_DATA);
        if (st & 0x20) continue;                            /* mouse byte: not ours */
        if (sc == 0xE0) { ext = 1; continue; }
        if (sc == 0xE1 || sc == 0xFA || sc == 0xFE) { ext = 0; continue; }                      /* pause / ack / resend (0xAA is left-Shift release: keep it) */
        kbd_scancode(sc & 0x7F, sc & 0x80, ext);
        ext = 0;
    }
}
static void irq(unsigned n) { drain(); }

static void kbc_wait(void) { for (int i = 0; i < 100000 && (inb(KBD_STAT) & 2); i++) { } }

static int probe(void) { return inb(KBD_STAT) == 0xFF ? -1 : 0; }      /* floating bus: no controller */
static int init(void) {
    for (int i = 0; i < 16 && (inb(KBD_STAT) & 1); i++) inb(KBD_DATA);  /* flush */
    kbc_wait(); outb(KBD_STAT, 0xAE);                                   /* enable first port */
    kbc_wait(); outb(KBD_STAT, 0x20);                                   /* read config byte */
    for (int i = 0; i < 100000 && !(inb(KBD_STAT) & 1); i++) { }
    uint8_t cfg = inb(KBD_DATA);
    cfg |= 0x01 | 0x40;                                                 /* IRQ1 on, set-1 translation */
    cfg &= ~0x10;                                                       /* clock enabled */
    kbc_wait(); outb(KBD_STAT, 0x60);
    kbc_wait(); outb(KBD_DATA, cfg);
    kbc_wait(); outb(KBD_DATA, 0xF4);                                   /* keyboard: enable scanning */
    if (irq_register(1, irq)) return -1;
    outb(0x21, inb(0x21) & ~0x02);                                      /* unmask IRQ1 on the master PIC */
    return 0;
}
static struct kdriver drv = { "ps2-kbd", "i8042 PS/2 keyboard", DRV_INPUT, 10, probe, init, drain, 0 };
KDRIVER(drv);
