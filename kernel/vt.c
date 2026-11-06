/* VT100/ANSI terminal emulator.  Each screen keeps a cell grid in memory and only the active screen
 * is painted, and only the cells that changed since the last flush, so output bursts (boot log,
 * `cat bigfile`) cost one repaint per timer tick instead of one per character.
 * Display backends: kernel/fbcon.c, drivers/x86_64/vga_text.c. */
#include "kernel.h"

const uint32_t vt_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

struct vt {
    uint16_t *cells, *shown;                     /* cols * rows; shown = what the display has */
    int cx, cy, sx, sy, top, bot;
    int fg, bg, bold, rev, wrap_pending;
    int st, params[8], np, priv, utf_need, utf_cp;
    int cursor_on, dirty, force_all, last_phase;
};

static const struct vt_display *disp;
static struct vt screens[VT_SCREENS];
static int cols = 80, rows = 25;
static int active;                               /* screen shown on the display */
static int graphics;                             /* display owned by a program: do not paint */

#define CELL(v, x, y) ((v)->cells[(y) * cols + (x)])

static uint16_t attr_byte(struct vt *v) {
    int f = v->fg | (v->bold && v->fg < 8 ? 8 : 0), b = v->bg;
    if (v->rev) { int t = f & 7; f = b; b = t; }
    return (uint16_t)((b << 4) | f) << 8;
}
static uint16_t blank(struct vt *v) { return attr_byte(v) | ' '; }

static void fill(struct vt *v, int x0, int y0, int x1, int y1) {   /* inclusive-exclusive rectangle */
    uint16_t b = blank(v);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) CELL(v, x, y) = b;
    v->dirty = 1;
}
static void scroll_up(struct vt *v, int n) {                       /* within [top, bot] */
    if (n > v->bot - v->top + 1) n = v->bot - v->top + 1;
    memmove(&CELL(v, 0, v->top), &CELL(v, 0, v->top + n), (size_t)(v->bot - v->top + 1 - n) * cols * 2);
    fill(v, 0, v->bot - n + 1, cols, v->bot + 1);
}
static void scroll_down(struct vt *v, int n) {
    if (n > v->bot - v->top + 1) n = v->bot - v->top + 1;
    memmove(&CELL(v, 0, v->top + n), &CELL(v, 0, v->top), (size_t)(v->bot - v->top + 1 - n) * cols * 2);
    fill(v, 0, v->top, cols, v->top + n);
}
static void lf(struct vt *v) { if (v->cy == v->bot) scroll_up(v, 1); else if (v->cy < rows - 1) v->cy++; }
static void ri(struct vt *v) { if (v->cy == v->top) scroll_down(v, 1); else if (v->cy > 0) v->cy--; }

static void put_glyph(struct vt *v, int ch) {
    if (v->wrap_pending) { v->cx = 0; lf(v); v->wrap_pending = 0; }
    CELL(v, v->cx, v->cy) = attr_byte(v) | (uint8_t)ch;
    v->dirty = 1;
    if (v->cx == cols - 1) v->wrap_pending = 1; else v->cx++;
}

static void sgr(struct vt *v) {
    if (!v->np) v->params[v->np++] = 0;
    for (int i = 0; i < v->np; i++) {
        int p = v->params[i];
        if (p == 0) { v->fg = 7; v->bg = 0; v->bold = v->rev = 0; }
        else if (p == 1) v->bold = 1;
        else if (p == 22) v->bold = 0;
        else if (p == 7) v->rev = 1;
        else if (p == 27) v->rev = 0;
        else if (p >= 30 && p <= 37) v->fg = ansi2vga[p - 30];
        else if (p == 39) v->fg = 7;
        else if (p >= 40 && p <= 47) v->bg = ansi2vga[p - 40];
        else if (p == 49) v->bg = 0;
        else if (p >= 90 && p <= 97) v->fg = ansi2vga[p - 90] | 8;
        else if (p >= 100 && p <= 107) v->bg = ansi2vga[p - 100] | 8;
    }
}
static int arg(struct vt *v, int i, int def) { return i < v->np && v->params[i] > 0 ? v->params[i] : def; }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void csi(struct vt *v, int c) {
    int n = arg(v, 0, 1);
    v->wrap_pending = 0;
    switch (c) {
    case 'A': v->cy = clampi(v->cy - n, v->top > v->cy ? 0 : v->top, rows - 1); break;
    case 'B': v->cy = clampi(v->cy + n, 0, v->bot < v->cy ? rows - 1 : v->bot); break;
    case 'C': v->cx = clampi(v->cx + n, 0, cols - 1); break;
    case 'D': v->cx = clampi(v->cx - n, 0, cols - 1); break;
    case 'E': v->cx = 0; v->cy = clampi(v->cy + n, 0, rows - 1); break;
    case 'F': v->cx = 0; v->cy = clampi(v->cy - n, 0, rows - 1); break;
    case 'G': v->cx = clampi(n - 1, 0, cols - 1); break;
    case 'd': v->cy = clampi(n - 1, 0, rows - 1); break;
    case 'H': case 'f': v->cy = clampi(arg(v, 0, 1) - 1, 0, rows - 1); v->cx = clampi(arg(v, 1, 1) - 1, 0, cols - 1); break;
    case 'J': {
        int m = v->np ? v->params[0] : 0;
        if (m == 0) { fill(v, v->cx, v->cy, cols, v->cy + 1); fill(v, 0, v->cy + 1, cols, rows); }
        else if (m == 1) { fill(v, 0, 0, cols, v->cy); fill(v, 0, v->cy, v->cx + 1, v->cy + 1); }
        else fill(v, 0, 0, cols, rows);
        break; }
    case 'K': {
        int m = v->np ? v->params[0] : 0;
        if (m == 0) fill(v, v->cx, v->cy, cols, v->cy + 1);
        else if (m == 1) fill(v, 0, v->cy, v->cx + 1, v->cy + 1);
        else fill(v, 0, v->cy, cols, v->cy + 1);
        break; }
    case 'L': if (v->cy >= v->top && v->cy <= v->bot) { int t = v->top; v->top = v->cy; scroll_down(v, n); v->top = t; } break;
    case 'M': if (v->cy >= v->top && v->cy <= v->bot) { int t = v->top; v->top = v->cy; scroll_up(v, n); v->top = t; } break;
    case 'P': { int k = clampi(n, 1, cols - v->cx);
        memmove(&CELL(v, v->cx, v->cy), &CELL(v, v->cx + k, v->cy), (size_t)(cols - v->cx - k) * 2);
        fill(v, cols - k, v->cy, cols, v->cy + 1); break; }
    case '@': { int k = clampi(n, 1, cols - v->cx);
        memmove(&CELL(v, v->cx + k, v->cy), &CELL(v, v->cx, v->cy), (size_t)(cols - v->cx - k) * 2);
        fill(v, v->cx, v->cy, v->cx + k, v->cy + 1); break; }
    case 'X': fill(v, v->cx, v->cy, clampi(v->cx + n, 0, cols), v->cy + 1); break;
    case 'm': sgr(v); break;
    case 's': v->sx = v->cx; v->sy = v->cy; break;
    case 'u': v->cx = v->sx; v->cy = v->sy; break;
    case 'r': {
        int t = arg(v, 0, 1) - 1, b = arg(v, 1, rows) - 1;
        if (t >= 0 && b < rows && t < b) { v->top = t; v->bot = b; v->cx = 0; v->cy = 0; }
        break; }
    case 'h': case 'l': if (v->priv && v->np && v->params[0] == 25) v->cursor_on = (c == 'h'); break;
    default: break;                                       /* unsupported: ignore */
    }
    v->dirty = 1;
}

static void reset(struct vt *v) {
    v->fg = 7; v->bg = 0; v->bold = v->rev = 0; v->cx = v->cy = v->sx = v->sy = 0;
    v->top = 0; v->bot = rows - 1; v->wrap_pending = 0; v->cursor_on = 1;
    fill(v, 0, 0, cols, rows);
}

static void putc_one(struct vt *v, int c) {
    c &= 0xFF;
    switch (v->st) {
    case 1:                                                /* after ESC */
        v->st = 0;
        if (c == '[') { v->st = 2; v->np = 0; v->priv = 0; memset(v->params, 0, sizeof v->params); }
        else if (c == '(' || c == ')') v->st = 3;
        else if (c == 'c') reset(v);
        else if (c == '7') { v->sx = v->cx; v->sy = v->cy; }
        else if (c == '8') { v->cx = v->sx; v->cy = v->sy; }
        else if (c == 'M') ri(v);
        else if (c == 'D') lf(v);
        else if (c == 'E') { v->cx = 0; lf(v); }
        v->dirty = 1;
        return;
    case 2:                                                /* CSI parameters */
        if (c >= '0' && c <= '9') { if (!v->np) v->np = 1; v->params[v->np - 1] = v->params[v->np - 1] * 10 + (c - '0'); if (v->params[v->np - 1] > 9999) v->params[v->np - 1] = 9999; return; }
        if (c == ';') { if (!v->np) v->np = 1; if (v->np < 8) v->np++; return; }
        if (c == '?') { v->priv = 1; return; }
        if (c >= 0x20 && c <= 0x2F) return;                /* intermediates: ignore */
        v->st = 0;
        if (c >= 0x40 && c <= 0x7E) csi(v, c);
        return;
    case 3: v->st = 0; return;                             /* charset designation: swallow */
    }
    if (c >= 0x80) {                                       /* UTF-8 -> Latin-1 subset, '?' otherwise */
        if (c >= 0xC0) { v->utf_need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; v->utf_cp = c & (0x3F >> v->utf_need); }
        else if (v->utf_need && --v->utf_need == 0) { v->utf_cp = (v->utf_cp << 6) | (c & 0x3F); put_glyph(v, v->utf_cp <= 0xFF && v->utf_cp >= 0xA0 ? v->utf_cp : '?'); }
        else if (v->utf_need) v->utf_cp = (v->utf_cp << 6) | (c & 0x3F);
        return;
    }
    v->utf_need = 0;
    switch (c) {
    case 0x1B: v->st = 1; return;
    case '\n': case 0x0B: case 0x0C: v->wrap_pending = 0; lf(v); v->dirty = 1; return;
    case '\r': v->cx = 0; v->wrap_pending = 0; v->dirty = 1; return;
    case '\b': if (v->cx > 0) v->cx--; v->wrap_pending = 0; v->dirty = 1; return;
    case '\t': { int nx = (v->cx + 8) & ~7; v->cx = nx >= cols ? cols - 1 : nx; v->wrap_pending = 0; v->dirty = 1; return; }
    }
    if (c >= 0x20 && c < 0x7F) put_glyph(v, c);
}

void vt_write(int screen, int c) {
    if (!disp || screen < 0 || screen >= VT_SCREENS) return;   /* no display: output is dropped */
    putc_one(&screens[screen], c);
}

void vt_flush(void) {
    if (!disp || graphics) return;
    struct vt *v = &screens[active];
    int phase = v->cursor_on ? (int)((ticks / (HZ / 2)) & 1) : 2;
    if (!v->dirty && !v->force_all && phase == v->last_phase) return;
    v->last_phase = phase; v->dirty = 0;
    int cur_i = v->cy * cols + v->cx;
    for (int i = 0; i < rows * cols; i++) {
        uint16_t want = v->cells[i];
        if (i == cur_i && phase == 0) want = (uint16_t)(((want & 0x0F00) << 4) | ((want & 0xF000) >> 4) | (want & 0xFF));
        if (!v->force_all && want == v->shown[i]) continue;
        disp->draw(i % cols, i / cols, want);
        v->shown[i] = want;
    }
    v->force_all = 0;
}

int vt_attach(const struct vt_display *d) {
    if (disp) return -EEXIST;
    cols = d->cols > VT_MAXCOLS ? VT_MAXCOLS : d->cols;
    rows = d->rows > VT_MAXROWS ? VT_MAXROWS : d->rows;
    if (cols < 20 || rows < 5) return -EINVAL;
    size_t bytes = (size_t)cols * rows * sizeof(uint16_t);
    for (int i = 0; i < VT_SCREENS; i++) {
        struct vt *v = &screens[i];
        v->cells = kmalloc(bytes);
        v->shown = kmalloc(bytes);
        if (!v->cells || !v->shown) return -ENOMEM;
        v->last_phase = -1;
        v->top = 0; v->bot = rows - 1;
        reset(v);
        v->force_all = 1;
    }
    disp = d;
    active = 0;
    return 0;
}
int vt_size(int *c, int *r) { if (!disp) return 0; *c = cols; *r = rows; return 1; }
int vt_graphics(void) { return graphics; }
int vt_active(void) { return active; }

void vt_set_graphics(int on) {
    on = !!on;
    if (on == graphics || !disp) return;
    graphics = on;
    if (!on) {
        if (disp->redraw_all) disp->redraw_all();
        screens[active].force_all = 1;
        screens[active].dirty = 1;
    }
}

void vt_switch(int screen) {
    if (!disp || screen < 0 || screen >= VT_SCREENS || screen == active) return;
    active = screen;
    screens[active].force_all = 1;
    screens[active].dirty = 1;
}

void vt_clear(void) {
    struct vt *v = &screens[active];
    v->cx = v->cy = 0; v->wrap_pending = 0;
    if (disp) fill(v, 0, 0, cols, rows);
}
