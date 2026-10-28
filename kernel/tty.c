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

struct ktermios { uint32_t iflag, oflag, cflag, lflag; uint8_t line; uint8_t cc[19]; };
static struct ktermios tio;
#define INQ 1024
static char inq[INQ];
static size_t ihead, itail;
static char line[256];
static size_t llen;
static int eof_flag;

void tty_init(void) {
    memset(&tio, 0, sizeof tio);
    tio.iflag = ICRNL;
    tio.oflag = OPOST | ONLCR;
    tio.cflag = 0xBF;                   /* B38400 | CS8 | CREAD | HUPCL */
    tio.lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | 0x800 | 0x8000;
    tio.cc[VINTR] = 3; tio.cc[VQUIT] = 0x1c; tio.cc[VERASE] = 0x7f;
    tio.cc[VKILL] = 0x15; tio.cc[VEOF] = 4; tio.cc[VMIN] = 1;
}

static void out(int c) {
    if (c == '\n' && (tio.oflag & OPOST) && (tio.oflag & ONLCR)) board_console_putc('\r');
    board_console_putc(c);
}
static void echo_char(int c) {
    if (!(tio.lflag & ECHO)) return;
    if ((tio.lflag & ECHOCTL) && c < 32 && c != '\n' && c != '\t') { out('^'); out(c + 64); }
    else out(c);
}
static void erase_echo(void) {
    if (tio.lflag & ECHO) { out('\b'); out(' '); out('\b'); }
}
static void commit(void) {
    for (size_t i = 0; i < llen; i++)
        if (ihead - itail < INQ) inq[ihead++ % INQ] = line[i];
    llen = 0;
    wakeup(inq);
}
static void signal_fg(int sig) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED && p->state != P_ZOMBIE && p->pid != init_pid) proc_signal(p, sig);
    }
}

void tty_input(int c) {
    if (c == '\r' && (tio.iflag & ICRNL)) c = '\n';
    else if (c == '\r' && (tio.iflag & IGNCR)) return;
    else if (c == '\n' && (tio.iflag & INLCR)) c = '\r';

    if (tio.lflag & ISIG) {
        if (c == tio.cc[VINTR]) { echo_char(c); out('\n'); llen = 0; signal_fg(2); return; }
        if (c == tio.cc[VQUIT]) { echo_char(c); out('\n'); llen = 0; signal_fg(3); return; }
    }
    if (tio.lflag & ICANON) {
        if (c == tio.cc[VERASE] || c == 8) { if (llen) { llen--; erase_echo(); } return; }
        if (c == tio.cc[VKILL]) { while (llen) { llen--; erase_echo(); } return; }
        if (c == tio.cc[VEOF]) { if (!llen) eof_flag = 1; commit(); wakeup(inq); return; }
        if (c == '\n') {
            if (llen < sizeof line) line[llen++] = '\n';
            if (tio.lflag & (ECHO | ECHONL)) out('\n');
            commit(); return;
        }
        if (llen < sizeof line - 1) { line[llen++] = c; echo_char(c); }
    } else {
        if (ihead - itail < INQ) inq[ihead++ % INQ] = c;
        echo_char(c);
        wakeup(inq);
    }
}

void console_poll(void) {
    int c;
    while ((c = board_console_getc()) >= 0) tty_input(c);
}

int tty_readable(void) { return ihead != itail || ((tio.lflag & ICANON) && eof_flag); }

ssize_t tty_read(void *buf, size_t n, int nonblock) {
    char *b = buf;
    uint64_t deadline = 0; int timed = 0;
    if (!n) return 0;
    for (;;) {
        if (cur->killed) return -EINTR;
        int canon = tio.lflag & ICANON;
        if (ihead != itail) {
            size_t got = 0;
            while (got < n && ihead != itail) {
                char c = inq[itail++ % INQ];
                b[got++] = c;
                if (canon && c == '\n') break;
            }
            return got;
        }
        if (canon && eof_flag) { eof_flag = 0; return 0; }
        if (!canon && tio.cc[VMIN] == 0) {
            if (!tio.cc[VTIME]) return 0;
            if (!timed) { timed = 1; deadline = ticks + tio.cc[VTIME] * (HZ / 10); }
            if (ticks >= deadline) return 0;
            sleep_on((void *)&ticks);
            continue;
        }
        if (nonblock) return -EAGAIN;
        sleep_on(inq);
    }
}

ssize_t tty_write(const void *buf, size_t n) {
    const char *b = buf;
    for (size_t i = 0; i < n; i++) out(b[i]);
    return n;
}

long tty_ioctl(unsigned long cmd, uintptr_t arg) {
    switch (cmd) {
    case 0x5401:                                   /* TCGETS */
        if (!uok(arg, sizeof tio)) return -EFAULT;
        memcpy((void *)arg, &tio, sizeof tio); return 0;
    case 0x5402: case 0x5403: case 0x5404:         /* TCSETS{,W,F} */
        if (!uok(arg, sizeof tio)) return -EFAULT;
        memcpy(&tio, (void *)arg, sizeof tio);
        if (cmd == 0x5404) { itail = ihead; llen = 0; }
        return 0;
    case 0x5413: {                                 /* TIOCGWINSZ */
        if (!uok(arg, 8)) return -EFAULT;
        uint16_t *w = (uint16_t *)arg; w[0] = 24; w[1] = 80; w[2] = w[3] = 0; return 0; }
    case 0x5414: return 0;                         /* TIOCSWINSZ */
    case 0x541B:                                   /* FIONREAD */
        if (!uok(arg, 4)) return -EFAULT;
        *(int *)arg = (int)(ihead - itail); return 0;
    default: return -ENOTTY;                       /* no job control / sessions */
    }
}
