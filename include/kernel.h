#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "arch.h"
#include "board.h"
#include "ipc.h"

typedef long ssize_t;
typedef long off_t;

#define HZ 100
#define VERSION "0.1"

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENOSPC 28
#define ELOOP 40

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define O_ACCMODE 3
#define O_CREAT   0x40
#define O_EXCL    0x80
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_NONBLOCK 0x800

#define PATH_MAX 256

#define NPROC 32
#define NOFILE 32
#define NVMA 24
#define KSTACK_SIZE 16384
#define STACK_SIZE (128 * 1024)
#define USER_STACK_TOP 0x00007FFFFFFFF000UL
#define MMAP_BASE      0x0000002000000000UL

#define MAX_PROC_CAPS 8
struct proc_cap {
    uint32_t type;
    uintptr_t base;
    size_t size;
};

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
char *strrchr(const char *, int);
void kprintf(const char *fmt, ...);
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));

int kstrcasecmp(const char *a, const char *b);
void cfg_load(const char *text, size_t len);
const char *cfg_get(const char *section, const char *key, const char *def);
int cfg_foreach(const char *section, void (*cb)(const char *key, const char *val, void *ctx), void *ctx);
void cmdline_set(const char *s);
const char *cmdline_get(const char *key);

void pmm_init(uintptr_t start, uintptr_t end);
uintptr_t pmm_alloc(void);
void pmm_free(uintptr_t pa);
size_t pmm_free_count(void);
size_t pmm_total_count(void);
void heap_init(void *base, size_t size);
void *kmalloc(size_t n);
void kfree(void *p);
void *krealloc(void *p, size_t n);

enum { P_UNUSED, P_EMBRYO, P_RUNNABLE, P_RUNNING, P_SLEEPING, P_ZOMBIE };

struct vma { uintptr_t start, end; int dev; };   /* dev: maps device memory (never freed to the pmm) */
struct mm {
    uintptr_t root;
    struct vma vma[NVMA];
    int nvma;
    uintptr_t brk_start, brk_end, mmap_next;
};
struct file;

struct proc {
    int pid, ppid, pgid, state, killed, exit_status, umask;
    uint32_t cloexec;
    void *chan;
    struct mm mm;
    uint8_t *kstack;
    uintptr_t ksp;
    struct trapframe *tf;
    struct file *fd[NOFILE];
    char cwd[PATH_MAX];
    uint64_t sigh[65];
    struct arch_uctx uctx;
    char name[16];
    /* IPC: a blocked ipc_call() waits for its reply here */
    int ipc_waiting, ipc_have_reply, ipc_wait_port;
    struct chi_ipc_msg ipc_rep;
    /* Hybrid driver capabilities */
    int is_driver_loader;
    int ncaps;
    struct proc_cap caps[MAX_PROC_CAPS];
};
extern struct proc *cur;
extern struct proc ptable[NPROC];
extern volatile uint64_t ticks;

/* Hybrid driver subsystem */
int  cap_check(struct proc *p, uint32_t type, uintptr_t val1, size_t val2);
long cap_grant(int pid, uint32_t type, uintptr_t val1, size_t val2);
long dev_mmap_phys(uintptr_t phys, size_t len);
long dev_irq_claim(unsigned irq);
long dev_irq_wait(unsigned irq);
long dev_info_query(int type, void *uinfo);
void dev_driver_cleanup(struct proc *p);

void sched_init(void);
void scheduler(void) __attribute__((noreturn));
void yield(void);
void sleep_on(void *chan);
void wakeup(void *chan);
struct proc *proc_alloc(void);
void proc_exit(int wstatus) __attribute__((noreturn));
int  proc_fork(struct trapframe *tf);
int  proc_exec(struct proc *p, const char *path, char **kargv, char **kenvp, struct trapframe *tf);
int  proc_wait(int pid, uintptr_t ustatus, int options);
int  proc_kill(int pid, int sig);
void proc_signal(struct proc *p, int sig);
void timer_tick(int from_user);
extern int init_pid;
int  proc_spawn_init(const char *path, char **argv, char **envp, const char *console);
void init_exited(int wstatus);
void check_killed(void);

int  mm_map(struct mm *mm, uintptr_t va, size_t len);
int  mm_map_dev(struct mm *mm, uintptr_t va, size_t len, uintptr_t pa);   /* map device memory (e.g. /dev/fb0) */
void mm_free(struct mm *mm);
int  mm_unmap(struct mm *mm, uintptr_t va, size_t len);
int  mm_copyout(struct mm *mm, uintptr_t va, const void *src, size_t len);
int  uok(uintptr_t va, size_t len);
int  ustrcpy(char *dst, uintptr_t usrc, size_t max);

enum { DEV_NONE, DEV_NULL, DEV_ZERO, DEV_TTY, DEV_FB };
struct node {
    char *path; uint32_t mode; int dev;
    uint8_t *data; size_t size, cap; int is_static;
    uint32_t ino, mtime;
    const struct fs_driver *fs; uint32_t fs_ino; int lazy;
    int refs;        /* open file descriptions that use this node */
    int unlinked;    /* removed from the tree; freed when the last open file closes */
    int base;        /* 1 = unchanged since the disk was mounted (never written to the log) */
    int unflushed;   /* contents changed since they were last written to the persistence log */
};
struct fs_driver {
    const char *name;
    int (*probe)(uint64_t base_lba);
    int (*mount)(uint64_t base_lba);
    int (*read)(struct node *n, void *buf);
    /* optional: filesystem identity for the persistence log */
    int (*ident)(uint64_t base_lba, uint64_t *fs_end_lba, uint64_t *fsid);
};
extern const struct fs_driver ext_driver;
enum { F_NODE = 1, F_PIPE_R, F_PIPE_W };
struct pipe;
struct file { int type, refs, flags; struct node *node; size_t off; struct pipe *pipe; };

void vfs_init(void);                                   /* empty tree with "/" */
void vfs_load_cpio(const void *cpio, size_t size);     /* import an initramfs */
void vfs_finish(void);                                 /* /dev, /tmp */
struct node *vfs_add(const char *abs, uint32_t mode, uint64_t size, uint32_t fs_ino, const struct fs_driver *fs);
int  node_load(struct node *n);
int  blk_mount_root(int part);                         /* block.c: 0 = auto */
int  vfs_resolve(const char *in, int follow_last, char *out);
struct node *node_find(const char *abs);
struct node *node_create(const char *abs, uint32_t mode);
int  node_unlink(struct node *n);
int  vfs_open(const char *path, int flags, int mode, struct file **out);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
ssize_t file_read(struct file *f, void *buf, size_t n);
ssize_t file_write(struct file *f, const void *buf, size_t n);
int  file_readable(struct file *f);
int  file_getdents(struct file *f, void *buf, size_t n);
int  pipe_create(struct file **r, struct file **w);
int  node_is_empty_dir(struct node *d);
int  vfs_rename(const char *from, const char *to);
int  vfs_symlink(const char *target, const char *abs);

void vfs_init(void);
void vfs_load_cpio(const void *cpio, size_t size);
void vfs_finish(void);
struct node *vfs_add(const char *abs, uint32_t mode, uint64_t size, uint32_t fs_ino, const struct fs_driver *fs);
int  node_load(struct node *n);
int  blk_mount_root(int part);
int  vfs_resolve(const char *in, int follow_last, char *out);
struct node *node_find(const char *abs);
struct node *node_create(const char *abs, uint32_t mode);
int  node_unlink(struct node *n);
int  vfs_open(const char *path, int flags, int mode, struct file **out);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
ssize_t file_read(struct file *f, void *buf, size_t n);
ssize_t file_write(struct file *f, const void *buf, size_t n);
int  file_readable(struct file *f);
int  file_getdents(struct file *f, void *buf, size_t n);
int  pipe_create(struct file **r, struct file **w);
int  node_is_empty_dir(struct node *d);
int  vfs_rename(const char *from, const char *to);
int  vfs_symlink(const char *target, const char *abs);
int  vfs_each_node(int (*fn)(struct node *, void *), void *ctx);   /* stops at the first nonzero result */
void vfs_mark_base(void);                                          /* current tree is the disk image */
struct node *vfs_put_node(const char *abs, uint32_t mode, const uint8_t *data, size_t size);
void kernel_poweroff(void) __attribute__((noreturn));              /* flush persistent state, then power off */

/* persist.c: changes that survive reboot (see the comment at the top of that file) */
void persist_attach_fs(const struct fs_driver *fs, uint64_t base_lba);
void persist_touch(struct node *n);
void persist_node_created(struct node *n);
void persist_deleted(const char *abs);
void persist_renamed(const char *from, const char *to);
void persist_moved(struct node *n);
void persist_chmod(struct node *n);
int  persist_flush(struct node *n);
int  persist_sync_all(void);

void procfs_refresh(void);

void tty_init(void);
void console_services_init(void);
void tty_input(int c);
void console_poll(void);
ssize_t tty_read(void *buf, size_t n, int nonblock);
ssize_t tty_write(const void *buf, size_t n);
long tty_ioctl(unsigned long cmd, uintptr_t arg);
int  tty_readable(void);

void syscall_dispatch(struct trapframe *tf);
void trap_user_fault(struct trapframe *tf, const char *why, uintptr_t addr, int sig);

#include "driver.h"
#include "console.h"
#include "fb.h"
#include "vt.h"
#include "font.h"
