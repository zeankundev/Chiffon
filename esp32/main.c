/* Chiffon on ESP32: minimal boot. Starts the UART console, reports memory, self-tests the heap
 * (kernel/mm.c, shared with the other architectures), and launches the init shell.
 * On panic, an illegal instruction exception is triggered to crash. */
#include "kernel.h"

/* ---- memory-mapped hardware (ESP32 technical reference manual) ---- */
#define UART0_FIFO    ((volatile uint32_t *)0x3FF40000)     /* UART_FIFO_REG: write a byte to send */
#define UART0_STATUS  ((volatile uint32_t *)0x3FF4001C)     /* UART_STATUS_REG: TX FIFO count [23:16], RX FIFO count [7:0] */
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

int console_getc(void) {
    if (((*UART0_STATUS) & 0xFF) == 0) return -1;
    return (int)(*UART0_FIFO & 0xFF);
}

void arch_halt(void) {
    console_flush();
    __asm__ volatile("ill");
    for (;;) ;
}

void arch_idle(void) {
    __asm__ volatile("waiti 0");
}

static void system_poweroff(void) {
    kprintf("[chiffon] system powering off\n");
    console_flush();
    for (;;) __asm__ volatile("waiti 0");
}

/* The ROM starts the watchdogs; they reset the chip unless fed. This kernel does not feed them
 * yet, so turn them off. */
static void watchdogs_off(void) {
    *TIMG0_WDTWP = WDT_KEY; *TIMG0_WDTCFG0 = 0; *TIMG0_WDTWP = 0;
    *TIMG1_WDTWP = WDT_KEY; *TIMG1_WDTCFG0 = 0; *TIMG1_WDTWP = 0;
    *RTC_WDTWP = WDT_KEY;   *RTC_WDTCFG0 = 0;   *RTC_WDTWP = 0;
}

static int s_last_was_cr = 0;

static int console_getchar(void) {
    for (;;) {
        while (((*UART0_STATUS) & 0xFF) == 0) ;
        int c = (int)(*UART0_FIFO & 0xFF);
        if (c == '\n' && s_last_was_cr) {
            s_last_was_cr = 0;
            continue;
        }
        if (c == '\r') {
            s_last_was_cr = 1;
            return '\n';
        }
        s_last_was_cr = 0;
        return c;
    }
}

static int readline(char *buf, size_t maxlen) {
    size_t len = 0;
    for (;;) {
        int c = console_getchar();
        if (c == '\n') {
            console_putc('\r');
            console_putc('\n');
            buf[len] = 0;
            return (int)len;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                console_putc('\b');
                console_putc(' ');
                console_putc('\b');
            }
            continue;
        }
        if (c == 0x03) { /* Ctrl+C */
            console_putc('^');
            console_putc('C');
            console_putc('\r');
            console_putc('\n');
            buf[0] = 0;
            return -1;
        }
        if (c == 0x04) { /* Ctrl+D */
            if (len == 0) {
                console_putc('\r');
                console_putc('\n');
                return -2; /* EOF */
            }
            continue;
        }
        if (c >= 32 && c <= 126) {
            if (len + 1 < maxlen) {
                buf[len++] = (char)c;
                console_putc(c);
            }
        }
    }
}

static void init_shell(void) {
    char line[256];
    kprintf("init: pid 1 is /sbin/init\n\n");
    kprintf("Welcome to your simple Chiffon system! Type 'help' for a list of commands.\n");

    for (;;) {
        kprintf("hello-init# ");
        console_flush();

        int n = readline(line, sizeof line);
        if (n == -2) break; /* EOF (^D) */
        if (n <= 0) continue;

        char *av[16];
        int ac = 0;
        for (char *p = line; *p && ac < 15;) {
            while (*p == ' ' || *p == '\t' || *p == '\n') *p++ = 0;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        }
        av[ac] = 0;
        if (!ac) continue;

        if (!strcmp(av[0], "help")) {
            kprintf("builtins: help echo pwd cd ls cat env uname poweroff exit panic\n"
                    "userland: drvloader fbserver ipcdemo fbdemo\n"
                    "anything else is looked up in $PATH\n");
        } else if (!strcmp(av[0], "echo")) {
            for (int i = 1; i < ac; i++) {
                kprintf("%s%s", av[i], (i + 1 < ac) ? " " : "");
            }
            kprintf("\n");
        } else if (!strcmp(av[0], "pwd")) {
            kprintf("/\n");
        } else if (!strcmp(av[0], "cd")) {
            if (ac > 1 && strcmp(av[1], "/") != 0)
                kprintf("cd: failed\n");
        } else if (!strcmp(av[0], "ls")) {
            kprintf("ls: cannot open %s\n", ac > 1 ? av[1] : ".");
        } else if (!strcmp(av[0], "cat")) {
            if (ac > 1)
                kprintf("cat: cannot open %s\n", av[1]);
        } else if (!strcmp(av[0], "env")) {
            kprintf("PATH=/bin:/sbin:/usr/bin\nHOME=/\nTERM=vt100\n");
        } else if (!strcmp(av[0], "uname")) {
            kprintf("Chiffon %s %s %s\n", VERSION, VERNAME, ARCH_NAME);
        } else if (!strcmp(av[0], "poweroff")) {
            system_poweroff();
        } else if (!strcmp(av[0], "exit")) {
            break;
        } else if (!strcmp(av[0], "panic")) {
            panic("manual panic from init");
        } else if (!strcmp(av[0], "crash")) {
            panic("manual crash from init");
        } else {
            kprintf("%s: command not found\n", av[0]);
        }
    }

    kprintf("[init] exited with status 0\n");
    system_poweroff();
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
    if (!ok) panic("heap self-test failed");
    kprintf("heap: self-test passed\n");

    kprintf("boot complete; stack top %p, heap %lu KiB\n", (void *)_stack_top, (unsigned long)(heap >> 10));

    init_shell();
}
