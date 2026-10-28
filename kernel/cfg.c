#include "kernel.h"

#define CFG_MAX 128
static struct { char *sect, *key, *val; } ent[CFG_MAX];
static int nent;

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int kstrcasecmp(const char *a, const char *b) {
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}
static char *dupn(const char *s, size_t n) {
    char *d = kmalloc(n + 1);
    if (!d) return 0;
    memcpy(d, s, n); d[n] = 0;
    return d;
}
static int sp(int c) { return c == ' ' || c == '\t' || c == '\r'; }

void cfg_load(const char *t, size_t len) {
    char sect[64] = "";
    const char *end = t + len;
    while (t < end) {
        const char *e = t;
        while (e < end && *e != '\n') e++;
        const char *a = t, *b = e;
        t = e < end ? e + 1 : e;
        while (a < b && sp(*a)) a++;
        while (b > a && sp(b[-1])) b--;
        if (a == b || *a == ';' || *a == '#') continue;
        if (*a == '[') {
            const char *r = a + 1;
            while (r < b && *r != ']') r++;
            size_t l = r - (a + 1);
            if (l >= sizeof sect) l = sizeof sect - 1;
            memcpy(sect, a + 1, l); sect[l] = 0;
            continue;
        }
        const char *eq = a;
        while (eq < b && *eq != '=') eq++;
        if (eq == b || nent >= CFG_MAX) continue;
        const char *ke = eq;
        while (ke > a && sp(ke[-1])) ke--;
        const char *v = eq + 1;
        while (v < b && sp(*v)) v++;
        const char *ve = b;
        if (v < ve && (*v == '"' || *v == '\'') && ve - v >= 2 && ve[-1] == *v) { v++; ve--; }
        else for (const char *c = v; c + 1 < ve; c++)
            if (sp(*c) && (c[1] == ';' || c[1] == '#')) { ve = c; break; }
        while (ve > v && sp(ve[-1])) ve--;
        ent[nent].sect = dupn(sect, strlen(sect));
        ent[nent].key = dupn(a, ke - a);
        ent[nent].val = dupn(v, ve - v);
        if (ent[nent].sect && ent[nent].key && ent[nent].val) nent++;
    }
}
const char *cfg_get(const char *sect, const char *key, const char *def) {
    for (int i = nent - 1; i >= 0; i--)
        if (!kstrcasecmp(ent[i].sect, sect) && !kstrcasecmp(ent[i].key, key)) return ent[i].val;
    return def;
}
int cfg_foreach(const char *sect, void (*cb)(const char *, const char *, void *), void *ctx) {
    int n = 0;
    for (int i = 0; i < nent; i++)
        if (!kstrcasecmp(ent[i].sect, sect)) { cb(ent[i].key, ent[i].val, ctx); n++; }
    return n;
}

static char cmdline[256], cmdval[128];
void cmdline_set(const char *s) { strlcpy(cmdline, s ? s : "", sizeof cmdline); }
const char *cmdline_get(const char *key) {
    size_t kl = strlen(key);
    for (const char *p = cmdline; *p;) {
        while (sp(*p)) p++;
        const char *w = p;
        while (*p && !sp(*p)) p++;
        if ((size_t)(p - w) > kl && !strncmp(w, key, kl) && w[kl] == '=') {
            size_t l = p - (w + kl + 1);
            if (l >= sizeof cmdval) l = sizeof cmdval - 1;
            memcpy(cmdval, w + kl + 1, l); cmdval[l] = 0;
            return cmdval;
        }
    }
    return 0;
}
