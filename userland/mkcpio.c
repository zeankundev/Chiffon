/* mkcpio DIR OUT.cpio : pack a directory tree as a cpio "newc" archive.
 * Plain POSIX C so it builds on Linux, Termux, Raspberry Pi OS, macOS... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static FILE *out; static unsigned ino = 1; static long pos;

static void wr(const void *p, size_t n) { fwrite(p, 1, n, out); pos += n; }
static void pad(void) { while (pos & 3) { fputc(0, out); pos++; } }
static void entry(const char *name, unsigned mode, const void *data, unsigned size) {
    char h[111];
    snprintf(h, sizeof h, "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
             ino++, mode, 0u, 0u, 1u, 0u, size, 0u, 0u, 0u, 0u, (unsigned)strlen(name) + 1, 0u);
    wr(h, 110); wr(name, strlen(name) + 1); pad();
    if (size) wr(data, size);
    pad();
}
static void walk(const char *root, const char *rel) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    DIR *d = opendir(path);
    if (!d) { perror(path); exit(1); }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[4096], f[8300];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(f, sizeof f, "%s/%s", root, r);
        struct stat st;
        if (lstat(f, &st)) { perror(f); exit(1); }
        if (S_ISDIR(st.st_mode)) { entry(r, st.st_mode, 0, 0); walk(root, r); }
        else if (S_ISLNK(st.st_mode)) {
            char t[4096]; ssize_t n = readlink(f, t, sizeof t - 1);
            if (n < 0) { perror(f); exit(1); }
            entry(r, st.st_mode, t, n);
        } else if (S_ISREG(st.st_mode)) {
            char *buf = malloc(st.st_size ? st.st_size : 1);
            FILE *in = fopen(f, "rb");
            if (!in || fread(buf, 1, st.st_size, in) != (size_t)st.st_size) { perror(f); exit(1); }
            fclose(in);
            entry(r, st.st_mode, buf, st.st_size);
            free(buf);
        }
    }
    closedir(d);
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: mkcpio DIR OUT\n"); return 1; }
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    walk(argv[1], "");
    entry("TRAILER!!!", 0, 0, 0);
    fclose(out);
    return 0;
}
