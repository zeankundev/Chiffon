/* VT100/ANSI terminal emulator.  Keeps a cell grid in memory and paints only the cells that changed
 * since the last flush, so output bursts (boot log, `cat bigfile`) cost one repaint per timer tick
 * instead of one per character.  Display backends: kernel/fbcon.c, drivers/x86_64/vga_text.c. */
#include "kernel.h"

const uint32_t vt_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static const struct vt_display *disp;
static uint16_t cells[VT_MAXROWS * VT_MAXCOLS], shown[VT_MAXROWS * VT_MAXCOLS];
static int cols = 80, rows = 25;
static int cx, cy, sx, sy, top, bot;
static int fg = 7, bg = 0, bold, rev, wrap_pending;
static int st, params[8], np, priv, utf_need, utf_cp;
static int cursor_on = 1, graphics, dirty, force_all, last_phase = -1;

static uint16_t attr_byte(void) {
    int f = fg | (bold && fg < 8 ? 8 : 0), b = bg;
    if (rev) { int t = f & 7; f = b; b = t; }
    return (uint16_t)((b << 4) | f) << 8;
}
static uint16_t blank(void) { return attr_byte() | ' '; }
#define CELL(x, y) cells[(y) * cols + (x)]

static void fill(int x0, int y0, int x1, int y1) {          /* inclusive-exclusive rectangle of blanks */
    uint16_t b = blank();
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) CELL(x, y) = b;
    dirty = 1;
}
static void scroll_up(int n) {                              /* within [top, bot] */
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(&CELL(0, top), &CELL(0, top + n), (size_t)(bot - top + 1 - n) * cols * 2);
    fill(0, bot - n + 1, cols, bot + 1);
}
static void scroll_down(int n) {
    if (n > bot - top + 1) n = bot - top + 1;
    memmove(&CELL(0, top + n), &CELL(0, top), (size_t)(bot - top + 1 - n) * cols * 2);
    fill(0, top, cols, top + n);
}
static void lf(void) { if (cy == bot) scroll_up(1); else if (cy < rows - 1) cy++; }
static void ri(void) { if (cy == top) scroll_down(1); else if (cy > 0) cy--; }

static void put_glyph(int ch) {
    if (wrap_pending) { cx = 0; lf(); wrap_pending = 0; }
    CELL(cx, cy) = attr_byte() | (uint8_t)ch;
    dirty = 1;
    if (cx == cols - 1) wrap_pending = 1; else cx++;
}

static void sgr(void) {
    if (!np) { params[np++] = 0; }
    for (int i = 0; i < np; i++) {
        int p = params[i];
        if (p == 0) { fg = 7; bg = 0; bold = rev = 0; }
        else if (p == 1) bold = 1;
        else if (p == 22) bold = 0;
        else if (p == 7) rev = 1;
        else if (p == 27) rev = 0;
        else if (p >= 30 && p <= 37) fg = ansi2vga[p - 30];
        else if (p == 39) fg = 7;
        else if (p >= 40 && p <= 47) bg = ansi2vga[p - 40];
        else if (p == 49) bg = 0;
        else if (p >= 90 && p <= 97) fg = ansi2vga[p - 90] | 8;
        else if (p >= 100 && p <= 107) bg = ansi2vga[p - 100] | 8;
    }
}
static int arg(int i, int def) { return i < np && params[i] > 0 ? params[i] : def; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void csi(int c) {
    int n = arg(0, 1);
    wrap_pending = 0;
    switch (c) {
    case 'A': cy = clampi(cy - n, top > cy ? 0 : top, rows - 1); break;
    case 'B': cy = clampi(cy + n, 0, bot < cy ? rows - 1 : bot); break;
    case 'C': cx = clampi(cx + n, 0, cols - 1); break;
    case 'D': cx = clampi(cx - n, 0, cols - 1); break;
    case 'E': cx = 0; cy = clampi(cy + n, 0, rows - 1); break;
    case 'F': cx = 0; cy = clampi(cy - n, 0, rows - 1); break;
    case 'G': cx = clampi(n - 1, 0, cols - 1); break;
    case 'd': cy = clampi(n - 1, 0, rows - 1); break;
    case 'H': case 'f': cy = clampi(arg(0, 1) - 1, 0, rows - 1); cx = clampi(arg(1, 1) - 1, 0, cols - 1); break;
    case 'J': {
        int m = np ? params[0] : 0;
        if (m == 0) { fill(cx, cy, cols, cy + 1); fill(0, cy + 1, cols, rows); }
        else if (m == 1) { fill(0, 0, cols, cy); fill(0, cy, cx + 1, cy + 1); }
        else fill(0, 0, cols, rows);
        break; }
    case 'K': {
        int m = np ? params[0] : 0;
        if (m == 0) fill(cx, cy, cols, cy + 1);
        else if (m == 1) fill(0, cy, cx + 1, cy + 1);
        else fill(0, cy, cols, cy + 1);
        break; }
    case 'L': if (cy >= top && cy <= bot) { int t = top; top = cy; scroll_down(n); top = t; } break;
    case 'M': if (cy >= top && cy <= bot) { int t = top; top = cy; scroll_up(n); top = t; } break;
    case 'P': { int k = clampi(n, 1, cols - cx);
        memmove(&CELL(cx, cy), &CELL(cx + k, cy), (size_t)(cols - cx - k) * 2); fill(cols - k, cy, cols, cy + 1); break; }
    case '@': { int k = clampi(n, 1, cols - cx);
        memmove(&CELL(cx + k, cy), &CELL(cx, cy), (size_t)(cols - cx - k) * 2); fill(cx, cy, cx + k, cy + 1); break; }
    case 'X': fill(cx, cy, clampi(cx + n, 0, cols), cy + 1); break;
    case 'm': sgr(); break;
    case 's': sx = cx; sy = cy; break;
    case 'u': cx = sx; cy = sy; break;
    case 'r': {
        int t = arg(0, 1) - 1, b = arg(1, rows) - 1;
        if (t >= 0 && b < rows && t < b) { top = t; bot = b; cx = 0; cy = 0; }
        break; }
    case 'h': case 'l': if (priv && np && params[0] == 25) cursor_on = (c == 'h'); break;
    default: break;                                       /* unsupported: ignore */
    }
    dirty = 1;
}

static void reset(void) {
    fg = 7; bg = 0; bold = rev = 0; cx = cy = sx = sy = 0; top = 0; bot = rows - 1; wrap_pending = 0; cursor_on = 1;
    fill(0, 0, cols, rows);
}

void vt_putc(int c) {
    c &= 0xFF;
    switch (st) {
    case 1:                                                /* after ESC */
        st = 0;
        if (c == '[') { st = 2; np = 0; priv = 0; memset(params, 0, sizeof params); }
        else if (c == '(' || c == ')') st = 3;
        else if (c == 'c') reset();
        else if (c == '7') { sx = cx; sy = cy; }
        else if (c == '8') { cx = sx; cy = sy; }
        else if (c == 'M') ri();
        else if (c == 'D') lf();
        else if (c == 'E') { cx = 0; lf(); }
        dirty = 1;
        return;
    case 2:                                                /* CSI parameters */
        if (c >= '0' && c <= '9') { if (!np) np = 1; params[np - 1] = params[np - 1] * 10 + (c - '0'); if (params[np - 1] > 9999) params[np - 1] = 9999; return; }
        if (c == ';') { if (!np) np = 1; if (np < 8) np++; return; }
        if (c == '?') { priv = 1; return; }
        if (c >= 0x20 && c <= 0x2F) return;                /* intermediates: ignore */
        st = 0;
        if (c >= 0x40 && c <= 0x7E) csi(c);
        return;
    case 3: st = 0; return;                                /* charset designation: swallow */
    }
    if (c >= 0x80) {                                       /* UTF-8 -> Latin-1 subset, '?' otherwise */
        if (c >= 0xC0) { utf_need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; utf_cp = c & (0x3F >> utf_need); }
        else if (utf_need && --utf_need == 0) { utf_cp = (utf_cp << 6) | (c & 0x3F); put_glyph(utf_cp <= 0xFF && utf_cp >= 0xA0 ? utf_cp : '?'); }
        else if (utf_need) utf_cp = (utf_cp << 6) | (c & 0x3F);
        return;
    }
    utf_need = 0;
    switch (c) {
    case 0x1B: st = 1; return;
    case '\n': case 0x0B: case 0x0C: wrap_pending = 0; lf(); dirty = 1; return;
    case '\r': cx = 0; wrap_pending = 0; dirty = 1; return;
    case '\b': if (cx > 0) cx--; wrap_pending = 0; dirty = 1; return;
    case '\t': { int nx = (cx + 8) & ~7; cx = nx >= cols ? cols - 1 : nx; wrap_pending = 0; dirty = 1; return; }
    }
    if (c >= 0x20 && c < 0x7F) put_glyph(c);
}

void vt_flush(void) {
    if (!disp || graphics) return;
    int phase = cursor_on ? (int)((ticks / (HZ / 2)) & 1) : 2;
    if (!dirty && !force_all && phase == last_phase) return;
    last_phase = phase; dirty = 0;
    int cur_i = cy * cols + cx;
    for (int i = 0; i < rows * cols; i++) {
        uint16_t want = cells[i];
        if (i == cur_i && phase == 0) want = (uint16_t)(((want & 0x0F00) << 4) | ((want & 0xF000) >> 4) | (want & 0xFF));
        if (!force_all && want == shown[i]) continue;
        disp->draw(i % cols, i / cols, want);
        shown[i] = want;
    }
    force_all = 0;
}

int vt_attach(const struct vt_display *d) {
    if (disp) return -EEXIST;
    cols = d->cols > VT_MAXCOLS ? VT_MAXCOLS : d->cols;
    rows = d->rows > VT_MAXROWS ? VT_MAXROWS : d->rows;
    if (cols < 20 || rows < 5) return -EINVAL;
    disp = d;
    reset();
    force_all = 1;
    return 0;
}
int vt_size(int *c, int *r) { if (!disp) return 0; *c = cols; *r = rows; return 1; }
int vt_graphics(void) { return graphics; }
void vt_set_graphics(int on) {
    on = !!on;
    if (on == graphics || !disp) return;
    graphics = on;
    if (!on) { if (disp->redraw_all) disp->redraw_all(); force_all = 1; dirty = 1; }
}
void vt_clear(void) { cx = cy = 0; wrap_pending = 0; fill(0, 0, cols, rows); }
