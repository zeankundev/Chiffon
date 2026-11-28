/* httpget HOST[:PORT] [PATH]: HTTP/1.0 GET over the kernel's TCP client; the body goes to stdout. */
#include "libchiffon.h"

static void strlcpy_(char *d, const char *s, size_t n) { size_t i = 0; for (; s[i] && i + 1 < n; i++) d[i] = s[i]; if (n) d[i] = 0; }

static int parse_port(char *host) {
    for (char *p = host; *p; p++)
        if (*p == ':') { *p = 0; int port = 0; for (char *q = p + 1; *q >= '0' && *q <= '9'; q++) port = port * 10 + (*q - '0'); return port ? port : 80; }
    return 80;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) { chiffon_puts("usage: httpget HOST[:PORT] [PATH]\n"); return 2; }
    static char host[128];
    strlcpy_(host, argv[1], sizeof host);
    int port = parse_port(host);
    const char *path = argc > 2 ? argv[2] : "/";
    unsigned ip;
    if (chiffon_net_resolve(host, &ip)) { chiffon_puts("httpget: cannot resolve "); chiffon_puts(host); chiffon_puts("\n"); return 1; }
    long c = chiffon_net_connect(ip, (unsigned)port);
    if (c < 0) { chiffon_puts("httpget: connect failed\n"); return 1; }
    static char req[512];
    size_t n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nConnection: close\r\n\r\n", 0 };
    for (int i = 0; parts[i]; i++) for (const char *s = parts[i]; *s && n < sizeof req - 1; s++) req[n++] = *s;
    if (chiffon_net_send(c, req, n) < 0) { chiffon_puts("httpget: send failed\n"); chiffon_net_close(c); return 1; }
    static char buf[512];
    long got;
    long total = 0;
    while ((got = chiffon_net_recv(c, buf, sizeof buf, 5000)) > 0) { chiffon_write(1, buf, (size_t)got); total += got; }
    chiffon_net_close(c);
    if (got < 0) { chiffon_puts("\nhttpget: timed out\n"); return 1; }
    chiffon_puts("\n--- "); chiffon_putu((unsigned long)total); chiffon_puts(" bytes\n");
    return 0;
}
