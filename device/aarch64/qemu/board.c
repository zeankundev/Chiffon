/* aarch64 "qemu" board: QEMU's `virt` machine with fixed, well-known addresses:
 *   PL011 UART 0x09000000, GICv2 0x08000000/0x08010000, virtio-mmio 0x0a000000.., RAM @ 0x40000000.
 * Run with:  qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 ...   (make run does this)
 * Use this file as the starting point for your own board (make new-device). */
#include "kernel.h"
#include "drivers/pl011.h"
#include "drivers/gicv2.h"
#include "drivers/armtimer.h"
#include "drivers/virtio_mmio_blk.h"
#include "drivers/fdt.h"

#define UART_BASE   0x09000000UL
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#define VIRTIO_BASE 0x0a000000UL
#define VIRTIO_STEP 0x200
#define TIMER_INTID 30                                  /* EL1 physical timer, PPI 14 */

static uintptr_t ram_base = 0x40000000UL, ram_size = 256UL << 20;
static char cmdline_buf[256];

static void on_node(const struct fdt_node *n, void *ctx) {
    if (n->devtype && !strcmp(n->devtype, "memory") && n->reg_len >= 16) { ram_base = fdt_be64(n->reg); ram_size = fdt_be64(n->reg + 8); }
    if (n->bootargs) strlcpy(cmdline_buf, n->bootargs, sizeof cmdline_buf);
}
void board_early_init(uintptr_t dtb) {
    struct fdt f;
    if (fdt_open(&f, dtb) == 0) fdt_walk(&f, on_node, 0);       /* RAM size + command line from QEMU's DTB */
}
void board_mem_range(uintptr_t *s, uintptr_t *e) { *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL; *e = ram_base + ram_size; }
const char *board_cmdline(void) { return cmdline_buf; }
void board_console_putc(int c) { pl011_putc(UART_BASE, c); }
int board_console_getc(void) { return pl011_getc(UART_BASE); }

void board_init(void) {
    gicv2_init(GICD_BASE, GICC_BASE, TIMER_INTID);
    armtimer_start(HZ);
    kprintf("board: aarch64 qemu-virt (RAM %lu MiB @ %lx)\n", (unsigned long)(ram_size >> 20), (unsigned long)ram_base);
}
void board_irq(struct trapframe *tf) {
    uint32_t iar = gicv2_ack(GICC_BASE), id = iar & 0x3FF;
    if (id == TIMER_INTID) {
        armtimer_reload();
        gicv2_eoi(GICC_BASE, iar);
        timer_tick(TF_FROM_USER(tf));
    } else if (id < 1020) gicv2_eoi(GICC_BASE, iar);
}

void board_poweroff(void) {                                    /* PSCI SYSTEM_OFF */
    register uint64_t x0 __asm__("x0") = 0x84000008;
    __asm__ volatile("hvc #0" :: "r"(x0));
    x0 = 0x84000008;
    __asm__ volatile("smc #0" :: "r"(x0));
    arch_halt();
}

int board_blk_init(void) {
    for (int i = 0; i < 32; i++)
        if (vblk_init_at(VIRTIO_BASE + i * VIRTIO_STEP) == 0) return 0;
    return -1;
}
uint64_t board_blk_sectors(void) { return vio_sectors; }
int board_blk_read(uint64_t lba, void *buf, uint32_t n) { return vblk_read(lba, buf, n); }
