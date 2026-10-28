#include "kernel.h"
#include "sysnum.h"

#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EMPTY_PATH 0x1000
#define O_CLOEXEC 0x80000
#define BOOT_EPOCH 1700000000UL

static struct file *getfd(long fd) { return (fd < 0 || fd >= NOFILE) ? 0 : cur->fd[fd]; }
static int fd_install(struct file *f, int minfd, int cloexec) {
    for (int i = minfd; i < NOFILE; i++)
        if (!cur->fd[i]) { cur->fd[i] = f; cur->cloexec = (cur->cloexec & ~(1u << i)) | (cloexec ? 1u << i : 0); return i; }
    return -EMFILE;
}
static int upath(uintptr_t u, char *buf) { return ustrcpy(buf, u, PATH_MAX); }
static struct proc *find_proc(int pid) {
    for (int i = 0; i < NPROC; i++) if (ptable[i].state != P_UNUSED && ptable[i].pid == pid) return &ptable[i];
    return 0;
}

/* ---- stat ---- */
static void put_stat(uintptr_t u, uint32_t mode, uint64_t size, uint64_t ino, uint32_t mtime) {
    uint8_t *b = (uint8_t *)u;
    memset(b, 0, STAT_SIZE);
    *(uint64_t *)(b + 8) = ino;
#ifdef ARCH_X86_64
    *(uint64_t *)(b + 16) = 1;            /* nlink */
    *(uint32_t *)(b + 24) = mode;
    *(uint64_t *)(b + 56) = 4096;         /* blksize */
#else
    *(uint32_t *)(b + 16) = mode;
    *(uint32_t *)(b + 20) = 1;
    *(uint32_t *)(b + 56) = 4096;
#endif
    *(uint64_t *)(b + 48) = size;
    *(uint64_t *)(b + 64) = (size + 511) / 512;
    *(uint64_t *)(b + 72) = *(uint64_t *)(b + 88) = *(uint64_t *)(b + 104) = mtime;
}
static long stat_file(struct file *f, uintptr_t ub) {
    if (!uok(ub, STAT_SIZE)) return -EFAULT;
    if (f->type != F_NODE) { put_stat(ub, S_IFIFO | 0600, 0, 1, BOOT_EPOCH); return 0; }
    put_stat(ub, f->node->mode, f->node->size, f->node->ino, f->node->mtime);
    return 0;
}
static long stat_path(const char *path, int follow, uintptr_t ub) {
    char abs[PATH_MAX];
    int r = vfs_resolve(path, follow, abs);
    if (r) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    if (!uok(ub, STAT_SIZE)) return -EFAULT;
    put_stat(ub, n->mode, n->size, n->ino, n->mtime);
    return 0;
}

static long __attribute__((unused)) stat_path_u(uintptr_t up, int follow, uintptr_t ub) {
    char p[PATH_MAX];
    int r = upath(up, p);
    return r ? r : stat_path(p, follow, ub);
}

/* ---- io ---- */
static long sys_read(long fd, uintptr_t buf, size_t n) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!uok(buf, n)) return -EFAULT;
    return file_read(f, (void *)buf, n);
}
static long sys_write(long fd, uintptr_t buf, size_t n) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!uok(buf, n)) return -EFAULT;
    return file_write(f, (const void *)buf, n);
}
struct iovec { uintptr_t base; size_t len; };
static long sys_iov(long fd, uintptr_t uiov, long cnt, int wr) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (cnt < 0 || cnt > 1024 || !uok(uiov, cnt * sizeof(struct iovec))) return -EINVAL;
    struct iovec *v = (struct iovec *)uiov; long tot = 0;
    for (long i = 0; i < cnt; i++) {
        if (!uok(v[i].base, v[i].len)) return tot ? tot : -EFAULT;
        long r = wr ? file_write(f, (void *)v[i].base, v[i].len) : file_read(f, (void *)v[i].base, v[i].len);
        if (r < 0) return tot ? tot : r;
        tot += r;
        if ((size_t)r < v[i].len) break;
    }
    return tot;
}
static long sys_lseek(long fd, long off, int whence) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (f->type != F_NODE || f->node->dev) return -ESPIPE;
    long base = whence == 0 ? 0 : whence == 1 ? (long)f->off : whence == 2 ? (long)f->node->size : -1;
    if (base < 0 || base + off < 0) return -EINVAL;
    f->off = base + off;
    return f->off;
}
static long sys_openat(long dirfd, uintptr_t upathp, int flags, int mode) {
    char p[PATH_MAX]; struct file *f;
    int r = upath(upathp, p);
    if (r) return r;
    if ((r = vfs_open(p, flags, mode, &f))) return r;
    r = fd_install(f, 0, flags & O_CLOEXEC);
    if (r < 0) file_close(f);
    return r;
}
static long sys_close(long fd) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    file_close(f); cur->fd[fd] = 0; cur->cloexec &= ~(1u << fd);
    return 0;
}
static long sys_dup2(long o, long n, int cloexec) {
    struct file *f = getfd(o);
    if (!f) return -EBADF;
    if (n < 0 || n >= NOFILE) return -EBADF;
    if (o == n) return n;
    if (cur->fd[n]) file_close(cur->fd[n]);
    cur->fd[n] = file_dup(f);
    cur->cloexec = (cur->cloexec & ~(1u << n)) | (cloexec ? 1u << n : 0);
    return n;
}
static long sys_fcntl(long fd, int cmd, long arg) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case 0: case 1030: {
        if (arg < 0 || arg >= NOFILE) return -EINVAL;
        int r = fd_install(file_dup(f), arg, cmd == 1030);
        if (r < 0) file_close(f);
        return r; }
    case 1: return cur->cloexec >> fd & 1;
    case 2: cur->cloexec = (cur->cloexec & ~(1u << fd)) | (arg & 1 ? 1u << fd : 0); return 0;
    case 3: return f->flags;
    case 4: f->flags = (f->flags & ~O_NONBLOCK & ~O_APPEND) | (arg & (O_NONBLOCK | O_APPEND)); return 0;
    }
    return -EINVAL;
}
static long sys_pipe2(uintptr_t ufds, int flags) {
    struct file *r, *w;
    if (!uok(ufds, 8)) return -EFAULT;
    int e = pipe_create(&r, &w);
    if (e) return e;
    int a = fd_install(r, 0, flags & O_CLOEXEC), b = a < 0 ? -1 : fd_install(w, 0, flags & O_CLOEXEC);
    if (a < 0 || b < 0) {
        if (a >= 0) cur->fd[a] = 0;
        file_close(r); file_close(w);
        return -EMFILE;
    }
    ((int *)ufds)[0] = a; ((int *)ufds)[1] = b;
    return 0;
}
static long sys_ioctl(long fd, unsigned long cmd, uintptr_t arg) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (f->type == F_NODE && f->node->dev == DEV_TTY) return tty_ioctl(cmd, arg);
    return -ENOTTY;
}

/* ---- namespace ---- */
static long sys_getcwd(uintptr_t buf, size_t size) {
    size_t l = strlen(cur->cwd) + 1;
    if (l > size) return -ERANGE;
    if (!uok(buf, l)) return -EFAULT;
    memcpy((void *)buf, cur->cwd, l);
    return l;
}
static long sys_chdir(uintptr_t up) {
    char p[PATH_MAX], abs[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 1, abs))) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    if (!S_ISDIR(n->mode)) return -ENOTDIR;
    strlcpy(cur->cwd, abs, sizeof cur->cwd);
    return 0;
}
static long sys_access(uintptr_t up) {
    char p[PATH_MAX], abs[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 1, abs))) return r;
    return node_find(abs) ? 0 : -ENOENT;
}
static long sys_readlink(uintptr_t up, uintptr_t buf, size_t sz) {
    char p[PATH_MAX], abs[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 0, abs))) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    if (!S_ISLNK(n->mode)) return -EINVAL;
    if (!uok(buf, sz)) return -EFAULT;
    size_t l = n->size < sz ? n->size : sz;
    memcpy((void *)buf, n->data, l);
    return l;
}
static long sys_mkdir(uintptr_t up, int mode) {
    char p[PATH_MAX], abs[PATH_MAX], par[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 0, abs))) return r;
    if (node_find(abs)) return -EEXIST;
    strlcpy(par, abs, sizeof par);
    char *s = strrchr(par, '/'); if (s == par) par[1] = 0; else *s = 0;
    struct node *pn = node_find(par);
    if (!pn || !S_ISDIR(pn->mode)) return -ENOENT;
    return node_create(abs, S_IFDIR | (mode & ~cur->umask & 0777)) ? 0 : -ENOMEM;
}
static long sys_unlink(uintptr_t up, int rmdir) {
    char p[PATH_MAX], abs[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 0, abs))) return r;
    struct node *n = node_find(abs);
    if (!n || !strcmp(abs, "/")) return n ? -EPERM : -ENOENT;
    if (S_ISDIR(n->mode)) {
        if (!rmdir) return -EISDIR;
        if (!node_is_empty_dir(n)) return -ENOTEMPTY;
    } else if (rmdir) return -ENOTDIR;
    return node_unlink(n);
}
static long sys_rename(uintptr_t a, uintptr_t b) {
    char p[PATH_MAX], q[PATH_MAX], x[PATH_MAX], y[PATH_MAX];
    int r;
    if ((r = upath(a, p)) || (r = upath(b, q))) return r;
    if ((r = vfs_resolve(p, 0, x)) || (r = vfs_resolve(q, 0, y))) return r;
    return vfs_rename(x, y);
}
static long sys_symlink(uintptr_t target, uintptr_t linkp) {
    char t[PATH_MAX], p[PATH_MAX], abs[PATH_MAX];
    int r;
    if ((r = upath(target, t)) || (r = upath(linkp, p))) return r;
    if ((r = vfs_resolve(p, 0, abs))) return r;
    return vfs_symlink(t, abs);
}
static long sys_chmod(uintptr_t up, int mode) {
    char p[PATH_MAX], abs[PATH_MAX];
    int r = upath(up, p);
    if (r) return r;
    if ((r = vfs_resolve(p, 1, abs))) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    n->mode = (n->mode & S_IFMT) | (mode & 07777);
    return 0;
}

/* ---- memory ---- */
static long sys_brk(uintptr_t addr) {
    struct mm *m = &cur->mm;
    if (!addr || addr < m->brk_start || addr >= MMAP_BASE) return m->brk_end;
    uintptr_t oe = (m->brk_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), ne = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (ne > oe) { if (mm_map(m, oe, ne - oe)) return m->brk_end; }
    else if (ne < oe) mm_unmap(m, ne, oe - ne);
    m->brk_end = addr;
    return addr;
}
static long sys_mmap(uintptr_t addr, size_t len, int prot, int flags, long fd, long off) {
    struct mm *m = &cur->mm;
    if (!len) return -EINVAL;
    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    struct file *f = 0;
    if (!(flags & 0x20)) { f = getfd(fd); if (!f || f->type != F_NODE) return -EBADF; }
    if (flags & 0x10) {
        if (addr & (PAGE_SIZE - 1)) return -EINVAL;
        mm_unmap(m, addr, len);
    } else {
        addr = m->mmap_next;
        if (addr + len > USER_STACK_TOP - STACK_SIZE * 8) return -ENOMEM;
        m->mmap_next += len;
    }
    int r = mm_map(m, addr, len);
    if (r) return r;
    if (f && f->node->lazy && node_load(f->node)) return -EIO;
    if (f && (size_t)off < f->node->size) {
        size_t n = f->node->size - off; if (n > len) n = len;
        memcpy((void *)addr, f->node->data + off, n);
    }
    return addr;
}

/* ---- processes ---- */
static long copy_strv(uintptr_t uv, char ***out) {
    char **v = kmalloc(257 * sizeof(char *));
    int n = 0;
    if (!v) return -ENOMEM;
    memset(v, 0, 257 * sizeof(char *));
    while (uv) {
        if (!uok(uv, 8)) { n = -EFAULT; break; }
        uintptr_t s = ((uintptr_t *)uv)[0];
        if (!s) break;
        if (n >= 256) { n = -ENAMETOOLONG; break; }
        char *tmp = kmalloc(4096);
        if (!tmp) { n = -ENOMEM; break; }
        int r = ustrcpy(tmp, s, 4096);
        if (r) { kfree(tmp); n = r; break; }
        v[n++] = tmp;
        uv += 8;
    }
    if (n < 0) { for (int i = 0; v[i]; i++) kfree(v[i]); kfree(v); return n; }
    *out = v;
    return n;
}
static void free_strv(char **v) { if (!v) return; for (int i = 0; v[i]; i++) kfree(v[i]); kfree(v); }
static long sys_execve(struct trapframe *tf, uintptr_t upathp, uintptr_t uargv, uintptr_t uenvp) {
    char p[PATH_MAX]; char **argv = 0, **envp = 0;
    int r = upath(upathp, p);
    if (r) return r;
    if ((r = copy_strv(uargv, &argv)) < 0) return r;
    if ((r = copy_strv(uenvp, &envp)) < 0) { free_strv(argv); return r; }
    r = proc_exec(cur, p, argv, envp, tf);
    free_strv(argv); free_strv(envp);
    return r;
}
static long sys_clone(struct trapframe *tf, unsigned long flags, uintptr_t stack, uintptr_t ptid, uintptr_t ctid) {
    if (flags & (0x100 | 0x10000)) return -ENOSYS;     /* CLONE_VM / CLONE_THREAD: no threads yet */
    int pid = proc_fork(tf);
    if (pid < 0) return pid;
    struct proc *c = find_proc(pid);
    if (stack) TF_SP(c->tf) = stack;
    if ((flags & 0x100000) && uok(ptid, 4)) *(int *)ptid = pid;
    if (flags & 0x1000000) mm_copyout(&c->mm, ctid, &pid, 4);
    return pid;
}
static long sys_sigaction(int sig, uintptr_t act, uintptr_t oact) {
    if (sig <= 0 || sig > 64 || sig == 9 || sig == 19) return -EINVAL;
    if (oact) {
        if (!uok(oact, SIGACT_SIZE)) return -EFAULT;
        memset((void *)oact, 0, SIGACT_SIZE);
        *(uint64_t *)oact = cur->sigh[sig];
    }
    if (act) {
        if (!uok(act, SIGACT_SIZE)) return -EFAULT;
        cur->sigh[sig] = *(uint64_t *)act;
    }
    return 0;
}

// Stub here: glibc aint starting if kernel is old af
static long sys_uname(uintptr_t u) {
    if (!uok(u, 390)) return -EFAULT;
    char *b = (char *)u;
    memset(b, 0, 390);
    strlcpy(b, "Chiffon", 65);
    strlcpy(b + 65, cfg_get("system", "hostname", "chiffon"), 65);
    strlcpy(b + 130, "6.18.45-chiffon", 65);
    strlcpy(b + 195, "#1", 65); 
    strlcpy(b + 260, ARCH_NAME, 65);
    return 0;
}

static long sys_clock_gettime(uintptr_t u) {
    if (!uok(u, 16)) return -EFAULT;
    uint64_t t = ticks;
    ((uint64_t *)u)[0] = BOOT_EPOCH + t / HZ;
    ((uint64_t *)u)[1] = (t % HZ) * (1000000000UL / HZ);
    return 0;
}
static long sys_nanosleep(uintptr_t req) {
    if (!uok(req, 16)) return -EFAULT;
    uint64_t s = ((uint64_t *)req)[0], ns = ((uint64_t *)req)[1];
    uint64_t dl = ticks + s * HZ + (ns + (1000000000UL / HZ) - 1) / (1000000000UL / HZ);
    while (ticks < dl) {
        if (cur->killed) return -EINTR;
        sleep_on((void *)&ticks);
    }
    return 0;
}

/* ---- poll ---- */
static long do_poll(uintptr_t ufds, unsigned nfds, long tmo_ms) {
    if (nfds > NOFILE * 4 || !uok(ufds, nfds * 8)) return -EINVAL;
    uint64_t dl = tmo_ms > 0 ? ticks + (tmo_ms * HZ + 999) / 1000 : 0;
    struct pfd { int fd; short ev, rev; } *p = (struct pfd *)ufds;
    for (;;) {
        int ready = 0;
        for (unsigned i = 0; i < nfds; i++) {
            p[i].rev = 0;
            if (p[i].fd < 0) continue;
            struct file *f = getfd(p[i].fd);
            if (!f) p[i].rev = 0x20;
            else {
                if ((p[i].ev & 1) && file_readable(f)) p[i].rev |= 1;
                if ((p[i].ev & 4) && f->type != F_PIPE_R) p[i].rev |= 4;
            }
            if (p[i].rev) ready++;
        }
        if (ready || tmo_ms == 0) return ready;
        if (tmo_ms > 0 && ticks >= dl) return 0;
        if (cur->killed) return -EINTR;
        sleep_on((void *)&ticks);
    }
}

/* ---- dispatcher ---- */
#ifdef SYSCALL_TRACE
#define TRACE(...) kprintf(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

static long handle(struct trapframe *tf, long no, long a0, long a1, long a2, long a3, long a4, long a5) {
    switch (no) {
    case SYS_read: return sys_read(a0, a1, a2);
    case SYS_write: return sys_write(a0, a1, a2);
    case SYS_readv: return sys_iov(a0, a1, a2, 0);
    case SYS_writev: return sys_iov(a0, a1, a2, 1);
    case SYS_close: return sys_close(a0);
    case SYS_lseek: return sys_lseek(a0, a1, a2);
    case SYS_openat: return sys_openat(a0, a1, a2, a3);
    case SYS_fstat: { struct file *f = getfd(a0); return f ? stat_file(f, a1) : -EBADF; }
    case SYS_newfstatat: {
        char p[PATH_MAX]; int r = upath(a1, p);
        if (r) return r;
        if (!p[0] && (a3 & AT_EMPTY_PATH)) { struct file *f = getfd(a0); return f ? stat_file(f, a2) : -EBADF; }
        return stat_path(p, !(a3 & AT_SYMLINK_NOFOLLOW), a2); }
    case SYS_faccessat: return sys_access(a1);
    case SYS_readlinkat: return sys_readlink(a1, a2, a3);
    case SYS_mkdirat: return sys_mkdir(a1, a2);
    case SYS_unlinkat: return sys_unlink(a1, a2 & AT_REMOVEDIR);
    case SYS_dup: { struct file *f = getfd(a0); if (!f) return -EBADF; int r = fd_install(file_dup(f), 0, 0); if (r < 0) file_close(f); return r; }
    case SYS_dup3: if (a0 == a1) return -EINVAL; return sys_dup2(a0, a1, a2 & O_CLOEXEC);
    case SYS_fcntl: return sys_fcntl(a0, a1, a2);
    case SYS_pipe2: return sys_pipe2(a0, a1);
    case SYS_ioctl: return sys_ioctl(a0, a1, a2);
    case SYS_getdents64: { struct file *f = getfd(a0); if (!f) return -EBADF; if (!uok(a1, a2)) return -EFAULT; return file_getdents(f, (void *)a1, a2); }
    case SYS_getcwd: return sys_getcwd(a0, a1);
    case SYS_chdir: return sys_chdir(a0);
    case SYS_renameat: return sys_rename(a1, a3);
    case SYS_symlinkat: return sys_symlink(a0, a2);
    case SYS_fchmodat: return sys_chmod(a1, a2);
    case SYS_fchmod: return 0;
    case SYS_umask: { int o = cur->umask; cur->umask = a0 & 0777; return o; }
    case SYS_ppoll: {
        long ms = -1;
        if (a2) { if (!uok(a2, 16)) return -EFAULT; ms = ((long *)a2)[0] * 1000 + ((long *)a2)[1] / 1000000; }
        return do_poll(a0, a1, ms); }

    case SYS_brk: return sys_brk(a0);
    case SYS_mmap: return sys_mmap(a0, a1, a2, a3, a4, a5);
    case SYS_munmap: return mm_unmap(&cur->mm, a0, a1);
    case SYS_mprotect: case SYS_madvise: return 0;
    case SYS_mremap: return -ENOMEM;

    case SYS_clone: return sys_clone(tf, a0, a1, a2, CLONE_CTID_ARG(a3, a4));
    case SYS_execve: return sys_execve(tf, a0, a1, a2);
    case SYS_exit: case SYS_exit_group: proc_exit((a0 & 0xff) << 8);
    case SYS_wait4: return proc_wait(a0, a1, a2);
    case SYS_kill: return proc_kill(a0, a1);
    case SYS_tgkill: return proc_kill(a1, a2);
    case SYS_rt_sigaction: return sys_sigaction(a0, a1, a2);
    case SYS_rt_sigprocmask:
        if (a2 && uok(a2, 8)) *(uint64_t *)a2 = 0;
        return 0;
    case SYS_sigaltstack: return 0;
    case SYS_getpid: case SYS_gettid: return cur->pid;
    case SYS_getppid: return cur->ppid;
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid: return 0;
    case SYS_setpgid: { struct proc *p = a0 ? find_proc(a0) : cur; if (!p) return -ESRCH; p->pgid = a1 ? a1 : p->pid; return 0; }
    case SYS_getpgid: { struct proc *p = a0 ? find_proc(a0) : cur; return p ? p->pgid : -ESRCH; }
    case SYS_setsid: cur->pgid = cur->pid; return cur->pid;
    case SYS_sched_yield: yield(); return 0;
    case SYS_set_tid_address: return cur->pid;
    case SYS_set_robust_list: return 0;
    case SYS_uname: return sys_uname(a0);
    case SYS_clock_gettime: return sys_clock_gettime(a1);
    case SYS_gettimeofday: {
        if (!a0) return 0;
        if (!uok(a0, 16)) return -EFAULT;
        ((uint64_t *)a0)[0] = BOOT_EPOCH + ticks / HZ; ((uint64_t *)a0)[1] = (ticks % HZ) * (1000000 / HZ);
        return 0; }
    case SYS_nanosleep: return sys_nanosleep(a0);
    case SYS_clock_nanosleep: return sys_nanosleep(a2);
    case SYS_prlimit64: {
        if (a3) {
            if (!uok(a3, 16)) return -EFAULT;
            ((uint64_t *)a3)[0] = ((uint64_t *)a3)[1] = ~0UL;
            if (a1 == 3) ((uint64_t *)a3)[0] = 8 << 20;
            if (a1 == 7) ((uint64_t *)a3)[0] = ((uint64_t *)a3)[1] = NOFILE;
        }
        return 0; }
    case SYS_getrandom: {
        if (!uok(a0, a1)) return -EFAULT;
        for (long i = 0; i < a1; i++) ((uint8_t *)a0)[i] = (uint8_t)(ticks * 2654435761UL >> 7) ^ (uint8_t)(i * 167 + (uintptr_t)tf);
        return a1; }
    case SYS_sysinfo: {
        if (!uok(a0, 112)) return -EFAULT;
        uint64_t *s = (uint64_t *)a0; memset(s, 0, 112);
        s[0] = ticks / HZ; s[4] = pmm_total_count() * PAGE_SIZE; s[5] = pmm_free_count() * PAGE_SIZE;
        *(uint16_t *)((char *)s + 80) = NPROC; *(uint32_t *)((char *)s + 104) = 1;
        return 0; }
    case SYS_reboot: board_poweroff();

#ifdef SYS_arch_prctl
    case SYS_arch_prctl:
        if (a0 == 0x1002) { cur->uctx.fs_base = a1; arch_uctx_restore(&cur->uctx); return 0; }
        if (a0 == 0x1003) { if (!uok(a1, 8)) return -EFAULT; *(uint64_t *)a1 = cur->uctx.fs_base; return 0; }
        return -EINVAL;
#endif
#ifdef SYS_open
    case SYS_open: return sys_openat(AT_FDCWD, a0, a1, a2);
    case SYS_stat: return stat_path_u(a0, 1, a1);
    case SYS_lstat: return stat_path_u(a0, 0, a1);
    case SYS_access: return sys_access(a0);
    case SYS_pipe: return sys_pipe2(a0, 0);
    case SYS_dup2: return sys_dup2(a0, a1, 0);
    case SYS_fork: case SYS_vfork: return proc_fork(tf);
    case SYS_readlink: return sys_readlink(a0, a1, a2);
    case SYS_mkdir: return sys_mkdir(a0, a1);
    case SYS_rmdir: return sys_unlink(a0, 1);
    case SYS_unlink: return sys_unlink(a0, 0);
    case SYS_rename: return sys_rename(a0, a1);
    case SYS_symlink: return sys_symlink(a0, a1);
    case SYS_chmod: return sys_chmod(a0, a1);
    case SYS_poll: return do_poll(a0, a1, a2);
    case SYS_getpgrp: return cur->pgid;
    case SYS_getrlimit: {
        if (!uok(a1, 16)) return -EFAULT;
        ((uint64_t *)a1)[0] = ((uint64_t *)a1)[1] = a0 == 7 ? NOFILE : ~0UL;
        return 0; }
    case SYS_time: if (a0 && uok(a0, 8)) *(uint64_t *)a0 = BOOT_EPOCH + ticks / HZ; return BOOT_EPOCH + ticks / HZ;
#endif
    }
    TRACE("[chiffon] unimplemented syscall: %ld (pid %d)\n", no, cur->pid);
    return -ENOSYS;
}

void syscall_dispatch(struct trapframe *tf) {
    long r = handle(tf, TF_SYSNO(tf), TF_A0(tf), TF_A1(tf), TF_A2(tf), TF_A3(tf), TF_A4(tf), TF_A5(tf));
    TF_RET(tf) = r;
    check_killed();
}
void trap_user_fault(struct trapframe *tf, const char *why, uintptr_t addr, int sig) {
    kprintf("[chiffon] user fault: pc=%p addr=%p\n", (void *)TF_PC(tf), (void *)TF_FAULT(tf));
    proc_exit(sig);
}