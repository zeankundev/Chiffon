/* Text terminal emulator (VT100/ANSI subset) that renders into any "display" backend: the
 * framebuffer console (kernel/fbcon.c) or the VGA text buffer (drivers/x86_64/vga_text.c).
 * A cell is 16 bits, laid out exactly like a VGA text cell: low byte = Latin-1 character,
 * high byte = (background << 4) | foreground, colours in the 16-entry VGA palette. */
#pragma once
#include <stdint.h>

#define VT_MAXCOLS 256
#define VT_MAXROWS 100

struct vt_display {
    const char *name;
    int cols, rows;
    void (*draw)(int x, int y, uint16_t cell);   /* paint one cell */
    void (*redraw_all)(void);                    /* optional: repaint background after graphics mode */
};
int  vt_attach(const struct vt_display *d);
void vt_putc(int c);
void vt_flush(void);                             /* paint changed cells; called from the timer tick */
int  vt_size(int *cols, int *rows);              /* 0 when no display is attached */
void vt_set_graphics(int on);                    /* 1: stop painting so a program can own the screen */
int  vt_graphics(void);
void vt_clear(void);
extern const uint32_t vt_palette[16];            /* 0xRRGGBB */
