/* memtest [MiB]: maps anonymous memory, writes a pattern to every page, reads it back.  With more
 * memory than the primary RAM range, it only succeeds if extra RAM (external or a second memory
 * node) is handed out by the frame allocator just like primary RAM. */
#include "libchiffon.h"

#define CHUNK (8UL << 20)
#define MAXCHUNKS 40
static unsigned int *chunk[MAXCHUNKS];

static void put_num(unsigned long v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    for (; v; v /= 10) b[--i] = (char)('0' + v % 10);
    chiffon_puts(b + i);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    unsigned long mib = 224;
    if (argc > 1) { mib = 0; for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) mib = mib * 10 + (unsigned long)(*s - '0'); }
    int n = (int)(mib / 8);
    if (n > MAXCHUNKS) n = MAXCHUNKS;
    int mapped = 0;
    for (int i = 0; i < n; i++) {
        long p = chiffon_mmap(0, CHUNK, 3 /* PROT_READ|WRITE */, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
        if (p < 0 && p > -4096) break;
        chunk[mapped++] = (unsigned int *)p;
    }
    for (int i = 0; i < mapped; i++) {
        unsigned int *c = chunk[i];
        for (unsigned long pg = 0; pg < CHUNK / 4096; pg++) c[pg * 1024] = (unsigned int)(i * 1000003u + pg);
    }
    unsigned long bad = 0, pages = 0;
    for (int i = 0; i < mapped; i++) {
        unsigned int *c = chunk[i];
        for (unsigned long pg = 0; pg < CHUNK / 4096; pg++, pages++)
            if (c[pg * 1024] != (unsigned int)(i * 1000003u + pg)) bad++;
    }
    chiffon_puts("memtest: mapped "); put_num((unsigned long)mapped * 8); chiffon_puts(" MiB requested "); put_num(mib);
    chiffon_puts(" MiB, checked "); put_num(pages); chiffon_puts(" pages, bad "); put_num(bad); chiffon_puts("\n");
    return (mapped == n && !bad) ? 0 : 1;
}
