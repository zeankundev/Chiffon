/* Framebuffer core: drivers register a linear framebuffer, the core binds the text console to the
 * primary one and exports it as /dev/fb0 (read/write/mmap/ioctl) and as the "fb" IPC service. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "uapi/chiffon_abi.h"

struct fb_info {
    uintptr_t phys;                 /* physical address of the pixel memory */
    uint8_t  *virt;                 /* kernel mapping (filled in by fb_register if 0) */
    uint32_t width, height;         /* visible resolution */
    uint32_t pitch;                 /* bytes per scanline */
    uint32_t bpp;                   /* 32 or 16 */
    uint32_t vheight;               /* virtual height in lines (>= height; extra lines allow page flips) */
    uint32_t xoff, yoff;            /* current pan offset */
    uint32_t size;                  /* bytes of pixel memory (vheight * pitch) */
    uint8_t  roff, rlen, goff, glen, boff, blen, toff, tlen;
};
struct fb_device {
    const char *name;
    struct fb_info info;
    int (*pan)(struct fb_device *d, uint32_t x, uint32_t y);   /* optional hardware pan/flip */
    struct fb_device *next;
};

int  fb_register(struct fb_device *d);
struct fb_device *fb_primary(void);
uint32_t fb_pack(const struct fb_info *i, uint32_t rgb);       /* 0xRRGGBB -> native pixel */
void fb_fill(struct fb_device *d, int x, int y, int w, int h, uint32_t rgb);
ssize_t fb_read(uint64_t off, void *buf, size_t n);
ssize_t fb_write(uint64_t off, const void *buf, size_t n);
long fb_ioctl(unsigned long cmd, uintptr_t arg);
size_t fb_dump(char *buf, size_t n);

void fbcon_attach(struct fb_device *d);                        /* bind the text console to a framebuffer */
