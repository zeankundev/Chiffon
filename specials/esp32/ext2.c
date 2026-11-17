/* Read-only ext2/3/4 on the ESP32's flash partition.  Covers the parts the boot shell needs: the
 * superblock, group descriptors, inodes, directory entries, extent trees (ext4) and classic
 * direct/indirect block maps (ext2/3), with any block size up to 4 KiB.  Not covered: writing,
 * the journal, triple-indirect blocks, and checksums (they are not checked). */
#include "kernel.h"

#ifndef EISDIR
#define EISDIR 21
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

int flash_read(uint32_t off, void *buf, size_t n);

#define EXT_SUPER_OFF  1024u
#define EXT_MAGIC      0xEF53
#define EXT_INCOMPAT_64BIT 0x80
#define EXT_FLAG_EXTENTS   0x80000
#define EXT_S_IFMT   0xF000
#define EXT_S_IFDIR  0x4000
#define EXT_S_IFREG  0x8000
#define EXT_S_IFLNK  0xA000

static struct {
    uint32_t part;                 /* flash offset of the filesystem */
    uint32_t bs, ipg, bpg, nblocks, first_data, inode_size, desc_size, nbgroups;
    uint32_t incompat;
    uint8_t *blk;                  /* scratch block: directory blocks, indirect and extent nodes */
    uint8_t *blk2;                 /* second scratch block (double-indirect) */
    int ready;
} E;

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int rd(uint64_t off, void *buf, size_t n) { return flash_read(E.part + (uint32_t)off, buf, n); }
static int rdblk(uint32_t blk, void *buf) { return rd((uint64_t)blk * E.bs, buf, E.bs); }

struct inode {
    uint32_t mode, size, flags;
    uint8_t iblock[60];            /* the 15 block pointers, or the extent tree root */
};

int ext_init(uint32_t part) {
    uint8_t sb[1024];
    E.part = part;
    if (rd(EXT_SUPER_OFF, sb, sizeof sb)) return -EIO;
    if (le16(sb + 56) != EXT_MAGIC) return -EINVAL;
    uint32_t log_bs = le32(sb + 24);
    if (log_bs > 2) return -EINVAL;                       /* block sizes 1, 2 and 4 KiB */
    E.bs = 1024u << log_bs;
    E.nblocks = le32(sb + 4);
    E.bpg = le32(sb + 32);
    E.ipg = le32(sb + 40);
    E.first_data = le32(sb + 20);
    uint32_t rev = le32(sb + 76);
    E.inode_size = rev >= 1 ? le16(sb + 88) : 128;
    E.incompat = le32(sb + 96);
    E.desc_size = (E.incompat & EXT_INCOMPAT_64BIT) ? le16(sb + 254) : 32;
    if (E.desc_size < 32 || E.desc_size > 64 || !E.bpg || !E.ipg || E.inode_size < 128 || E.inode_size > 256) return -EINVAL;
    E.nbgroups = (E.nblocks - E.first_data + E.bpg - 1) / E.bpg;
    if (!E.blk) { E.blk = kmalloc(E.bs); E.blk2 = kmalloc(E.bs); }
    if (!E.blk || !E.blk2) return -ENOMEM;
    E.ready = 1;
    return 0;
}

static int get_inode(uint32_t n, struct inode *in) {
    if (!n || n > E.ipg * E.nbgroups) return -EINVAL;
    uint32_t g = (n - 1) / E.ipg, idx = (n - 1) % E.ipg;
    uint8_t desc[64];
    uint64_t desc_off = (uint64_t)(E.first_data + 1) * E.bs + (uint64_t)g * E.desc_size;
    if (rd(desc_off, desc, E.desc_size)) return -EIO;
    uint32_t table = le32(desc + 8);
    uint8_t raw[128];
    if (rd((uint64_t)table * E.bs + (uint64_t)idx * E.inode_size, raw, sizeof raw)) return -EIO;
    in->mode = le16(raw);
    in->size = le32(raw + 4);
    in->flags = le32(raw + 32);
    for (int i = 0; i < 60; i++) in->iblock[i] = raw[40 + i];
    return 0;
}

/* Walk an extent tree node (a block or the inode's own root) for logical block lb. */
static int extent_find(const uint8_t *node, uint32_t lb, uint32_t *pb, int depth_left) {
    if (le16(node) != 0xF30A || depth_left < 0) return -EIO;
    uint32_t n = le16(node + 2), depth = le16(node + 6);
    if (depth == 0) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = node + 12 + 12 * i;
            uint32_t start = le32(e), len = le16(e + 4);
            uint64_t phys = le32(e + 8) | (uint64_t)le16(e + 6) << 32;
            if (len > 32768) len -= 32768;                 /* unwritten extent: still maps */
            if (lb >= start && lb - start < len) { *pb = (uint32_t)(phys + (lb - start)); return 0; }
        }
        return -ENOENT;                                    /* a hole */
    }
    const uint8_t *pick = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = node + 12 + 12 * i;
        if (le32(e) <= lb) pick = e;
    }
    if (!pick) return -ENOENT;
    uint32_t child = le32(pick + 4);
    if (rdblk(child, E.blk)) return -EIO;
    return extent_find(E.blk, lb, pb, depth_left - 1);
}

/* Physical block of file block lb.  0 = hole. */
static int bmap(const struct inode *in, uint32_t lb, uint32_t *pb) {
    *pb = 0;
    if (in->flags & EXT_FLAG_EXTENTS) {
        int r = extent_find(in->iblock, lb, pb, 5);
        return r == -ENOENT ? 0 : r;
    }
    uint32_t ppb = E.bs / 4;
    const uint8_t *ib = in->iblock;
    if (lb < 12) { *pb = le32(ib + 4 * lb); return 0; }
    lb -= 12;
    if (lb < ppb) {
        uint32_t ind = le32(ib + 48);
        if (!ind) return 0;
        if (rdblk(ind, E.blk)) return -EIO;
        *pb = le32(E.blk + 4 * lb);
        return 0;
    }
    lb -= ppb;
    if (lb < (uint64_t)ppb * ppb) {
        uint32_t dind = le32(ib + 52);
        if (!dind) return 0;
        if (rdblk(dind, E.blk)) return -EIO;
        uint32_t ind = le32(E.blk + 4 * (lb / ppb));
        if (!ind) return 0;
        if (rdblk(ind, E.blk2)) return -EIO;
        *pb = le32(E.blk2 + 4 * (lb % ppb));
        return 0;
    }
    return -EFBIG;
}

/* Read up to n bytes of a file starting at off.  Returns the byte count (0 at end). */
static long ext_file_read(const struct inode *in, uint64_t off, uint8_t *buf, size_t n) {
    if (off >= in->size) return 0;
    if (n > in->size - off) n = in->size - off;
    size_t done = 0;
    while (done < n) {
        uint32_t lb = (uint32_t)((off + done) / E.bs), within = (uint32_t)((off + done) % E.bs);
        uint32_t pb;
        if (bmap(in, lb, &pb)) return -EIO;
        size_t chunk = E.bs - within;
        if (chunk > n - done) chunk = n - done;
        if (!pb) memset(buf + done, 0, chunk);
        else if (rd((uint64_t)pb * E.bs + within, buf + done, chunk)) return -EIO;
        done += chunk;
    }
    return (long)done;
}

/* Look up name in directory dir.  On success fills *ino. */
static int dir_lookup(uint32_t dir, const char *name, uint32_t *ino) {
    struct inode d;
    if (get_inode(dir, &d)) return -EIO;
    if ((d.mode & EXT_S_IFMT) != EXT_S_IFDIR) return -ENOTDIR;
    size_t nl = strlen(name);
    for (uint64_t off = 0; off < d.size; off += E.bs) {
        long got = ext_file_read(&d, off, E.blk, E.bs);
        if (got <= 0) return got < 0 ? (int)got : -ENOENT;
        for (uint32_t p = 0; p + 8 <= (uint32_t)got;) {
            uint32_t i = le32(E.blk + p), rl = le16(E.blk + p + 4), nlen = E.blk[p + 6];
            if (rl < 8 || p + rl > (uint32_t)got) break;
            if (i && nlen == nl && !memcmp(E.blk + p + 8, name, nl)) { *ino = i; return 0; }
            p += rl;
        }
    }
    return -ENOENT;
}

/* Resolve a path to an inode number.  Relative paths are taken from the root (the shell
 * resolves them against its working directory first).  ".." walks back up the path. */
int ext_resolve(const char *path, uint32_t *ino) {
    if (!E.ready) return -ENODEV;
    uint32_t stack[32];
    int depth = 0;
    uint32_t node = 2;                                       /* root */
    char comp[256];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        size_t l = 0;
        while (path[l] && path[l] != '/') { if (l + 1 >= sizeof comp) return -ENAMETOOLONG; comp[l] = path[l]; l++; }
        comp[l] = 0;
        path += l;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            if (depth > 0) node = stack[--depth];
            continue;
        }
        if (depth >= 32) return -ENAMETOOLONG;
        stack[depth] = node;
        int r = dir_lookup(node, comp, &node);
        if (r) return r;
        depth++;
    }
    *ino = node;
    return 0;
}

/* Type and size of an inode, for ls and cat. */
int ext_stat(const char *path, uint32_t *mode, uint32_t *size, uint32_t *ino) {
    uint32_t n;
    int r = ext_resolve(path, &n);
    if (r) return r;
    struct inode in;
    if (get_inode(n, &in)) return -EIO;
    *mode = in.mode; *size = in.size; *ino = n;
    return 0;
}

/* Print a directory listing, or a file's name when path is a file. */
int ext_list(const char *path, void (*emit)(const char *name, uint32_t mode, uint32_t size)) {
    uint32_t n;
    int r = ext_resolve(path, &n);
    if (r) return r;
    struct inode d;
    if (get_inode(n, &d)) return -EIO;
    if ((d.mode & EXT_S_IFMT) != EXT_S_IFDIR) return -ENOTDIR;
    for (uint64_t off = 0; off < d.size; off += E.bs) {
        long got = ext_file_read(&d, off, E.blk, E.bs);
        if (got <= 0) break;
        for (uint32_t p = 0; p + 8 <= (uint32_t)got;) {
            uint32_t i = le32(E.blk + p), rl = le16(E.blk + p + 4), nlen = E.blk[p + 6];
            if (rl < 8 || p + rl > (uint32_t)got) break;
            if (i && nlen) {
                char name[256];
                memcpy(name, E.blk + p + 8, nlen); name[nlen] = 0;
                struct inode c;
                uint32_t mode = 0, size = 0;
                if (!get_inode(i, &c)) { mode = c.mode; size = c.size; }
                emit(name, mode, size);
            }
            p += rl;
        }
    }
    return 0;
}

/* Read a whole file into buf (at most cap bytes).  Returns the number of bytes read. */
long ext_read_file(const char *path, uint8_t *buf, size_t cap, uint32_t *size) {
    uint32_t n;
    int r = ext_resolve(path, &n);
    if (r) return r;
    struct inode in;
    if (get_inode(n, &in)) return -EIO;
    if ((in.mode & EXT_S_IFMT) == EXT_S_IFDIR) return -EISDIR;
    *size = in.size;
    return ext_file_read(&in, 0, buf, cap);
}

/* Open a file for streaming reads: its inode number and size. */
int ext_open(const char *path, uint32_t *ino, uint32_t *size) {
    uint32_t n;
    int r = ext_resolve(path, &n);
    if (r) return r;
    struct inode in;
    if (get_inode(n, &in)) return -EIO;
    if ((in.mode & EXT_S_IFMT) == EXT_S_IFDIR) return -EISDIR;
    *ino = n; *size = in.size;
    return 0;
}
/* Read n bytes at off from an inode opened with ext_open.  Returns the count (0 at end). */
long ext_read_ino(uint32_t ino, uint64_t off, uint8_t *buf, size_t n) {
    struct inode in;
    if (get_inode(ino, &in)) return -EIO;
    return ext_file_read(&in, off, buf, n);
}
