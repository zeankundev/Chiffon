/* fbserver: User-space framebuffer driver server for Chiffon.
 * Runs in ring 3 as part of Chiffon's hybrid kernel architecture.
 * Maps hardware MMIO directly via capability (CHI_CAP_MMIO) and serves
 * client drawing requests over IPC (port "fb").
 */
#include "libchiffon.h"

static void put_hex(unsigned long v) {
    char b[20]; int i = 19; b[i] = 0;
    if (!v) b[--i] = '0';
    for (; v; v >>= 4) b[--i] = "0123456789abcdef"[v & 15];
    chiffon_puts(b + i);
}

static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (v < 0) { chiffon_puts("-"); v = -v; }
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = '0' + v % 10;
    chiffon_puts(b + i);
}

static int parse_arg(const char *s) {
    unsigned value = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || value > (2147483647U - (*s - '0')) / 10)
            return -1;
        value = value * 10 + (*s - '0');
    }
    return value;
}

static int report_stage(int fd, int stage) {
    return chiffon_write(fd, &stage, sizeof stage) == sizeof stage;
}

static void fill_rect(unsigned char *fb, const struct chi_dev_fb_info *info,
                      const chi_u32 in[5]) {
    long x = (chi_s32)in[0], y = (chi_s32)in[1];
    long right = x + (chi_s32)in[2], bottom = y + (chi_s32)in[3];
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > info->width) right = info->width;
    if (bottom > info->height) bottom = info->height;
    if (right <= x || bottom <= y) return;
    for (long row = y; row < bottom; row++) {
        volatile chi_u32 *line = (volatile chi_u32 *)(fb + (size_t)row * info->pitch);
        for (long col = x; col < right; col++) line[col] = in[4];
    }
}

int main(int argc, char **argv, char **envp) {
    if (argc != 5 || chiffon_strcmp(argv[1], "--bootstrap")) return 1;
    int report = parse_arg(argv[2]), grant = parse_arg(argv[3]), loader = parse_arg(argv[4]);
    if (report < 0 || grant < 0 || report == grant || loader <= 0) return 1;
    int stage;
    if (!report_stage(report, 0) ||
        chiffon_read(grant, &stage, sizeof stage) != 0)
        return 1;
    chiffon_close(grant);

    struct chi_dev_fb_info info;
    long error = chiffon_dev_info(DEV_TYPE_FB, &info);
    if (error < 0 || info.size == 0) {
        chiffon_puts("[fbserver] failed to get framebuffer device info\n");
        report_stage(report, error < 0 ? error : -19);
        return 1;
    }
    if (info.bpp != 32 || !info.width || !info.height || info.pitch % 4 ||
        info.width > info.pitch / 4 || (chi_u64)info.height * info.pitch > info.size ||
        info.size > 0xffffffffUL) {
        chiffon_puts("[fbserver] unsupported framebuffer layout\n");
        report_stage(report, -22); /* EINVAL */
        return 1;
    }

    chiffon_puts("[fbserver] device info: phys=0x");
    put_hex((unsigned long)info.phys);
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
        report_stage(report, m);
        return 1;
    }
    unsigned char *fbp = (unsigned char *)m;
    chiffon_puts("[fbserver] MMIO mapped at user VA 0x");
    put_hex((unsigned long)m);
    chiffon_puts("\n");

    /* Create IPC port "fb" */
    long port = chiffon_ipc_create(IPC_SVC_FB);
    if (port < 0) {
        chiffon_puts("[fbserver] cannot create 'fb' IPC port: ");
        put_num(port);
        chiffon_puts("\n");
        report_stage(report, port);
        return 1;
    }
    if (!report_stage(report, port)) {
        chiffon_ipc_close(port);
        return 1;
    }
    chiffon_close(report);
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
                chi_u32 in[5];
                memcpy(in, req.data, sizeof in);
                fill_rect(fbp, &info, in);
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
            if (req.src != loader) {
                rep.tag = -1; /* EPERM */
                break;
            }
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
