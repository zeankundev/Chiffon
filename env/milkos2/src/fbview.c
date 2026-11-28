/* fbview - framebuffer image viewer for Chiffon.
 *
 *   fbview [-l N] FILE...
 *
 * Formats: PNG, JPEG (baseline and progressive), GIF (animated).
 * Needs a 32bpp framebuffer, e.g. `make run-gfx`.
 *
 * Input is direct: the keyboard of the active screen (/dev/tty0) is put in CHI_KBDMODE_DIRECT,
 * so every key acts as soon as it is pressed (no Enter). Ctrl-C does not quit: use q or Esc.
 *   n, Enter   next image (Enter on the last image quits)     p   previous image
 *   f          fit: shrink to the screen, never enlarge (default)
 *   e          enlarge-or-shrink to fit the screen (smooth scaling)
 *   a, 1       actual size: one image pixel per screen pixel (pixel-perfect)
 *   i          integer fit: largest whole-number zoom that fits (pixel-perfect)
 *   2 .. 9     fixed whole-number zoom, nearest-neighbour (pixel-perfect)
 *   h j k l    pan by a quarter screen (arrow keys too) when the image is bigger than the screen
 *   space      pause or resume animation
 *   q, Esc     quit
 * -l N        loop an animated GIF N times, then hold the last frame (0 = forever, default 3)
 */
#include "libchiffon.h"
#include "codecs.h"

#define FIONREAD 0x541B
#define MAX_FILES 256

enum { A_NONE, A_REDRAW, A_NEXT, A_PREV, A_TOGGLE, A_QUIT };

/* mode: 'f' fit (shrink only), 'e' fit (both ways), 'a' actual size, 'i' integer fit, 'z' fixed zoom */
struct view { int mode, zoom, px, py; };

static unsigned *fbp;             /* the framebuffer */
static int SW, SH, stride;        /* screen size; stride in pixels */
static unsigned *back;            /* off-screen frame, copied to fbp once per frame */
static int con = -1;              /* /dev/console, for KD_TEXT / KD_GRAPHICS */
static int in_text = 1;           /* console is in text mode (set graphics before the next blit) */
static int kbd = -1;              /* /dev/tty0: the active screen, whose keyboard arrives in direct mode */
static struct chi_input_event evbuf[16];
static int evn, evi;              /* key records read from kbd, not yet handled */

static void say(const char *s) {
    if (con >= 0) {
        chiffon_ioctl(con, CHI_KDSETMODE, (void *)(long)CHI_KD_TEXT);
        in_text = 1;
    }
    chiffon_puts(s);
}

static void graphics(void) {
    if (con >= 0 && in_text) {
        chiffon_ioctl(con, CHI_KDSETMODE, (void *)(long)CHI_KD_GRAPHICS);
        in_text = 0;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* display size of the whole image for the current mode */
static void view_size(int iw, int ih, const struct view *v, int *dw, int *dh) {
    if (v->mode == 'a') {
        *dw = iw; *dh = ih;
    } else if (v->mode == 'z') {
        *dw = iw * v->zoom; *dh = ih * v->zoom;
    } else if (v->mode == 'i') {
        int n = 1;
        while ((n + 1) * iw <= SW && (n + 1) * ih <= SH) n++;
        *dw = n * iw; *dh = n * ih;
    } else if (v->mode == 'f' && iw <= SW && ih <= SH) {
        *dw = iw; *dh = ih;
    } else if ((long)iw * SH > (long)ih * SW) {          /* wider than the screen: fit the width */
        *dw = SW; *dh = (int)((long)ih * SW / iw);
    } else {                                             /* taller than the screen: fit the height */
        *dh = SH; *dw = (int)((long)iw * SH / ih);
    }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

/*
 * Renders src (iw x ih) into the back buffer. Each screen pixel averages the block of
 * source pixels it covers. For whole-number zoom and 1:1 that block is exactly one
 * source pixel (or an exact n x n block), so those modes stay pixel-perfect. Shrinking
 * averages, which keeps detail from aliasing.
 */
static void render(const unsigned *src, int iw, int ih, const struct view *v) {
    int dw, dh;
    view_size(iw, ih, v, &dw, &dh);
    int vw = dw < SW ? dw : SW, vh = dh < SH ? dh : SH;
    int px = dw > SW ? clampi(v->px, 0, dw - SW) : 0;
    int py = dh > SH ? clampi(v->py, 0, dh - SH) : 0;
    int ox = (SW - vw) / 2, oy = (SH - vh) / 2;

    fill32(back, BG, (unsigned long)SW * SH);
    for (int y = 0; y < vh; y++) {
        int sy0 = (y + py) * ih / dh, sy1 = (y + py + 1) * ih / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ih) sy1 = ih;
        unsigned *row = back + (unsigned long)(oy + y) * SW + ox;
        for (int x = 0; x < vw; x++) {
            int sx0 = (x + px) * iw / dw, sx1 = (x + px + 1) * iw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > iw) sx1 = iw;
            unsigned long r = 0, g = 0, b = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned *s = src + (unsigned long)sy * iw;
                for (int sx = sx0; sx < sx1; sx++) {
                    unsigned c = s[sx];
                    r += c >> 16 & 255;
                    g += c >> 8 & 255;
                    b += c & 255;
                    cnt++;
                }
            }
            row[x] = (unsigned)(r / cnt) << 16 | (unsigned)(g / cnt) << 8 | (unsigned)(b / cnt);
        }
    }
}

static void present(const struct anim *a, int frame, const struct view *v) {
    graphics();
    render(a->frame[frame], a->w, a->h, v);
    for (int y = 0; y < SH; y++)
        copy32(fbp + (unsigned long)y * stride, back + (unsigned long)y * SW, SW);
}

/* ---- keyboard: direct mode on the active screen -------------------------------------------- */

/* the active screen is in direct mode and at least one key record is waiting (never blocks) */
static int key_pending(void) {
    int m = -1, n = 0;
    if (chiffon_ioctl(kbd, CHI_KBDGETMODE, &m) || m != CHI_KBDMODE_DIRECT) return 0;
    return chiffon_ioctl(kbd, FIONREAD, &n) == 0 && n >= (int)sizeof evbuf[0];
}

/* a key record is already buffered, or waiting in the kernel */
static int input_ready(void) { return evi < evn || key_pending(); }

/* key code (scancode set 1, 0x100 = E0 prefix) -> the command character it means, 0 = none */
static char key_cmd(unsigned code) {
    switch (code) {
    case 0x1C: case 0x11C: return '\n';                  /* Enter, keypad Enter: next image */
    case 0x01: case 0x10:  return 'q';                   /* Esc, q */
    case 0x19: return 'p';
    case 0x31: return 'n';
    case 0x39: return ' ';
    case 0x12: return 'e';
    case 0x17: return 'i';
    case 0x1E: return 'a';
    case 0x21: return 'f';
    case 0x23: return 'h';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x14B: return 'h';                              /* arrows pan */
    case 0x148: return 'k';
    case 0x150: return 'j';
    case 0x14D: return 'l';
    }
    if (code >= 0x02 && code <= 0x0A) return (char)('1' + (code - 0x02));   /* keys 1..9 */
    return 0;
}

/* the next key press as a command character; 0 when no key is waiting */
static char next_command(void) {
    for (;;) {
        if (evi >= evn) {
            if (!key_pending()) return 0;
            long n = chiffon_read(kbd, evbuf, sizeof evbuf);     /* key_pending() said records are waiting */
            if (n < (long)sizeof evbuf[0]) return 0;
            evn = (int)(n / (long)sizeof evbuf[0]);
            evi = 0;
        }
        const struct chi_input_event *e = &evbuf[evi++];
        if (e->type == CHI_EV_KEY && e->value) {         /* presses only; releases are ignored */
            char c = key_cmd(e->code);
            if (c) return c;
        }
    }
}

/* takes the keyboard of the active screen. The mode ends by itself when this program exits. */
static int kbd_open(void) {
    kbd = chiffon_open("/dev/tty0", O_RDWR, 0);
    if (kbd < 0) return -1;
    if (chiffon_ioctl(kbd, CHI_KBDSETMODE, (void *)(long)CHI_KBDMODE_DIRECT)) return -1;
    int m = -1;
    return chiffon_ioctl(kbd, CHI_KBDGETMODE, &m) == 0 && m == CHI_KBDMODE_DIRECT ? 0 : -1;
}

/* ---- commands ------------------------------------------------------------------------------ */

static int run_command(const char *s, struct view *v) {
    if (s[0] >= '2' && s[0] <= '9') {
        v->mode = 'z';
        v->zoom = s[0] - '0';
    } else {
        switch (s[0]) {
        case 0:
        case '\n':
        case 'n':  return A_NEXT;
        case 'p':  return A_PREV;
        case 'q':  return A_QUIT;
        case ' ':  return A_TOGGLE;
        case 'f': case 'e': case 'a': case 'i':
            v->mode = s[0];
            break;
        case '1':
            v->mode = 'a';
            break;
        case 'h': v->px -= SW / 4; return A_REDRAW;
        case 'l': v->px += SW / 4; return A_REDRAW;
        case 'k': v->py -= SH / 4; return A_REDRAW;
        case 'j': v->py += SH / 4; return A_REDRAW;
        default:  return A_NONE;
        }
    }
    v->px = v->py = 0;
    return A_REDRAW;
}

/* frees the previous image and decodes the new one; returns an error string or 0 */
static const char *load_into(const char *path, struct anim **out) {
    free_all();
    unsigned long n = 0;
    unsigned char *d = read_file(path, &n);
    if (!d) return "cannot read file";
    struct anim *a = alloc(sizeof *a);
    if (!a) return "out of memory";
    const char *err = decode_image(d, n, a);
    if (err) return err;
    *out = a;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    const char *files[MAX_FILES];
    int nf = 0, loops = 3;

    con = chiffon_open("/dev/console", O_RDWR, 0);
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (s[0] == '-' && s[1] == 'l' && s[2] == 0 && i + 1 < argc) {
            const char *q = argv[++i];
            loops = 0;
            while (*q >= '0' && *q <= '9') loops = loops * 10 + (*q++ - '0');
        } else if (nf < MAX_FILES) {
            files[nf++] = s;
        }
    }
    if (!nf) {
        say("usage: fbview [-l N] FILE...\n");
        return 1;
    }

    int fd = chiffon_open("/dev/fb0", O_RDWR, 0);
    if (fd < 0) {
        say("fbview: no /dev/fb0 (is there a display? try `make run-gfx`)\n");
        return 1;
    }
    struct chi_fb_var_screeninfo vi;
    struct chi_fb_fix_screeninfo fi;
    if (chiffon_ioctl(fd, CHI_FBIOGET_VSCREENINFO, &vi) || chiffon_ioctl(fd, CHI_FBIOGET_FSCREENINFO, &fi)) {
        say("fbview: ioctl on /dev/fb0 failed\n");
        return 1;
    }
    if (vi.bits_per_pixel != 32) {
        say("fbview: needs a 32bpp framebuffer\n");
        return 1;
    }
    SW = (int)vi.xres;
    SH = (int)vi.yres;
    stride = (int)(fi.line_length / 4);

    long m = chiffon_mmap(0, (size_t)fi.line_length * vi.yres_virtual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    long bk = chiffon_mmap(0, (size_t)SW * SH * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((m < 0 && m > -4096) || (bk < 0 && bk > -4096)) {
        say("fbview: mmap failed\n");
        return 1;
    }
    fbp = (unsigned *)m;
    back = (unsigned *)bk;

    if (kbd_open()) {
        say("fbview: cannot take the keyboard: run it on a screen (make run-gfx, then type in the QEMU window)\n");
        return 1;
    }

    say("fbview: n/Enter next, p prev, f fit, e enlarge-fit, a or 1 actual size, i integer fit,\n"
        "        2-9 zoom, hjkl or arrows pan, space pause, q or Esc quit. Keys act at once, no Enter.\n");
    chiffon_sleep_ms(1500);

    int idx = 0, frame = 0, loops_done = 0, need = 1, playing = 1, running = 1;
    struct view v = { 'f', 2, 0, 0 };
    struct anim *a = 0;

    while (running) {
        if (need) {
            need = 0;
            const char *err = load_into(files[idx], &a);
            if (err) {
                say("fbview: ");
                say(files[idx]);
                say(": ");
                say(err);
                say("\n");
                chiffon_sleep_ms(1500);
                if (idx + 1 < nf) { idx++; need = 1; continue; }
                break;
            }
            frame = 0;
            loops_done = 0;
            playing = a->n > 1;
            v.px = v.py = 0;
        }

        present(a, frame, &v);

        if (playing) {                                  /* animation: the frame delay is the wait for a key */
            int d = a->delay[frame], waited = 0;
            while (waited < d && !input_ready()) {
                chiffon_sleep_ms(10);
                waited += 10;
            }
            if (!input_ready()) {                       /* delay over, no key: next frame */
                if (++frame >= a->n) {
                    frame = 0;
                    if (loops && ++loops_done >= loops) playing = 0;
                }
                continue;
            }
        } else {
            while (!input_ready()) chiffon_sleep_ms(10);
        }

        char c = next_command();
        if (!c) continue;
        char cmd[2] = { c, 0 };
        switch (run_command(cmd, &v)) {
        case A_QUIT:
            running = 0;
            break;
        case A_NEXT:
            if (idx + 1 < nf) { idx++; need = 1; }
            else running = 0;
            break;
        case A_PREV:
            if (idx > 0) { idx--; need = 1; }
            break;
        case A_TOGGLE:
            if (a->n > 1) { playing = !playing; loops_done = 0; }
            break;
        default:
            break;
        }
    }

    if (kbd >= 0) chiffon_close(kbd);
    if (con >= 0) chiffon_ioctl(con, CHI_KDSETMODE, (void *)(long)CHI_KD_TEXT);
    chiffon_munmap(fbp, (size_t)fi.line_length * vi.yres_virtual);
    chiffon_munmap(back, (size_t)SW * SH * 4);
    free_all();
    return 0;
}
