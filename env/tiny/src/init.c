#include "libchiffon.h"

static char **g_envp;
static char line[256];

static const char *getenv_(const char *k) {
    size_t l = chiffon_strlen(k);
    for (char **e = g_envp; e && *e; e++)
        if (!memcmp(*e, k, l) && (*e)[l] == '=') return *e + l + 1;
    return 0;
}
static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = '0' + v % 10;
    chiffon_puts(b + i);
}
static void cmd_ls(const char *dir) {
    int fd = chiffon_open(dir, O_RDONLY, 0);
    if (fd < 0) { chiffon_puts("ls: cannot open "); chiffon_puts(dir); chiffon_puts("\n"); return; }
    static char buf[2048];
    long n;
    while ((n = chiffon_getdents(fd, buf, sizeof buf)) > 0)
        for (long p = 0; p < n;) {
            unsigned short rl = *(unsigned short *)(buf + p + 16);
            const char *name = buf + p + 19;
            if (chiffon_strcmp(name, ".") && chiffon_strcmp(name, "..")) {
                chiffon_puts(name); if (buf[p + 18] == 4) chiffon_puts("/"); chiffon_puts("  ");
            }
            p += rl;
        }
    chiffon_puts("\n");
    chiffon_close(fd);
}
static void cmd_cat(const char *path) {
    int fd = chiffon_open(path, O_RDONLY, 0);
    if (fd < 0) { chiffon_puts("cat: cannot open "); chiffon_puts(path); chiffon_puts("\n"); return; }
    char b[512]; long n;
    while ((n = chiffon_read(fd, b, sizeof b)) > 0) chiffon_write(1, b, n);
    chiffon_close(fd);
}
static void run(char **argv) {
    const char *path = getenv_("PATH");
    if (!path) path = "/bin:/sbin:/usr/bin";
    int pid = chiffon_fork();
    if (pid == 0) {
        if (argv[0][0] == '/' || argv[0][0] == '.') chiffon_execve(argv[0], argv, g_envp);
        else for (const char *p = path; *p;) {
            char full[160]; size_t i = 0;
            while (*p && *p != ':' && i < 100) full[i++] = *p++;
            if (*p == ':') p++;
            full[i++] = '/';
            for (const char *a = argv[0]; *a && i < 158;) full[i++] = *a++;
            full[i] = 0;
            chiffon_execve(full, argv, g_envp);
        }
        chiffon_puts(argv[0]); chiffon_puts(": command not found\n");
        chiffon_exit(127);
    }
    int st = 0;
    chiffon_wait(&st);
}

int main(int argc, char **argv, char **envp) {
    g_envp = envp;
    chiffon_puts("Welcome to your simple Chiffon system! Type 'help' for a list of commands.\n");
        for (;;) {
        chiffon_puts("hello-init# ");
        long n = chiffon_read(0, line, sizeof line - 1);
        if (n <= 0) break;                                  /* EOF (^D) */
        line[n] = 0;
        char *av[16]; int ac = 0;
        for (char *p = line; *p && ac < 15;) {
            while (*p == ' ' || *p == '\t' || *p == '\n') *p++ = 0;
            if (!*p) break;
            av[ac++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        }
        av[ac] = 0;
        if (!ac) continue;
        if (!chiffon_strcmp(av[0], "help"))
            chiffon_puts("builtins: help echo pwd cd ls cat env uname poweroff exit\nuserland: drvloader fbserver ipcdemo fbdemo\nanything else is looked up in $PATH\n");
        else if (!chiffon_strcmp(av[0], "echo")) { for (int i = 1; i < ac; i++) { chiffon_puts(av[i]); chiffon_puts(i + 1 < ac ? " " : ""); } chiffon_puts("\n"); }
        else if (!chiffon_strcmp(av[0], "pwd")) { char b[256]; if (chiffon_getcwd(b, sizeof b) > 0) { chiffon_puts(b); chiffon_puts("\n"); } }
        else if (!chiffon_strcmp(av[0], "cd")) { if (chiffon_chdir(ac > 1 ? av[1] : "/") < 0) chiffon_puts("cd: failed\n"); }
        else if (!chiffon_strcmp(av[0], "ls")) cmd_ls(ac > 1 ? av[1] : ".");
        else if (!chiffon_strcmp(av[0], "cat") && ac > 1) cmd_cat(av[1]);
        else if (!chiffon_strcmp(av[0], "env")) { for (char **e = envp; *e; e++) { chiffon_puts(*e); chiffon_puts("\n"); } }
        else if (!chiffon_strcmp(av[0], "uname")) {
            char u[390]; chiffon_uname(u);
            chiffon_puts(u); chiffon_puts(" "); chiffon_puts(u + 65); chiffon_puts(" "); chiffon_puts(u + 130); chiffon_puts(" "); chiffon_puts(u + 260); chiffon_puts("\n");
        }
        else if (!chiffon_strcmp(av[0], "poweroff")) chiffon_poweroff();
        else if (!chiffon_strcmp(av[0], "exit")) break;
        else run(av);
    }
    return 0;
}
