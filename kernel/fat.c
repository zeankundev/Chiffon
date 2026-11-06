/* Read-only FAT32 and exFAT volumes, mounted as a directory tree under a path such as /media/usb0.
 * Directory entries are read at mount time; file data is read on first use through the sector reader.
 * Every node is read-only and marked base, so it is never written to the persistence log: unmounting
 * just unlinks the nodes.  Supports 8.3 and long names (FAT32) and UTF-16 names (exFAT). */
#include "kernel.h"
#include "fat.h"

#define FAT_SECTOR     512u        /* volumes are read in 512-byte sectors (the mass-storage block size) */
#define FAT_MAX_ENTS   4096
#define FAT_MAX_DEPTH  12

struct fat_ent { uint32_t cluster; uint64_t size; uint8_t dir, nofat; };
struct fat_vol {
    struct fs_driver drv;                  /* first member: node->fs points here */
    fat_read_fn rd; void *ctx;
    int exfat, gone;
    uint64_t base;                         /* first sector of the volume on the device */
    uint32_t spc, clus_bytes, fat_lba, heap_lba, nclusters, root;
    uint32_t cache_sec; int cache_ok;
    uint8_t *fatbuf, *clbuf;
    struct fat_ent *ents; uint32_t nents;
};

static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd_le64(const uint8_t *p) { return rd_le32(p) | (uint64_t)rd_le32(p + 4) << 32; }

static int vread(struct fat_vol *v, uint64_t lba, uint32_t n, void *buf) { return v->rd(v->ctx, v->base + lba, n, buf); }
static int is_eoc(const struct fat_vol *v, uint32_t c) { return v->exfat ? c >= 0xFFFFFFF8u : c >= 0x0FFFFFF8u; }

/* next cluster in a FAT chain; 0xFFFFFFFF on a read error (which also reads as end-of-chain) */
static uint32_t fat_next(struct fat_vol *v, uint32_t c) {
    uint64_t off = (uint64_t)c * 4;
    uint32_t sec = v->fat_lba + (uint32_t)(off / FAT_SECTOR), o = (uint32_t)(off % FAT_SECTOR);
    if (!v->cache_ok || v->cache_sec != sec) {
        if (vread(v, sec, 1, v->fatbuf)) return 0xFFFFFFFFu;
        v->cache_sec = sec; v->cache_ok = 1;
    }
    uint32_t val = rd_le32(v->fatbuf + o);
    return v->exfat ? val : val & 0x0FFFFFFFu;
}

/* copy size bytes of a cluster chain (or a contiguous run when nofat) into out */
static int read_chain(struct fat_vol *v, uint32_t c, int nofat, uint64_t size, uint8_t *out) {
    uint64_t done = 0;
    while (done < size) {
        if (c < 2 || c - 2 >= v->nclusters) return -EIO;
        uint64_t lba = v->heap_lba + (uint64_t)(c - 2) * v->spc;
        uint64_t take = size - done;
        if (take >= v->clus_bytes) {
            if (vread(v, lba, v->spc, out + done)) return -EIO;
            done += v->clus_bytes;
        } else {
            if (vread(v, lba, v->spc, v->clbuf)) return -EIO;
            memcpy(out + done, v->clbuf, take);
            done += take;
        }
        if (done >= size) break;
        if (nofat) c++;
        else {
            uint32_t nx = fat_next(v, c);
            if (is_eoc(v, nx)) return -EIO;
            c = nx;
        }
    }
    return 0;
}

/* whole directory contents; size 0 means "walk the FAT chain" (FAT32 directories record no size) */
static uint8_t *read_dir(struct fat_vol *v, uint32_t c, int nofat, uint64_t size, uint64_t *len) {
    if (!size && !nofat) {
        uint32_t clus = c; uint64_t n = 0;
        while (clus >= 2 && clus - 2 < v->nclusters && !is_eoc(v, clus) && n < (1u << 20)) {
            n += v->clus_bytes;
            clus = fat_next(v, clus);
        }
        size = n;
    }
    *len = 0;
    if (!size) return 0;
    uint8_t *buf = kmalloc(size);
    if (!buf) return 0;
    if (read_chain(v, c, nofat, size, buf)) { kfree(buf); return 0; }
    *len = size;
    return buf;
}

static void utf16_name(const uint16_t *u, unsigned max, char *out) {
    unsigned n = 0;
    for (unsigned i = 0; i < max && n < 255; i++) {
        if (!u[i] || u[i] == 0xFFFF) break;
        out[n++] = u[i] < 128 ? (char)u[i] : '_';
    }
    out[n] = 0;
}

/* 8.3 name with the NT case flags in byte 12 */
static void sfn_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = i ? e[i] : (e[0] == 0x05 ? 0xE5 : e[0]);
        if (c == ' ') break;
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z') c += 32;
        out[n++] = (char)c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            uint8_t c = e[i];
            if ((e[12] & 0x10) && c >= 'A' && c <= 'Z') c += 32;
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

static void join_path(char *out, const char *dir, const char *name) {
    strlcpy(out, dir, PATH_MAX);
    if (!(dir[0] == '/' && !dir[1])) strlcat(out, "/", PATH_MAX);
    strlcat(out, name, PATH_MAX);
}

static void build_dir(struct fat_vol *v, const char *dir, uint32_t c, int nofat, uint64_t size, int depth);

static void add_entry(struct fat_vol *v, const char *dir, const char *name, int isdir, uint32_t cl,
                      uint64_t size, int nofat, int depth) {
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= 200 || v->nents >= FAT_MAX_ENTS) return;
    char path[PATH_MAX];
    join_path(path, dir, name);
    uint32_t idx = v->nents++;
    v->ents[idx] = (struct fat_ent){ cl, isdir ? 0 : size, (uint8_t)isdir, (uint8_t)nofat };
    struct node *n = vfs_add(path, isdir ? (S_IFDIR | 0555) : (S_IFREG | 0444), isdir ? 0 : size, idx, &v->drv);
    if (!n) return;
    n->readonly = 1;
    n->base = 1;                           /* never written to the persistence log */
    if (isdir && depth < FAT_MAX_DEPTH) build_dir(v, path, cl, nofat, size, depth + 1);
}

static void parse_fat32(struct fat_vol *v, const char *dir, const uint8_t *buf, uint64_t len, int depth) {
    static const uint8_t lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint16_t lfn[260]; int lfn_ok = 0;
    for (uint64_t o = 0; o + 32 <= len; o += 32) {
        const uint8_t *e = buf + o;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0F) {                                /* long-name fragment, highest sequence first */
            int seq = e[0] & 0x1F;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; }
            if (!lfn_ok || seq < 1 || seq > 20) { lfn_ok = 0; continue; }
            for (int k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = rd_le16(e + lfn_pos[k]);
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; }         /* volume label */
        char name[256];
        if (lfn_ok) utf16_name(lfn, 260, name); else sfn_name(e, name);
        lfn_ok = 0;
        int isdir = (e[11] & 0x10) != 0;
        uint32_t cl = (uint32_t)rd_le16(e + 20) << 16 | rd_le16(e + 26);
        add_entry(v, dir, name, isdir, cl, rd_le32(e + 28), 0, depth);
    }
}

static void parse_exfat(struct fat_vol *v, const char *dir, const uint8_t *buf, uint64_t len, int depth) {
    for (uint64_t o = 0; o + 32 <= len; o += 32) {
        const uint8_t *e = buf + o;
        if (e[0] == 0) break;
        if (e[0] != 0x85) continue;                         /* file entry: stream extension + name entries follow */
        unsigned sec = e[1];
        if (sec < 2 || o + 32 * (uint64_t)(sec + 1) > len) break;
        const uint8_t *st = e + 32;
        if (st[0] != 0xC0) { o += 32 * sec; continue; }
        int isdir = (rd_le16(e + 4) & 0x10) != 0;
        int nofat = (st[1] & 0x02) != 0;
        unsigned namelen = st[3];
        uint32_t cl = rd_le32(st + 20);
        uint64_t sz = rd_le64(st + 24);
        uint16_t u[256]; unsigned nl = 0;
        for (unsigned k = 2; k <= sec && nl < namelen && nl < 255; k++) {
            const uint8_t *ne = e + 32 * k;
            if (ne[0] != 0xC1) break;
            for (int c = 0; c < 15 && nl < namelen && nl < 255; c++) u[nl++] = rd_le16(ne + 2 + 2 * c);
        }
        char name[256];
        utf16_name(u, nl, name);
        add_entry(v, dir, name, isdir, cl, sz, nofat, depth);
        o += 32 * sec;
    }
}

static void build_dir(struct fat_vol *v, const char *dir, uint32_t c, int nofat, uint64_t size, int depth) {
    uint64_t len = 0;
    uint8_t *buf = read_dir(v, c, nofat, size, &len);
    if (!buf) return;
    if (v->exfat) parse_exfat(v, dir, buf, len, depth);
    else parse_fat32(v, dir, buf, len, depth);
    kfree(buf);
}

static int fat_read_node(struct node *n, void *buf) {
    struct fat_vol *v = (struct fat_vol *)n->fs;
    if (v->gone || n->fs_ino >= v->nents) return -EIO;
    const struct fat_ent *e = &v->ents[n->fs_ino];
    return read_chain(v, e->cluster, e->nofat, n->size, buf);
}

/* boot sector checks; the fields are only stored once a volume is known to be valid */
static int probe_fat32(struct fat_vol *v, const uint8_t *s) {
    if (memcmp(s + 82, "FAT32   ", 8) || s[510] != 0x55 || s[511] != 0xAA) return -1;
    uint16_t bps = rd_le16(s + 11), rsv = rd_le16(s + 14), rootent = rd_le16(s + 17), fat16 = rd_le16(s + 22);
    uint8_t spc = s[13], nf = s[16];
    uint32_t tot = rd_le32(s + 32), fat32 = rd_le32(s + 36), root = rd_le32(s + 44);
    if (bps != FAT_SECTOR || !spc || (spc & (spc - 1)) || !nf || rootent || fat16 || !fat32) return -1;
    uint32_t dataoff = rsv + (uint32_t)nf * fat32;
    if (tot <= dataoff) return -1;
    v->exfat = 0; v->spc = spc; v->clus_bytes = bps * spc; v->fat_lba = rsv; v->heap_lba = dataoff;
    v->nclusters = (tot - dataoff) / spc; v->root = root;
    return 0;
}
static int probe_exfat(struct fat_vol *v, const uint8_t *s) {
    if (memcmp(s + 3, "EXFAT   ", 8) || s[510] != 0x55 || s[511] != 0xAA) return -1;
    uint8_t bpsh = s[108], spcsh = s[109];
    if (bpsh != 9 || spcsh > 25) return -1;                 /* 512-byte sectors only */
    uint32_t spc = 1u << spcsh;
    v->exfat = 1; v->spc = spc; v->clus_bytes = FAT_SECTOR * spc;
    v->fat_lba = rd_le32(s + 80); v->heap_lba = rd_le32(s + 88);
    v->nclusters = rd_le32(s + 92); v->root = rd_le32(s + 96);
    return 0;
}

int fat_mount(const char *dir, fat_read_fn rd, void *ctx, uint64_t nsect, void **handle) {
    (void)nsect;
    struct fat_vol *v = kmalloc(sizeof *v);
    uint8_t *sec = kmalloc(FAT_SECTOR);
    if (!v || !sec) { kfree(v); kfree(sec); return -ENOMEM; }
    memset(v, 0, sizeof *v);
    v->rd = rd; v->ctx = ctx;
    /* candidates: the whole device (a volume with no partition table), then each MBR partition */
    uint64_t cand[5]; int nc = 0;
    cand[nc++] = 0;
    if (!rd(ctx, 0, 1, sec) && sec[510] == 0x55 && sec[511] == 0xAA)
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = sec + 446 + 16 * i;
            if (e[4] && rd_le32(e + 8)) cand[nc++] = rd_le32(e + 8);
        }
    int found = 0;
    for (int i = 0; i < nc && !found; i++) {
        v->base = cand[i];
        if (rd(ctx, cand[i], 1, sec)) continue;
        if (!probe_exfat(v, sec) || !probe_fat32(v, sec)) found = 1;
        if (found) v->base = cand[i];
    }
    if (!found) { kfree(v); kfree(sec); return -ENOENT; }
    kfree(sec);
    v->fatbuf = kmalloc(FAT_SECTOR);
    v->clbuf = kmalloc(v->clus_bytes);
    v->ents = kmalloc(FAT_MAX_ENTS * sizeof *v->ents);
    if (!v->fatbuf || !v->clbuf || !v->ents) { kfree(v->fatbuf); kfree(v->clbuf); kfree(v->ents); kfree(v); return -ENOMEM; }
    v->drv.name = v->exfat ? "exfat" : "fat32";
    v->drv.read = fat_read_node;
    if (!node_find("/media")) node_create("/media", S_IFDIR | 0755);
    if (node_find(dir)) return -EEXIST;
    struct node *mp = node_create(dir, S_IFDIR | 0555);
    if (!mp) return -ENOMEM;
    mp->fs = &v->drv; mp->readonly = 1; mp->base = 1;
    build_dir(v, dir, v->root, 0, 0, 0);
    kprintf("[fat] %s volume mounted at %s (%u clusters of %u bytes)\n", v->drv.name, dir, v->nclusters, v->clus_bytes);
    *handle = v;
    return 0;
}

static int unlink_vol(struct node *n, void *ctx) { if (n->fs == ctx) node_unlink(n); return 0; }

void fat_unmount(void *handle) {
    struct fat_vol *v = handle;
    if (!v) return;
    v->gone = 1;                           /* open files keep the volume struct; reads fail from now on */
    vfs_each_node(unlink_vol, &v->drv);
}
