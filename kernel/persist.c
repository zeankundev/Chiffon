/* persist.c -- changes to the root filesystem that survive a reboot.
 *
 * The ext filesystem stays read-only.  Every change made through the file syscalls
 * (create, write, truncate, mkdir, unlink, rmdir, rename, symlink, chmod) is recorded
 * in an append-only log that lives in the last PST_RESERVE_SECTORS sectors of the same
 * disk.  At boot the log is replayed over the filesystem tree, so the tree looks the
 * way it was left.  `make` rebuilds the disk image, which zeroes that area, so a rebuilt
 * environment starts clean.
 *
 * Disk layout, relative to region = device_sectors - PST_RESERVE_SECTORS:
 *   slot 0 and slot 1 (PST_SLOT_SECTORS each).  Sector 0 of a slot is its header,
 *   the rest is a stream of records.  The newest valid header wins; the other slot is
 *   the compaction target.
 *
 * Header (512 bytes, little endian):
 *   "CHIFLOG2" | gen u64 | fsid u64 | 8 zero bytes | crc32 u32 of the first 32 bytes
 *
 * Record (24-byte header, then path, then data; all little endian):
 *   magic u32 | type u32 | mode u32 | plen u32 | dlen u32 | crc32 u32
 *   crc32 covers the first 20 header bytes, the path and the data.
 *   Records start on sector boundaries. The next record's first sector is cleared
 *   before writing the body and publishing the first sector of the current record.
 *   A record with a bad magic or CRC ends the log. CHIFLOG1 packs records without
 *   alignment and is replayed read-only before migrating to the other slot.
 *
 * Record types:
 *   PUT    path, mode, data : create or replace a whole node (file contents or symlink target)
 *   DEL    path             : remove a node
 *   RENAME path, data=to    : rename (replayed through vfs_rename)
 *   CHMOD  path, mode       : change permission bits only
 *
 * Base tree: a node that is still unchanged since the image was mounted has base=1 and
 * is never written to the log.  Removing a base path leaves a tombstone so that replay
 * does not bring it back.  Compaction writes DEL records for all tombstones, then a PUT for
 * every changed node, into the other slot, and commits by writing that slot's header with
 * gen+1.  Until that header is written the old slot still wins, so a crash during compaction
 * loses nothing.
 *
 * Only /proc and /dev are excluded.  They are generated at boot.  /tmp persists like the
 * rest of the tree.
 *
 * The fsid is a hash of the ext UUID.  A log from a different filesystem is never replayed;
 * persistence is then disabled and a message says to run make.
 */
#include "kernel.h"

#ifdef CHI_TINY
#define PST_RESERVE_SECTORS 256u                        /* 128 KiB; tiny profile: mkimage PERSIST_KB=128 */
#define PST_SLOT_SECTORS    128u                        /* 64 KiB per slot: read in one heap block at attach */
#else
#define PST_RESERVE_SECTORS 8192u                       /* 4 MiB; tools/mkimage.sh keeps this much free */
#define PST_SLOT_SECTORS    4096u                       /* 2 MiB per slot */
#endif
#define PST_SLOT_BYTES      ((PST_SLOT_SECTORS - 1) * 512u)
#define PST_HDR_LEGACY      "CHIFLOG1"
#define PST_HDR_MAGIC       "CHIFLOG2"
#define PST_SECTOR_BYTES    512u
#define REC_MAGIC           0xC1F0C0DEu
#define REC_HDR             24u

enum { REC_PUT = 1, REC_DEL, REC_RENAME, REC_CHMOD };

static struct {
    int on;               /* log is attached: changes are recorded */
    int replaying;        /* applying the log at boot: do not record */
    int error;            /* persistence unavailable or replay/tracking failed */
    int snapshot;         /* recover a failed append or migrate the legacy slot */
    uint64_t region;      /* first device sector of the reserved area */
    uint64_t fsid;
    uint64_t gen;         /* generation of the active slot */
    int active;           /* 0 or 1 */
    uint32_t end;         /* byte offset in the active slot's record area */
} P;

struct tomb { struct tomb *next; char *path; };
static struct tomb *tombs;

/* ---- little helpers ---- */

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc32(uint32_t crc, const void *buf, size_t n) {
    const uint8_t *b = buf;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int under(const char *p, const char *dir) {
    size_t n = strlen(dir);
    return !strncmp(p, dir, n) && (p[n] == 0 || p[n] == '/');
}

/* Only ordinary tree paths are persisted. */
static int persistable(const char *p) {
    return p && p[0] == '/' && p[1] && !under(p, "/proc") && !under(p, "/dev");
}

/* ---- tombstones: base paths that must not come back on replay ---- */

static int tomb_add(const char *p) {
    for (struct tomb *t = tombs; t; t = t->next)
        if (!strcmp(t->path, p)) return 0;
    struct tomb *t = kmalloc(sizeof *t);
    char *s = kmalloc(strlen(p) + 1);
    if (!t || !s) { kfree(t); kfree(s); return -ENOMEM; }
    strlcpy(s, p, strlen(p) + 1);
    t->path = s; t->next = tombs; tombs = t;
    return 0;
}

static void tomb_del(const char *p) {
    for (struct tomb **pp = &tombs; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->path, p)) {
            struct tomb *t = *pp;
            *pp = t->next;
            kfree(t->path); kfree(t);
            return;
        }
}

/* ---- device access ---- */

static uint64_t slot_lba(int slot) { return P.region + (uint64_t)slot * PST_SLOT_SECTORS; }

static int clear_sector(uint64_t lba) {
    uint8_t zero[PST_SECTOR_BYTES] = { 0 };
    return board_blk_write(lba, zero, 1) ? -EIO : 0;
}

static int hdr_read(int slot, uint64_t *gen, uint64_t *fsid, int *nonempty) {
    uint8_t s[512];
    if (board_blk_read(slot_lba(slot), s, 1)) return -EIO;
    for (unsigned i = 0; i < sizeof s; i++)
        if (s[i]) { *nonempty = 1; break; }
    int version = !memcmp(s, PST_HDR_MAGIC, 8) ? 2 : !memcmp(s, PST_HDR_LEGACY, 8) ? 1 : 0;
    if (!version || crc32(0, s, 32) != get32(s + 32)) return 0;
    *gen = get64(s + 8);
    *fsid = get64(s + 16);
    return version;
}

static int hdr_write(int slot, uint64_t gen, uint64_t fsid) {
    uint8_t s[512];
    memset(s, 0, sizeof s);
    memcpy(s, PST_HDR_MAGIC, 8);
    put64(s + 8, gen);
    put64(s + 16, fsid);
    put32(s + 32, crc32(0, s, 32));
    return board_blk_write(slot_lba(slot), s, 1) ? -EIO : 0;
}

/* ---- records ---- */

static int rec_put(int slot, uint32_t *pos, uint32_t type, uint32_t mode,
                   const char *path, const void *data, uint32_t dlen) {
    uint32_t plen = (uint32_t)strlen(path);
    uint64_t total = REC_HDR + (uint64_t)plen + dlen;
    if ((uint64_t)*pos + total > PST_SLOT_BYTES) return -ENOSPC;
    uint32_t bytes = ((uint32_t)total + PST_SECTOR_BYTES - 1) & ~(PST_SECTOR_BYTES - 1);
    uint32_t sectors = bytes / PST_SECTOR_BYTES;
    uint64_t first = slot_lba(slot) + 1 + *pos / PST_SECTOR_BYTES;
    uint8_t *b = kmalloc(bytes);
    if (!b) return -ENOMEM;
    put32(b, REC_MAGIC); put32(b + 4, type); put32(b + 8, mode);
    put32(b + 12, plen); put32(b + 16, dlen);
    memcpy(b + REC_HDR, path, plen);
    if (dlen) memcpy(b + REC_HDR + plen, data, dlen);
    put32(b + 20, crc32(crc32(0, b, 20), b + REC_HDR, plen + dlen));
    memset(b + total, 0, bytes - total);
    int r = 0;
    if (*pos + bytes < PST_SLOT_BYTES) r = clear_sector(first + sectors);
    if (!r && sectors > 1 && board_blk_write(first + 1, b + PST_SECTOR_BYTES, sectors - 1))
        r = -EIO;
    if (!r && board_blk_write(first, b, 1)) r = -EIO;
    kfree(b);
    if (!r) *pos += bytes;
    return r;
}

static int payload(struct node *n, const void **d, uint32_t *len) {
    *d = 0; *len = 0;
    if ((n->mode & S_IFMT) == S_IFREG || S_ISLNK(n->mode)) {
        if (n->size > PST_SLOT_BYTES) return -ENOSPC;
        if (n->lazy && node_load(n)) return -EIO;
        *d = n->data;
        *len = (uint32_t)n->size;
    }
    return 0;
}

struct snap { int slot; uint32_t pos; };

/* Compaction: every changed node (base == 0) as a PUT into the target slot. */
static int snap_node(struct node *n, void *c) {
    struct snap *s = c;
    if (n->base || n->dev || !persistable(n->path)) return 0;
    const void *d; uint32_t len;
    int r = payload(n, &d, &len);
    if (r) return r;
    return rec_put(s->slot, &s->pos, REC_PUT, n->mode, n->path, d, len);
}

static int mark_flushed(struct node *n, void *ctx) {
    (void)ctx;
    n->unflushed = 0;
    return 0;
}

/* Copy the live state into the inactive slot and make it the active one. */
static int compact(void) {
    int t = 1 - P.active;
    struct snap s = { t, 0 };
    if (P.gen == ~(uint64_t)0) return -ENOSPC;
    int r = clear_sector(slot_lba(t));
    if (!r) r = clear_sector(slot_lba(t) + 1);
    for (struct tomb *x = tombs; x && !r; x = x->next)
        r = rec_put(t, &s.pos, REC_DEL, 0, x->path, 0, 0);
    if (!r) r = vfs_each_node(snap_node, &s);
    if (!r) r = hdr_write(t, P.gen + 1, P.fsid);
    if (r) { kprintf("[persist] compaction failed (%d)\n", -r); return r; }
    P.active = t;
    P.gen++;
    P.end = s.pos;
    P.snapshot = 0;
    vfs_each_node(mark_flushed, 0);
    return 0;
}

/* The live tree already includes the change, so a snapshot can commit it directly. */
static int log_rec(uint32_t type, uint32_t mode, const char *path, const void *data, uint32_t dlen) {
    if (P.error) return P.error;
    if (P.snapshot) return compact();
    int r = rec_put(P.active, &P.end, type, mode, path, data, dlen);
    if (r) P.snapshot = 1;
    if (r == -ENOSPC) r = compact();
    return r;
}

static int log_node(struct node *n) {
    const void *d; uint32_t len;
    int r = payload(n, &d, &len);
    if (r) return r;
    return log_rec(REC_PUT, n->mode, n->path, d, len);
}

/* ---- replay ---- */

static char *dup_n(const uint8_t *s, uint32_t n) {
    char *r = kmalloc(n + 1);
    if (!r) return 0;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int apply(uint32_t type, uint32_t mode, const uint8_t *p, uint32_t plen, const uint8_t *d, uint32_t dlen) {
    char *path = dup_n(p, plen);
    if (!path) return -ENOMEM;
    int r = 0;
    if (persistable(path)) {
        switch (type) {
        case REC_PUT:
            if (!vfs_put_node(path, mode, d, dlen)) r = -ENOMEM;
            else tomb_del(path);
            break;
        case REC_DEL: {
            struct node *n = node_find(path);
            if (n) node_unlink(n);
            persist_deleted(path);
            break;
        }
        case REC_RENAME: {
            char *to = dup_n(d, dlen);
            if (!to) r = -ENOMEM;
            else { r = vfs_rename(path, to); kfree(to); }
            break;
        }
        case REC_CHMOD: {
            struct node *n = node_find(path);
            if (n) { n->mode = (n->mode & S_IFMT) | (mode & 07777); n->base = 0; }
            else r = -ENOENT;
            break;
        }
        }
    }
    kfree(path);
    return r;
}

/* Parse records from the start of the slot's record area.  Returns the end offset. */
static uint32_t replay_buf(const uint8_t *d, uint32_t len, int version, uint32_t *count) {
    uint32_t pos = 0;
    *count = 0;
    while ((uint64_t)pos + REC_HDR <= len) {
        uint32_t magic = get32(d + pos);
        if (!magic) break;
        if (magic != REC_MAGIC) { P.snapshot = 1; break; }
        uint32_t type = get32(d + pos + 4), mode = get32(d + pos + 8);
        uint32_t plen = get32(d + pos + 12), dlen = get32(d + pos + 16), crc = get32(d + pos + 20);
        if ((uint64_t)pos + REC_HDR + plen + dlen > len) { P.snapshot = 1; break; }
        const uint8_t *body = d + pos + REC_HDR;
        if (crc32(crc32(0, d + pos, 20), body, plen + dlen) != crc) {
            P.snapshot = 1;
            break;
        }
        if (!plen || plen >= PATH_MAX || type < REC_PUT || type > REC_CHMOD ||
            (type == REC_RENAME && (!dlen || dlen >= PATH_MAX)) ||
            ((type == REC_DEL || type == REC_CHMOD) && dlen) ||
            apply(type, mode, body, plen, body + plen, dlen) || P.error) {
            P.error = -EIO;
            break;
        }
        uint32_t bytes = REC_HDR + plen + dlen;
        if (version == 2) bytes = (bytes + PST_SECTOR_BYTES - 1) & ~(PST_SECTOR_BYTES - 1);
        pos += bytes;
        (*count)++;
    }
    return pos;
}

/* ---- attach at boot ---- */

static void disable(const char *why) {
    kprintf("[persist] disabled: %s\n", why);
    P.on = 0;
    P.replaying = 0;
    P.error = -EIO;
}

static int attach(uint64_t fs_end, uint64_t fsid) {
    uint64_t total = board_blk_sectors();
    if (total <= PST_RESERVE_SECTORS || fs_end > total - PST_RESERVE_SECTORS) {
        disable("no reserved log area at the end of the disk (rebuild the image with make)");
        return -1;
    }
    P.region = total - PST_RESERVE_SECTORS;
    P.fsid = fsid;

    uint64_t gen[2] = { 0, 0 };
    int valid[2] = { 0, 0 }, best = -1, nonempty = 0;
    for (int s = 0; s < 2; s++) {
        uint64_t id = 0;
        valid[s] = hdr_read(s, &gen[s], &id, &nonempty);
        if (valid[s] < 0) { disable("cannot read a log header"); return -1; }
        if (!valid[s]) continue;
        if (id == fsid && (best < 0 || gen[s] > gen[best])) best = s;
    }

    if (best < 0) {
        if (valid[0] || valid[1]) {
            disable("the log on this disk belongs to another filesystem (rebuild with make to reset it)");
            return -1;
        }
        if (nonempty) { disable("both log headers are invalid"); return -1; }
        if (clear_sector(slot_lba(0) + 1) || hdr_write(0, 1, fsid)) {
            disable("cannot initialize the log");
            return -1;
        }
        P.active = 0; P.gen = 1; P.end = 0; P.on = 1;
        kprintf("[persist] new log on disk\n");
        return 0;
    }

    P.active = best;
    P.gen = gen[best];
    P.on = 1;
    P.replaying = 1;
    uint8_t *buf = kmalloc(PST_SLOT_BYTES);
    if (!buf || board_blk_read(slot_lba(best) + 1, buf, PST_SLOT_SECTORS - 1)) {
        kfree(buf);
        disable("cannot read the log");
        return -1;
    }
    uint32_t count;
    P.end = replay_buf(buf, PST_SLOT_BYTES, valid[best], &count);
    kfree(buf);
    P.replaying = 0;
    if (P.error) { disable("cannot replay the complete log"); return -1; }
    if (board_blk_write(P.region, 0, 0)) { disable("cannot flush the log device"); return -1; }
    P.snapshot |= valid[best] == 1;
    kprintf("[persist] log gen %lu: %u records replayed, %u bytes used\n",
            (unsigned long)P.gen, count, P.end);
    return 0;
}

/* Called after the filesystem is mounted.  The fsid comes from the ext UUID (see ext_ident). */
void persist_attach_fs(const struct fs_driver *fs, uint64_t base) {
    uint64_t end = 0, id = 0;
    if (!fs->ident || fs->ident(base, &end, &id)) { disable("filesystem has no identity"); return; }
    attach(end, id);
}

/* ---- hooks called from the VFS and syscalls ---- */

void persist_touch(struct node *n) {
    n->base = 0;
    n->unflushed = 1;
}

void persist_node_created(struct node *n) {
    if (P.replaying || !persistable(n->path) || n->dev) return;
    persist_touch(n);
    if (P.on && persist_flush(n)) kprintf("[persist] cannot record creation of %s\n", n->path);
}

void persist_deleted(const char *abs) {
    if (!P.on || !persistable(abs)) return;
    int r = tomb_add(abs);
    if (r) { P.error = r; kprintf("[persist] cannot track removal of %s\n", abs); return; }
    if (!P.replaying && log_rec(REC_DEL, 0, abs, 0, 0)) kprintf("[persist] cannot record removal of %s\n", abs);
}

void persist_renamed(const char *from, const char *to) {
    if (!P.on || P.replaying) return;
    if (!persistable(from) && !persistable(to)) return;
    if (!persistable(from) || !persistable(to)) P.snapshot = 1;
    if (log_rec(REC_RENAME, 0, from, to, (uint32_t)strlen(to)))
        kprintf("[persist] cannot record rename of %s\n", from);
}

/* Called for each node about to change its path (rename).  The old path must not come back. */
void persist_moved(struct node *n) {
    if (!P.on) return;
    if (persistable(n->path)) {
        int r = tomb_add(n->path);
        if (r) { P.error = r; kprintf("[persist] cannot track move of %s\n", n->path); }
    }
    n->base = 0;
}

void persist_chmod(struct node *n) {
    if (!persistable(n->path) || n->dev) return;
    n->base = 0;
    if (P.on && !P.replaying && log_rec(REC_CHMOD, n->mode & 07777, n->path, 0, 0))
        kprintf("[persist] cannot record chmod of %s\n", n->path);
}

int persist_flush(struct node *n) {
    if (P.replaying || n->unlinked || n->dev ||
        (!persistable(n->path) && strcmp(n->path, "/"))) return 0;
    if (P.error) return P.error;
    if (!P.on) return 0;
    if (P.snapshot) return compact();
    if (!n->unflushed || !persistable(n->path)) return 0;
    int r = log_node(n);
    if (r) {
        kprintf("[persist] cannot save %s\n", n->path);
        return r;
    }
    n->unflushed = 0;
    tomb_del(n->path);
    return 0;
}

static int sync_one(struct node *n, void *c) {
    (void)c;
    return n->unflushed ? persist_flush(n) : 0;
}

int persist_sync_all(void) {
    if (P.error) return P.error;
    if (P.snapshot) return compact();
    return P.on ? vfs_each_node(sync_one, 0) : 0;
}
