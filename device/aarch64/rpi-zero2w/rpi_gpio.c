/* Broadcom BCM2837 GPIO & Mini UART driver for Chiffon */
#include "kernel.h"
#include "drivers/armtimer.h"

#define BCM2837_PERIPH_BASE 0x3F000000UL
#define GPIO_BASE           (BCM2837_PERIPH_BASE + 0x200000)
#define AUX_BASE            (BCM2837_PERIPH_BASE + 0x215000)

/* GPIO Registers */
#define GPFSEL0   ((volatile uint32_t *)P2V(GPIO_BASE + 0x00))
#define GPFSEL1   ((volatile uint32_t *)P2V(GPIO_BASE + 0x04))
#define GPFSEL2   ((volatile uint32_t *)P2V(GPIO_BASE + 0x08))
#define GPSET0    ((volatile uint32_t *)P2V(GPIO_BASE + 0x1C))
#define GPCLR0    ((volatile uint32_t *)P2V(GPIO_BASE + 0x28))
#define GPLEV0    ((volatile uint32_t *)P2V(GPIO_BASE + 0x34))
#define GPPUD     ((volatile uint32_t *)P2V(GPIO_BASE + 0x94))
#define GPPUDCLK0 ((volatile uint32_t *)P2V(GPIO_BASE + 0x98))

/* AUX / Mini UART Registers */
#define AUX_ENABLES     ((volatile uint32_t *)P2V(AUX_BASE + 0x04))
#define AUX_MU_IO_REG   ((volatile uint32_t *)P2V(AUX_BASE + 0x40))
#define AUX_MU_IER_REG  ((volatile uint32_t *)P2V(AUX_BASE + 0x44))
#define AUX_MU_IIR_REG  ((volatile uint32_t *)P2V(AUX_BASE + 0x48))
#define AUX_MU_LCR_REG  ((volatile uint32_t *)P2V(AUX_BASE + 0x4C))
#define AUX_MU_MCR_REG  ((volatile uint32_t *)P2V(AUX_BASE + 0x50))
#define AUX_MU_LSR_REG  ((volatile uint32_t *)P2V(AUX_BASE + 0x54))
#define AUX_MU_CNTL_REG ((volatile uint32_t *)P2V(AUX_BASE + 0x60))
#define AUX_MU_BAUD_REG ((volatile uint32_t *)P2V(AUX_BASE + 0x68))

static inline void delay_cycles(int32_t count) {
    __asm__ volatile("1: subs %w0, %w0, #1; b.ne 1b" : "+r"(count) :: "cc");
}

void rpi_gpio_fsel(unsigned pin, unsigned func) {
    if (pin > 53) return;
    volatile uint32_t *reg = (volatile uint32_t *)P2V(GPIO_BASE + (pin / 10) * 4);
    unsigned shift = (pin % 10) * 3;
    uint32_t val = *reg;
    val &= ~(7U << shift);
    val |= ((func & 7U) << shift);
    *reg = val;
}

void rpi_gpio_set(unsigned pin, int high) {
    if (pin > 31) return;
    if (high) *GPSET0 = (1U << pin);
    else      *GPCLR0 = (1U << pin);
}

int rpi_gpio_get(unsigned pin) {
    if (pin > 31) return 0;
    return (*GPLEV0 & (1U << pin)) ? 1 : 0;
}

void rpi_gpio_pud(unsigned pin, unsigned pud) {
    *GPPUD = pud;
    delay_cycles(150);
    *GPPUDCLK0 = (1U << pin);
    delay_cycles(150);
    *GPPUD = 0;
    *GPPUDCLK0 = 0;
}

void rpi_uart_init(void) {
    *AUX_ENABLES |= 1;      /* Enable Mini UART */
    *AUX_MU_CNTL_REG = 0;   /* Disable transmitter & receiver during configuration */
    *AUX_MU_IER_REG = 0;    /* Disable UART interrupts */
    *AUX_MU_LCR_REG = 3;    /* 8-bit mode */
    *AUX_MU_MCR_REG = 0;    /* RTS high */
    *AUX_MU_IIR_REG = 0xC6; /* Clear FIFO buffers (0x06) and enable FIFOs */
    /* Baud rate formula: baud = sys_freq / (8 * (baud_reg + 1))
     * With 250 MHz core clock: baud_reg = (250,000,000 / (8 * 115200)) - 1 = 270 */
    *AUX_MU_BAUD_REG = 270;

    /* Route GPIO14 (TXD1) and GPIO15 (RXD1) to Alt5 (Mini UART) */
    rpi_gpio_fsel(14, 2); /* Alt5 = 010 (2) */
    rpi_gpio_fsel(15, 2); /* Alt5 = 010 (2) */

    rpi_gpio_pud(14, 0);   /* Disable pull-up/down on TXD1 */
    rpi_gpio_pud(15, 0);   /* Disable pull-up/down on RXD1 */

    *AUX_MU_CNTL_REG = 3;  /* Enable transmitter and receiver */
}

void rpi_uart_putc(int c) {
    while (!(*AUX_MU_LSR_REG & 0x20)) { }
    *AUX_MU_IO_REG = (uint32_t)(c & 0xFF);
}

int rpi_uart_getc(void) {
    if (*AUX_MU_LSR_REG & 0x01) {
        return (int)(*AUX_MU_IO_REG & 0xFF);
    }
    return -1;
}

static int rpi_gpio_driver_init(void) {
    /* Blink / pulse OK / ACT LED (GPIO 29 on Pi Zero 2W is active-low) */
    rpi_gpio_fsel(29, 1); /* Output */
    rpi_gpio_set(29, 0);  /* Turn on ACT LED */
    driver_note("bcm2837-gpio", "BCM2837 GPIO Controller (40-pin header + ACT LED)", DRV_MISC);
    driver_note("bcm2837-miniuart", "BCM2837 AUX Mini UART (115200 baud)", DRV_CHAR);
    return 0;
}

static struct kdriver rpi_gpio_driver = {
    .name = "rpi_gpio",
    .desc = "Raspberry Pi GPIO Driver",
    .class = DRV_MISC,
    .prio = 10,
    .init = rpi_gpio_driver_init,
};
KDRIVER(rpi_gpio_driver);
