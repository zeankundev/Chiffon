/* fbserver: User-space framebuffer driver server for Chiffon.
 * Runs in ring 3 as part of Chiffon's hybrid kernel architecture.
 * Maps hardware MMIO directly via capability (CHI_CAP_MMIO) and serves
 * client drawing requests over IPC (port "fb").
 */
#include "libchiffon.h"

static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (v < 0) { chiffon_puts("-"); v = -v; }
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = '0' + v % 10;
    chiffon_puts(b + i);
}

int main(int argc, char **argv, char **envp) {
    struct chi_dev_fb_info info;
    if (chiffon_dev_info(DEV_TYPE_FB, &info) < 0 || info.size == 0) {
        chiffon_puts("[fbserver] failed to get framebuffer device info\n");
        return 1;
    }

    chiffon_puts("[fbserver] device info: phys=0x");
    put_num((long)info.phys);
    chiffon_puts(" size=");
    put_num((long)info.size);
    chiffon_puts(" (");
    put_num(info.width);
    chiffon_puts("x");
    put_num(info.height);
    chiffon_puts(" ");
    put_num(info.bpp);
    chiffon_puts("bpp, pitch ");
    put_num(info.pitch);
    chiffon_puts(")\n");

    /* Map physical MMIO framebuffer into user space using capability */
    long m = chiffon_dev_map((unsigned long)info.phys, (size_t)info.size);
    if (m < 0 && m > -4096) {
        chiffon_puts("[fbserver] chiffon_dev_map failed with error ");
        put_num(m);
        chiffon_puts("\n");
        return 1;
    }
    unsigned char *fbp = (unsigned char *)m;
    chiffon_puts("[fbserver] MMIO mapped at user VA 0x");
    put_num(m);
    chiffon_puts("\n");

    /* Create IPC port "fb" */
    long port = chiffon_ipc_create(IPC_SVC_FB);
    if (port < 0) {
        chiffon_puts("[fbserver] cannot create 'fb' IPC port: ");
        put_num(port);
        chiffon_puts("\n");
        return 1;
    }
    chiffon_puts("[fbserver] IPC port 'fb' registered (port ID ");
    put_num(port);
    chiffon_puts("). Driver is ready.\n");

    struct chi_ipc_msg req, rep;
    for (;;) {
        long err = chiffon_ipc_recv(port, &req, 0);
        if (err < 0) break;

        memset(&rep, 0, sizeof rep);
        rep.dst = req.src;
        rep.src = 0;

        switch (req.tag) {
        case FB_OP_INFO: {
            unsigned *out = (unsigned *)rep.data;
            out[0] = info.width;
            out[1] = info.height;
            out[2] = info.pitch;
            out[3] = info.bpp;
            out[4] = info.width;
            out[5] = info.height;
            out[6] = 0;
            out[7] = 0;
            out[8] = (unsigned)info.size;
            rep.len = 36;
            rep.tag = 0;
            break;
        }
        case FB_OP_FILL: {
            if (req.len >= 20) {
                const unsigned *in = (const unsigned *)req.data;
                int x = (int)in[0], y = (int)in[1], w = (int)in[2], h = (int)in[3];
                unsigned rgb = in[4];

                if (x < 0) { w += x; x = 0; }
                if (y < 0) { h += y; y = 0; }
                if (x + w > (int)info.width) w = (int)info.width - x;
                if (y + h > (int)info.height) h = (int)info.height - y;

                if (w > 0 && h > 0) {
                    for (int r = 0; r < h; r++) {
                        unsigned *line = (unsigned *)(fbp + (size_t)(y + r) * info.pitch + (size_t)x * 4);
                        for (int c = 0; c < w; c++) line[c] = rgb;
                    }
                }
                rep.tag = 0;
            } else {
                rep.tag = -22; /* -EINVAL */
            }
            break;
        }
        case FB_OP_PAN: {
            rep.tag = 0;
            break;
        }
        case FB_OP_CRASH: {
            chiffon_puts("[fbserver] CRASH TEST: simulating abrupt server crash without replying!\n");
            chiffon_exit(137); /* Die immediately: kernel closes port and fails caller with -EPIPE */
        }
        default:
            rep.tag = -38; /* -ENOSYS */
            break;
        }

        chiffon_ipc_reply(&rep);
    }

    chiffon_ipc_close(port);
    return 0;
}
