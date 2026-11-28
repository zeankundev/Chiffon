/* ipcdemo: talks to the kernel's "fb" and "console" services over IPC, then runs a tiny
 * user-space server ("demo.echo") with a forked client to show process-to-process calls. */
#include "libchiffon.h"

static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (v < 0) { chiffon_puts("-"); v = -v; }
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = '0' + v % 10;
    chiffon_puts(b + i);
}

/* `ipcdemo -crash`: the server dies without answering; the blocked client must get an error, not hang. */
static int crash_test(void) {
    struct chi_ipc_msg q, r;
    long port = chiffon_ipc_create("demo.crash");
    int pid = chiffon_fork();
    if (pid == 0) {
        long p = chiffon_ipc_lookup("demo.crash");
        memset(&q, 0, sizeof q); q.tag = 1;
        long e = chiffon_ipc_call(p, &q, &r);
        if (e == 0) e = r.tag;
        chiffon_puts("client: call returned "); put_num(e); chiffon_puts(e == -32 ? " (EPIPE: server died, as expected)\n" : " (unexpected)\n");
        chiffon_exit(0);
    }
    chiffon_ipc_recv(port, &q, 0);                        /* take the request ... */
    chiffon_puts("server: got a request, exiting without replying\n");
    int st = 0;
    if (chiffon_fork() == 0) chiffon_exit(0);             /* (reap helper keeps the shell's wait() accounting simple) */
    chiffon_wait(&st);
    chiffon_exit(0);                                      /* ... and die: kernel closes the port and fails the client */
}

int main(int argc, char **argv, char **envp) {
    if (argc > 1 && !chiffon_strcmp(argv[1], "-crash")) return crash_test();
    struct chi_ipc_msg q, r;
    unsigned *u = (unsigned *)r.data;

    long con = chiffon_ipc_lookup(IPC_SVC_CONSOLE);
    if (con > 0) {
        memset(&q, 0, sizeof q); q.tag = CON_OP_SIZE;
        if (!chiffon_ipc_call(con, &q, &r) && r.tag == 0) { chiffon_puts("console service: "); put_num(u[0]); chiffon_puts("x"); put_num(u[1]); chiffon_puts(" cells\n"); }
        memset(&q, 0, sizeof q); q.tag = CON_OP_WRITE;
        const char *t = "\033[1;32m>> hi from the console service\033[0m\r\n";   /* raw VT bytes: CR+LF; <= IPC_DATA_MAX */
        q.len = chiffon_strlen(t); memcpy(q.data, t, q.len);
        if (chiffon_ipc_call(con, &q, &r) || r.tag) chiffon_puts("console write failed\n");
    }

    long fb = chiffon_ipc_lookup(IPC_SVC_FB);
    if (fb > 0) {
        memset(&q, 0, sizeof q); q.tag = FB_OP_INFO;
        if (!chiffon_ipc_call(fb, &q, &r) && r.tag == 0) {
            chiffon_puts("fb service: "); put_num(u[0]); chiffon_puts("x"); put_num(u[1]); chiffon_puts(", "); put_num(u[3]); chiffon_puts("bpp\n");
            memset(&q, 0, sizeof q); q.tag = FB_OP_FILL; q.len = 20;
            unsigned *a = (unsigned *)q.data;
            a[0] = u[0] - 120; a[1] = 8; a[2] = 112; a[3] = 24; a[4] = 0x00C000;              /* a green badge in the top-right corner */
            chiffon_ipc_call(fb, &q, &r);
        }
    } else chiffon_puts("fb service: not present (no framebuffer driver active)\n");

    long port = chiffon_ipc_create("demo.echo");
    if (port < 0) { chiffon_puts("cannot create port\n"); return 1; }
    int pid = chiffon_fork();
    if (pid == 0) {                                       /* client */
        long p = chiffon_ipc_lookup("demo.echo");
        for (int i = 1; i <= 3; i++) {
            memset(&q, 0, sizeof q); q.tag = 1;
            const char *m = "hello server #"; q.len = chiffon_strlen(m); memcpy(q.data, m, q.len); q.data[q.len++] = '0' + i;
            if (chiffon_ipc_call(p, &q, &r) || r.tag) { chiffon_puts("client: call failed\n"); chiffon_exit(1); }
            chiffon_puts("client got: "); chiffon_write(1, r.data, r.len); chiffon_puts("\n");
        }
        chiffon_exit(0);
    }
    for (int i = 0; i < 3; i++) {                         /* server: upper-case whatever arrives */
        if (chiffon_ipc_recv(port, &q, 0)) break;
        memset(&r, 0, sizeof r);
        r.dst = q.src; r.tag = 0; r.len = q.len;
        for (unsigned k = 0; k < q.len; k++) r.data[k] = (q.data[k] >= 'a' && q.data[k] <= 'z') ? q.data[k] - 32 : q.data[k];
        chiffon_ipc_reply(&r);
    }
    int st = 0; chiffon_wait(&st);
    chiffon_ipc_close(port);
    chiffon_puts("ipcdemo: done\n");
    return 0;
}
