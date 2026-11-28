/* codecs.h - allocator, file reader and the PNG / JPEG / GIF decoders for fbview.
 *
 * Freestanding (no libc). Included once from fbview.c, after libchiffon.h.
 * Every decoder fills a struct anim with 0x00RRGGBB frames. Transparency is
 * flattened onto the viewer background BG, so the viewer never sees alpha.
 * Integer maths only: no floating point is used anywhere in this file.
 */

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#define SEEK_SET 0
#define SEEK_END 2
#if defined(__x86_64__)
#define SYS_LSEEK 8
#else
#define SYS_LSEEK 62
#endif

#define BG 0x1E1E1E                         /* viewer background, also the GIF canvas colour */
#define MAX_FRAMES 1024
#define MAX_ANIM_BYTES (160UL << 20)        /* GIF frames are kept whole; cap the total (Pi Zero 2W has 512 MB) */

struct anim { int w, h, n; unsigned *frame[MAX_FRAMES]; int delay[MAX_FRAMES]; };

/* ---------- memory: each allocation is its own mapping, headed by {next, size} ---------- */
struct blk { struct blk *next; unsigned long total; };
static struct blk *blocks;

static void *alloc(unsigned long n) {
    unsigned long total = (n + sizeof(struct blk) + 4095) & ~4095UL;
    long p = chiffon_mmap(0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p < 0 && p > -4096) return 0;
    struct blk *b = (struct blk *)p;
    b->next = blocks;
    b->total = total;
    blocks = b;
    return b + 1;
}

static void free_all(void) {
    while (blocks) {
        struct blk *next = blocks->next;
        chiffon_munmap(blocks, blocks->total);
        blocks = next;
    }
}

/* ---------- small helpers ---------- */
static void fill32(unsigned *d, unsigned v, unsigned long n) { while (n--) *d++ = v; }
static void copy32(unsigned *d, const unsigned *s, unsigned long n) { while (n--) *d++ = *s++; }
static int iabs(int x) { return x < 0 ? -x : x; }
static int clampc(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned be32(const unsigned char *p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

/* composite a colour with alpha a (0..255) over the background */
static unsigned blend(unsigned c, unsigned a) {
    if (a >= 255) return c;
    unsigned out = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        unsigned fg = (c >> sh) & 255, bg = (BG >> sh) & 255;
        out |= ((fg * a + bg * (255 - a)) / 255) << sh;
    }
    return out;
}

static unsigned char *read_file(const char *path, unsigned long *len) {
    int fd = chiffon_open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    long size = ksys(SYS_LSEEK, fd, 0, SEEK_END, 0, 0, 0);
    ksys(SYS_LSEEK, fd, 0, SEEK_SET, 0, 0, 0);
    if (size <= 0) { chiffon_close(fd); return 0; }
    unsigned char *buf = alloc((unsigned long)size);
    unsigned long got = 0;
    while (buf && got < (unsigned long)size) {
        long r = chiffon_read(fd, buf + got, (unsigned long)size - got);
        if (r <= 0) break;
        got += (unsigned long)r;
    }
    chiffon_close(fd);
    if (!buf || got != (unsigned long)size) return 0;
    *len = (unsigned long)size;
    return buf;
}

/* ====================================================================== */
/* inflate (RFC 1951), used by PNG                                        */
/* ====================================================================== */

struct bits { const unsigned char *in, *end; unsigned long buf; int cnt; };
struct huff { short count[16]; short sym[288]; };

static int getbits(struct bits *s, int need) {
    while (s->cnt < need) {
        if (s->in >= s->end) return -1;
        s->buf |= (unsigned long)*s->in++ << s->cnt;
        s->cnt += 8;
    }
    int v = (int)(s->buf & ((1UL << need) - 1));
    s->buf >>= need;
    s->cnt -= need;
    return v;
}

/* canonical Huffman table; returns <0 if over-subscribed, >0 if incomplete */
static int huff_build(struct huff *h, const unsigned char *len, int n) {
    int i, left = 1;
    short offs[16];
    for (i = 0; i < 16; i++) h->count[i] = 0;
    for (i = 0; i < n; i++) h->count[len[i]]++;
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return left;
}

static int huff_decode(struct bits *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = getbits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_codes(struct bits *s, unsigned char *out, unsigned long *o, unsigned long cap,
                         const struct huff *lit, const struct huff *dst) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*o >= cap) return -1;
            out[(*o)++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = getbits(s, LEXT[sym]);
            int ds = huff_decode(s, dst);
            if (e < 0 || ds < 0 || ds >= 30) return -1;
            int de = getbits(s, DEXT[ds]);
            if (de < 0) return -1;
            unsigned long len = (unsigned long)LBASE[sym] + e, dist = (unsigned long)DBASE[ds] + de;
            if (dist > *o || *o + len > cap) return -1;
            for (unsigned long i = 0; i < len; i++, (*o)++) out[*o] = out[*o - dist];
        }
    }
}

static int dynamic_tables(struct bits *s, struct huff *lit, struct huff *dst) {
    static const unsigned char order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlit = getbits(s, 5), ndist = getbits(s, 5), ncode = getbits(s, 4);
    if (nlit < 0 || ndist < 0 || ncode < 0) return -1;
    nlit += 257;
    ndist += 1;
    ncode += 4;
    if (nlit > 286 || ndist > 30) return -1;
    unsigned char cl[19] = {0};
    for (int i = 0; i < ncode; i++) {
        int v = getbits(s, 3);
        if (v < 0) return -1;
        cl[order[i]] = (unsigned char)v;
    }
    struct huff clh;
    if (huff_build(&clh, cl, 19) < 0) return -1;
    unsigned char lens[320];
    int total = nlit + ndist, i = 0;
    while (i < total) {
        int sym = huff_decode(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) { lens[i++] = (unsigned char)sym; continue; }
        int ex, rep, val = 0;
        if (sym == 16) {
            if (!i) return -1;
            val = lens[i - 1];
            ex = getbits(s, 2);
            rep = 3 + ex;
        } else if (sym == 17) {
            ex = getbits(s, 3);
            rep = 3 + ex;
        } else {
            ex = getbits(s, 7);
            rep = 11 + ex;
        }
        if (ex < 0 || i + rep > total) return -1;
        while (rep--) lens[i++] = (unsigned char)val;
    }
    if (lens[256] == 0) return -1;
    if (huff_build(lit, lens, nlit) < 0 || huff_build(dst, lens + nlit, ndist) < 0) return -1;
    return 0;
}

static int inflate(const unsigned char *in, unsigned long inlen, unsigned char *out, unsigned long cap) {
    struct bits s = { in, in + inlen, 0, 0 };
    unsigned long o = 0;
    int last;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {                                /* stored block */
            s.buf = 0;
            s.cnt = 0;                                  /* align to a byte boundary */
            if (s.end - s.in < 4) return -1;
            unsigned long len = (unsigned long)(s.in[0] | s.in[1] << 8);
            s.in += 4;                                  /* skip LEN and NLEN */
            if ((unsigned long)(s.end - s.in) < len || o + len > cap) return -1;
            for (unsigned long i = 0; i < len; i++) out[o++] = s.in[i];
            s.in += len;
        } else if (type == 1) {                         /* fixed Huffman */
            struct huff lit, dst;
            unsigned char l[288];
            int i;
            for (i = 0; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dst, l, 30);
            if (inflate_codes(&s, out, &o, cap, &lit, &dst) < 0) return -1;
        } else if (type == 2) {                         /* dynamic Huffman */
            struct huff lit, dst;
            if (dynamic_tables(&s, &lit, &dst) < 0) return -1;
            if (inflate_codes(&s, out, &o, cap, &lit, &dst) < 0) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

/* ====================================================================== */
/* PNG                                                                    */
/* ====================================================================== */

/* Adam7 passes: x0, y0, dx, dy */
static const int X0[7] = {0,4,0,2,0,1,0}, Y0[7] = {0,0,4,0,2,0,1};
static const int DX[7] = {8,8,4,4,2,2,1}, DY[7] = {8,8,8,4,4,2,2};

static void pass_dims(int w, int h, int q, int il, int *pw, int *ph) {
    int x0 = il ? X0[q] : 0, y0 = il ? Y0[q] : 0, dx = il ? DX[q] : 1, dy = il ? DY[q] : 1;
    *pw = w > x0 ? (w - x0 + dx - 1) / dx : 0;
    *ph = h > y0 ? (h - y0 + dy - 1) / dy : 0;
}

struct png { int ctype, depth, ch, npal, ntrns; const unsigned char *pal, *trns; };

/* raw sample value at sample index si (16-bit samples are returned in full) */
static unsigned raw_sample(const unsigned char *row, unsigned long si, int depth) {
    if (depth == 16) return (unsigned)row[si * 2] << 8 | row[si * 2 + 1];
    if (depth == 8) return row[si];
    int per = 8 / depth;
    int shift = 8 - depth * ((int)(si % per) + 1);
    return (row[si / per] >> shift) & ((1u << depth) - 1);
}

static unsigned to8(unsigned v, int depth) {
    if (depth == 16) return v >> 8;
    if (depth == 8) return v;
    return v * 255 / ((1u << depth) - 1);
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static void unfilter(unsigned char *row, const unsigned char *prev, unsigned long n, int bpp, int type) {
    for (unsigned long i = 0; i < n; i++) {
        int a = i >= (unsigned long)bpp ? row[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= (unsigned long)bpp) ? prev[i - bpp] : 0;
        int x = row[i];
        if (type == 1) x += a;
        else if (type == 2) x += b;
        else if (type == 3) x += (a + b) / 2;
        else if (type == 4) x += paeth(a, b, c);
        row[i] = (unsigned char)x;
    }
}

static unsigned png_pixel(const struct png *P, const unsigned char *row, int x) {
    unsigned long si = (unsigned long)x * P->ch;
    int d = P->depth;
    unsigned r = 0, g = 0, b = 0, al = 255, R, G, B;
    switch (P->ctype) {
    case 0:
        R = raw_sample(row, si, d);
        if (P->ntrns >= 2 && R == be16(P->trns)) al = 0;
        r = g = b = to8(R, d);
        break;
    case 2:
        R = raw_sample(row, si, d);
        G = raw_sample(row, si + 1, d);
        B = raw_sample(row, si + 2, d);
        if (P->ntrns >= 6 && R == be16(P->trns) && G == be16(P->trns + 2) && B == be16(P->trns + 4)) al = 0;
        r = to8(R, d); g = to8(G, d); b = to8(B, d);
        break;
    case 3: {
        unsigned idx = raw_sample(row, si, d);
        if (idx >= (unsigned)P->npal) idx = 0;
        r = P->pal[idx * 3]; g = P->pal[idx * 3 + 1]; b = P->pal[idx * 3 + 2];
        if (idx < (unsigned)P->ntrns) al = P->trns[idx];
        break;
    }
    case 4:
        r = g = b = to8(raw_sample(row, si, d), d);
        al = to8(raw_sample(row, si + 1, d), d);
        break;
    default:
        r = to8(raw_sample(row, si, d), d);
        g = to8(raw_sample(row, si + 1, d), d);
        b = to8(raw_sample(row, si + 2, d), d);
        al = to8(raw_sample(row, si + 3, d), d);
        break;
    }
    return blend(r << 16 | g << 8 | b, al);
}

static const char *png_load(const unsigned char *d, unsigned long n, struct anim *a) {
    const unsigned char *end = d + n, *p;
    struct png P = {0};
    int w = 0, h = 0, il = 0, depth = 0;
    unsigned long idat = 0;
    unsigned char *ibuf = 0;

    for (int pass = 0; pass < 2; pass++) {       /* pass 0: headers and sizes; pass 1: copy IDAT */
        unsigned long k = 0;
        p = d + 8;
        while (end - p >= 12) {
            unsigned long len = be32(p);
            const unsigned char *type = p + 4, *data = p + 8;
            if ((unsigned long)(end - data) < len + 4) break;
            if (!memcmp(type, "IEND", 4)) break;
            if (pass == 0) {
                if (!memcmp(type, "IHDR", 4) && len >= 13) {
                    w = be32(data); h = be32(data + 4);
                    depth = data[8]; P.ctype = data[9]; il = data[12];
                } else if (!memcmp(type, "PLTE", 4)) {
                    P.pal = data; P.npal = (int)(len / 3);
                } else if (!memcmp(type, "tRNS", 4)) {
                    P.trns = data; P.ntrns = (int)len;
                } else if (!memcmp(type, "IDAT", 4)) {
                    idat += len;
                }
            } else if (!memcmp(type, "IDAT", 4)) {
                for (unsigned long i = 0; i < len; i++) ibuf[k++] = data[i];
            }
            p = data + len + 4;
        }
        if (pass == 0) {
            int ch = P.ctype == 0 ? 1 : P.ctype == 2 ? 3 : P.ctype == 3 ? 1 : P.ctype == 4 ? 2 : P.ctype == 6 ? 4 : 0;
            if (!w || !h || w > 30000 || h > 30000 || !ch || idat < 2) return "bad PNG header";
            if (!(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) return "unsupported PNG bit depth";
            if (P.ctype == 3 && (!P.pal || depth > 8)) return "bad PNG palette";
            if ((P.ctype == 2 || P.ctype == 4 || P.ctype == 6) && depth < 8) return "bad PNG bit depth";
            P.depth = depth;
            P.ch = ch;
            ibuf = alloc(idat);
            if (!ibuf) return "out of memory";
        }
    }

    int bpp = P.ch * P.depth / 8;
    if (bpp < 1) bpp = 1;
    int np = il ? 7 : 1;
    unsigned long rawlen = 0;
    for (int q = 0; q < np; q++) {
        int pw, ph;
        pass_dims(w, h, q, il, &pw, &ph);
        if (pw && ph) rawlen += (unsigned long)ph * (1 + ((unsigned long)pw * P.ch * P.depth + 7) / 8);
    }
    unsigned char *raw = alloc(rawlen + 1);
    if (!raw || inflate(ibuf + 2, idat - 2, raw, rawlen) < 0) return "PNG data is corrupt";
    unsigned *out = alloc((unsigned long)w * h * 4);
    if (!out) return "out of memory";
    a->w = w; a->h = h; a->n = 1; a->frame[0] = out; a->delay[0] = 0;

    unsigned long pos = 0;
    for (int q = 0; q < np; q++) {
        int pw, ph;
        pass_dims(w, h, q, il, &pw, &ph);
        if (!pw || !ph) continue;
        unsigned long rb = ((unsigned long)pw * P.ch * P.depth + 7) / 8;
        const unsigned char *prev = 0;
        for (int r = 0; r < ph; r++, pos += 1 + rb) {
            int ft = raw[pos];
            if (ft > 4) return "bad PNG filter";
            unsigned char *row = raw + pos + 1;
            unfilter(row, prev, rb, bpp, ft);
            int y = (il ? Y0[q] : 0) + r * (il ? DY[q] : 1);
            for (int x = 0; x < pw; x++) {
                int X = (il ? X0[q] : 0) + x * (il ? DX[q] : 1);
                out[y * w + X] = png_pixel(&P, row, x);
            }
            prev = row;
        }
    }
    return 0;
}

/* ====================================================================== */
/* GIF (LZW), including animation, disposal, transparency and interlace   */
/* ====================================================================== */

struct lzw { short pre[4096]; unsigned char suf[4096], tmp[4096]; };

/* writes the string for code into tmp, last byte first; returns its length */
static int lzw_str(const struct lzw *t, int code, unsigned char *tmp) {
    int len = 0;
    while (code >= 0 && len < 4096) {
        tmp[len++] = t->suf[code];
        code = t->pre[code];
    }
    return len;
}

static int gif_lzw(struct lzw *t, const unsigned char *data, unsigned long dlen, int minc,
                   unsigned char *out, unsigned long outn) {
    int clear = 1 << minc, eoi = clear + 1, next = eoi + 1, csize = minc + 1, prev = -1;
    unsigned long pos = 0, o = 0;
    unsigned buf = 0;
    int cnt = 0;
    for (int i = 0; i < clear; i++) { t->pre[i] = -1; t->suf[i] = (unsigned char)i; }
    for (;;) {
        while (cnt < csize) {
            if (pos >= dlen) return 0;
            buf |= (unsigned)data[pos++] << cnt;
            cnt += 8;
        }
        int code = (int)(buf & ((1u << csize) - 1));
        buf >>= csize;
        cnt -= csize;
        if (code == clear) { next = eoi + 1; csize = minc + 1; prev = -1; continue; }
        if (code == eoi) return 0;
        int len, fb = 0;
        if (prev < 0) {
            if (code >= clear) return -1;
        } else {
            if (code < next) { len = lzw_str(t, code, t->tmp); fb = t->tmp[len - 1]; }
            else if (code == next) { len = lzw_str(t, prev, t->tmp); fb = t->tmp[len - 1]; }
            else return -1;
            if (next < 4096) {
                t->pre[next] = (short)prev;
                t->suf[next] = (unsigned char)fb;
                next++;
            }
            if (next == (1 << csize) && csize < 12) csize++;
        }
        len = lzw_str(t, code, t->tmp);
        for (int i = len - 1; i >= 0; i--)
            if (o < outn) out[o++] = t->tmp[i];
        prev = code;
    }
}

/* GIF interlace: decoded row k belongs at this frame row */
static int il_row(int k, int h) {
    static const int st[4] = {0, 4, 2, 1}, sp[4] = {8, 8, 4, 2};
    for (int i = 0; i < 4; i++) {
        int cnt = h > st[i] ? (h - st[i] + sp[i] - 1) / sp[i] : 0;
        if (k < cnt) return st[i] + k * sp[i];
        k -= cnt;
    }
    return 0;
}

static const unsigned char *skip_blocks(const unsigned char *p, const unsigned char *end) {
    while (p < end && *p) p += *p + 1;
    return p < end ? p + 1 : end;
}

static void fill_rect(unsigned *c, int W, int H, int x0, int y0, int w, int h, unsigned v) {
    for (int y = y0; y < y0 + h && y < H; y++)
        for (int x = x0; x < x0 + w && x < W; x++)
            c[(unsigned long)y * W + x] = v;
}

static const char *gif_load(const unsigned char *d, unsigned long n, struct anim *a) {
    if (n < 13) return "truncated GIF";
    const unsigned char *end = d + n, *p = d + 13;
    int W = d[6] | d[7] << 8, H = d[8] | d[9] << 8, gfl = d[10];
    const unsigned char *gct = 0;
    int gctn = 0;
    if (gfl & 0x80) { gctn = 2 << (gfl & 7); gct = p; p += 3 * gctn; }
    if (W <= 0 || H <= 0 || p > end) return "bad GIF header";

    unsigned long npx = (unsigned long)W * H;
    unsigned *canvas = alloc(npx * 4), *saved = 0;
    struct lzw *t = alloc(sizeof *t);
    if (!canvas || !t) return "out of memory";
    fill32(canvas, BG, npx);
    a->w = W; a->h = H; a->n = 0;

    int gdelay = 0, gtr = -1, gdisp = 0;
    int pdisp = 0, px0 = 0, py0 = 0, pw0 = 0, ph0 = 0;

    while (p < end && a->n < MAX_FRAMES && (unsigned long)(a->n + 1) * npx * 4 <= MAX_ANIM_BYTES) {
        int b = *p++;
        if (b == 0x3B) break;                                  /* trailer */

        if (b == 0x21) {                                       /* extension */
            if (p >= end) break;
            int label = *p++;
            if (label == 0xF9 && end - p >= 5 && p[0] == 4) {  /* graphic control */
                gdisp = (p[1] >> 2) & 7;
                gdelay = p[2] | p[3] << 8;
                gtr = (p[1] & 1) ? p[4] : -1;
            }
            p = skip_blocks(p, end);
        } else if (b == 0x2C) {                                /* image descriptor */
            if (end - p < 10) break;
            int fx = p[0] | p[1] << 8, fy = p[2] | p[3] << 8;
            int fw = p[4] | p[5] << 8, fh = p[6] | p[7] << 8, fl = p[8];
            p += 9;
            const unsigned char *ct = gct;
            int ctn = gctn;
            if (fl & 0x80) { ctn = 2 << (fl & 7); ct = p; p += 3 * ctn; }
            if (p >= end) break;
            int minc = *p++;

            /* gather the LZW sub-blocks into one buffer */
            unsigned long total = 0;
            for (const unsigned char *q = p; q < end && *q && q + *q + 1 <= end; q += *q + 1) total += *q;
            unsigned char *data = alloc(total + 1);
            if (!data) return "out of memory";
            unsigned long k = 0;
            const unsigned char *q = p;
            while (q < end && *q && q + *q + 1 <= end) {
                for (unsigned i = 1; i <= *q; i++) data[k++] = q[i];
                q += *q + 1;
            }
            p = (q < end && *q == 0) ? q + 1 : end;

            unsigned long fpx = (unsigned long)fw * fh;
            unsigned char *idx = alloc(fpx + 1);
            if (!idx) return "out of memory";
            if (minc >= 2 && minc <= 8) gif_lzw(t, data, k, minc, idx, fpx);

            /* dispose of the previous frame, then keep a copy if this one needs restoring */
            if (a->n > 0) {
                if (pdisp == 2) fill_rect(canvas, W, H, px0, py0, pw0, ph0, BG);
                else if (pdisp == 3 && saved) copy32(canvas, saved, npx);
            }
            if (gdisp == 3) {
                if (!saved) saved = alloc(npx * 4);
                if (saved) copy32(saved, canvas, npx);
            }

            for (int r = 0; r < fh; r++) {
                int y = fy + ((fl & 0x40) ? il_row(r, fh) : r);
                if (y >= H) continue;
                for (int c = 0; c < fw; c++) {
                    int x = fx + c, ci = idx[(unsigned long)r * fw + c];
                    if (x >= W || ci == gtr || ci >= ctn) continue;
                    canvas[(unsigned long)y * W + x] = (unsigned)ct[3 * ci] << 16 | (unsigned)ct[3 * ci + 1] << 8 | ct[3 * ci + 2];
                }
            }

            unsigned *f = alloc(npx * 4);
            if (!f) return "out of memory";
            copy32(f, canvas, npx);
            int ms = gdelay * 10;
            if (ms <= 10) ms = 100;                            /* same as browsers: 0 means 100 ms */
            a->frame[a->n] = f;
            a->delay[a->n] = ms;
            a->n++;

            pdisp = gdisp; px0 = fx; py0 = fy; pw0 = fw; ph0 = fh;
            gtr = -1; gdelay = 0; gdisp = 0;
        } else {
            break;
        }
    }
    return a->n ? 0 : "no frames in GIF";
}

/* ====================================================================== */
/* JPEG: baseline and progressive Huffman, 1 or 3 components              */
/* ====================================================================== */

static const unsigned char ZZ[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

/* 4096 * cos(k*pi/16) for k = 0..8; the rest follows from symmetry */
static const int CT[9] = {4096, 4017, 3784, 3406, 2896, 2276, 1568, 799, 0};

static int cosq(int m) {                                       /* 4096 * cos(m*pi/16), m in 0..31 */
    if (m <= 8) return CT[m];
    if (m <= 16) return -CT[16 - m];
    if (m <= 24) return -CT[m - 16];
    return CT[32 - m];
}

struct jh { int count[17]; unsigned char val[256]; };
struct jcomp {
    int id, h, v, tq, td, ta, bw, bh, cw, ch, pred, pstride;
    short *coef;                                               /* bw*bh blocks of 64, natural order */
    unsigned char *plane;
};
struct jdec {
    int W, H, nc, hmax, vmax, mcux, mcuy, ri;
    int eobrun, st, nextv, r;                                  /* progressive AC state */
    struct jcomp c[3];
    int qt[4][64];
    struct jh dct[4], act[4];
    int cs[8][8];                                              /* IDCT basis, scale 4096 */
};

struct jbits { const unsigned char *p, *end; unsigned cur; int cnt, marker; };

static void jbyte(struct jbits *s) {
    unsigned b = 0;
    if (!s->marker && s->p < s->end) {
        b = *s->p;
        if (b == 0xFF) {
            unsigned nx = s->p + 1 < s->end ? s->p[1] : 0xD9;
            if (nx == 0) s->p += 2;                            /* stuffed 0xFF */
            else { s->marker = 1; b = 0; }                     /* a real marker: feed zeros */
        } else {
            s->p++;
        }
    }
    s->cur = b;
    s->cnt = 8;
}

static int jbit(struct jbits *s) {
    if (!s->cnt) jbyte(s);
    s->cnt--;
    return (s->cur >> s->cnt) & 1;
}

static int jreceive(struct jbits *s, int n) {
    int v = 0;
    while (n-- > 0) v = v << 1 | jbit(s);
    return v;
}

static int jextend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static int jdecode(struct jbits *s, const struct jh *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 16; l++) {
        code |= jbit(s);
        int count = h->count[l];
        if (code - count < first) return h->val[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* DC: first pass and refinement. AC: first pass (also used for baseline) and refinement. */
static void ac_first(struct jdec *j, struct jbits *s, short *blk, int ss, int se, int al, const struct jh *t) {
    if (j->eobrun > 0) { j->eobrun--; return; }
    int k = ss;
    while (k <= se) {
        int rs = jdecode(s, t);
        if (rs < 0) return;
        int sz = rs & 15, r = rs >> 4;
        if (sz == 0) {
            if (r < 15) { j->eobrun = jreceive(s, r) + (1 << r) - 1; return; }
            k += 16;                                           /* ZRL: 16 zeros */
            continue;
        }
        k += r;
        if (k > 63) return;
        blk[ZZ[k]] = (short)(jextend(jreceive(s, sz), sz) * (1 << al));
        k++;
    }
}

static void ac_refine(struct jdec *j, struct jbits *s, short *blk, int ss, int se, int al, const struct jh *t) {
    int k = ss;
    while (k <= se) {
        int z = ZZ[k];
        int sign = blk[z] < 0 ? -1 : 1;
        if (j->st == 0) {
            int rs = jdecode(s, t);
            if (rs < 0) return;
            int sz = rs & 15, r = rs >> 4;
            if (sz == 0) {
                if (r < 15) { j->eobrun = jreceive(s, r) + (1 << r); j->st = 4; }
                else { j->r = 16; j->st = 1; }
            } else {
                j->nextv = jextend(jreceive(s, sz), sz);
                j->r = r;
                j->st = r ? 2 : 3;
            }
            continue;
        }
        switch (j->st) {
        case 1: case 2:
            if (blk[z]) blk[z] = (short)(blk[z] + sign * (jbit(s) << al));
            else if (--j->r == 0) j->st = j->st == 2 ? 3 : 0;
            break;
        case 3:
            if (blk[z]) blk[z] = (short)(blk[z] + sign * (jbit(s) << al));
            else { blk[z] = (short)(j->nextv * (1 << al)); j->st = 0; }
            break;
        case 4:
            if (blk[z]) blk[z] = (short)(blk[z] + sign * (jbit(s) << al));
            break;
        }
        k++;
    }
    if (j->st == 4 && --j->eobrun == 0) j->st = 0;
}

static void jblock(struct jdec *j, struct jbits *s, struct jcomp *c, short *blk, int ss, int se, int ah, int al) {
    if (ss == 0) {
        if (ah == 0) {
            int t = jdecode(s, &j->dct[c->td]);
            int diff = (t > 0 && t <= 16) ? jextend(jreceive(s, t), t) : 0;
            c->pred += diff;
            blk[0] = (short)(c->pred * (1 << al));
        } else if (jbit(s)) {
            blk[0] = (short)(blk[0] | (1 << al));
        }
    }
    int as = ss ? ss : 1;
    if (se >= as) {
        if (ah == 0) ac_first(j, s, blk, as, se, al, &j->act[c->ta]);
        else ac_refine(j, s, blk, as, se, al, &j->act[c->ta]);
    }
}

static void jrestart(struct jdec *j, struct jbits *s) {
    while (s->p + 1 < s->end && !(s->p[0] == 0xFF && s->p[1] >= 0xD0 && s->p[1] <= 0xD7)) s->p++;
    if (s->p + 1 < s->end) s->p += 2;
    s->cnt = 0;
    s->marker = 0;
    for (int k = 0; k < 3; k++) j->c[k].pred = 0;
    j->eobrun = 0;
    j->st = 0;
}

/* decodes one scan; returns the position of the next marker */
static const unsigned char *jscan(struct jdec *j, const unsigned char *p, const unsigned char *end,
                                  int ns, const int *ci, int ss, int se, int ah, int al) {
    struct jbits s = { p, end, 0, 0, 0 };
    for (int k = 0; k < ns; k++) j->c[ci[k]].pred = 0;
    j->eobrun = 0;
    j->st = 0;
    int total = ns == 1 ? j->c[ci[0]].cw * j->c[ci[0]].ch : j->mcux * j->mcuy;
    for (int u = 0; u < total; u++) {
        if (j->ri && u && u % j->ri == 0) jrestart(j, &s);
        if (ns == 1) {
            struct jcomp *c = &j->c[ci[0]];
            int bx = u % c->cw, by = u / c->cw;
            jblock(j, &s, c, c->coef + (by * c->bw + bx) * 64, ss, se, ah, al);
        } else {
            int mx = u % j->mcux, my = u / j->mcux;
            for (int k = 0; k < ns; k++) {
                struct jcomp *c = &j->c[ci[k]];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        jblock(j, &s, c, c->coef + (by * c->bw + bx) * 64, ss, se, ah, al);
                    }
            }
        }
    }
    const unsigned char *q = s.p;
    while (q + 1 < end && !(q[0] == 0xFF && q[1] != 0 && !(q[1] >= 0xD0 && q[1] <= 0xD7))) q++;
    return q;
}

static void init_idct(struct jdec *j) {
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            j->cs[x][u] = cosq((((2 * x + 1) * u)) & 31) * (u ? 4096 : 2896) / 4096;
}

/* dequantise and inverse-DCT one 8x8 block into dst */
static void jidct(const struct jdec *j, const short *blk, const int *q, unsigned char *dst, int stride) {
    long tmp[64];
    for (int u = 0; u < 8; u++)
        for (int y = 0; y < 8; y++) {
            long s = 0;
            for (int v = 0; v < 8; v++) s += (long)j->cs[y][v] * ((long)blk[v * 8 + u] * q[v * 8 + u]);
            tmp[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            long s = 0;
            for (int u = 0; u < 8; u++) s += (long)j->cs[x][u] * tmp[y * 8 + u];
            long val = ((s + (1L << 25)) >> 26) + 128;
            dst[y * stride + x] = (unsigned char)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
}

static int jsamp(const struct jdec *j, int k, int x, int y) {
    const struct jcomp *c = &j->c[k];
    return c->plane[(y * c->v / j->vmax) * c->pstride + x * c->h / j->hmax];
}

static const char *jpeg_load(const unsigned char *d, unsigned long n, struct anim *a) {
    struct jdec *j = alloc(sizeof *j);
    if (!j) return "out of memory";
    init_idct(j);
    const unsigned char *p = d + 2, *end = d + n;
    int have = 0;

    for (;;) {
        while (p < end && *p != 0xFF) p++;
        while (p < end && *p == 0xFF) p++;
        if (p >= end) break;
        int m = *p++;
        if (m == 0xD9) break;                                  /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   /* standalone markers */
        if (end - p < 2) break;
        unsigned len = be16(p);
        if (len < 2 || (unsigned long)(end - p) < len) return "truncated JPEG";
        const unsigned char *seg = p + 2, *segend = p + len, *next = segend;

        if (m == 0xDB) {                                       /* quantisation tables */
            while (segend - seg >= 65) {
                int pq = seg[0] >> 4, tq = seg[0] & 3, bytes = pq ? 2 : 1;
                seg++;
                if (segend - seg < 64 * bytes) break;
                for (int k = 0; k < 64; k++, seg += bytes)
                    j->qt[tq][ZZ[k]] = pq ? (int)be16(seg) : seg[0];
            }
        } else if (m == 0xC4) {                                /* Huffman tables */
            while (segend - seg >= 17) {
                int tc = seg[0] >> 4, th = seg[0] & 3;
                struct jh *t = tc ? &j->act[th] : &j->dct[th];
                int total = 0;
                t->count[0] = 0;
                for (int i = 1; i <= 16; i++) { t->count[i] = seg[i]; total += seg[i]; }
                seg += 17;
                if (total > 256 || segend - seg < total) return "bad JPEG Huffman table";
                for (int i = 0; i < total; i++) t->val[i] = seg[i];
                seg += total;
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {      /* frame header */
            if (segend - seg < 6 || seg[0] != 8) return "unsupported JPEG precision";
            j->H = (int)be16(seg + 1);
            j->W = (int)be16(seg + 3);
            j->nc = seg[5];
            if (j->W <= 0 || j->H <= 0 || (j->nc != 1 && j->nc != 3) || segend - seg < 6 + 3 * j->nc)
                return "unsupported JPEG (need 1 or 3 components)";
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->nc; i++) {
                const unsigned char *q = seg + 6 + 3 * i;
                struct jcomp *c = &j->c[i];
                c->id = q[0];
                c->h = q[1] >> 4;
                c->v = q[1] & 15;
                c->tq = q[2] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return "bad JPEG sampling factors";
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            j->mcux = (j->W + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->H + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                struct jcomp *c = &j->c[i];
                c->bw = j->mcux * c->h;
                c->bh = j->mcuy * c->v;
                c->cw = ((j->W * c->h + j->hmax - 1) / j->hmax + 7) / 8;
                c->ch = ((j->H * c->v + j->vmax - 1) / j->vmax + 7) / 8;
                c->coef = alloc((unsigned long)c->bw * c->bh * 64 * sizeof(short));
                if (!c->coef) return "out of memory";
            }
            have = 1;
        } else if (m == 0xDD) {                                /* restart interval */
            if (len >= 4) j->ri = (int)be16(seg);
        } else if (m == 0xDA) {                                /* start of scan */
            if (!have || segend - seg < 1) return "JPEG scan before frame header";
            int ns = seg[0];
            if (ns < 1 || ns > j->nc || segend - seg < 1 + 2 * ns + 3) return "bad JPEG scan header";
            int ci[3];
            for (int k = 0; k < ns; k++) {
                int f = -1;
                for (int i = 0; i < j->nc; i++)
                    if (j->c[i].id == seg[1 + 2 * k]) f = i;
                if (f < 0) return "bad JPEG scan header";
                ci[k] = f;
                j->c[f].td = (seg[2 + 2 * k] >> 4) & 3;
                j->c[f].ta = seg[2 + 2 * k] & 3;
            }
            const unsigned char *q = seg + 1 + 2 * ns;
            next = jscan(j, segend, end, ns, ci, q[0], q[1], q[2] >> 4, q[2] & 15);
        } else if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            return "unsupported JPEG type (lossless, hierarchical or arithmetic)";
        }
        p = next;
    }
    if (!have) return "no JPEG frame found";

    int W = j->W, H = j->H;
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        c->pstride = c->bw * 8;
        c->plane = alloc((unsigned long)c->pstride * c->bh * 8);
        if (!c->plane) return "out of memory";
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++)
                jidct(j, c->coef + (by * c->bw + bx) * 64, j->qt[c->tq],
                      c->plane + (unsigned long)by * 8 * c->pstride + bx * 8, c->pstride);
    }

    unsigned *out = alloc((unsigned long)W * H * 4);
    if (!out) return "out of memory";
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int Y = jsamp(j, 0, x, y);
            unsigned rgb;
            if (j->nc == 1) {
                rgb = (unsigned)Y * 0x010101u;
            } else {                                           /* YCbCr -> RGB, 16.16 fixed point */
                int cb = jsamp(j, 1, x, y) - 128, cr = jsamp(j, 2, x, y) - 128;
                int r = Y + ((91881 * cr + 32768) >> 16);
                int g = Y - ((22554 * cb + 46802 * cr + 32768) >> 16);
                int b = Y + ((116130 * cb + 32768) >> 16);
                rgb = (unsigned)clampc(r) << 16 | (unsigned)clampc(g) << 8 | (unsigned)clampc(b);
            }
            out[(unsigned long)y * W + x] = rgb;
        }

    a->w = W; a->h = H; a->n = 1; a->frame[0] = out; a->delay[0] = 0;
    return 0;
}

/* ====================================================================== */
/* dispatch                                                               */
/* ====================================================================== */

/* Fills *a and returns 0, or returns a short error message. */
static const char *decode_image(const unsigned char *d, unsigned long n, struct anim *a) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return png_load(d, n, a);
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return gif_load(d, n, a);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_load(d, n, a);
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "WebP is not supported yet";
    return "unrecognised image format";
}
