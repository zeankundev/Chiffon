#include "kernel.h"

static uint32_t le16(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8; }
static uint32_t le32(const void *p) { const uint8_t *b = p; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static struct {
    uint64_t base, blocks, *itable;
    uint32_t bs, ipg, isz, ngroups, incompat;
    uint8_t *blk, *xbuf;
} E;

struct einode { uint32_t mode, flags; uint64_t size; uint8_t blk[60]; };

/* Directory file types, extents, 64-bit block numbers, flex_bg and large directories. */
#define INCOMPAT_SUPPORTED (0x2 | 0x4 | 0x40 | 0x80 | 0x200 | 0x2000 | 0x4000)

static int rdblk(uint64_t pblk, void *buf) {
    uint32_t spb = E.bs / 512;
    if (pblk >= E.blocks) return -EIO;
    return board_blk_read(E.base + pblk * spb, buf, spb);
}

static int get_inode(uint32_t ino, struct einode *o) {
    if (!ino) return -EIO;
    uint32_t g = (ino - 1) / E.ipg, idx = (ino - 1) % E.ipg;
    if (g >= E.ngroups) return -EIO;
    uint64_t off = E.itable[g] * E.bs + (uint64_t)idx * E.isz;
    uint8_t t[512];
    if (off / E.bs >= E.blocks || board_blk_read(E.base + off / 512, t, 1)) return -EIO;
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
            if (ent > ((guard ? E.bs : sizeof in->blk) - 12) / 12) return -EIO;
            if (depth == 0) {
                for (uint32_t i = 0; i < ent; i++) {
                    const uint8_t *e = nd + 12 + i * 12;
                    uint32_t start = le32(e), len = le16(e + 4);
                    int unwritten = len > 32768;
                    if (unwritten) len -= 32768;
                    if (lblk >= start && lblk < (uint64_t)start + len) {
                        if (!unwritten)
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
    if (lblk >= (uint64_t)ppb * ppb * ppb) return -EIO;
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

#define EXT_WALK_MAX_DEPTH 20
#define EXT_WALK_MAX_BLOCKS 65536
#define EXT_WALK_MAX_ENTRIES 65536
struct walk_state {
    uint32_t blocks, entries, ancestors[EXT_WALK_MAX_DEPTH + 1];
};

static int walk(uint32_t dino, const char *path, int depth, struct walk_state *state) {
    struct einode *in = kmalloc(2 * sizeof *in);
    int err = -EIO;
    if (!in) return -ENOMEM;
    if (depth > EXT_WALK_MAX_DEPTH) { kfree(in); return -EIO; }
    for (int i = 0; i < depth; i++)
        if (state->ancestors[i] == dino) { kfree(in); return -EIO; }
    state->ancestors[depth] = dino;
    if (get_inode(dino, &in[0]) || (in[0].mode & S_IFMT) != S_IFDIR || in[0].size % E.bs) { kfree(in); return -EIO; }
    uint8_t *dbuf = kmalloc(E.bs);
    char *cp = kmalloc(PATH_MAX);
    if (!dbuf || !cp) { err = -ENOMEM; goto out; }
    for (uint64_t off = 0; off < in[0].size; off += E.bs) {
        if (state->blocks++ >= EXT_WALK_MAX_BLOCKS || read_range(&in[0], off, dbuf, E.bs)) goto out;
        for (uint32_t p = 0; p < E.bs;) {
            if (E.bs - p < 8 || state->entries++ >= EXT_WALK_MAX_ENTRIES) goto out;
            uint32_t ino = le32(dbuf + p), rl = le16(dbuf + p + 4), nl = dbuf[p + 6];
            if (rl < 8 || (rl & 3) || rl > E.bs - p || nl > rl - 8) goto out;
            const char *name = (const char *)dbuf + p + 8;
            int dots = nl == 1 && name[0] == '.' ? 1 : nl == 2 && name[0] == '.' && name[1] == '.' ? 1 : 0;
            int valid = 1;
            for (uint32_t j = 0; j < nl; j++)
                if (!name[j] || name[j] == '/') { valid = 0; break; }
            if (ino && (!nl || !valid)) goto out;
            if (ino && !dots) {
                if (strlen(path) + nl + 2 > PATH_MAX) { err = -ENAMETOOLONG; goto out; }
                strlcpy(cp, path, PATH_MAX); strlcat(cp, "/", PATH_MAX);
                size_t l = strlen(cp); memcpy(cp + l, name, nl); cp[l + nl] = 0;
                struct einode *ci = &in[1];
                if (get_inode(ino, ci)) goto out;
                {
                    uint32_t t = ci->mode & S_IFMT;
                    if (t == S_IFLNK) {
                        if (ci->size >= PATH_MAX) { err = -ENAMETOOLONG; goto out; }
                        char *tgt = kmalloc(PATH_MAX);
                        if (!tgt) { err = -ENOMEM; goto out; }
                        size_t tl = ci->size;
                        if (ci->size < 60 && !(ci->flags & 0x80000)) memcpy(tgt, ci->blk, tl);
                        else if (read_range(ci, 0, (uint8_t *)tgt, tl)) { kfree(tgt); goto out; }
                        tgt[tl] = 0;
                        int r = vfs_symlink(tgt, cp);
                        kfree(tgt);
                        if (r) { err = r; goto out; }
                    } else {
                        if (!vfs_add(cp, ci->mode, t == S_IFREG ? ci->size : 0, ino, &ext_driver)) { err = -ENOMEM; goto out; }
                        if (t == S_IFDIR) {
                            err = walk(ino, cp, depth + 1, state);
                            if (err) goto out;
                            err = -EIO;
                        }
                    }
                }
            }
            p += rl;
        }
    }
    err = 0;
out:
    kfree(dbuf); kfree(cp); kfree(in);
    return err;
}

static int ext_mount(uint64_t base, uint64_t limit, uint64_t *end, uint64_t *fsid) {
    uint8_t sb[1024];
    if (base > limit || limit - base < 4) return -EINVAL;
    if (board_blk_read(base + 2, sb, 2)) return -EIO;
    if (le16(sb + 56) != 0xEF53) return -ENOENT;
    /* No ext journal replay, orphan cleanup or in-inode directory reader. */
    if (le16(sb + 58) != 1 || le32(sb + 232) ||
        (le32(sb + 100) & 0x10000)) {
        kprintf("[ext] volume requires offline recovery\n");
        return -EINVAL;
    }
    E.base = base;
    if (le32(sb + 24) > 6) return -EINVAL;
    E.bs = 1024u << le32(sb + 24);
    uint32_t bpg = le32(sb + 32), rev = le32(sb + 76), first = le32(sb + 20);
    E.ipg = le32(sb + 40);
    E.isz = rev ? le16(sb + 88) : 128;
    E.incompat = rev ? le32(sb + 96) : 0;
    int is64 = E.incompat & 0x80;
    uint32_t dsz = is64 ? le16(sb + 254) : 32;
    uint64_t blocks = le32(sb + 4) | (is64 ? (uint64_t)le32(sb + 0x150) << 32 : 0);
    if (!bpg || !E.ipg || E.isz < 128 || E.isz > 1024 || (E.isz & (E.isz - 1)) ||
        first != (E.bs == 1024) || blocks <= first ||
        blocks > (limit - base) / (E.bs / 512) || blocks > UINT64_MAX / E.bs ||
        dsz < (is64 ? 64u : 32u) || dsz > E.bs || E.bs % dsz) return -EINVAL;
    E.blocks = blocks;
    if (E.incompat & ~INCOMPAT_SUPPORTED) { kprintf("[ext] unsupported incompat features 0x%x\n", E.incompat & ~INCOMPAT_SUPPORTED); return -EINVAL; }
    uint64_t groups = 1 + (blocks - first - 1) / bpg;
    if (groups > UINT32_MAX) return -EINVAL;
    E.ngroups = groups;
    E.itable = kmalloc(groups * sizeof *E.itable);
    E.blk = kmalloc(E.bs); E.xbuf = kmalloc(E.bs);
    int err = -ENOMEM;
    if (!E.itable || !E.blk || !E.xbuf) goto fail;
    err = -EIO;
    for (uint32_t g = 0; g < E.ngroups; g++) {
        uint64_t off = (uint64_t)g * dsz;
        if (!(off % E.bs) && rdblk(first + 1 + off / E.bs, E.blk)) goto fail;
        const uint8_t *d = E.blk + off % E.bs;
        E.itable[g] = le32(d + 8) | (is64 && dsz >= 64 ? (uint64_t)le32(d + 0x28) << 32 : 0);
        uint64_t table_blocks = ((uint64_t)E.ipg * E.isz + E.bs - 1) / E.bs;
        if (!E.itable[g] || E.itable[g] >= blocks || table_blocks > blocks - E.itable[g]) goto fail;
    }
    kprintf("ext: block size %u, %u groups, inode size %u, features 0x%x\n", E.bs, E.ngroups, E.isz, E.incompat);
    struct walk_state state = {0};
    struct vfs_mount_snapshot *snapshot = vfs_mount_snapshot();
    if (!snapshot) { err = -ENOMEM; goto fail; }
    err = walk(2, "", 0, &state);
    vfs_mount_finish(snapshot, err != 0);
    if (err) goto fail;
    *end = base + blocks * (E.bs / 512);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 16; i++) { h ^= sb[104 + i]; h *= 0x100000001b3ULL; }
    *fsid = h;
    return 0;
fail:
    kfree(E.itable); kfree(E.blk); kfree(E.xbuf);
    return err;
}

const struct fs_driver ext_driver = { .name = "ext2/3/4", .mount = ext_mount, .read = ext_read };
