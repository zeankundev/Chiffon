/* Terminal layer, one struct tty per device (multisession):
 *   minor 0            /dev/ttyS0 (also /dev/console): the board's serial port
 *   minor 1..VT_SCREENS /dev/tty1..tty4: one screen each (framebuffer or VGA text)
 * Every tty has its own termios, input queue, line buffer and session leader.
 * Keyboard input goes to the active screen's tty; Alt+F1..F4 switches the active screen.
 * Ctrl-C / Ctrl-\ signal the processes whose controlling tty is this tty, except its session leader
 * (the shell that owns the terminal stays alive). */
#include "kernel.h"

#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8
#define ECHOE 0x10
#define ECHOK 0x20
#define ECHONL 0x40
#define ECHOCTL 0x200
#define ICRNL 0x100
#define INLCR 0x40
#define IGNCR 0x80
#define OPOST 1
#define ONLCR 4
enum { VINTR, VQUIT, VERASE, VKILL, VEOF, VTIME, VMIN };

#define TIOCSCTTY 0x540E
#define TIOCNOTTY 0x5422

struct ktermios { uint32_t iflag, oflag, cflag, lflag; uint8_t line; uint8_t cc[19]; };

#define INQ 1024
#define LINEMAX 256
#define NTTY (1 + VT_SCREENS)

struct tty {
    struct ktermios tio;
    int minor, leader;                     /* leader: pid of the session leader (never signalled by ^C) */
    char inq[INQ];
    size_t ihead, itail;
    char line[LINEMAX];
    size_t llen;
    int eof_flag;
};
static struct tty ttys[NTTY];

void tty_init(void) {
    for (int m = 0; m < NTTY; m++) {
        struct tty *t = &ttys[m];
        memset(t, 0, sizeof *t);
        t->minor = m;
        t->tio.iflag = ICRNL;
        t->tio.oflag = OPOST | ONLCR;
        t->tio.cflag = 0xBF;                /* B38400 | CS8 | CREAD | HUPCL */
        t->tio.lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | 0x800 | 0x8000;
        t->tio.cc[VINTR] = 3; t->tio.cc[VQUIT] = 0x1c; t->tio.cc[VERASE] = 0x7f;
        t->tio.cc[VKILL] = 0x15; t->tio.cc[VEOF] = 4; t->tio.cc[VMIN] = 1;
    }
}

int tty_resolve(int minor) {
    if (minor == TTY_CTTY_MINOR) return cur && cur->ctty ? cur->ctty - 1 : -ENXIO;
    if (minor == TTY_ACTIVE_MINOR) return vt_active() + 1;
    return minor >= 0 && minor < NTTY ? minor : -ENODEV;
}

void tty_set_session(int minor, int pid) { if (minor >= 0 && minor < NTTY) ttys[minor].leader = pid; }

static void dev_putc(struct tty *t, int c) {
    if (t->minor == TTY_SERIAL_MINOR) console_serial_putc(c);
    else vt_write(t->minor - 1, c);
}
static void out(struct tty *t, int c) {
    if (c == '\n' && (t->tio.oflag & OPOST) && (t->tio.oflag & ONLCR)) dev_putc(t, '\r');
    dev_putc(t, c);
}
static void echo_char(struct tty *t, int c) {
    if (!(t->tio.lflag & ECHO)) return;
    if ((t->tio.lflag & ECHOCTL) && c < 32 && c != '\n' && c != '\t') { out(t, '^'); out(t, c + 64); }
    else out(t, c);
}
static void erase_echo(struct tty *t) {
    if (t->tio.lflag & ECHO) { out(t, '\b'); out(t, ' '); out(t, '\b'); }
}
static void commit(struct tty *t) {
    for (size_t i = 0; i < t->llen; i++)
        if (t->ihead - t->itail < INQ) t->inq[t->ihead++ % INQ] = t->line[i];
    t->llen = 0;
    wakeup(t);
}
static void signal_fg(struct tty *t, int sig) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED && p->state != P_ZOMBIE && p->ctty == t->minor + 1 && p->pid != t->leader)
            proc_signal(p, sig);
    }
}

void tty_rx(int minor, int c) {
    if (minor < 0 || minor >= NTTY) return;
    struct tty *t = &ttys[minor];
    struct ktermios *tio = &t->tio;
    if (c == '\r' && (tio->iflag & ICRNL)) c = '\n';
    else if (c == '\r' && (tio->iflag & IGNCR)) return;
    else if (c == '\n' && (tio->iflag & INLCR)) c = '\r';

    if (tio->lflag & ISIG) {
        if (c == tio->cc[VINTR]) { echo_char(t, c); out(t, '\n'); t->llen = 0; signal_fg(t, 2); return; }
        if (c == tio->cc[VQUIT]) { echo_char(t, c); out(t, '\n'); t->llen = 0; signal_fg(t, 3); return; }
    }
    if (tio->lflag & ICANON) {
        if (c == tio->cc[VERASE] || c == 8) { if (t->llen) { t->llen--; erase_echo(t); } return; }
        if (c == tio->cc[VKILL]) { while (t->llen) { t->llen--; erase_echo(t); } return; }
        if (c == tio->cc[VEOF]) { if (!t->llen) t->eof_flag = 1; commit(t); wakeup(t); return; }
        if (c == '\n') {
            if (t->llen < sizeof t->line) t->line[t->llen++] = '\n';
            if (tio->lflag & (ECHO | ECHONL)) out(t, '\n');
            commit(t); return;
        }
        if (t->llen < sizeof t->line - 1) { t->line[t->llen++] = c; echo_char(t, c); }
    } else {
        if (t->ihead - t->itail < INQ) t->inq[t->ihead++ % INQ] = c;
        echo_char(t, c);
        wakeup(t);
    }
}

void tty_input(int c) { tty_rx(vt_active() + 1, c); }

void console_poll(void) {
    int c;
    while ((c = board_console_getc()) >= 0) tty_rx(TTY_SERIAL_MINOR, c);
    drivers_poll();                                   /* input drivers that also poll (PS/2 keyboard) */
    console_flush();                                  /* paint what changed on the active screen */
}

int tty_readable(int minor) {
    if (minor < 0 || minor >= NTTY) return 1;
    struct tty *t = &ttys[minor];
    return t->ihead != t->itail || ((t->tio.lflag & ICANON) && t->eof_flag);
}

ssize_t tty_read(int minor, void *buf, size_t n, int nonblock) {
    struct tty *t = &ttys[minor];
    char *b = buf;
    uint64_t deadline = 0; int timed = 0;
    if (!n) return 0;
    for (;;) {
        if (cur->killed) return -EINTR;
        int canon = t->tio.lflag & ICANON;
        if (t->ihead != t->itail) {
            size_t got = 0;
            while (got < n && t->ihead != t->itail) {
                char c = t->inq[t->itail++ % INQ];
                b[got++] = c;
                if (canon && c == '\n') break;
            }
            return got;
        }
        if (canon && t->eof_flag) { t->eof_flag = 0; return 0; }
        if (!canon && t->tio.cc[VMIN] == 0) {
            if (!t->tio.cc[VTIME]) return 0;
            if (!timed) { timed = 1; deadline = ticks + t->tio.cc[VTIME] * (HZ / 10); }
            if (ticks >= deadline) return 0;
            sleep_on((void *)&ticks);
            continue;
        }
        if (nonblock) return -EAGAIN;
        sleep_on(t);
    }
}

ssize_t tty_write(int minor, const void *buf, size_t n) {
    struct tty *t = &ttys[minor];
    const char *b = buf;
    for (size_t i = 0; i < n; i++) out(t, b[i]);
    return n;
}

long tty_ioctl(int minor, unsigned long cmd, uintptr_t arg) {
    struct tty *t = &ttys[minor];
    switch (cmd) {
    case 0x5401:                                   /* TCGETS */
        if (!uok(arg, sizeof t->tio)) return -EFAULT;
        memcpy((void *)arg, &t->tio, sizeof t->tio); return 0;
    case 0x5402: case 0x5403: case 0x5404:         /* TCSETS{,W,F} */
        if (!uok(arg, sizeof t->tio)) return -EFAULT;
        memcpy(&t->tio, (void *)arg, sizeof t->tio);
        if (cmd == 0x5404) { t->itail = t->ihead; t->llen = 0; }
        return 0;
    case 0x5413: {                                 /* TIOCGWINSZ */
        if (!uok(arg, 8)) return -EFAULT;
        uint16_t *w = (uint16_t *)arg; int c = 80, r = 24; vt_size(&c, &r);
        w[0] = r; w[1] = c; w[2] = w[3] = 0; return 0; }
    case 0x5414: return 0;                         /* TIOCSWINSZ */
    case 0x541B:                                   /* FIONREAD */
        if (!uok(arg, 4)) return -EFAULT;
        *(int *)arg = (int)(t->ihead - t->itail); return 0;
    case TIOCSCTTY:                                /* this tty becomes the caller's controlling terminal */
        cur->ctty = minor + 1; t->leader = cur->pid; return 0;
    case TIOCNOTTY:
        if (cur->ctty == minor + 1) cur->ctty = 0;
        return 0;
    case CHI_KDSETMODE:                            /* a graphics program takes over the display */
        if (arg != CHI_KD_TEXT && arg != CHI_KD_GRAPHICS) return -EINVAL;
        vt_set_graphics(arg == CHI_KD_GRAPHICS); return 0;
    case CHI_KDGETMODE:
        if (!uok(arg, 4)) return -EFAULT;
        *(int *)arg = vt_graphics(); return 0;
    default: return -ENOTTY;                       /* no job control (TIOCGPGRP/TIOCSPGRP) yet */
    }
}
