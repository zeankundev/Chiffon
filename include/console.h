/* Console fan-out: kernel messages go to the board's serial port and to every registered console.
 * Terminal devices use their own paths: a serial tty writes to the serial port and to the consoles
 * that are not screens (console_serial_putc); a screen tty writes only to its own vt screen.
 * A small boot log is kept so that a console that registers late (after the display driver came
 * up) still shows the early boot messages. */
#pragma once
struct console {
    const char *name;
    void (*write)(int c);
    void (*flush)(void);                         /* optional, called from the timer tick and on panic */
    struct console *next;
    int screen;                                  /* 1: a screen (vt) console; it gets kernel messages but not serial tty output */
};
void console_putc(int c);                        /* kernel messages: serial + every console */
void console_serial_putc(int c);                 /* serial tty output: serial + non-screen consoles */
void console_register(struct console *c);
void console_flush(void);
