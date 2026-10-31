/* Raspberry Pi Zero 2W board configuration for Chiffon
 * SoC: BCM2837B0 (quad Cortex-A53, 512 MiB RAM)
 * Serial: AUX Mini UART on GPIO 14 (TXD) and GPIO 15 (RXD) -> USB-TTL to host Pi 5 minicom
 * Timer: ARMv8 generic physical timer via Core Local Interrupt Controller (0x40000000)
 */
#include "kernel.h"
#include "drivers/armtimer.h"
#include "drivers/fdt.h"

/* BCM2837 Local interrupt routing (ARM Local Peripherals @ 0x40000000) */
#define LOCAL_INTC_BASE         0x40000000UL
#define LOCAL_TIMER_INT_CONTROL ((volatile uint32_t *)P2V(LOCAL_INTC_BASE + 0x40)) /* Core 0 Timers interrupt control */
#define LOCAL_IRQ_PENDING0      ((volatile uint32_t *)P2V(LOCAL_INTC_BASE + 0x60)) /* Core 0 IRQ pending source */

/* Mini UART & GPIO prototypes declared in drivers/aarch64/rpi_gpio.c */
void rpi_uart_init(void);
void rpi_uart_putc(int c);
int  rpi_uart_getc(void);

/* Default to 512 MiB RAM starting at 0x00000000 */
static uintptr_t ram_base = 0x00000000UL;
static uintptr_t ram_size = 512UL << 20;
static char cmdline_buf[256];

static void on_node(const struct fdt_node *n, void *ctx) {
    if (n->devtype && !strcmp(n->devtype, "memory") && n->reg_len >= 16) {
        ram_base = fdt_be64(n->reg);
        ram_size = fdt_be64(n->reg + 8);
    }
    if (n->bootargs) strlcpy(cmdline_buf, n->bootargs, sizeof cmdline_buf);
}

void board_early_init(uintptr_t dtb) {
    /* Map BCM2837 peripheral space (0x3F000000) and Local INTC (0x40000000) */
    arch_map_device(0x3F000000UL, 0x01000000UL);
    arch_map_device(0x40000000UL, 0x00100000UL);

    struct fdt f;
    if (fdt_open(&f, dtb) == 0) {
        fdt_walk(&f, on_node, 0);
    }
    /* Initialize Mini UART as early as possible so early kprintf works over USB-TTL */
    rpi_uart_init();
}

void board_mem_range(uintptr_t *s, uintptr_t *e) {
    *s = (arch_kernel_end() + 0xFFFF) & ~0xFFFFUL;
    *e = ram_base + ram_size;
}

const char *board_cmdline(void) {
    return cmdline_buf;
}

void board_console_putc(int c) {
    if (c == '\n') rpi_uart_putc('\r');
    rpi_uart_putc(c);
}

int board_console_getc(void) {
    return rpi_uart_getc();
}

void board_init(void) {
    /* Route Core 0 CNTPNSIRQ (EL1 physical non-secure timer) to Core 0 IRQ.
     * Bit 1 = nCNTPNSIRQ IRQ control */
    *LOCAL_TIMER_INT_CONTROL = (1 << 1);

    armtimer_start(HZ);

    driver_note("bcm2837-timer", "ARM generic timer routed via BCM2837 local INTC, 100 Hz", DRV_MISC);
    kprintf("board: Raspberry Pi Zero 2W (BCM2837, RAM %lu MiB @ 0x%lx)\n",
            (unsigned long)(ram_size >> 20), (unsigned long)ram_base);
}

void board_irq(struct trapframe *tf) {
    uint32_t pend = *LOCAL_IRQ_PENDING0;
    if (pend & (1 << 1)) { /* Core 0 CNTPNSIRQ pending */
        armtimer_reload();
        timer_tick(TF_FROM_USER(tf));
    }
}

void board_poweroff(void) {
    /* Raspberry Pi Power Manager Watchdog reset */
    volatile uint32_t *pm_rstc = (volatile uint32_t *)P2V(0x3F10001CUL);
    volatile uint32_t *pm_wdog = (volatile uint32_t *)P2V(0x3F100024UL);
    *pm_wdog = 0x5A000001; /* Short timeout */
    *pm_rstc = 0x5A000102; /* Full reset */
    arch_halt();
}

int board_blk_init(void) {
    return -1; /* SD/MMC block driver can be added or boot from initramfs */
}

uint64_t board_blk_sectors(void) {
    return 0;
}

int board_blk_read(uint64_t lba, void *buf, uint32_t n) {
    return -1;
}
