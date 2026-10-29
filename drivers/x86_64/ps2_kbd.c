/* PS/2 keyboard (i8042), scancode set 1 (the controller translates for us), US layout.  Keys become
 * the bytes a terminal would send, so the tty line discipline treats them exactly like serial input:
 * Enter = CR (ICRNL turns it into NL), Backspace = DEL, arrows/Home/End/F-keys = VT100/xterm sequences. */
#include "kernel.h"
#include "drivers/x86io.h"

#define KBD_DATA 0x60
#define KBD_STAT 0x64

static const char km_norm[] = "\0\033" "1234567890-=\177\t" "qwertyuiop[]\r\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char km_shft[] = "\0\033" "!@#$%^&*()_+\177\t" "QWERTYUIOP{}\r\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";
static const char *const fkeys[12] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~", "\033[18~", "\033[19~",
                                      "\033[20~", "\033[21~", "\033[23~", "\033[24~" };

static int shift, ctrl, alt, caps, numlock = 1, ext;

static void emit(const char *s) { while (*s) tty_input((unsigned char)*s++); }

static void key(uint8_t sc, int release, int e0) {
    switch (sc) {                                           /* modifiers */
    case 0x2A: case 0x36: if (!e0) { shift = !release; return; } break;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    case 0x45: if (!release && !e0) numlock = !numlock; return;
    }
    if (release) return;

    const char *seq = 0;
    if (e0 || (sc >= 0x47 && sc <= 0x53 && !numlock)) {     /* navigation block (or keypad with NumLock off) */
        switch (sc) {
        case 0x48: seq = "\033[A"; break;  case 0x50: seq = "\033[B"; break;
        case 0x4D: seq = "\033[C"; break;  case 0x4B: seq = "\033[D"; break;
        case 0x47: seq = "\033[H"; break;  case 0x4F: seq = "\033[F"; break;
        case 0x49: seq = "\033[5~"; break; case 0x51: seq = "\033[6~"; break;
        case 0x52: seq = "\033[2~"; break; case 0x53: seq = "\033[3~"; break;
        case 0x1C: tty_input('\r'); return;                 /* keypad Enter */
        case 0x35: tty_input('/'); return;                  /* keypad / */
        }
        if (seq) emit(seq);
        return;
    }
    if (sc >= 0x3B && sc <= 0x44) { emit(fkeys[sc - 0x3B]); return; }
    if (sc == 0x57 || sc == 0x58) { emit(fkeys[10 + sc - 0x57]); return; }

    char c = 0;
    if (sc >= 0x47 && sc <= 0x53) {                         /* keypad with NumLock on */
        static const char kp[] = "789-456+1230.";
        c = kp[sc - 0x47];
    } else if (sc < sizeof km_norm - 1) {
        c = shift ? km_shft[sc] : km_norm[sc];
        if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c ^= 0x20;
    }
    if (!c) return;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') c -= 'a' - 1;
        else if (c >= 'A' && c <= 'Z') c -= 'A' - 1;
        else if (c == '[') c = 0x1B; else if (c == '\\') c = 0x1C; else if (c == ']') c = 0x1D;
        else if (c == ' ' || c == '2') c = 0;
    }
    if (alt) tty_input(0x1B);                               /* Alt = Meta prefix */
    tty_input((unsigned char)c);
}

static void drain(void) {
    for (int n = 0; n < 64; n++) {
        uint8_t st = inb(KBD_STAT);
        if (!(st & 1)) return;
        uint8_t sc = inb(KBD_DATA);
        if (st & 0x20) continue;                            /* mouse byte: not ours */
        if (sc == 0xE0) { ext = 1; continue; }
        if (sc == 0xE1 || sc == 0xFA || sc == 0xFE || sc == 0xAA) { ext = 0; continue; }       /* pause / ack / resend / self-test */
        key(sc & 0x7F, sc & 0x80, ext);
        ext = 0;
    }
}
static void irq(unsigned n) { drain(); }

static void kbc_wait(void) { for (int i = 0; i < 100000 && (inb(KBD_STAT) & 2); i++) { } }

static int probe(void) { return inb(KBD_STAT) == 0xFF ? -1 : 0; }      /* floating bus: no controller */
static int init(void) {
    for (int i = 0; i < 16 && (inb(KBD_STAT) & 1); i++) inb(KBD_DATA);  /* flush */
    kbc_wait(); outb(KBD_STAT, 0xAE);                                   /* enable first port */
    kbc_wait(); outb(KBD_STAT, 0x20);                                   /* read config byte */
    for (int i = 0; i < 100000 && !(inb(KBD_STAT) & 1); i++) { }
    uint8_t cfg = inb(KBD_DATA);
    cfg |= 0x01 | 0x40;                                                 /* IRQ1 on, set-1 translation */
    cfg &= ~0x10;                                                       /* clock enabled */
    kbc_wait(); outb(KBD_STAT, 0x60);
    kbc_wait(); outb(KBD_DATA, cfg);
    kbc_wait(); outb(KBD_DATA, 0xF4);                                   /* keyboard: enable scanning */
    if (irq_register(1, irq)) return -1;
    outb(0x21, inb(0x21) & ~0x02);                                      /* unmask IRQ1 on the master PIC */
    return 0;
}
static struct kdriver drv = { "ps2-kbd", "i8042 PS/2 keyboard, set 1, US layout", DRV_INPUT, 10, probe, init, drain, 0 };
KDRIVER(drv);
