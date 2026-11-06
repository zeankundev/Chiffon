/* Text terminal emulator (VT100/ANSI subset) that renders into any "display" backend: the
 * framebuffer console (kernel/fbcon.c) or the VGA text buffer (drivers/x86_64/vga_text.c).
 * A cell is 16 bits, laid out exactly like a VGA text cell: low byte = Latin-1 character,
 * high byte = (background << 4) | foreground, colours in the 16-entry VGA palette.
 *
 * There are VT_SCREENS independent screens (each with its own cursor, attributes and contents)
 * but only one display: the active screen is the one painted; the others keep updating in memory
 * and are repainted when they become active (vt_switch). */
#pragma once
#include <stdint.h>

#define VT_MAXCOLS 256
#define VT_MAXROWS 100
#define VT_SCREENS 4                                 /* screen index 0..3 = /dev/tty1..tty4 */

struct vt_display {
    const char *name;
    int cols, rows;
    void (*draw)(int x, int y, uint16_t cell);   /* paint one cell */
    void (*redraw_all)(void);                    /* optional: repaint background after graphics mode */
};
int  vt_attach(const struct vt_display *d);
void vt_write(int screen, int c);                /* feed one byte to a screen's emulator */
void vt_flush(void);                             /* paint the active screen's changed cells; timer tick */
int  vt_size(int *cols, int *rows);              /* 0 when no display is attached */
void vt_set_graphics(int on);                    /* 1: stop painting so a program can own the display */
int  vt_graphics(void);
void vt_clear(void);                             /* clear the active screen */
int  vt_active(void);                            /* index of the screen on the display */
void vt_switch(int screen);                      /* show another screen (Alt+F1..F4) */
extern const uint32_t vt_palette[16];            /* 0xRRGGBB */
