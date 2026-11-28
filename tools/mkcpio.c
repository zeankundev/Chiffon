/* mkcpio DIR OUT.cpio : pack a directory tree as a cpio "newc" archive. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

static FILE *out;
static const char *output_path;
static char *temp_path;
static const char *output_name, *temp_name;
static dev_t output_dir_dev;
static ino_t output_dir_ino;
static uint32_t ino = 1;
static unsigned pos;

static void fail(const char *what) {
    perror(what);
    if (out) fclose(out);
    if (temp_path) unlink(temp_path);
    exit(1);
}
static void invalid(const char *what, const char *reason) {
    fprintf(stderr, "%s: %s\n", what, reason);
    if (out) fclose(out);
    if (temp_path) unlink(temp_path);
    exit(1);
}
static void wr(const void *p, size_t n) {
    if (fwrite(p, 1, n, out) != n) fail(output_path);
    pos = (pos + (unsigned)(n & 3)) & 3;
}
static void pad(void) {
    static const char zero[3] = {0};
    if (pos) wr(zero, 4 - pos);
}
static void entry(const char *name, unsigned mode, const void *data, uint32_t size) {
    char h[111];
    size_t len = strlen(name);
    if (len >= UINT32_MAX) invalid(name, "archive name too long");
    if (!ino) invalid(name, "too many archive entries");
    if (snprintf(h, sizeof h, "070701%08" PRIX32 "%08" PRIX32 "%08X%08X%08X%08X%08" PRIX32 "%08X%08X%08X%08X%08" PRIX32 "%08X",
                 ino++, (uint32_t)mode, 0u, 0u, 1u, 0u, size, 0u, 0u, 0u, 0u, (uint32_t)len + 1, 0u) != 110)
        invalid(name, "cannot encode archive header");
    wr(h, 110); wr(name, len + 1); pad();
    if (size) wr(data, size);
    pad();
}
static char *join(const char *a, const char *b, int slash) {
    size_t na = strlen(a), nb = strlen(b);
    char *p;
    if (na > SIZE_MAX - nb || na + nb > SIZE_MAX - (size_t)slash - 1)
        invalid(a, "path too long");
    p = malloc(na + nb + (size_t)slash + 1);
    if (!p) fail("malloc");
    memcpy(p, a, na);
    if (slash) p[na++] = '/';
    memcpy(p + na, b, nb + 1);
    return p;
}
static int compare_names(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}
static void walk(const char *root, const char *rel) {
    char *path = join(root, rel, 1);
    DIR *d = opendir(path);
    struct dirent *e;
    struct stat dir_st;
    char **names = NULL;
    size_t count = 0, i;
    if (!d) fail(path);
    if (fstat(dirfd(d), &dir_st)) fail(path);
    for (;;) {
        char **next;
        errno = 0;
        e = readdir(d);
        if (!e) {
            if (errno) fail(path);
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (dir_st.st_dev == output_dir_dev && dir_st.st_ino == output_dir_ino &&
            (!strcmp(e->d_name, output_name) || !strcmp(e->d_name, temp_name))) continue;
        if (count == SIZE_MAX / sizeof *names) invalid(path, "too many directory entries");
        next = realloc(names, (count + 1) * sizeof *names);
        if (!next) fail("realloc");
        names = next;
        names[count] = strdup(e->d_name);
        if (!names[count]) fail("strdup");
        count++;
    }
    if (closedir(d)) fail(path);
    free(path);
    if (count > 1) qsort(names, count, sizeof *names, compare_names);
    for (i = 0; i < count; i++) {
        char *r = join(rel, names[i], *rel != 0);
        char *f = join(root, r, 1);
        struct stat st;
        if (lstat(f, &st)) fail(f);
        if (S_ISDIR(st.st_mode)) { entry(r, st.st_mode, NULL, 0); walk(root, r); }
        else if (S_ISLNK(st.st_mode)) {
            size_t cap = 256;
            char *t;
            ssize_t n;
            for (;;) {
                t = malloc(cap);
                if (!t) fail("malloc");
                n = readlink(f, t, cap);
                if (n < 0) fail(f);
                if ((size_t)n < cap) break;
                free(t);
                if (cap > UINT32_MAX / 2 || cap > SIZE_MAX / 2) invalid(f, "symlink target too long");
                cap *= 2;
            }
            entry(r, st.st_mode, t, (uint32_t)n);
            free(t);
        } else if (S_ISREG(st.st_mode)) {
            size_t size;
            char *buf;
            FILE *in;
            if (st.st_size < 0 || (uintmax_t)st.st_size > UINT32_MAX || (uintmax_t)st.st_size > SIZE_MAX)
                invalid(f, "file too large for newc");
            size = (size_t)st.st_size;
            buf = malloc(size ? size : 1);
            if (!buf) fail("malloc");
            in = fopen(f, "rb");
            if (!in) fail(f);
            if (fread(buf, 1, size, in) != size) {
                if (ferror(in)) fail(f);
                invalid(f, "file shortened while reading");
            }
            if (fgetc(in) != EOF) invalid(f, "file grew while reading");
            if (ferror(in)) fail(f);
            if (fclose(in)) fail(f);
            entry(r, st.st_mode, buf, (uint32_t)size);
            free(buf);
        }
        free(f); free(r); free(names[i]);
    }
    free(names);
}
int main(int argc, char **argv) {
    char *parent;
    const char *slash;
    struct stat dir_st, old_st;
    mode_t mode, mask;
    int fd;
    if (argc != 3) { fprintf(stderr, "usage: mkcpio DIR OUT\n"); return 1; }
    output_path = argv[2];
    slash = strrchr(output_path, '/');
    output_name = slash ? slash + 1 : output_path;
    if (!*output_name) invalid(output_path, "output must name a file");
    if (slash) {
        size_t n = slash == output_path ? 1 : (size_t)(slash - output_path);
        parent = malloc(n + 1);
        if (!parent) fail("malloc");
        memcpy(parent, output_path, n);
        parent[n] = 0;
    } else {
        parent = strdup(".");
        if (!parent) fail("strdup");
    }
    if (stat(parent, &dir_st)) fail(parent);
    output_dir_dev = dir_st.st_dev;
    output_dir_ino = dir_st.st_ino;
    mask = umask(0);
    umask(mask);
    mode = 0666 & ~mask;
    if (!stat(output_path, &old_st) && S_ISREG(old_st.st_mode))
        mode = old_st.st_mode & 07777;
    temp_path = join(parent, ".mkcpio-XXXXXX", 1);
    free(parent);
    fd = mkstemp(temp_path);
    if (fd < 0) { temp_path = NULL; fail(output_path); }
    temp_name = strrchr(temp_path, '/') + 1;
    if (fchmod(fd, mode)) { close(fd); fail(temp_path); }
    out = fdopen(fd, "wb");
    if (!out) { close(fd); fail(temp_path); }
    walk(argv[1], "");
    entry("TRAILER!!!", 0, NULL, 0);
    if (fclose(out)) { out = NULL; fail(temp_path); }
    out = NULL;
    if (rename(temp_path, output_path)) fail(output_path);
    free(temp_path);
    return 0;
}
