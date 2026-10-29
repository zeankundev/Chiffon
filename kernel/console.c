/* Console fan-out + boot log.  See include/console.h. */
#include "kernel.h"

#define KLOG_SIZE 16384
static char klog[KLOG_SIZE];
static size_t klog_len;                      /* total bytes ever written (ring: last KLOG_SIZE kept) */
static struct console *consoles;

void console_putc(int c) {
    klog[klog_len++ % KLOG_SIZE] = (char)c;
    board_console_putc(c);                   /* the board's serial port is always console #0 */
    for (struct console *cn = consoles; cn; cn = cn->next) cn->write(c);
}

void console_register(struct console *c) {
    c->next = 0;
    struct console **pp = &consoles;
    while (*pp) pp = &(*pp)->next;
    *pp = c;
    size_t n = klog_len < KLOG_SIZE ? klog_len : KLOG_SIZE, start = klog_len - n;    /* replay the boot log */
    for (size_t i = 0; i < n; i++) c->write(klog[(start + i) % KLOG_SIZE]);
}

void console_flush(void) {
    for (struct console *cn = consoles; cn; cn = cn->next) if (cn->flush) cn->flush();
}
