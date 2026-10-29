/* Console fan-out: everything the kernel or the tty writes goes to the board's serial port and to
 * every registered console (framebuffer / VGA text).  A small boot log is kept so that a console
 * that registers late (after the display driver came up) still shows the early boot messages. */
#pragma once
struct console {
    const char *name;
    void (*write)(int c);
    void (*flush)(void);                         /* optional, called from the timer tick and on panic */
    struct console *next;
};
void console_putc(int c);
void console_register(struct console *c);
void console_flush(void);
