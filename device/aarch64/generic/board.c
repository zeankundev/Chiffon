/* aarch64 "generic" board: configures itself from the device tree the boot loader passes in x0.
 * Finds: RAM (/memory), command line (/chosen), PL011 UART, GICv2, ARMv8 timer PPI, PSCI
 * method, virtio-mmio block devices.  Works on QEMU virt and on any machine with the same kinds
 * of devices whose RAM starts at 0x40000000 (the load address is fixed at link time; for a
 * different RAM base, or other UART/interrupt controller, make a custom device).
 * Limitations: GICv2 only (no GICv3), PL011 only, #address-cells/#size-cells = 2. */
#include "kernel.h"
#include "drivers/pl011.h"
#include "drivers/gicv2.h"
#include "drivers/armtimer.h"
#include "drivers/virtio_mmio_blk.h"
#include "drivers/fdt.h"

static struct {
    uintptr_t ram_base, ram_size, uart, gicd, gicc;
    unsigned timer_intid;
    int psci_smc, nvirtio, gicv3;
    uintptr_t virtio[32];
} D = { 0x40000000UL, 256UL << 20, 0x09000000UL, 0x08000000UL, 0x08010000UL, 30, 0, 0, 0, {0} };
static char cmdline_buf[256];

static void on_node(const struct fdt_node *n, void *ctx) {
    if (n->devtype && !strcmp(n->devtype, "memory") && n->reg_len >= 16) { D.ram_base = fdt_be64(n->reg); D.ram_size = fdt_be64(n->reg + 8); }
    if (n->bootargs) strlcpy(cmdline_buf, n->bootargs, sizeof cmdline_buf);
    if (fdt_compat(n, "arm,pl011") && n->reg_len >= 16) D.uart = fdt_be64(n->reg);
    if ((fdt_compat(n, "arm,cortex-a15-gic") || fdt_compat(n, "arm,gic-400")) && n->reg_len >= 32) { D.gicd = fdt_be64(n->reg); D.gicc = fdt_be64(n->reg + 16); }
    if (fdt_compat(n, "arm,gic-v3")) D.gicv3 = 1;
    if (fdt_compat(n, "arm,armv8-timer") && n->irq_len >= 24) {          /* tuples: (type, number, flags); [1] = EL1 physical timer */
        uint32_t type = fdt_be32(n->irq + 12), num = fdt_be32(n->irq + 16);
        D.timer_intid = (type == 1 ? 16 : 32) + num;
    }
    if (fdt_compat(n, "arm,psci-0.2") || fdt_compat(n, "arm,psci-1.0") || fdt_compat(n, "arm,psci"))
        D.psci_smc = n->method && !strcmp(n->method, "smc");
    if (fdt_compat(n, "virtio,mmio") && n->reg_len >= 16 && D.nvirtio < 32) D.virtio[D.nvirtio++] = fdt_be64(n->reg);
}

void board_early_init(uintptr_t dtb) {
    struct fdt f;
    if (fdt_open(&f, dtb) == 0) fdt_walk(&f, on_node, 0);
    arch_map_device(D.uart, 0x1000);
}
void board_mem_range(uintptr_t *s, uintptr_t *e) { *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL; *e = D.ram_base + D.ram_size; }
const char *board_cmdline(void) { return cmdline_buf; }
void board_console_putc(int c) { pl011_putc(D.uart, c); }
int board_console_getc(void) { return pl011_getc(D.uart); }

void board_init(void) {
    if (D.gicv3) panic("generic aarch64 board: GICv3 is not supported; write a custom device");
    arch_map_device(D.gicd, 0x10000); arch_map_device(D.gicc, 0x10000);
    gicv2_init(D.gicd, D.gicc, D.timer_intid);
    armtimer_start(HZ);
    kprintf("board: aarch64 generic (device tree): uart %lx gic %lx/%lx timer irq %u, %d virtio-mmio\n",
            (unsigned long)D.uart, (unsigned long)D.gicd, (unsigned long)D.gicc, D.timer_intid, D.nvirtio);
}
void board_irq(struct trapframe *tf) {
    uint32_t iar = gicv2_ack(D.gicc), id = iar & 0x3FF;
    if (id == D.timer_intid) {
        armtimer_reload();
        gicv2_eoi(D.gicc, iar);
        timer_tick(TF_FROM_USER(tf));
    } else if (id < 1020) gicv2_eoi(D.gicc, iar);
}
void board_poweroff(void) {
    register uint64_t x0 __asm__("x0") = 0x84000008;                     /* PSCI SYSTEM_OFF */
    if (D.psci_smc) __asm__ volatile("smc #0" :: "r"(x0)); else __asm__ volatile("hvc #0" :: "r"(x0));
    arch_halt();
}

int board_blk_init(void) {
    for (int i = 0; i < D.nvirtio; i++) {
        arch_map_device(D.virtio[i], 0x200);
        if (vblk_init_at(D.virtio[i]) == 0) return 0;
    }
    return -1;
}
uint64_t board_blk_sectors(void) { return vio_sectors; }
int board_blk_read(uint64_t lba, void *buf, uint32_t n) { return vblk_read(lba, buf, n); }
int board_blk_write(uint64_t lba, const void *buf, uint32_t n) { return vblk_write(lba, buf, n); }
