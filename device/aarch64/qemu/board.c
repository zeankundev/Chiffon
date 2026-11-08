/* aarch64 "qemu" board: QEMU's `virt` machine with fixed, well-known addresses:
 *   PL011 UART 0x09000000, GICv2 0x08000000/0x08010000, virtio-mmio 0x0a000000.., RAM @ 0x40000000.
 * Run with:  qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 ...   (make run does this)
 * Use this file as the starting point for your own board (make new-device). */
#include "kernel.h"
#include "drivers/pl011.h"
#include "drivers/gicv2.h"
#include "drivers/armtimer.h"
#include "drivers/virtio_mmio_blk.h"
#include "net.h"
#include "drivers/fdt.h"

#define UART_BASE   0x09000000UL
#define GICD_BASE   0x08000000UL
#define GICC_BASE   0x08010000UL
#define VIRTIO_BASE 0x0a000000UL
#define VIRTIO_STEP 0x200
#define TIMER_INTID 30                                  /* EL1 physical timer, PPI 14 */

static uintptr_t ram_base = 0x40000000UL, ram_size = 256UL << 20;
static char cmdline_buf[256];
static uint64_t cpu_mpidr[MAX_CPUS];                    /* from the DTB /cpus nodes (reg = MPIDR Aff0) */
static int cpu_nodes;
static int psci_smc;                                    /* DTB "method": hvc or smc */

static void on_node(const struct fdt_node *n, void *ctx) {
    if (n->devtype && !strcmp(n->devtype, "memory") && n->reg_len >= 16) { ram_base = fdt_be64(n->reg); ram_size = fdt_be64(n->reg + 8); }
    if (n->bootargs) strlcpy(cmdline_buf, n->bootargs, sizeof cmdline_buf);
    if (n->devtype && !strcmp(n->devtype, "cpu") && n->reg_len >= 4 && cpu_nodes < MAX_CPUS)
        cpu_mpidr[cpu_nodes++] = fdt_be32(n->reg) & 0xFFFFFF;
    if (n->method && (fdt_compat(n, "arm,psci-1.0") || fdt_compat(n, "arm,psci-0.2") || fdt_compat(n, "arm,psci")))
        psci_smc = !strcmp(n->method, "smc");
}
void board_early_init(uintptr_t dtb) {
    struct fdt f;
    if (fdt_open(&f, dtb) == 0) fdt_walk(&f, on_node, 0);       /* RAM size + command line from QEMU's DTB */
#ifdef CHI_RAM_CAP_MB
    /* RAMCAP=<MiB> in the environment: use only this much RAM, even if the machine has more.  QEMU
     * needs a larger -m to place its device tree after the kernel; the kernel must not spend it. */
    if (ram_size > ((uintptr_t)CHI_RAM_CAP_MB << 20)) ram_size = (uintptr_t)CHI_RAM_CAP_MB << 20;
#endif
}
void board_mem_range(uintptr_t *s, uintptr_t *e) { *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL; *e = ram_base + ram_size; }
const char *board_cmdline(void) { return cmdline_buf; }
void board_console_putc(int c) { pl011_putc(UART_BASE, c); }
int board_console_getc(void) { return pl011_getc(UART_BASE); }

void board_init(void) {
    gicv2_init(GICD_BASE, GICC_BASE, TIMER_INTID);
    gicv2_cpu_init(GICD_BASE, GICC_BASE, TIMER_INTID, SGI_RESCHED);
    armtimer_start(HZ);
    driver_note("pl011", "PL011 UART console", DRV_CHAR);
    driver_note("gicv2+timer", "GICv2 + ARM generic timer, 100 Hz tick", DRV_MISC);
    driver_note("virtio-blk", "virtio-mmio block device", DRV_BLOCK);
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

#define PSCI_CPU_ON64 0xC4000003UL
static int psci_call(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
    if (psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                         : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
    else
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                         : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
    return (int)x0;
}
int board_smp_start(uintptr_t entry_pa) {
    uint64_t me;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(me));
    me &= 0xFFFFFF;
    int started = 0;
    for (int i = 0; i < cpu_nodes && started + 1 < MAX_CPUS; i++) {
        if (cpu_mpidr[i] == me) continue;                     /* the boot CPU */
        int r = psci_call(PSCI_CPU_ON64, cpu_mpidr[i], entry_pa, (uint64_t)(started + 1));
        if (r == 0) started++;
        else kprintf("smp: CPU_ON for mpidr %lx failed (%d)\n", (unsigned long)cpu_mpidr[i], r);
    }
    return started;
}
void board_secondary_init(int id) {
    (void)id;
    gicv2_cpu_init(GICD_BASE, GICC_BASE, TIMER_INTID, SGI_RESCHED);
    armtimer_reload();
}
void board_ipi_send(int cpu) { gicv2_sgi(GICD_BASE, cpu, SGI_RESCHED); }

void board_poweroff(void) {                                    /* PSCI SYSTEM_OFF */
    register uint64_t x0 __asm__("x0") = 0x84000008;
    __asm__ volatile("hvc #0" :: "r"(x0));
    x0 = 0x84000008;
    __asm__ volatile("smc #0" :: "r"(x0));
    arch_halt();
}

int board_net_probe(void) {                             /* the first virtio-net device found */
    for (int i = 0; i < 32; i++)
        if (vnet_probe(VIRTIO_BASE + i * VIRTIO_STEP) == 0) return 0;
    return -1;
}

int board_blk_init(void) {
    for (int i = 0; i < 32; i++)
        if (vblk_init_at(VIRTIO_BASE + i * VIRTIO_STEP) == 0) return 0;
    return -1;
}
uint64_t board_blk_sectors(void) { return vio_sectors; }
int board_blk_read(uint64_t lba, void *buf, uint32_t n) { return vblk_read(lba, buf, n); }
int board_blk_write(uint64_t lba, const void *buf, uint32_t n) { return vblk_write(lba, buf, n); }
