/* Guest smoke check for ordinary mapping and protection syscall behavior. */
#include "libchiffon.h"

#if defined(__x86_64__)
#define MPROTECT_SYSCALL 10
#elif defined(__aarch64__)
#define MPROTECT_SYSCALL 226
#endif

#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define EACCES 13
#define PAGE_BYTES 4096

static long protect(void *page, int prot) {
    return ksys(MPROTECT_SYSCALL, (long)page, PAGE_BYTES, prot, 0, 0, 0);
}

static int fail(const char *stage) {
    chiffon_puts("FAIL vm-probe ");
    chiffon_puts(stage);
    chiffon_puts("\n");
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int rw = PROT_READ | PROT_WRITE;
    int rwx = rw | PROT_EXEC;
    if (chiffon_mmap(0, PAGE_BYTES, rwx, flags, -1, 0) != -EACCES)
        return fail("mmap denial");

    long address = chiffon_mmap(0, PAGE_BYTES, rw, flags, -1, 0);
    if (address <= 0) return fail("anonymous mmap");
    void *page = (void *)address;
    volatile unsigned char *data = page;
    if (data[0] != 0) return fail("anonymous mmap contents");
    data[0] = 42;

    if (protect(page, PROT_READ | PROT_EXEC) != 0)
        return fail("mprotect read-execute");
    if (protect(page, rwx) != -EACCES) return fail("mprotect denial");
    if (protect(page, PROT_READ) != 0) return fail("mprotect read-only");
    if (data[0] != 42) return fail("parent mapping contents");

    int child = chiffon_fork();
    if (child < 0) return fail("fork");
    if (child == 0) {
        if (data[0] != 42) chiffon_exit(fail("child mapping contents"));
        if (protect(page, rw) != 0)
            chiffon_exit(fail("child mprotect read-write"));
        data[0] = 17;
        if (data[0] != 17) chiffon_exit(fail("child write"));
        chiffon_exit(0);
    }

    int status = -1;
    if (chiffon_wait(&status) != child || status != 0)
        return fail("child exit status");
    if (data[0] != 42) return fail("private mapping after fork");
    if (chiffon_munmap(page, PAGE_BYTES) != 0) return fail("munmap");
    chiffon_puts("PASS vm-probe\n");
    return 0;
}
