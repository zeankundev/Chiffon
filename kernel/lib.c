#include "kernel.h"

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) a++, b++, n--;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
char *strrchr(const char *s, int c) {
    const char *r = 0;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) break; }
    return (char *)r;
}

struct sink { char *buf; size_t pos, cap; };
static void put(struct sink *s, char c) {
    if (s->buf) { if (s->pos + 1 < s->cap) s->buf[s->pos] = c; }
    else { if (c == '\n') console_putc('\r'); console_putc(c); }
    s->pos++;
}
static void putnum(struct sink *s, unsigned long long v, int base, int neg, int width, int zero, int left, int upper) {
    char tmp[24]; int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dg[v % base]; v /= base; } while (v);
    int len = n + neg, pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad--) put(s, ' ');
    if (neg) put(s, '-');
    if (!left && zero) while (pad--) put(s, '0');
    while (n) put(s, tmp[--n]);
    if (left) while (pad--) put(s, ' ');
}
static void vfmt(struct sink *s, const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { put(s, *f); continue; }
        f++;
        int zero = 0, left = 0, width = 0, lng = 0;
        for (;; f++) { if (*f == '0') zero = 1; else if (*f == '-') left = 1; else break; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z') { lng++; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            putnum(s, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, v < 0, width, zero, left, 0); break; }
        case 'u': putnum(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, zero, left, 0); break;
        case 'x': case 'X': putnum(s, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, width, zero, left, *f == 'X'); break;
        case 'p': put(s, '0'); put(s, 'x'); putnum(s, (uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, 0, 0); break;
        case 'c': put(s, (char)va_arg(ap, int)); break;
        case 's': {
            const char *t = va_arg(ap, const char *); if (!t) t = "(null)";
            int pad = width - (int)strlen(t);
            if (!left) while (pad-- > 0) put(s, ' ');
            while (*t) put(s, *t++);
            if (left) while (pad-- > 0) put(s, ' ');
            break; }
        case '%': put(s, '%'); break;
        default: put(s, '%'); if (*f) put(s, *f); else f--; break;
        }
    }
}
void kprintf(const char *fmt, ...) {
    struct sink s = {0, 0, 0}; va_list ap;
    va_start(ap, fmt); vfmt(&s, fmt, ap); va_end(ap);
}
int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    struct sink s = {buf, 0, n}; va_list ap;
    va_start(ap, fmt); vfmt(&s, fmt, ap); va_end(ap);
    if (n) buf[s.pos < n ? s.pos : n - 1] = 0;
    return (int)s.pos;
}
void panic(const char *fmt, ...) {
    struct sink s = {0, 0, 0}; va_list ap;
    kprintf("\n>>>> CRITICAL KERNEL PANIC: ");
    va_start(ap, fmt); vfmt(&s, fmt, ap); va_end(ap);
    kprintf("\r\n");
    console_flush();                                    /* make sure the message reaches the screen */
    arch_halt();
}