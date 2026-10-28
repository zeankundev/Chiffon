#include "kernel.h"

#ifndef MAXNODES
#define MAXNODES 4096                      /* max files+dirs in the whole tree */
#endif
static struct node **nodes;
static uint32_t next_ino = 1;

struct node *node_find(const char *abs) {
    for (int i = 0; i < MAXNODES; i++)
        if (nodes[i] && !strcmp(nodes[i]->path, abs)) return nodes[i];
    return 0;
}
struct node *node_create(const char *abs, uint32_t mode) {
    int slot = -1;
    for (int i = 0; i < MAXNODES; i++) if (!nodes[i]) { slot = i; break; }
    if (slot < 0) return 0;
    struct node *n = kmalloc(sizeof *n);
    char *p = kmalloc(strlen(abs) + 1);
    if (!n || !p) { kfree(n); kfree(p); return 0; }
    memset(n, 0, sizeof *n);
    strlcpy(p, abs, strlen(abs) + 1);
    n->path = p; n->mode = mode; n->ino = next_ino++; n->mtime = 1700000000 + ticks / HZ;
    nodes[slot] = n;
    return n;
}
int node_unlink(struct node *n) {
    for (int i = 0; i < MAXNODES; i++)
        if (nodes[i] == n) {
            nodes[i] = 0;
            if (!n->is_static) kfree(n->data);
            kfree(n->path); kfree(n);
            return 0;
        }
    return -ENOENT;
}

static void pop_comp(char *res) {
    char *s = strrchr(res, '/');
    if (s == res) res[1] = 0; else *s = 0;
}

/* Canonicalise a path (cwd-relative or absolute), following symlinks. */
int vfs_resolve(const char *in, int follow_last, char *out) {
    char work[PATH_MAX * 2], res[PATH_MAX], nw[PATH_MAX * 2];
    int links = 0;
    if (!in[0]) return -ENOENT;
    if (in[0] == '/') strlcpy(work, in, sizeof work);
    else {
        strlcpy(work, cur ? cur->cwd : "/", sizeof work);
        strlcat(work, "/", sizeof work);
        strlcat(work, in, sizeof work);
    }
    strlcpy(res, "/", sizeof res);
    if (!strncmp(work, "/proc", 5)) procfs_refresh();
    const char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len == 1 && p[0] == '.') { p = e; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') { pop_comp(res); p = e; continue; }
        size_t rl = strlen(res);
        if (len > 200 || rl + len + 2 > PATH_MAX) return -ENAMETOOLONG;
        if (rl > 1) res[rl++] = '/';
        memcpy(res + rl, p, len); res[rl + len] = 0;
        p = e;
        const char *q = p; while (*q == '/') q++;
        int last = (*q == 0);
        struct node *n = node_find(res);
        if (n && S_ISLNK(n->mode) && (!last || follow_last)) {
            if (++links > 16) return -ELOOP;
            char tgt[PATH_MAX];
            size_t tl = n->size < sizeof tgt - 1 ? n->size : sizeof tgt - 1;
            memcpy(tgt, n->data, tl); tgt[tl] = 0;
            if (tgt[0] == '/') strlcpy(res, "/", sizeof res); else pop_comp(res);
            strlcpy(nw, tgt, sizeof nw); strlcat(nw, "/", sizeof nw); strlcat(nw, q, sizeof nw);
            strlcpy(work, nw, sizeof work);
            p = work;
            continue;
        }
        if (!last) {
            if (!n) return -ENOENT;
            if (!S_ISDIR(n->mode)) return -ENOTDIR;
        }
    }
    strlcpy(out, res, PATH_MAX);
    return 0;
}

/* ---- file contents ---- */
static int node_make_dynamic(struct node *n) {
    if (n->lazy) { int r = node_load(n); if (r) return r; }
    if (!n->is_static) return 0;
    size_t cap = n->size + 256;
    uint8_t *d = kmalloc(cap);
    if (!d) return -ENOMEM;
    memcpy(d, n->data, n->size);
    n->data = d; n->cap = cap; n->is_static = 0;
    return 0;
}
static ssize_t node_write(struct node *n, size_t off, const void *buf, size_t len) {
    int r = node_make_dynamic(n);
    if (r) return r;
    if (off + len > n->cap) {
        size_t nc = (off + len) * 2 + 256;
        uint8_t *d = krealloc(n->data, nc);
        if (!d) return -ENOMEM;
        n->data = d; n->cap = nc;
    }
    if (off > n->size) memset(n->data + n->size, 0, off - n->size);
    memcpy(n->data + off, buf, len);
    if (off + len > n->size) n->size = off + len;
    return len;
}

/* ---- pipes ---- */
struct pipe { uint8_t buf[4096]; size_t rd, wr; int readers, writers; };

int pipe_create(struct file **r, struct file **w) {
    struct pipe *p = kmalloc(sizeof *p);
    struct file *a = kmalloc(sizeof *a), *b = kmalloc(sizeof *b);
    if (!p || !a || !b) { kfree(p); kfree(a); kfree(b); return -ENOMEM; }
    memset(p, 0, sizeof *p); memset(a, 0, sizeof *a); memset(b, 0, sizeof *b);
    p->readers = p->writers = 1;
    a->type = F_PIPE_R; a->refs = 1; a->pipe = p;
    b->type = F_PIPE_W; b->refs = 1; b->pipe = p; b->flags = 1;
    *r = a; *w = b;
    return 0;
}
static ssize_t pipe_read(struct pipe *p, void *buf, size_t n) {
    uint8_t *b = buf;
    while (p->wr == p->rd) {
        if (!p->writers) return 0;
        if (cur->killed) return -EINTR;
        sleep_on(p);
    }
    size_t got = 0;
    while (got < n && p->rd != p->wr) b[got++] = p->buf[p->rd++ % sizeof p->buf];
    wakeup(p);
    return got;
}
static ssize_t pipe_write(struct pipe *p, const void *buf, size_t n) {
    const uint8_t *b = buf; size_t put = 0;
    while (put < n) {
        if (!p->readers) { proc_signal(cur, 13); return put ? (ssize_t)put : -EPIPE; }
        if (p->wr - p->rd == sizeof p->buf) {
            if (cur->killed) return -EINTR;
            wakeup(p); sleep_on(p); continue;
        }
        p->buf[p->wr++ % sizeof p->buf] = b[put++];
    }
    wakeup(p);
    return put;
}

/* ---- open files ---- */
struct file *file_dup(struct file *f) { f->refs++; return f; }
void file_close(struct file *f) {
    if (--f->refs > 0) return;
    if (f->pipe) {
        if (f->type == F_PIPE_R) f->pipe->readers--; else f->pipe->writers--;
        wakeup(f->pipe);
        if (!f->pipe->readers && !f->pipe->writers) kfree(f->pipe);
    }
    kfree(f);
}

int vfs_open(const char *path, int flags, int mode, struct file **out) {
    char abs[PATH_MAX];
    int r = vfs_resolve(path, 1, abs);
    if (r) return r;
    struct node *n = node_find(abs);
    if (!n) {
        if (!(flags & O_CREAT)) return -ENOENT;
        char par[PATH_MAX];
        strlcpy(par, abs, sizeof par); pop_comp(par);
        struct node *pn = node_find(par);
        if (!pn || !S_ISDIR(pn->mode)) return -ENOENT;
        n = node_create(abs, S_IFREG | (mode & ~(cur ? cur->umask : 022) & 0777));
        if (!n) return -ENOMEM;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (S_ISDIR(n->mode) && (flags & O_ACCMODE)) return -EISDIR;
    if ((flags & O_TRUNC) && (n->mode & S_IFMT) == S_IFREG) {
        if (n->is_static) { n->data = 0; n->cap = 0; n->is_static = 0; }
        n->lazy = 0;
        n->size = 0;
    }
    struct file *f = kmalloc(sizeof *f);
    if (!f) return -ENOMEM;
    memset(f, 0, sizeof *f);
    f->type = F_NODE; f->refs = 1; f->flags = flags; f->node = n;
    *out = f;
    return 0;
}

ssize_t file_read(struct file *f, void *buf, size_t n) {
    if (f->type == F_PIPE_R) return pipe_read(f->pipe, buf, n);
    if (f->type != F_NODE) return -EBADF;
    struct node *nd = f->node;
    if (S_ISDIR(nd->mode)) return -EISDIR;
    switch (nd->dev) {
    case DEV_NULL: return 0;
    case DEV_ZERO: memset(buf, 0, n); return n;
    case DEV_TTY: return tty_read(buf, n, f->flags & O_NONBLOCK);
    }
    if (nd->lazy && node_load(nd)) return -EIO;
    if (f->off >= nd->size) return 0;
    if (n > nd->size - f->off) n = nd->size - f->off;
    memcpy(buf, nd->data + f->off, n);
    f->off += n;
    return n;
}
ssize_t file_write(struct file *f, const void *buf, size_t n) {
    if (f->type == F_PIPE_W) return pipe_write(f->pipe, buf, n);
    if (f->type != F_NODE) return -EBADF;
    struct node *nd = f->node;
    if (S_ISDIR(nd->mode)) return -EISDIR;
    switch (nd->dev) {
    case DEV_NULL: case DEV_ZERO: return n;
    case DEV_TTY: return tty_write(buf, n);
    }
    if (f->flags & O_APPEND) f->off = nd->size;
    ssize_t r = node_write(nd, f->off, buf, n);
    if (r > 0) f->off += r;
    return r;
}
int file_readable(struct file *f) {
    if (f->type == F_PIPE_R) return f->pipe->rd != f->pipe->wr || !f->pipe->writers;
    if (f->type == F_NODE && f->node->dev == DEV_TTY) return tty_readable();
    return 1;
}

/* ---- directories ---- */
static const char *child_name(const char *dir, const char *path) {
    size_t dl = strlen(dir);
    const char *name;
    if (dl == 1) { if (path[0] != '/' || !path[1]) return 0; name = path + 1; }
    else {
        if (strncmp(path, dir, dl) || path[dl] != '/' || !path[dl + 1]) return 0;
        name = path + dl + 1;
    }
    for (const char *s = name; *s; s++) if (*s == '/') return 0;
    return name;
}
int file_getdents(struct file *f, void *buf, size_t n) {
    if (f->type != F_NODE || !S_ISDIR(f->node->mode)) return -ENOTDIR;
    uint8_t *out = buf; size_t used = 0;
    for (;;) {
        const char *name = 0; uint32_t mode = S_IFDIR, ino = 1;
        size_t idx = f->off;
        if (idx == 0) name = ".";
        else if (idx == 1) name = "..";
        else {
            size_t k = idx - 2;
            while (k < MAXNODES) {
                if (nodes[k] && (name = child_name(f->node->path, nodes[k]->path))) { mode = nodes[k]->mode; ino = nodes[k]->ino; break; }
                k++;
            }
            if (k >= MAXNODES) break;
            idx = k + 2;
        }
        size_t nl = strlen(name), reclen = (19 + nl + 1 + 7) & ~7UL;
        if (used + reclen > n) { if (!used) return -EINVAL; break; }
        uint8_t *d = out + used;
        memset(d, 0, reclen);
        *(uint64_t *)d = ino;
        *(int64_t *)(d + 8) = idx + 1;
        *(uint16_t *)(d + 16) = reclen;
        d[18] = S_ISDIR(mode) ? 4 : S_ISLNK(mode) ? 10 : (mode & S_IFMT) == S_IFCHR ? 2 : 8;
        memcpy(d + 19, name, nl);
        used += reclen;
        f->off = idx + 1;
    }
    return used;
}

/* ---- initramfs ---- */
static uint32_t hex8(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v = v << 4 | (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}
static void mkparents(const char *abs) {
    char tmp[PATH_MAX];
    strlcpy(tmp, abs, sizeof tmp);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (!node_find(tmp)) node_create(tmp, S_IFDIR | 0755);
            *s = '/';
        }
}
static void mkdev(const char *path, int dev) {
    struct node *n = node_find(path);
    if (!n) n = node_create(path, S_IFCHR | 0666);
    if (n) { n->dev = dev; n->mode = S_IFCHR | 0666; }
}

void vfs_init(void) {
    nodes = kmalloc(MAXNODES * sizeof *nodes);
    if (!nodes) panic("vfs: out of memory");
    memset(nodes, 0, MAXNODES * sizeof *nodes);
    node_create("/", S_IFDIR | 0755);
}

struct node *vfs_add(const char *abs, uint32_t mode, uint64_t size, uint32_t fs_ino, const struct fs_driver *fs) {
    struct node *n = node_find(abs);
    if (!n) n = node_create(abs, mode);
    if (!n) return 0;
    n->mode = mode; n->size = size; n->fs = fs; n->fs_ino = fs_ino;
    n->lazy = ((mode & S_IFMT) == S_IFREG) && size > 0;
    return n;
}
int node_load(struct node *n) {
    if (!n->lazy) return 0;
    uint8_t *b = kmalloc(n->size ? n->size : 1);
    if (!b) return -ENOMEM;
    if (n->fs->read(n, b)) { kfree(b); return -EIO; }
    n->data = b; n->cap = n->size; n->is_static = 0; n->lazy = 0;
    return 0;
}

void vfs_load_cpio(const void *cpio, size_t size) {
    const uint8_t *p = cpio, *end = p + size;
    int count = 0;
    while (p + 110 <= end && !memcmp(p, "070701", 6)) {
        uint32_t mode = hex8((const char *)p + 14), fsz = hex8((const char *)p + 54), nsz = hex8((const char *)p + 94);
        const char *name = (const char *)p + 110;
        const uint8_t *data = p + ((110 + nsz + 3) & ~3u);
        p = data + ((fsz + 3) & ~3u);
        if (!strcmp(name, "TRAILER!!!")) break;
        while (name[0] == '.' && name[1] == '/') name += 2;
        while (name[0] == '/') name++;
        if (!name[0] || !strcmp(name, ".")) continue;
        char abs[PATH_MAX];
        abs[0] = '/'; strlcpy(abs + 1, name, sizeof abs - 1);
        mkparents(abs);
        struct node *n = node_find(abs);
        if (!n) n = node_create(abs, mode);
        if (!n) continue;
        n->mode = mode;
        if ((mode & S_IFMT) == S_IFREG || S_ISLNK(mode)) { n->data = (uint8_t *)data; n->size = fsz; n->is_static = 1; }
        count++;
    }
    kprintf("vfs: initramfs: %d entries\n", count);
}

void vfs_finish(void) {
    mkdev("/dev/console", DEV_TTY); mkdev("/dev/tty", DEV_TTY); mkdev("/dev/ttyS0", DEV_TTY);
    mkdev("/dev/null", DEV_NULL); mkdev("/dev/zero", DEV_ZERO);
    if (!node_find("/dev")) node_create("/dev", S_IFDIR | 0755);
    if (!node_find("/tmp")) node_create("/tmp", S_IFDIR | 01777);
}

/* ---- extra namespace operations ---- */
int node_is_empty_dir(struct node *d) {
    for (int i = 0; i < MAXNODES; i++)
        if (nodes[i] && child_name(d->path, nodes[i]->path)) return 0;
    return 1;
}
int vfs_rename(const char *from, const char *to) {
    struct node *n = node_find(from);
    if (!n) return -ENOENT;
    if (!strcmp(from, to)) return 0;
    struct node *t = node_find(to);
    size_t fl = strlen(from);
    if (S_ISDIR(n->mode) && !strncmp(to, from, fl) && to[fl] == '/') return -EINVAL;
    if (t) {
        if (S_ISDIR(t->mode) != S_ISDIR(n->mode)) return S_ISDIR(t->mode) ? -EISDIR : -ENOTDIR;
        if (S_ISDIR(t->mode) && !node_is_empty_dir(t)) return -ENOTEMPTY;
        node_unlink(t);
    }
    for (int i = 0; i < MAXNODES; i++) {
        struct node *c = nodes[i];
        if (!c || (c != n && !(S_ISDIR(n->mode) && !strncmp(c->path, from, fl) && c->path[fl] == '/'))) continue;
        char *np = kmalloc(strlen(to) + strlen(c->path) - fl + 1);
        if (!np) return -ENOMEM;
        strlcpy(np, to, strlen(to) + 1);
        strlcat(np, c->path + fl, strlen(to) + strlen(c->path) - fl + 1);
        kfree(c->path); c->path = np;
    }
    return 0;
}
int vfs_symlink(const char *target, const char *abs) {
    if (node_find(abs)) return -EEXIST;
    struct node *n = node_create(abs, S_IFLNK | 0777);
    if (!n) return -ENOMEM;
    size_t l = strlen(target);
    n->data = kmalloc(l + 1); n->cap = l + 1;
    if (!n->data) { node_unlink(n); return -ENOMEM; }
    memcpy(n->data, target, l); n->size = l;
    return 0;
}
