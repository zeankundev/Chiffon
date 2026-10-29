/* Keyboard translation shared by all keyboard drivers (PS/2, virtio-input, ...).
 * Input is a scancode-set-1 style code (the numbering Linux keycodes 1..88 share) plus a release flag
 * and an "extended" (E0) flag; output is what a terminal would send, fed to the tty line discipline:
 * Enter = CR (ICRNL turns it into NL), Backspace = DEL, arrows/Home/End/F-keys = VT100/xterm sequences. */
#include "kernel.h"

static const char km_norm[] = "\0\033" "1234567890-=\177\t" "qwertyuiop[]\r\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char km_shft[] = "\0\033" "!@#$%^&*()_+\177\t" "QWERTYUIOP{}\r\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";
static const char *const fkeys[12] = { "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~", "\033[18~", "\033[19~",
                                      "\033[20~", "\033[21~", "\033[23~", "\033[24~" };

static int shift, ctrl, alt, caps, numlock = 1;

static void emit(const char *s) { while (*s) tty_input((unsigned char)*s++); }

void kbd_scancode(uint8_t sc, int release, int e0) {
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

