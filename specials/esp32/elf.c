/* Loader for static Xtensa ELF32 programs read from the root filesystem.  Segments go to the fixed
 * addresses in programs/prog.ld.  IRAM only accepts 32-bit writes, so code is copied word by word. */
#include "kernel.h"
#include "../programs/api.h"

int ext_open(const char *path, uint32_t *ino, uint32_t *size);
long ext_read_ino(uint32_t ino, uint64_t off, uint8_t *buf, size_t n);

#define PT_LOAD 1
#define EM_XTENSA 94
#define PROG_TEXT_LO 0x40090000u
#define PROG_TEXT_HI 0x40098000u
#define PROG_DATA_LO 0x3FFD4000u
#define PROG_DATA_HI 0x3FFDC000u
#define PROG_MAX     0x10000u

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* The API handed to programs. */
static int api_printf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
    return 0;
}
static void api_putc(int c) { console_putc(c); }
static void *api_malloc(size_t n) { return kmalloc(n); }
static void api_free(void *p) { kfree(p); }
static uint32_t api_ccount(void) { uint32_t c; __asm__ volatile("rsr.ccount %0" : "=a"(c)); return c; }

static int in_window(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi) {
    return a >= lo && a + n <= hi && a + n >= a;
}

/* Load and run the program at path.  Returns its exit value, or a negative error. */
int elf_run(const char *path, int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t ino, size;
    if (ext_open(path, &ino, &size)) return -ENOENT;
    if (size < 52 || size > PROG_MAX) return -ENOEXEC;
    uint8_t *img = kmalloc(size);
    if (!img) return -ENOMEM;
    if (ext_read_ino(ino, 0, img, size) != (long)size) { kfree(img); return -EIO; }
    int ret = -ENOEXEC;
    if (img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F' || img[4] != 1 || img[5] != 1 ||
        le16(img + 18) != EM_XTENSA) goto done;
    uint32_t entry = le32(img + 24), phoff = le32(img + 28), phentsize = le16(img + 42), phnum = le16(img + 44);
    if (phentsize < 32 || phoff + phnum * phentsize > size) goto done;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + i * phentsize;
        if (le32(ph) != PT_LOAD) continue;
        uint32_t off = le32(ph + 4), vaddr = le32(ph + 8), filesz = le32(ph + 16), memsz = le32(ph + 20);
        if (filesz > memsz || off + filesz > size) goto done;
        int text = in_window(vaddr, memsz, PROG_TEXT_LO, PROG_TEXT_HI);
        int data = in_window(vaddr, memsz, PROG_DATA_LO, PROG_DATA_HI);
        if (!text && !data) goto done;                    /* the program asked for memory it may not use */
        if (text) {
            volatile uint32_t *dst = (volatile uint32_t *)vaddr;
            for (uint32_t w = 0; w < memsz; w += 4) {
                uint32_t v = 0;
                for (int b = 0; b < 4; b++) if (w + b < filesz) v |= (uint32_t)img[off + w + b] << (8 * b);
                dst[w / 4] = v;
            }
            __asm__ volatile("memw" ::: "memory");
        } else {
            uint8_t *dst = (uint8_t *)vaddr;
            for (uint32_t w = 0; w < memsz; w++) dst[w] = w < filesz ? img[off + w] : 0;
        }
    }
    {
        struct esp_api api = { api_printf, api_putc, api_malloc, api_free, api_ccount };
        int (*run)(const struct esp_api *) = (int (*)(const struct esp_api *))entry;
        ret = run(&api);
    }
done:
    kfree(img);
    return ret;
}
