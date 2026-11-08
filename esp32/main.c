/* Chiffon on ESP32: minimal boot.  Starts the UART console, reports memory, self-tests the heap
 * (kernel/mm.c, shared with the other architectures), then prints a heartbeat.  No interrupts,
 * processes or filesystem yet: those need the scheduler, the MMU or the register-window code, and
 * are listed as next steps in esp32/README.md. */
#include "kernel.h"

/* ---- memory-mapped hardware (ESP32 technical reference manual) ---- */
#define UART0_FIFO    ((volatile uint32_t *)0x3FF40000)     /* UART_FIFO_REG: write a byte to send */
#define UART0_STATUS  ((volatile uint32_t *)0x3FF4001C)     /* UART_STATUS_REG: TX FIFO count in bits 23:16 */
#define TIMG0_WDTWP   ((volatile uint32_t *)0x3FF5F064)     /* timer group 0 watchdog write protect */
#define TIMG0_WDTCFG0 ((volatile uint32_t *)0x3FF5F048)
#define TIMG1_WDTWP   ((volatile uint32_t *)0x3FF60064)
#define TIMG1_WDTCFG0 ((volatile uint32_t *)0x3FF60048)
#define RTC_WDTWP     ((volatile uint32_t *)0x3FF480A8)     /* RTC_CNTL watchdog write protect */
#define RTC_WDTCFG0   ((volatile uint32_t *)0x3FF4808C)
#define WDT_KEY       0x50D83AA1u

extern char _heap_start[], _heap_end[], _stack_top[];

/* ---- console: the UART is already configured by the ROM (115200 baud) ---- */
void console_putc(int c) {
    while (((*UART0_STATUS) >> 16 & 0xFF) >= 120) ;        /* the TX FIFO holds 128 bytes */
    *UART0_FIFO = (uint32_t)(uint8_t)c;
}
void console_flush(void) {
    while (((*UART0_STATUS) >> 16 & 0xFF) != 0) ;
}

void arch_halt(void) {
    for (;;) __asm__ volatile("waiti 0");
}
void arch_idle(void) { __asm__ volatile("waiti 0"); }

/* The ROM starts the watchdogs; they reset the chip unless fed.  This kernel does not feed them
 * yet, so turn them off. */
static void watchdogs_off(void) {
    *TIMG0_WDTWP = WDT_KEY; *TIMG0_WDTCFG0 = 0; *TIMG0_WDTWP = 0;
    *TIMG1_WDTWP = WDT_KEY; *TIMG1_WDTCFG0 = 0; *TIMG1_WDTWP = 0;
    *RTC_WDTWP = WDT_KEY;   *RTC_WDTCFG0 = 0;   *RTC_WDTWP = 0;
}

static uint32_t ccount(void) {
    uint32_t c;
    __asm__ volatile("rsr.ccount %0" : "=a"(c));
    return c;
}
static void spin_cycles(uint32_t n) {
    uint32_t t = ccount();
    while ((uint32_t)(ccount() - t) < n) ;
}

void kmain(void) {
    watchdogs_off();
    kprintf("\nChiffon %s (%s, %s)\n", VERSION, VERNAME, ARCH_NAME);
    kprintf("board: ESP32-D0WD-V3, 520 KiB SRAM, no PSRAM\n");

    size_t heap = (size_t)(_heap_end - _heap_start);
    kprintf("mem: %lu KiB kernel heap above the 8 KiB stack (dram0 0x3FFB0000..0x3FFDC200 holds data, bss, stack, heap)\n",
            (unsigned long)(heap >> 10));
    heap_init(_heap_start, heap);

    /* heap self-test: allocate, free into the middle, reallocate, check the bytes */
    unsigned char *a = kmalloc(1000), *b = kmalloc(2000), *c;
    int ok = a && b;
    if (ok) { memset(a, 0xA5, 1000); memset(b, 0x5A, 2000); }
    kfree(a);
    c = kmalloc(900);
    ok = ok && c && b[0] == 0x5A && b[1999] == 0x5A;
    if (c) memset(c, 0x3C, 900);
    ok = ok && b[1000] == 0x5A;
    kfree(b); kfree(c);
    kprintf("heap: self-test %s\n", ok ? "passed" : "FAILED");

    kprintf("boot complete; stack top %p, heap %lu KiB\n", (void *)_stack_top, (unsigned long)(heap >> 10));

    for (unsigned long beat = 0;; beat++) {
        kprintf("heartbeat %lu (ccount %u)\n", beat, (unsigned)ccount());
        spin_cycles(80u * 1000u * 1000u);                   /* roughly one second at the ROM-default 80 MHz */
    }
}
