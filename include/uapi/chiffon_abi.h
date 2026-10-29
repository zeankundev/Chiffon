/* Chiffon kernel <-> userland ABI.
 *
 * Freestanding on purpose: no libc and no kernel headers are required, so both the kernel
 * and user programs (userland/libchiffon.h users) can include it.  Everything here is
 * architecture independent; the IPC syscalls live above the Linux-compatible number range. */
#pragma once

typedef __INT32_TYPE__  chi_s32;
typedef __UINT8_TYPE__  chi_u8;
typedef __UINT16_TYPE__ chi_u16;
typedef __UINT32_TYPE__ chi_u32;
typedef __UINT64_TYPE__ chi_u64;

/* ------------------------------------------------------------------------------------------
 * IPC: ports + fixed-size messages
 * ------------------------------------------------------------------------------------------ */
#define CSYS_ipc_create  1000   /* (const char *name, flags)            -> port id | -errno      */
#define CSYS_ipc_lookup  1001   /* (const char *name)                   -> port id | -errno      */
#define CSYS_ipc_close   1002   /* (port)                               -> 0 | -errno            */
#define CSYS_ipc_send    1003   /* (port, const msg *, flags)           -> 0 | -errno            */
#define CSYS_ipc_recv    1004   /* (port, msg *, flags)                 -> 0 | -errno            */
#define CSYS_ipc_call    1005   /* (port, const msg *req, msg *rep)     -> 0 | -errno            */
#define CSYS_ipc_reply   1006   /* (const msg *rep)  rep->dst = pid of the caller (msg.src)      */

#define IPC_NONBLOCK     1
#define IPC_NAME_MAX     24     /* including the NUL */
#define IPC_DATA_MAX     48

/* 64 bytes.  src is filled in by the kernel.  In a request, tag is the operation; in a
 * reply, tag is the status (0 or -errno) -- same convention for kernel and user servers. */
struct chi_ipc_msg {
    chi_s32 src;            /* sender pid (kernel-assigned; 0 = kernel) */
    chi_s32 dst;            /* ipc_reply(): pid of the process being answered */
    chi_s32 tag;
    chi_u32 len;            /* valid bytes in data[], <= IPC_DATA_MAX */
    chi_u8  data[IPC_DATA_MAX];
};

/* Well-known kernel services (registered by the drivers that own the hardware). */
#define IPC_SVC_FB       "fb"
#define IPC_SVC_CONSOLE  "console"

/* "fb": data[] layouts are arrays of chi_u32 (native endian) */
#define FB_OP_INFO   1      /* reply: w, h, pitch, bpp, vw, vh, xoff, yoff, size           (36 bytes) */
#define FB_OP_FILL   2      /* req:   x, y, w, h, 0xRRGGBB                                 (20 bytes) */
#define FB_OP_PAN    3      /* req:   x, y  (page flip / scroll the visible window)         (8 bytes) */
/* "console": */
#define CON_OP_WRITE    1   /* req: up to IPC_DATA_MAX bytes of text (VT100 sequences allowed)  */
#define CON_OP_SIZE     2   /* reply: cols, rows                                               */
#define CON_OP_CLEAR    3   /* clear the screen and home the cursor                            */
#define CON_OP_GRAPHICS 4   /* req: u32 0 = text mode, 1 = graphics (console stops drawing)    */

/* ------------------------------------------------------------------------------------------
 * /dev/fb0 -- Linux-compatible ioctls and structures (so existing tools/programs work)
 * ------------------------------------------------------------------------------------------ */
#define CHI_FBIOGET_VSCREENINFO 0x4600
#define CHI_FBIOPUT_VSCREENINFO 0x4601
#define CHI_FBIOGET_FSCREENINFO 0x4602
#define CHI_FBIOPAN_DISPLAY     0x4606
#define CHI_FBIOBLANK           0x4611
#define CHI_KDSETMODE           0x4B3A   /* on the tty: KD_TEXT 0 / KD_GRAPHICS 1 */
#define CHI_KDGETMODE           0x4B3B
#define CHI_KD_TEXT     0
#define CHI_KD_GRAPHICS 1

struct chi_fb_bitfield { chi_u32 offset, length, msb_right; };
struct chi_fb_var_screeninfo {                        /* 160 bytes, == struct fb_var_screeninfo */
    chi_u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct chi_fb_bitfield red, green, blue, transp;
    chi_u32 nonstd, activate, height, width, accel_flags;
    chi_u32 pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len;
    chi_u32 sync, vmode, rotate, colorspace, reserved[4];
};
struct chi_fb_fix_screeninfo {                        /* 80 bytes on LP64, == struct fb_fix_screeninfo */
    char id[16];
    unsigned long smem_start;
    chi_u32 smem_len, type, type_aux, visual;
    chi_u16 xpanstep, ypanstep, ywrapstep;
    chi_u32 line_length;
    unsigned long mmio_start;
    chi_u32 mmio_len, accel;
    chi_u16 capabilities, reserved[2];
};
