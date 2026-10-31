#pragma once
typedef unsigned long size_t;
typedef long ssize_t;
#include "uapi/chiffon_abi.h"

#if defined(__x86_64__)
enum { CF_read = 0, CF_write = 1, CF_close = 3, CF_clone = 56, CF_execve = 59, CF_exit = 60, CF_wait4 = 61,
       CF_uname = 63, CF_getcwd = 79, CF_chdir = 80, CF_reboot = 169, CF_getdents64 = 217, CF_openat = 257,
       CF_mkdirat = 258, CF_pipe2 = 293, CF_dup3 = 292, CF_getpid = 39,
       CF_ioctl = 16, CF_mmap = 9, CF_munmap = 11, CF_nanosleep = 35,
       CF_unlinkat = 263, CF_symlinkat = 266, CF_renameat = 264, CF_fchmodat = 268, CF_sync = 162, CF_fsync = 74 };
static inline long ksys(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
__asm__(".globl _start\n_start:\n xor %ebp,%ebp\n mov (%rsp),%rdi\n lea 8(%rsp),%rsi\n and $-16,%rsp\n call chiffon_entry\n");
#elif defined(__aarch64__)
enum { CF_read = 63, CF_write = 64, CF_close = 57, CF_clone = 220, CF_execve = 221, CF_exit = 93, CF_wait4 = 260,
       CF_uname = 160, CF_getcwd = 17, CF_chdir = 49, CF_reboot = 142, CF_getdents64 = 61, CF_openat = 56,
       CF_mkdirat = 34, CF_pipe2 = 59, CF_dup3 = 24, CF_getpid = 172,
       CF_ioctl = 29, CF_mmap = 222, CF_munmap = 215, CF_nanosleep = 101,
       CF_unlinkat = 35, CF_symlinkat = 36, CF_renameat = 38, CF_fchmodat = 53, CF_sync = 81, CF_fsync = 82 };
static inline long ksys(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c,
                  x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
__asm__(".globl _start\n_start:\n mov x29,xzr\n ldr x0,[sp]\n add x1,sp,#8\n bl chiffon_entry\n");
#else
#error "libchiffon.h: unsupported architecture"
#endif

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200

static inline ssize_t chiffon_read(int fd, void *b, size_t n) { return ksys(CF_read, fd, (long)b, n, 0, 0, 0); }
static inline ssize_t chiffon_write(int fd, const void *b, size_t n) { return ksys(CF_write, fd, (long)b, n, 0, 0, 0); }
static inline int chiffon_open(const char *p, int fl, int mode) { return ksys(CF_openat, AT_FDCWD, (long)p, fl, mode, 0, 0); }
static inline int chiffon_close(int fd) { return ksys(CF_close, fd, 0, 0, 0, 0, 0); }
static inline int chiffon_fork(void) { return ksys(CF_clone, 17 /*SIGCHLD*/, 0, 0, 0, 0, 0); }
static inline int chiffon_execve(const char *p, char **argv, char **envp) { return ksys(CF_execve, (long)p, (long)argv, (long)envp, 0, 0, 0); }
static inline int chiffon_wait(int *status) { return ksys(CF_wait4, -1, (long)status, 0, 0, 0, 0); }
static inline int chiffon_chdir(const char *p) { return ksys(CF_chdir, (long)p, 0, 0, 0, 0, 0); }
static inline long chiffon_getcwd(char *b, size_t n) { return ksys(CF_getcwd, (long)b, n, 0, 0, 0, 0); }
static inline int chiffon_mkdir(const char *p, int mode) { return ksys(CF_mkdirat, AT_FDCWD, (long)p, mode, 0, 0, 0); }
static inline int chiffon_unlink(const char *p, int rmdir) { return ksys(CF_unlinkat, AT_FDCWD, (long)p, rmdir ? 0x200 : 0, 0, 0, 0); }
static inline int chiffon_rename(const char *a, const char *b) { return ksys(CF_renameat, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 0, 0); }
static inline int chiffon_symlink(const char *target, const char *link) { return ksys(CF_symlinkat, (long)target, AT_FDCWD, (long)link, 0, 0, 0); }
static inline int chiffon_chmod(const char *p, int mode) { return ksys(CF_fchmodat, AT_FDCWD, (long)p, mode, 0, 0, 0); }
static inline int chiffon_fsync(int fd) { return ksys(CF_fsync, fd, 0, 0, 0, 0, 0); }
static inline int chiffon_sync(void) { return ksys(CF_sync, 0, 0, 0, 0, 0, 0); }
static inline long chiffon_getdents(int fd, void *b, size_t n) { return ksys(CF_getdents64, fd, (long)b, n, 0, 0, 0); }
static inline int chiffon_uname(void *u) { return ksys(CF_uname, (long)u, 0, 0, 0, 0, 0); }
static inline int chiffon_pipe(int fds[2]) { return ksys(CF_pipe2, (long)fds, 0, 0, 0, 0, 0); }
static inline int chiffon_dup2(int a, int b) { return ksys(CF_dup3, a, b, 0, 0, 0, 0); }
static inline __attribute__((noreturn)) void chiffon_exit(int c) { for (;;) ksys(CF_exit, c, 0, 0, 0, 0, 0); }
static inline __attribute__((noreturn)) void chiffon_poweroff(void) { for (;;) ksys(CF_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0); }
static inline long chiffon_ioctl(int fd, unsigned long cmd, void *arg) { return ksys(CF_ioctl, fd, cmd, (long)arg, 0, 0, 0); }
/* returns the mapping address, or a small negative errno */
static inline long chiffon_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) { return ksys(CF_mmap, (long)addr, len, prot, flags, fd, off); }
static inline int chiffon_munmap(void *addr, size_t len) { return ksys(CF_munmap, (long)addr, len, 0, 0, 0, 0); }
static inline int chiffon_sleep_ms(unsigned ms) { long ts[2] = { ms / 1000, (long)(ms % 1000) * 1000000L }; return ksys(CF_nanosleep, (long)ts, 0, 0, 0, 0, 0); }
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1

/* IPC (see include/uapi/chiffon_abi.h) */
static inline long chiffon_ipc_create(const char *name) { return ksys(CSYS_ipc_create, (long)name, 0, 0, 0, 0, 0); }
static inline long chiffon_ipc_lookup(const char *name) { return ksys(CSYS_ipc_lookup, (long)name, 0, 0, 0, 0, 0); }
static inline long chiffon_ipc_close(long port) { return ksys(CSYS_ipc_close, port, 0, 0, 0, 0, 0); }
static inline long chiffon_ipc_send(long port, const struct chi_ipc_msg *m, int flags) { return ksys(CSYS_ipc_send, port, (long)m, flags, 0, 0, 0); }
static inline long chiffon_ipc_recv(long port, struct chi_ipc_msg *m, int flags) { return ksys(CSYS_ipc_recv, port, (long)m, flags, 0, 0, 0); }
static inline long chiffon_ipc_call(long port, const struct chi_ipc_msg *req, struct chi_ipc_msg *rep) { return ksys(CSYS_ipc_call, port, (long)req, (long)rep, 0, 0, 0); }
static inline long chiffon_ipc_reply(const struct chi_ipc_msg *rep) { return ksys(CSYS_ipc_reply, (long)rep, 0, 0, 0, 0, 0); }

/* Driver capabilities & hardware device operations */
static inline long chiffon_cap_grant(int pid, unsigned int type, unsigned long val1, unsigned long val2) {
    return ksys(CSYS_cap_grant, pid, type, val1, val2, 0, 0);
}
static inline long chiffon_dev_map(unsigned long phys, size_t len) {
    return ksys(CSYS_dev_map, phys, len, 0, 0, 0, 0);
}
static inline long chiffon_irq_claim(unsigned int irq) {
    return ksys(CSYS_irq_claim, irq, 0, 0, 0, 0, 0);
}
static inline long chiffon_irq_wait(unsigned int irq) {
    return ksys(CSYS_irq_wait, irq, 0, 0, 0, 0, 0);
}
static inline long chiffon_dev_info(int type, void *info) {
    return ksys(CSYS_dev_info, type, (long)info, 0, 0, 0, 0);
}
static inline int chiffon_kill(int pid, int sig) {
#if defined(__x86_64__)
    return ksys(62 /* SYS_kill */, pid, sig, 0, 0, 0, 0);
#elif defined(__aarch64__)
    return ksys(129 /* SYS_kill */, pid, sig, 0, 0, 0, 0);
#endif
}

static inline unsigned long chiffon_cycles(void) {
#if defined(__aarch64__)
    unsigned long val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__x86_64__)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long)hi << 32) | lo;
#else
    return 0;
#endif
}

static inline unsigned long chiffon_timer_freq(void) {
#if defined(__aarch64__)
    unsigned long freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq ? freq : 62500000UL;
#elif defined(__x86_64__)
    return 2000000000UL;
#else
    return 1000000UL;
#endif
}

/* minimal string helpers (also satisfy compiler-generated calls) */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
static inline size_t chiffon_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline int chiffon_strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
static inline void chiffon_puts(const char *s) { chiffon_write(1, s, chiffon_strlen(s)); }

int main(int argc, char **argv, char **envp);
__attribute__((noreturn, used)) void chiffon_entry(long argc, char **argv) { chiffon_exit(main((int)argc, argv, argv + argc + 1)); }
