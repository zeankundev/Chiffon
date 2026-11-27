/* drvloader: User-space driver loader & manager for Chiffon's hybrid kernel.
 * - Manages user-space device driver servers (like fbserver)
 * - Grants capability tokens for MMIO and IRQs
 * - Measures IPC round-trip latency
 * - Tests crash resilience (server dies => kernel survives, callers get -EPIPE)
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

static void print_float(long whole, long thousandths) {
    put_num(whole);
    chiffon_puts(".");
    if (thousandths < 0) thousandths = -thousandths;
    if (thousandths < 10) chiffon_puts("00");
    else if (thousandths < 100) chiffon_puts("0");
    put_num(thousandths);
}

static int fb_pid;
static int fb_port;

static void decimal_arg(char buf[12], int value) {
    int i = 11;
    buf[i] = 0;
    do {
        buf[--i] = '0' + value % 10;
        value /= 10;
    } while (value);
    for (int j = 0; i < 12; j++, i++) buf[j] = buf[i];
}

static int wait_child(int pid, int flags) {
    return ksys(CF_wait4, pid, 0, flags, 0, 0, 0);
}

static void refresh_fb(void) {
    if (fb_pid && wait_child(fb_pid, 1 /* WNOHANG */) != 0)
        fb_pid = fb_port = 0;
}

static long start_fb(void) {
    refresh_fb();
    if (fb_pid) return 0;

    struct chi_dev_fb_info info;
    long r = chiffon_dev_info(DEV_TYPE_FB, &info);
    if (r < 0) return r;
    if (!info.size) return -19; /* ENODEV */

    int report[2], grant[2];
    r = chiffon_pipe(report);
    if (r < 0) return r;
    r = chiffon_pipe(grant);
    if (r < 0) {
        chiffon_close(report[0]);
        chiffon_close(report[1]);
        return r;
    }

    int loader_pid = ksys(CF_getpid, 0, 0, 0, 0, 0, 0);
    int pid = chiffon_fork();
    if (!pid) {
        chiffon_close(report[0]);
        chiffon_close(grant[1]);
        char report_arg[12], grant_arg[12], pid_arg[12];
        decimal_arg(report_arg, report[1]);
        decimal_arg(grant_arg, grant[0]);
        decimal_arg(pid_arg, loader_pid);
        char *av[] = { "/bin/fbserver", "--bootstrap", report_arg, grant_arg, pid_arg, 0 };
        int error = chiffon_execve(av[0], av, 0);
        chiffon_write(report[1], &error, sizeof error);
        chiffon_exit(1);
    }
    chiffon_close(report[1]);
    chiffon_close(grant[0]);

    int stage;
    r = pid;
    if (pid > 0) {
        r = -32; /* EPIPE */
        if (chiffon_read(report[0], &stage, sizeof stage) == sizeof stage && stage <= 0)
            r = stage;
        if (!r) r = chiffon_cap_grant(pid, CHI_CAP_MMIO, info.phys, info.size);
        if (!r) r = chiffon_cap_grant(pid, CHI_CAP_DRIVER, DEV_TYPE_FB, 0);
    }
    /* Closing the grant pipe releases the child. Mapping and port creation
     * still require the capabilities checked above. */
    chiffon_close(grant[1]);
    if (pid > 0 && !r) {
        if (chiffon_read(report[0], &stage, sizeof stage) != sizeof stage) {
            r = -32;
        } else if (stage > 0) {
            fb_pid = pid;
            fb_port = stage;
        } else {
            r = stage < 0 ? stage : -32;
        }
    }
    chiffon_close(report[0]);
    if (pid > 0 && r < 0) {
        chiffon_kill(pid, 9);
        wait_child(pid, 0);
    }
    return r;
}

static long loader_call(int op, struct chi_ipc_msg *reply) {
    long port = chiffon_ipc_lookup(IPC_SVC_LOADER);
    if (port < 0) return port;
    struct chi_ipc_msg req = { .tag = op };
    long r = chiffon_ipc_call(port, &req, reply);
    return r < 0 ? r : reply->tag;
}

static void cmd_status(void) {
    chiffon_puts("=== Driver Subsystem Status ===\n");
    struct chi_ipc_msg rep;
    long r = loader_call(DRV_OP_STATUS, &rep);
    chiffon_puts("  Driver loader:            ");
    if (!r && rep.len == sizeof(struct chi_drv_fb_status)) {
        struct chi_drv_fb_status status;
        memcpy(&status, rep.data, sizeof status);
        if (status.pid) {
            chiffon_puts("fbserver PID ");
            put_num(status.pid);
            chiffon_puts("\n");
        } else {
            chiffon_puts("ready\n");
        }
    } else {
        chiffon_puts("unavailable\n");
    }
    long fb = chiffon_ipc_lookup(IPC_SVC_FB);
    chiffon_puts("  Service 'fb':            ");
    if (fb > 0) { chiffon_puts("ACTIVE (port "); put_num(fb); chiffon_puts(")\n"); }
    else chiffon_puts("NOT REGISTERED\n");

    long con = chiffon_ipc_lookup(IPC_SVC_CONSOLE);
    chiffon_puts("  Service 'console':       ");
    if (con > 0) { chiffon_puts("ACTIVE (port "); put_num(con); chiffon_puts(")\n"); }
    else chiffon_puts("NOT REGISTERED\n");

    struct chi_dev_fb_info fb_info;
    if (chiffon_dev_info(DEV_TYPE_FB, &fb_info) == 0 && fb_info.size > 0) {
        chiffon_puts("  Hardware FB Device:      ");
        put_num(fb_info.width); chiffon_puts("x"); put_num(fb_info.height);
        chiffon_puts(" "); put_num(fb_info.bpp); chiffon_puts("bpp @ phys 0x");
        put_hex((unsigned long)fb_info.phys); chiffon_puts("\n");
    }

    struct chi_dev_uart_info uart_info;
    if (chiffon_dev_info(DEV_TYPE_UART, &uart_info) == 0 && uart_info.size > 0) {
        chiffon_puts("  Hardware UART Device:    phys 0x");
        put_hex((unsigned long)uart_info.phys); chiffon_puts(" IRQ ");
        put_num(uart_info.irq); chiffon_puts("\n");
    }
}

/* Measure IPC round-trip latency */
static int cmd_bench(int iters) {
    if (iters <= 0) iters = 1000;
    chiffon_puts("=== Chiffon IPC Latency Benchmark ===\n");
    chiffon_puts("Benchmarking round-trip IPC over ");
    put_num(iters);
    chiffon_puts(" iterations...\n");

    int ready[2];
    if (chiffon_pipe(ready) < 0) return 1;
    int client_pid = ksys(CF_getpid, 0, 0, 0, 0, 0, 0);
    int srv_pid = chiffon_fork();
    if (srv_pid == 0) {
        chiffon_close(ready[0]);
        char name[IPC_NAME_MAX] = "bench.";
        decimal_arg(name + 6, ksys(CF_getpid, 0, 0, 0, 0, 0, 0));
        long port = chiffon_ipc_create(name);
        if (chiffon_write(ready[1], &port, sizeof port) != sizeof port || port < 0)
            chiffon_exit(1);
        chiffon_close(ready[1]);
        struct chi_ipc_msg m;
        for (;;) {
            if (chiffon_ipc_recv(port, &m, 0) < 0) break;
            if (m.src == client_pid && m.tag == -1) break;
            struct chi_ipc_msg rep = { .dst = m.src, .tag = -1 /* EPERM */ };
            if (m.src == client_pid) {
                rep.tag = m.tag;
                rep.len = m.len;
                memcpy(rep.data, m.data, m.len);
            }
            chiffon_ipc_reply(&rep);
        }
        chiffon_ipc_close(port);
        chiffon_exit(0);
    }

    chiffon_close(ready[1]);
    long target_port = -1;
    if (srv_pid > 0 && chiffon_read(ready[0], &target_port, sizeof target_port) != sizeof target_port)
        target_port = -1;
    chiffon_close(ready[0]);
    if (target_port <= 0) {
        chiffon_puts("Cannot start benchmark echo server\n");
        if (srv_pid > 0) {
            chiffon_kill(srv_pid, 9);
            wait_child(srv_pid, 0);
        }
        return 1;
    }

    struct chi_ipc_msg req, rep;
    memset(&req, 0, sizeof req);
    req.tag = 42;
    const char *payload = "ping";
    req.len = 4;
    memcpy(req.data, payload, 4);

    unsigned long freq = chiffon_timer_freq();
    unsigned long min_cycles = ~0UL;
    unsigned long max_cycles = 0;

    int failed = 0;
    for (int i = 0; i < 50; i++) {
        if (chiffon_ipc_call(target_port, &req, &rep) < 0 || rep.tag != 42 || rep.src != srv_pid) {
            failed = 1;
            break;
        }
    }

    unsigned long start_time = chiffon_cycles();
    for (int i = 0; !failed && i < iters; i++) {
        unsigned long t0 = chiffon_cycles();
        long r = chiffon_ipc_call(target_port, &req, &rep);
        unsigned long t1 = chiffon_cycles();
        if (r < 0 || rep.tag != 42 || rep.src != srv_pid) {
            chiffon_puts("IPC call error during benchmark: ");
            put_num(r);
            chiffon_puts("\n");
            failed = 1;
            break;
        }
        unsigned long diff = t1 - t0;
        if (diff < min_cycles) min_cycles = diff;
        if (diff > max_cycles) max_cycles = diff;
    }
    unsigned long end_time = chiffon_cycles();

    if (!failed) {
        req.tag = -1;
        long stop = chiffon_ipc_call(target_port, &req, &rep);
        failed = stop != -32 && (stop < 0 || rep.tag != -32);
    }
    if (failed) chiffon_kill(srv_pid, 9);
    wait_child(srv_pid, 0);
    if (failed) return 1;

    unsigned long total_cycles = end_time - start_time;
    /* Convert cycles to microseconds */
    /* latency_us = (cycles * 1000000) / freq */
    unsigned long avg_cycles = total_cycles / iters;
    unsigned long avg_us_whole = (avg_cycles * 1000000UL) / freq;
    unsigned long avg_us_frac = ((avg_cycles * 1000000UL) % freq) * 1000UL / freq;

    unsigned long min_us_whole = (min_cycles * 1000000UL) / freq;
    unsigned long min_us_frac = ((min_cycles * 1000000UL) % freq) * 1000UL / freq;

    unsigned long max_us_whole = (max_cycles * 1000000UL) / freq;
    unsigned long max_us_frac = ((max_cycles * 1000000UL) % freq) * 1000UL / freq;

    unsigned long total_ms = (total_cycles * 1000UL) / freq;
    unsigned long ops_per_sec = (total_cycles > 0) ? ((unsigned long)iters * freq / total_cycles) : 0;

    chiffon_puts("Results:\n");
    chiffon_puts("  Iterations:       "); put_num(iters); chiffon_puts("\n");
    chiffon_puts("  Timer Frequency:  "); put_num(freq); chiffon_puts(" Hz\n");
    chiffon_puts("  Total Time:       "); put_num(total_ms); chiffon_puts(" ms\n");
    chiffon_puts("  Average Latency:  "); print_float(avg_us_whole, avg_us_frac); chiffon_puts(" us/round-trip\n");
    chiffon_puts("  Minimum Latency:  "); print_float(min_us_whole, min_us_frac); chiffon_puts(" us\n");
    chiffon_puts("  Maximum Latency:  "); print_float(max_us_whole, max_us_frac); chiffon_puts(" us\n");
    chiffon_puts("  Throughput:       "); put_num(ops_per_sec); chiffon_puts(" round-trips/sec\n");
    chiffon_puts("======================================\n");
    return 0;
}

static long crash_fb(struct chi_ipc_msg *reply) {
    long r = start_fb();
    if (r < 0) return r;

    struct chi_ipc_msg req = { .tag = FB_OP_INFO }, rep;
    r = chiffon_ipc_call(fb_port, &req, &rep);
    if (r < 0) return r;
    if (rep.tag < 0) return rep.tag;
    if (rep.src != fb_pid || rep.len != 36) return -5; /* EIO */
    memcpy(reply->data, rep.data, 2 * sizeof(chi_u32));

    req.tag = FB_OP_CRASH;
    r = chiffon_ipc_call(fb_port, &req, &rep);
    if (r != -32 && (r < 0 || rep.tag != -32))
        return r < 0 ? r : -5;

    int dead_pid = fb_pid, dead_port = fb_port;
    if (wait_child(dead_pid, 0) != dead_pid) return -5;
    fb_pid = fb_port = 0;
    req.tag = FB_OP_INFO;
    if (chiffon_ipc_call(dead_port, &req, &rep) != -32) return -5;
    reply->len = 2 * sizeof(chi_u32);
    return 0;
}

static int loader_service(void) {
    long port = chiffon_ipc_create(IPC_SVC_LOADER);
    if (port < 0) return 1;
    struct chi_ipc_msg req;
    while (chiffon_ipc_recv(port, &req, 0) == 0) {
        struct chi_ipc_msg rep = { .dst = req.src };
        refresh_fb();
        if (req.len) {
            rep.tag = -22;
        } else if (req.tag == DRV_OP_CRASH) {
            rep.tag = crash_fb(&rep);
        } else if (req.tag == DRV_OP_LOAD || req.tag == DRV_OP_STATUS) {
            if (req.tag == DRV_OP_LOAD) rep.tag = start_fb();
            if (!rep.tag) {
                struct chi_drv_fb_status status = { .pid = fb_pid, .port = fb_port };
                memcpy(rep.data, &status, sizeof status);
                rep.len = sizeof status;
            }
        } else {
            rep.tag = -38;
        }
        chiffon_ipc_reply(&rep);
    }
    chiffon_ipc_close(port);
    return 1;
}

static int cmd_start(void) {
    struct chi_ipc_msg rep;
    long r = loader_call(DRV_OP_LOAD, &rep);
    if (r < 0 || rep.len != sizeof(struct chi_drv_fb_status)) {
        chiffon_puts("Unable to start fbserver (status ");
        put_num(r < 0 ? r : -5);
        chiffon_puts(")\n");
        return 1;
    }
    struct chi_drv_fb_status status;
    memcpy(&status, rep.data, sizeof status);
    chiffon_puts("fbserver ready (PID ");
    put_num(status.pid);
    chiffon_puts(", port ");
    put_num(status.port);
    chiffon_puts(")\n");
    return 0;
}

static int cmd_crash_test(void) {
    struct chi_ipc_msg rep;
    long r = loader_call(DRV_OP_CRASH, &rep);
    if (r < 0 || rep.len != 2 * sizeof(chi_u32)) {
        chiffon_puts("Framebuffer crash test failed (status ");
        put_num(r < 0 ? r : -5);
        chiffon_puts(")\n");
        return 1;
    }
    chi_u32 dim[2];
    memcpy(dim, rep.data, sizeof dim);
    chiffon_puts("Framebuffer replied before termination (");
    put_num(dim[0]);
    chiffon_puts("x");
    put_num(dim[1]);
    chiffon_puts("). The loader received -EPIPE and replied.\n");
    char cwd[128];
    if (chiffon_getcwd(cwd, sizeof cwd) <= 0) {
        chiffon_puts("Framebuffer crash test failed: subsequent syscall failed\n");
        return 1;
    }
    chiffon_puts("Framebuffer crash test passed.\n");
    return 0;
}

int main(int argc, char **argv, char **envp) {
    if (argc > 1) {
        if (!chiffon_strcmp(argv[1], "--service")) return loader_service();
        if (!chiffon_strcmp(argv[1], "status")) {
            cmd_status();
            return 0;
        }
        if (!chiffon_strcmp(argv[1], "start") && argc > 2 && !chiffon_strcmp(argv[2], "fb")) {
            return cmd_start();
        }
        if (!chiffon_strcmp(argv[1], "bench")) {
            int n = 1000;
            if (argc > 2) {
                int val = 0;
                const char *p = argv[2];
                for (; *p >= '0' && *p <= '9'; p++) {
                    int digit = *p - '0';
                    if (val > (2147483647 - digit) / 10) return 1;
                    val = val * 10 + digit;
                }
                if (*p) return 1;
                if (val > 0) n = val;
            }
            return cmd_bench(n);
        }
        if (!chiffon_strcmp(argv[1], "test-crash")) {
            return cmd_crash_test();
        }
        if (!chiffon_strcmp(argv[1], "test")) {
            cmd_status();
            if (cmd_bench(1000)) return 1;
            return cmd_crash_test();
        }
    }

    chiffon_puts("Usage: drvloader [status | start fb | bench <n> | test-crash | test]\n");
    cmd_status();
    return 0;
}
