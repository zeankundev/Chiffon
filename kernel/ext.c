#include "kernel.h"

static uint32_t le16(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
static uint32_t le32(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static struct {
    uint64_t base, *itable;
    uint32_t bs, ipg, isz, ngroups, incompat;
    uint8_t *blk, *xbuf;
} E;

struct einode { uint32_t mode, flags; uint64_t size; uint8_t blk[60]; };

#define INCOMPAT_SUPPORTED (0x2 | 0x4 | 0x40 | 0x80 | 0x200 | 0x2000 | 0x4000)

static int rdblk(uint64_t pblk, void *buf) {
    uint32_t spb = E.bs / 512;
    return board_blk_read(E.base + pblk * spb, buf, spb);
}

static int ext_probe(uint64_t base) {
    uint8_t sb[1024];
    if (board_blk_read(base + 2, sb, 2)) return -1;
    return le16(sb + 56) == 0xEF53 ? 0 : -1;
}

static int get_inode(uint32_t ino, struct einode *o) {
    if (!ino) return -EIO;
    uint32_t g = (ino - 1) / E.ipg, idx = (ino - 1) % E.ipg;
    if (g >= E.ngroups) return -EIO;
    uint64_t off = E.itable[g] * E.bs + (uint64_t)idx * E.isz;
    uint8_t t[1024];
    if (board_blk_read(E.base + off / 512, t, 2)) return -EIO;
    const uint8_t *p = t + off % 512;
    o->mode = le16(p); o->flags = le32(p + 32);
    o->size = le32(p + 4);
    if ((o->mode & S_IFMT) == S_IFREG) o->size |= (uint64_t)le32(p + 108) << 32;
    memcpy(o->blk, p + 40, 60);
    return 0;
}

static int ind_get(uint32_t blockno, uint32_t idx, uint32_t *out) {
    *out = 0;
    if (!blockno) return 0;
    if (rdblk(blockno, E.xbuf)) return -EIO;
    *out = le32(E.xbuf + 4 * idx);
    return 0;
}

static int bmap(const struct einode *in, uint64_t lblk, uint64_t *pblk) {
    *pblk = 0;
    if (in->flags & 0x80000) {
        const uint8_t *nd = in->blk;
        for (int guard = 0; guard < 8; guard++) {
            if (le16(nd) != 0xF30A) return -EIO;
            uint32_t ent = le16(nd + 2), depth = le16(nd + 6);
            if (depth == 0) {
                for (uint32_t i = 0; i < ent; i++) {
                    const uint8_t *e = nd + 12 + i * 12;
                    uint32_t start = le32(e), len = le16(e + 4);
                    if (len > 32768) len -= 32768;
                    if (lblk >= start && lblk < (uint64_t)start + len) {
                        *pblk = ((uint64_t)le16(e + 6) << 32 | le32(e + 8)) + (lblk - start);
                        return 0;
                    }
                }
                return 0;
            }
            const uint8_t *pick = 0;
            for (uint32_t i = 0; i < ent; i++) {
                const uint8_t *e = nd + 12 + i * 12;
                if (le32(e) <= lblk) pick = e; else break;
            }
            if (!pick) return 0;
            uint64_t child = le32(pick + 4) | (uint64_t)le16(pick + 8) << 32;
            if (rdblk(child, E.xbuf)) return -EIO;
            nd = E.xbuf;
        }
        return -EIO;
    }
    uint32_t ppb = E.bs / 4, v = 0, r;
    if (lblk < 12) { *pblk = le32(in->blk + 4 * lblk); return 0; }
    lblk -= 12;
    if (lblk < ppb) { if ((r = ind_get(le32(in->blk + 48), lblk, &v))) return r; *pblk = v; return 0; }
    lblk -= ppb;
    if (lblk < (uint64_t)ppb * ppb) {
        if ((r = ind_get(le32(in->blk + 52), lblk / ppb, &v))) return r;
        if ((r = ind_get(v, lblk % ppb, &v))) return r;
        *pblk = v; return 0;
    }
    lblk -= (uint64_t)ppb * ppb;
    if ((r = ind_get(le32(in->blk + 56), lblk / ((uint64_t)ppb * ppb), &v))) return r;
    if ((r = ind_get(v, (lblk / ppb) % ppb, &v))) return r;
    if ((r = ind_get(v, lblk % ppb, &v))) return r;
    *pblk = v;
    return 0;
}

static int read_range(const struct einode *in, uint64_t off, uint8_t *buf, size_t len) {
    while (len) {
        uint64_t pblk; size_t boff = off % E.bs, n = E.bs - boff;
        if (n > len) n = len;
        int r = bmap(in, off / E.bs, &pblk);
        if (r) return r;
        if (!pblk) memset(buf, 0, n);
        else if (boff == 0 && n == E.bs) { if (rdblk(pblk, buf)) return -EIO; }
        else { if (rdblk(pblk, E.blk)) return -EIO; memcpy(buf, E.blk + boff, n); }
        off += n; buf += n; len -= n;
    }
    return 0;
}

static int ext_read(struct node *n, void *buf) {
    struct einode in;
    if (get_inode(n->fs_ino, &in)) return -EIO;
    return read_range(&in, 0, buf, n->size);
}

static int truncated;

static void walk(uint32_t dino, const char *path, int depth) {
    struct einode di;
    if (depth > 20 || get_inode(dino, &di)) return;
    uint8_t *dbuf = kmalloc(E.bs);
    char *cp = kmalloc(PATH_MAX + 260);
    if (!dbuf || !cp) { kfree(dbuf); kfree(cp); return; }
    for (uint64_t off = 0; off < di.size; off += E.bs) {
        if (read_range(&di, off, dbuf, E.bs)) break;
        for (uint32_t p = 0; p + 8 <= E.bs;) {
            uint32_t ino = le32(dbuf + p), rl = le16(dbuf + p + 4), nl = dbuf[p + 6];
            if (rl < 8 || p + rl > E.bs) break;
            const char *name = (const char *)dbuf + p + 8;
            int dots = nl == 1 && name[0] == '.' ? 1 : nl == 2 && name[0] == '.' && name[1] == '.' ? 1 : 0;
            if (ino && nl && !dots && nl < 255 && strlen(path) + nl + 2 < PATH_MAX) {
                strlcpy(cp, path, PATH_MAX); strlcat(cp, "/", PATH_MAX);
                size_t l = strlen(cp); memcpy(cp + l, name, nl); cp[l + nl] = 0;
                struct einode ci;
                if (!get_inode(ino, &ci)) {
                    uint32_t t = ci.mode & S_IFMT;
                    if (t == S_IFLNK) {
                        char tgt[PATH_MAX]; size_t tl = ci.size < PATH_MAX - 1 ? ci.size : PATH_MAX - 1;
                        if (ci.size < 60 && !(ci.flags & 0x80000)) memcpy(tgt, ci.blk, tl);
                        else if (read_range(&ci, 0, (uint8_t *)tgt, tl)) tl = 0;
                        tgt[tl] = 0;
                        if (vfs_symlink(tgt, cp)) truncated = 1;
                    } else if (!vfs_add(cp, ci.mode, t == S_IFREG ? ci.size : 0, ino, &ext_driver)) truncated = 1;
                    else if (t == S_IFDIR) {
                        char *sub = kmalloc(strlen(cp) + 1);
                        if (sub) { strlcpy(sub, cp, strlen(cp) + 1); walk(ino, sub, depth + 1); kfree(sub); }
                    }
                }
            }
            p += rl;
        }
    }
    kfree(dbuf); kfree(cp);
}

static int ext_mount(uint64_t base) {
    uint8_t sb[1024];
    if (board_blk_read(base + 2, sb, 2)) return -EIO;
    E.base = base;
    E.bs = 1024u << le32(sb + 24);
    uint32_t bpg = le32(sb + 32), rev = le32(sb + 76), first = le32(sb + 20);
    E.ipg = le32(sb + 40);
    E.isz = rev ? le16(sb + 88) : 128;
    E.incompat = rev ? le32(sb + 96) : 0;
    int is64 = E.incompat & 0x80;
    uint32_t dsz = is64 ? le16(sb + 254) : 32;
    if (!dsz) dsz = 64;
    uint64_t blocks = le32(sb + 4) | (is64 ? (uint64_t)le32(sb + 0x150) << 32 : 0);
    if (E.bs > 65536 || !bpg || !E.ipg || E.isz < 128 || E.isz > 1024) { kprintf("[ext] bad superblock\n"); return -EINVAL; }
    if (E.incompat & ~INCOMPAT_SUPPORTED) { kprintf("[ext] unsupported incompat features 0x%x\n", E.incompat & ~INCOMPAT_SUPPORTED); return -EINVAL; }
    if (E.incompat & 0x4) kprintf("[ext] warning: journal needs recovery, showing on-disk state\n");
    E.ngroups = (blocks - first + bpg - 1) / bpg;
    E.itable = kmalloc(E.ngroups * 8);
    E.blk = kmalloc(E.bs); E.xbuf = kmalloc(E.bs);
    uint32_t gdblocks = (E.ngroups * dsz + E.bs - 1) / E.bs;
    uint8_t *gd = kmalloc(gdblocks * E.bs);
    if (!E.itable || !E.blk || !E.xbuf || !gd) return -ENOMEM;
    for (uint32_t i = 0; i < gdblocks; i++)
        if (rdblk(first + 1 + i, gd + i * E.bs)) return -EIO;
    for (uint32_t g = 0; g < E.ngroups; g++) {
        const uint8_t *d = gd + g * dsz;
        E.itable[g] = le32(d + 8) | (is64 && dsz >= 64 ? (uint64_t)le32(d + 0x28) << 32 : 0);
    }
    kfree(gd);
    kprintf("ext: block size %u, %u groups, inode size %u, features 0x%x\n", E.bs, E.ngroups, E.isz, E.incompat);
    truncated = 0;
    walk(2, "", 0);
    if (truncated) kprintf("ext: warning: tree larger than the VFS node table; some files are missing\n");
    return 0;
}

const struct fs_driver ext_driver = { "ext2/3/4", ext_probe, ext_mount, ext_read };