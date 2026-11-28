/* drvloader: User-space driver loader & manager for Chiffon's hybrid kernel.
 * - Manages user-space device driver servers (like fbserver)
 * - Grants capability tokens for MMIO and IRQs
 * - Measures IPC round-trip latency
 * - Tests crash resilience (server dies => kernel survives, callers get -EPIPE)
 */
#include "libchiffon.h"

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

/* Spawn fbserver with capabilities */
static int spawn_fb_server(void) {
    struct chi_dev_fb_info info;
    if (chiffon_dev_info(DEV_TYPE_FB, &info) < 0 || info.size == 0) {
        chiffon_puts("[drvloader] cannot get framebuffer device info\n");
        return -1;
    }

    int pid = chiffon_fork();
    if (pid < 0) {
        chiffon_puts("[drvloader] fork failed\n");
        return -1;
    }

    if (pid == 0) {
        /* Child: exec /bin/fbserver */
        char *av[] = { "/bin/fbserver", 0 };
        chiffon_execve("/bin/fbserver", av, 0);
        chiffon_puts("[drvloader] execve /bin/fbserver failed\n");
        chiffon_exit(1);
    }

    /* Parent (loader): grant capabilities to the newly spawned driver server */
    chiffon_cap_grant(pid, CHI_CAP_MMIO, (unsigned long)info.phys, (unsigned long)info.size);
    chiffon_cap_grant(pid, CHI_CAP_DRIVER, 0, 0);

    /* Give server time to initialize and register the port */
    chiffon_sleep_ms(50);
    return pid;
}

static void cmd_status(void) {
    chiffon_puts("=== Driver Subsystem Status ===\n");
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
        put_num((long)fb_info.phys); chiffon_puts("\n");
    }

    struct chi_dev_uart_info uart_info;
    if (chiffon_dev_info(DEV_TYPE_UART, &uart_info) == 0 && uart_info.size > 0) {
        chiffon_puts("  Hardware UART Device:    phys 0x");
        put_num((long)uart_info.phys); chiffon_puts(" IRQ ");
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

    int srv_pid = chiffon_fork();
    if (srv_pid == 0) {
        /* Server creates its own port so pt->owner == srv_pid */
        long port = chiffon_ipc_create("bench.echo");
        if (port < 0) chiffon_exit(1);
        struct chi_ipc_msg m;
        for (;;) {
            if (chiffon_ipc_recv(port, &m, 0) < 0) break;
            if (m.tag == -1) break; /* exit command */
            struct chi_ipc_msg rep;
            memset(&rep, 0, sizeof rep);
            rep.dst = m.src;
            rep.tag = m.tag;
            rep.len = m.len;
            memcpy(rep.data, m.data, m.len);
            chiffon_ipc_reply(&rep);
        }
        chiffon_ipc_close(port);
        chiffon_exit(0);
    }

    /* Client benchmark loop: wait for server to register port */
    long target_port = -1;
    for (int retry = 0; retry < 100; retry++) {
        target_port = chiffon_ipc_lookup("bench.echo");
        if (target_port > 0) break;
        chiffon_sleep_ms(5);
    }
    if (target_port < 0) {
        chiffon_puts("Cannot lookup bench.echo\n");
        chiffon_kill(srv_pid, 9);
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

    /* Warm-up */
    for (int i = 0; i < 50; i++) {
        chiffon_ipc_call(target_port, &req, &rep);
    }

    unsigned long start_time = chiffon_cycles();
    for (int i = 0; i < iters; i++) {
        unsigned long t0 = chiffon_cycles();
        long r = chiffon_ipc_call(target_port, &req, &rep);
        unsigned long t1 = chiffon_cycles();
        if (r < 0 || rep.tag != 42) {
            chiffon_puts("IPC call error during benchmark: ");
            put_num(r);
            chiffon_puts("\n");
            break;
        }
        unsigned long diff = t1 - t0;
        if (diff < min_cycles) min_cycles = diff;
        if (diff > max_cycles) max_cycles = diff;
    }
    unsigned long end_time = chiffon_cycles();

    /* Tell echo server to exit */
    req.tag = -1;
    chiffon_ipc_call(target_port, &req, &rep);
    int st = 0;
    chiffon_wait(&st);

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

/* Phase 0 Crash Test:
 * 1. Ensure user-space fbserver is running.
 * 2. Send valid FB_OP_INFO to verify service is healthy.
 * 3. Client issues a request that induces an abrupt crash (FB_OP_CRASH).
 * 4. Verify client returns -EPIPE (-32).
 * 5. Verify the kernel survives without panic and subsequent syscalls work.
 */
static int cmd_crash_test(void) {
    chiffon_puts("=== Starting Phase 0 Driver Server Crash Test ===\n");

    /* Spawn a fresh user-space fbserver */
    chiffon_puts("[test 1/4] Spawning user-space fbserver with MMIO capabilities...\n");
    int srv_pid = spawn_fb_server();
    if (srv_pid < 0) {
        chiffon_puts("FAILED: unable to start fbserver\n");
        return 1;
    }
    chiffon_puts("[test 1/4] fbserver spawned with PID ");
    put_num(srv_pid);
    chiffon_puts("\n");

    /* Step 2: Lookup "fb" and verify normal operation */
    chiffon_puts("[test 2/4] Verifying normal IPC service communication...\n");
    long fb_port = chiffon_ipc_lookup(IPC_SVC_FB);
    if (fb_port <= 0) {
        chiffon_puts("FAILED: 'fb' port lookup failed\n");
        chiffon_kill(srv_pid, 9);
        return 1;
    }

    struct chi_ipc_msg req, rep;
    memset(&req, 0, sizeof req);
    req.tag = FB_OP_INFO;
    long r = chiffon_ipc_call(fb_port, &req, &rep);
    if (r != 0 || rep.tag != 0) {
        chiffon_puts("FAILED: initial FB_OP_INFO call failed (r=");
        put_num(r); chiffon_puts(" tag="); put_num(rep.tag); chiffon_puts(")\n");
        chiffon_kill(srv_pid, 9);
        return 1;
    }
    unsigned *dim = (unsigned *)rep.data;
    chiffon_puts("[test 2/4] Normal IPC call succeeded: screen is ");
    put_num(dim[0]); chiffon_puts("x"); put_num(dim[1]); chiffon_puts("\n");

    /* Step 3: Send FB_OP_CRASH to the user-space driver server.
     * The server will exit immediately without calling ipc_reply().
     * The kernel's ipc_proc_exit() must detect server termination,
     * drop the port, wake the blocked caller, and return -EPIPE.
     */
    chiffon_puts("[test 3/4] Triggering abrupt server crash while client is in ipc_call...\n");
    memset(&req, 0, sizeof req);
    req.tag = FB_OP_CRASH;

    long crash_res = chiffon_ipc_call(fb_port, &req, &rep);
    long caller_status = (crash_res != 0) ? crash_res : rep.tag;

    chiffon_puts("[test 3/4] Caller returned from ipc_call with status: ");
    put_num(caller_status);
    chiffon_puts("\n");

    if (caller_status != -32) { /* -EPIPE is 32 */
        chiffon_puts("FAILED: expected -EPIPE (-32), got ");
        put_num(caller_status);
        chiffon_puts("\n");
        return 1;
    }
    chiffon_puts("[test 3/4] PASS: Caller received expected -EPIPE (-32)\n");

    /* Step 4: Verify kernel survived completely intact and healthy */
    chiffon_puts("[test 4/4] Verifying kernel integrity and system survival...\n");
    char cwd[128];
    if (chiffon_getcwd(cwd, sizeof cwd) <= 0) {
        chiffon_puts("FAILED: subsequent syscall failed\n");
        return 1;
    }
    int reap_st = 0;
    chiffon_wait(&reap_st);

    chiffon_puts("\n============================================================\n");
    chiffon_puts("=== PHASE 0 CRASH TEST PASSED: KERNEL SURVIVED & CALLER GOT -EPIPE ===\n");
    chiffon_puts("============================================================\n\n");
    return 0;
}

int main(int argc, char **argv, char **envp) {
    if (argc > 1) {
        if (!chiffon_strcmp(argv[1], "status")) {
            cmd_status();
            return 0;
        }
        if (!chiffon_strcmp(argv[1], "start") && argc > 2 && !chiffon_strcmp(argv[2], "fb")) {
            chiffon_puts("Starting user-space fbserver...\n");
            spawn_fb_server();
            return 0;
        }
        if (!chiffon_strcmp(argv[1], "bench")) {
            int n = 1000;
            if (argc > 2) {
                int val = 0;
                for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) val = val * 10 + (*p - '0');
                if (val > 0) n = val;
            }
            return cmd_bench(n);
        }
        if (!chiffon_strcmp(argv[1], "test-crash")) {
            return cmd_crash_test();
        }
        if (!chiffon_strcmp(argv[1], "test")) {
            cmd_status();
            cmd_bench(1000);
            return cmd_crash_test();
        }
    }

    chiffon_puts("Usage: drvloader [status | start fb | bench <n> | test-crash | test]\n");
    cmd_status();
    return 0;
}
