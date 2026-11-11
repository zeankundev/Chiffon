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
#include "drivers/x86_platform.h"
#ifdef BOARD_QEMU
#include "drivers/x86io.h"
#endif

#define COM1 0x3F8
static uintptr_t ram_end;
static char cmdline_buf[256];
static int com1_ok;                              /* a serial port answered at COM1; output is optional */

void board_early_init(uintptr_t mbi) {
    com1_ok = ns16550_io_probe(COM1);
    if (com1_ok) ns16550_io_init(COM1);
    uint32_t *m = P2V(mbi);                          /* multiboot info: flags, mem_lower, mem_upper, ..., cmdline@16 */
    uint64_t upper_kb = (m[0] & 1) ? m[2] : 64 * 1024;
    ram_end = 0x100000 + upper_kb * 1024;
    if (ram_end > 0xC0000000UL) ram_end = 0xC0000000UL;           /* stay below the PCI hole */
    mb_fb_boot_info(m);                              /* framebuffer from the bootloader, if any */
    if (m[0] & 4) {
        const char *c = P2V(m[4]);
        size_t i = 0;
        while (c[i] && i < sizeof cmdline_buf - 1) { cmdline_buf[i] = c[i]; i++; }
        cmdline_buf[i] = 0;
    }
}
void board_mem_range(uintptr_t *s, uintptr_t *e) { *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL; *e = ram_end; }
const char *board_cmdline(void) { return cmdline_buf; }
void board_console_putc(int c) { if (com1_ok) ns16550_io_putc(COM1, c); }
int board_console_getc(void) { return com1_ok ? ns16550_io_getc(COM1) : -1; }

void board_init(void) {
    pic_pit_init(HZ);
    if (com1_ok) driver_note("ns16550", "COM1 serial console 115200 8N1", DRV_CHAR);
    driver_note("pic-pit", "8259 PIC + 8254 PIT, 100 Hz tick", DRV_MISC);
    driver_note("ata-pio", "legacy ATA PIO disk (primary master)", DRV_BLOCK);
    kprintf("board: x86_64 %s (PIC/PIT/COM1/ATA)\n", BOARD_NAME);
    /* LAPIC and CPU discovery run from kmain() via smp_init(), once, after init has started. */
}
void board_irq(struct trapframe *tf) {
    unsigned v = tf->trapno;
    pic_eoi(v);
    if (v == 32) timer_tick(TF_FROM_USER(tf));
    else irq_dispatch(v - 32);                       /* drivers registered with irq_register() */
}

void board_poweroff(void) {
#ifdef BOARD_QEMU
    /* ACPI is firmware-described: find the FADT/DSDT and obtain the S5 sleep
     * type from the _S5 object.  Hard-coding 0x2000 requests type 0, which is
     * not the ACPI soft-off state on most machines. */
    uint16_t pm1a = 0, pm1b = 0, slp_typa = 0, slp_typb = 0;
    const uint8_t *rsdp = 0;
    for (uintptr_t p = 0xE0000; p < 0x100000; p += 16) {
        const uint8_t *q = P2V(p);
        if (!memcmp(q, "RSD PTR ", 8)) {
            unsigned sum = 0; for (unsigned i = 0; i < 20; i++) sum += q[i];
            if (!(sum & 255)) { rsdp = q; break; }
        }
    }
    if (rsdp) {
        uint32_t rsdt = *(const uint32_t *)(rsdp + 16);
        const uint8_t *root = P2V(rsdt);
        if (!memcmp(root, "RSDT", 4)) {
            uint32_t len = *(const uint32_t *)(root + 4);
            if (len >= 36 && len < 65536) {
                const uint32_t *entries = (const uint32_t *)(root + 36);
                for (unsigned i = 0; i < (len - 36) / 4; i++) {
                    const uint8_t *t = P2V(entries[i]);
                    if (memcmp(t, "FACP", 4)) continue;
                    uint32_t flen = *(const uint32_t *)(t + 4);
                    if (flen < 90 || flen > 4096) break;
                    pm1a = *(const uint32_t *)(t + 64);
                    pm1b = *(const uint32_t *)(t + 68);
                    uint32_t dsdt = *(const uint32_t *)(t + 40);
                    const uint8_t *d = P2V(dsdt);
                    if (memcmp(d, "DSDT", 4)) break;
                    uint32_t dlen = *(const uint32_t *)(d + 4);
                    if (dlen < 40 || dlen > 1024 * 1024) break;
                    /* AML encoding: NameOp, _S5_, PackageOp, count, two
                     * integer literals.  Support Zero/One/Byte/Word/DWord. */
                    for (uint32_t j = 36; j + 10 < dlen; j++) {
                        if (d[j] != 0x08 || memcmp(d + j + 1, "_S5_", 4)) continue;
                        uint32_t k = j + 5;
                        if (d[k] == 0x12) k += 2; /* Package length (common short form) */
                        else continue;
                        if (k >= dlen) break;
                        k++; /* element count */
                        uint32_t vals[2] = {0, 0};
                        for (int n = 0; n < 2 && k < dlen; n++) {
                            uint8_t op = d[k++];
                            if (op == 0x00 || op == 0x01) vals[n] = op;
                            else if (op == 0x0A && k < dlen) vals[n] = d[k++];
                            else if (op == 0x0B && k + 1 < dlen) { vals[n] = *(const uint16_t *)(d+k); k += 2; }
                            else if (op == 0x0C && k + 3 < dlen) { vals[n] = *(const uint32_t *)(d+k); k += 4; }
                            else break;
                        }
                        slp_typa = (uint16_t)((vals[0] & 7) << 10);
                        slp_typb = (uint16_t)((vals[1] & 7) << 10);
                        break;
                    }
                    break;
                }
            }
        }
    }
    if (pm1a && slp_typa) {
        outw((uint16_t)pm1a, (uint16_t)(slp_typa | (1u << 13)));
        if (pm1b) outw((uint16_t)pm1b, (uint16_t)(slp_typb | (1u << 13)));
    } else {
        /* Compatibility path for older QEMU/Bochs firmware without discoverable
         * ACPI tables. */
        outw(0x604, 0x2000); outw(0xB004, 0x2000);
    }
#else
    kprintf("\nSystem halted. It is now safe to power off.\n");
#endif
    arch_halt();
}

int board_blk_init(void) { return ata_init(); }
uint64_t board_blk_sectors(void) { return ata_nsect; }
int board_blk_read(uint64_t lba, void *buf, uint32_t n) { return ata_read(lba, buf, n); }
int board_blk_write(uint64_t lba, const void *buf, uint32_t n) { return ata_write(lba, buf, n); }

int board_net_probe(void) {
    /* Network device discovery and initialization happens via drivers/x86_64/virtio_net.c.
     * The virtio-net-pci driver will auto-probe via the PCI driver during drivers_init().
     * This function just signals that we've attempted to set up networking; if no device
     * was found, that's OK -- networking is optional. */
    return 0;
}
