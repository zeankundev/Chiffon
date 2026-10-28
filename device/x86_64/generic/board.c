/* x86_64 "generic" board: any PC-compatible machine booted by a Multiboot-1 loader.
 * Hardware used: COM1 serial console, 8259 PIC + 8254 PIT (100 Hz), ATA PIO disk on the
 * primary channel.  Real PCs without legacy ATA/COM need a custom board (see docs/BOARD-SHIM.md).
 * The "qemu" board includes this file and only changes how power-off works. */
#ifndef BOARD_NAME
#define BOARD_NAME "generic"
#endif
#include "kernel.h"
#include "drivers/ns16550_io.h"
#include "drivers/pic_pit.h"
#include "drivers/ata_pio.h"

#define COM1 0x3F8
static uintptr_t ram_end;
static char cmdline_buf[256];

void board_early_init(uintptr_t mbi) {
    ns16550_io_init(COM1);
    uint32_t *m = P2V(mbi);                          /* multiboot info: flags, mem_lower, mem_upper, ..., cmdline@16 */
    uint64_t upper_kb = (m[0] & 1) ? m[2] : 64 * 1024;
    ram_end = 0x100000 + upper_kb * 1024;
    if (ram_end > 0xC0000000UL) ram_end = 0xC0000000UL;           /* stay below the PCI hole */
    if (m[0] & 4) {
        const char *c = P2V(m[4]);
        size_t i = 0;
        while (c[i] && i < sizeof cmdline_buf - 1) { cmdline_buf[i] = c[i]; i++; }
        cmdline_buf[i] = 0;
    }
}
void board_mem_range(uintptr_t *s, uintptr_t *e) { *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL; *e = ram_end; }
const char *board_cmdline(void) { return cmdline_buf; }
void board_console_putc(int c) { ns16550_io_putc(COM1, c); }
int board_console_getc(void) { return ns16550_io_getc(COM1); }

void board_init(void) { pic_pit_init(HZ); kprintf("board: x86_64 %s (PIC/PIT/COM1/ATA)\n", BOARD_NAME); }
void board_irq(struct trapframe *tf) {
    unsigned v = tf->trapno;
    pic_eoi(v);
    if (v == 32) timer_tick(TF_FROM_USER(tf));
}

void board_poweroff(void) {
#ifdef BOARD_QEMU
    outw(0x604, 0x2000); outw(0xB004, 0x2000);       /* QEMU ACPI / older QEMU+Bochs */
#else
    kprintf("\nSystem halted. It is now safe to power off.\n");
#endif
    arch_halt();
}

int board_blk_init(void) { return ata_init(); }
uint64_t board_blk_sectors(void) { return ata_nsect; }
int board_blk_read(uint64_t lba, void *buf, uint32_t n) { return ata_read(lba, buf, n); }
