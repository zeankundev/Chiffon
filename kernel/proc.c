#include "kernel.h"

struct proc ptable[NPROC];
volatile uint64_t ticks;
static int next_pid = 1;
int init_pid = 1;
static uint8_t kstacks[NPROC][KSTACK_SIZE] __attribute__((aligned(16)));

#define PGDN(x) ((x) & ~(PAGE_SIZE - 1))
#define PGUP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

static void vma_add(struct mm *mm, uintptr_t s, uintptr_t e, int dev) {
    for (int i = 0; i < mm->nvma; i++)
        if (mm->vma[i].end == s && mm->vma[i].dev == dev) { mm->vma[i].end = e; return; }
    if (mm->nvma < NVMA) mm->vma[mm->nvma++] = (struct vma){s, e, dev};
}
static int vma_is_dev(struct mm *mm, uintptr_t a) {
    for (int i = 0; i < mm->nvma; i++) if (a >= mm->vma[i].start && a < mm->vma[i].end) return mm->vma[i].dev;
    return 0;
}
int mm_map(struct mm *mm, uintptr_t va, size_t len) {
    uintptr_t s = PGDN(va), e = PGUP(va + len);
    if (e > USER_TOP || e < s || (e - s) / PAGE_SIZE > pmm_free_count()) return -ENOMEM;
    vma_add(mm, s, e, 0);
    for (uintptr_t a = s; a < e; a += PAGE_SIZE) {
        if (arch_translate(mm->root, a)) continue;
        uintptr_t pa = pmm_alloc();
        if (!pa) return -ENOMEM;
        if (arch_map(mm->root, a, pa)) { pmm_free(pa); return -ENOMEM; }
    }
    return 0;
}
/* Map physical device memory (framebuffer) at va.  The pages are not owned by the process: they are
 * shared on fork and never handed back to the page allocator. */
int mm_map_dev(struct mm *mm, uintptr_t va, size_t len, uintptr_t pa) {
    uintptr_t s = PGDN(va), e = PGUP(va + len);
    if (e > USER_TOP || e < s || (pa & (PAGE_SIZE - 1))) return -EINVAL;
    if (mm->nvma >= NVMA) return -ENOMEM;
    for (uintptr_t a = s; a < e; a += PAGE_SIZE)
        if (arch_translate(mm->root, a) || arch_map(mm->root, a, pa + (a - s))) {
            for (uintptr_t u = s; u < a; u += PAGE_SIZE) arch_unmap(mm->root, u);
            return -ENOMEM;
        }
    vma_add(mm, s, e, 1);
    return 0;
}
int mm_unmap(struct mm *mm, uintptr_t va, size_t len) {
    uintptr_t s = PGDN(va), e = PGUP(va + len);
    for (uintptr_t a = s; a < e; a += PAGE_SIZE) {
        int dev = vma_is_dev(mm, a);
        uintptr_t pa = arch_unmap(mm->root, a);
        if (pa && !dev) pmm_free(pa);
    }
    for (int i = 0; i < mm->nvma; i++) {
        struct vma *v = &mm->vma[i];
        if (v->end <= s || v->start >= e) continue;
        if (v->start >= s && v->end <= e) { mm->vma[i--] = mm->vma[--mm->nvma]; }
        else if (v->start >= s) v->start = e;
        else if (v->end <= e) v->end = s;
        else if (mm->nvma < NVMA) { mm->vma[mm->nvma++] = (struct vma){e, v->end, v->dev}; v->end = s; }
    }
    return 0;
}
void mm_free(struct mm *mm) {
    if (!mm->root) return;
    for (int i = 0; i < mm->nvma; i++)
        for (uintptr_t a = mm->vma[i].start; a < mm->vma[i].end; a += PAGE_SIZE) {
            uintptr_t pa = arch_unmap(mm->root, a);
            if (pa && !mm->vma[i].dev) pmm_free(pa);
        }
    arch_aspace_free(mm->root);
    memset(mm, 0, sizeof *mm);
}
int mm_copyout(struct mm *mm, uintptr_t va, const void *src, size_t len) {
    const uint8_t *s = src;
    while (len) {
        uintptr_t pa = arch_translate(mm->root, PGDN(va));
        if (!pa) return -EFAULT;
        size_t off = va & (PAGE_SIZE - 1), n = PAGE_SIZE - off;
        if (n > len) n = len;
        memcpy((uint8_t *)P2V(pa) + off, s, n);
        va += n; s += n; len -= n;
    }
    return 0;
}
int uok(uintptr_t va, size_t len) {
    if (!len) return 1;
    if (va + len < va || va + len > USER_TOP) return 0;
    for (uintptr_t a = PGDN(va); a < va + len; a += PAGE_SIZE)
        if (!arch_translate(cur->mm->root, a)) return 0;
    return 1;
}
int ustrcpy(char *dst, uintptr_t usrc, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((usrc + i) & (PAGE_SIZE - 1)) == 0) && !uok(usrc + i, 1)) return -EFAULT;
        dst[i] = *(char *)(usrc + i);
        if (!dst[i]) return 0;
    }
    return -ENAMETOOLONG;
}

/* ---- per-process resources: the address space and the open-file table are shared by a thread group ---- */
static struct filetab *fdtab_new(void) {
    struct filetab *t = kmalloc(sizeof *t);
    if (t) { memset(t, 0, sizeof *t); t->refs = 1; }
    return t;
}
static void fdtab_put(struct filetab *t) {
    if (!t || --t->refs > 0) return;
    for (int i = 0; i < NOFILE; i++) if (t->f[i]) { file_close(t->f[i]); t->f[i] = 0; }
    kfree(t);
}
void mm_put(struct mm *m) {
    if (!m || --m->refs > 0) return;
    mm_free(m);
    kfree(m);
}

/* ---- futexes: a waiter sleeps on the kernel alias of the futex word's physical address, so threads
 * (and processes sharing memory) meet on the same key.  Wakeups happen under the kernel lock, so the
 * check-then-sleep in FUTEX_WAIT cannot lose a wakeup. ---- */
void *futex_key(struct mm *m, uintptr_t uaddr) {
    uintptr_t pa = arch_translate(m->root, uaddr & ~(PAGE_SIZE - 1));
    return pa ? P2V(pa + (uaddr & (PAGE_SIZE - 1))) : 0;
}
int futex_wake_key(void *key, int n) {
    int woke = 0;
    if (!key) return 0;
    for (int i = 0; i < NPROC && woke < n; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_SLEEPING && p->chan == key) { p->futex_woken = 1; p->state = P_RUNNABLE; woke++; }
    }
    if (woke) sched_kick();
    return woke;
}
static void futex_timeouts(void) {
    int woke = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_SLEEPING && p->futex_deadline && ticks >= p->futex_deadline) {
            p->futex_deadline = 0; p->state = P_RUNNABLE; woke = 1;
        }
    }
    if (woke) sched_kick();
}

struct proc *proc_alloc(void) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED) continue;
        memset(p, 0, sizeof *p);
        p->state = P_EMBRYO;
        p->pid = next_pid++;
        p->tgid = p->pid;
        p->kstack = kstacks[i];
        p->umask = 022;
        strlcpy(p->cwd, "/", sizeof p->cwd);
        arch_uctx_init(&p->uctx);
        return p;
    }
    return 0;
}

/* The scheduler loop of one CPU.  It is entered holding the kernel lock and keeps it while it
 * switches to a process (the process resumes holding it); it releases it only while the CPU idles. */
void scheduler(void) {
    struct cpu *c = this_cpu();
    int last = c->id * 4;
    for (;;) {
        for (int i = 0; i < NPROC; i++) if (ptable[i].state == P_ZOMBIE && ptable[i].ppid == 0) ptable[i].state = P_UNUSED;
        struct proc *n = 0;
        for (int i = 1; i <= NPROC; i++) {
            struct proc *p = &ptable[(last + i) % NPROC];
            if (p->state == P_RUNNABLE) { n = p; last = p - ptable; break; }
        }
        if (!n) {
            c->idle = 1;
            klock_leave();
            arch_idle();
            klock_enter();
            c->idle = 0;
            continue;
        }
        c->proc = n; n->state = P_RUNNING;
        arch_aspace_switch(n->mm->root);
        arch_set_kstack((uintptr_t)n->kstack + KSTACK_SIZE);
        arch_uctx_restore(&n->uctx);
        arch_switch(&c->sched_ctx, n->ksp);
        arch_uctx_save(&n->uctx);
        arch_aspace_switch(arch_kernel_root());
        c->proc = 0;
    }
}
static void sched(void) { arch_switch(&cur->ksp, this_cpu()->sched_ctx); }
void yield(void) { cur->state = P_RUNNABLE; sched(); }
void sleep_on(void *chan) { cur->chan = chan; cur->state = P_SLEEPING; sched(); cur->chan = 0; }
void wakeup(void *chan) {
    int woke = 0;
    for (int i = 0; i < NPROC; i++)
        if (ptable[i].state == P_SLEEPING && ptable[i].chan == chan) { ptable[i].state = P_RUNNABLE; woke = 1; }
    if (woke) sched_kick();
}
void timer_tick(int from_user) {
    if (this_cpu()->id == 0) {                /* one clock: only the boot CPU counts ticks */
        ticks++;
        wakeup((void *)&ticks);
        futex_timeouts();
    }
    console_poll();
    if (from_user && cur && cur->state == P_RUNNING) yield();
}

void proc_signal(struct proc *p, int sig) {
    if (sig <= 0 || sig > 64) return;
    if (!p->is_thread && p->pid == init_pid && (sig == 10 || sig == 12 || sig == 15)) {
        kprintf("[chiffon] powering off\n");
        kernel_poweroff();
    }
    if (sig != 9 && p->sigh[sig]) return;
    switch (sig) {
    case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 28: return;
    }
    p->killed = sig;
    if (p->state == P_SLEEPING) { p->state = P_RUNNABLE; sched_kick(); }
}
/* kill(pid): every thread of the process group leader pid (pid > 0), or a process group. */
int proc_kill(int pid, int sig) {
    int found = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || p->state == P_EMBRYO || p->state == P_ZOMBIE) continue;
        if (pid > 0 ? p->tgid == pid : pid == 0 ? p->pgid == cur->pgid : pid == -1 ? p->pid != init_pid : p->pgid == -pid) {
            found = 1; proc_signal(p, sig);
        }
    }
    return found ? 0 : -ESRCH;
}
/* tgkill(tgid, tid, sig): one thread. */
int proc_kill_tid(int tid, int sig) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED && p->state != P_EMBRYO && p->state != P_ZOMBIE && p->pid == tid) {
            proc_signal(p, sig);
            return 0;
        }
    }
    return -ESRCH;
}
void check_killed(void) { if (cur->killed) proc_exit(cur->killed); }

int proc_fork(struct trapframe *tf) {
    struct proc *c = proc_alloc();
    if (!c) return -EAGAIN;
    c->ft = fdtab_new();
    if (!c->ft) { c->state = P_UNUSED; return -ENOMEM; }
    struct mm *pm = cur->mm, *cm = kmalloc(sizeof *cm);
    if (!cm) { fdtab_put(c->ft); c->ft = 0; c->state = P_UNUSED; return -ENOMEM; }
    memset(cm, 0, sizeof *cm); cm->refs = 1; c->mm = cm;
    cm->root = arch_aspace_new();
    if (!cm->root) { kfree(cm); c->mm = 0; fdtab_put(c->ft); c->ft = 0; c->state = P_UNUSED; return -ENOMEM; }
    cm->nvma = pm->nvma; memcpy(cm->vma, pm->vma, sizeof pm->vma);
    cm->brk_start = pm->brk_start; cm->brk_end = pm->brk_end; cm->mmap_next = pm->mmap_next;
    for (int i = 0; i < pm->nvma; i++)
        for (uintptr_t a = pm->vma[i].start; a < pm->vma[i].end; a += PAGE_SIZE) {
            uintptr_t src = arch_translate(pm->root, a);
            if (!src || arch_translate(cm->root, a)) continue;
            if (pm->vma[i].dev) {
                if (arch_map(cm->root, a, src)) { mm_put(cm); c->mm = 0; c->state = P_UNUSED; return -ENOMEM; }
                continue;
            }
            uintptr_t dst = pmm_alloc();
            if (!dst) { mm_put(cm); c->mm = 0; c->state = P_UNUSED; return -ENOMEM; }
            memcpy(P2V(dst), P2V(src), PAGE_SIZE);
            arch_sync_icache(P2V(dst), PAGE_SIZE);
            arch_map(cm->root, a, dst);
        }
    for (int i = 0; i < NOFILE; i++) if (cur->ft->f[i]) c->ft->f[i] = file_dup(cur->ft->f[i]);
    c->cloexec = cur->cloexec;
    strlcpy(c->cwd, cur->cwd, sizeof c->cwd);
    memcpy(c->sigh, cur->sigh, sizeof c->sigh);
    memcpy(c->name, cur->name, sizeof c->name);
    c->umask = cur->umask; c->pgid = cur->pgid; c->ppid = cur->pid;
    c->ctty = cur->ctty;
    c->is_driver_loader = cur->is_driver_loader;
    c->ncaps = cur->ncaps;
    memcpy(c->caps, cur->caps, sizeof cur->caps);
    arch_uctx_save(&cur->uctx);
    c->uctx = cur->uctx;
    c->tf = arch_kstack_setup((uintptr_t)c->kstack + KSTACK_SIZE, &c->ksp, tf);
    TF_RET(c->tf) = 0;
    c->state = P_RUNNABLE;
    sched_kick();
    return c->pid;
}

/* Terminate the calling thread.  The group leader's exit reparents its children; a thread just goes
 * away (reaped by the scheduler, nobody waits for it).  The address space and the open-file table are
 * released when the last thread of the group drops them. */
void proc_exit(int wstatus) {
    struct proc *p = cur;
    int was_init = !p->is_thread && p->pid == init_pid;
    if (was_init) init_exited(wstatus);
    if (p->clear_child_tid) {                  /* pthread_join waits for this word to become 0 */
        uint32_t zero = 0;
        if (uok(p->clear_child_tid, 4)) mm_copyout(p->mm, p->clear_child_tid, &zero, 4);
        futex_wake_key(futex_key(p->mm, p->clear_child_tid), NPROC);
        p->clear_child_tid = 0;
    }
    ipc_proc_exit(p);
    dev_driver_cleanup(p);
    arch_aspace_switch(arch_kernel_root());
    fdtab_put(p->ft); p->ft = 0;
    mm_put(p->mm); p->mm = 0;
    if (!p->is_thread) {
        for (int i = 0; i < NPROC; i++) {
            struct proc *q = &ptable[i];
            if (q->state != P_UNUSED && q->ppid == p->tgid) { q->ppid = init_pid; if (q->state == P_ZOMBIE) for (int j = 0; j < NPROC; j++) if (ptable[j].pid == init_pid) wakeup(&ptable[j]); }
        }
    }
    p->exit_status = wstatus;
    if (was_init || p->is_thread) p->ppid = 0;
    p->state = P_ZOMBIE;
    for (int i = 0; i < NPROC; i++) if (ptable[i].pid == p->ppid && ptable[i].state != P_UNUSED) wakeup(&ptable[i]);
    sched();
    panic("zombie scheduled");
}

/* exit_group: every other thread of the group is killed, then the caller exits. */
void proc_exit_group(int wstatus) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &ptable[i];
        if (q != cur && q->tgid == cur->tgid && q->state != P_UNUSED && q->state != P_EMBRYO && q->state != P_ZOMBIE)
            proc_signal(q, 9);
    }
    proc_exit(wstatus);
}

int proc_wait(int pid, uintptr_t ustatus, int options) {
    for (;;) {
        int have = 0;
        for (int i = 0; i < NPROC; i++) {
            struct proc *q = &ptable[i];
            if (q->state == P_UNUSED || q->state == P_EMBRYO || q->is_thread || q->ppid != cur->tgid) continue;
            if (pid > 0 && q->pid != pid) continue;
            have = 1;
            if (q->state == P_ZOMBIE) {
                int id = q->pid;
                if (ustatus) { if (!uok(ustatus, 4)) return -EFAULT; *(int *)ustatus = q->exit_status; }
                q->state = P_UNUSED;
                return id;
            }
        }
        if (!have) return -ECHILD;
        if (options & 1) return 0;
        if (cur->killed) return -EINTR;
        sleep_on(cur);
    }
}

/* clone(CLONE_VM | CLONE_THREAD ...): a thread is a process that shares its group's address space,
 * open files and thread group id.  The per-process state (registers, signals, cwd) is copied by
 * proc_fork and then the shared parts are pointed at the group's objects. */
int proc_thread(struct trapframe *tf, unsigned long flags, uintptr_t stack, uintptr_t tls, uintptr_t ctid) {
    int pid = proc_fork(tf);
    if (pid < 0) return pid;
    struct proc *c = 0;
    for (int i = 0; i < NPROC; i++) if (ptable[i].pid == pid) c = &ptable[i];
    mm_put(c->mm);
    c->mm = cur->mm; cur->mm->refs++;
    if (flags & 0x400) {                        /* CLONE_FILES: share the open-file table */
        fdtab_put(c->ft);
        c->ft = cur->ft; cur->ft->refs++;
    }
    c->tgid = cur->tgid;
    c->is_thread = 1;
    c->ppid = cur->ppid;
    TF_RET(c->tf) = 0;
    if (stack) TF_SP(c->tf) = stack;
    if (flags & 0x80000) arch_uctx_set_tls(&c->uctx, tls);            /* CLONE_SETTLS */
    if (flags & 0x200000) c->clear_child_tid = ctid;                  /* CLONE_CHILD_CLEARTID */
    if ((flags & 0x1000000) && uok(ctid, 4)) *(int *)ctid = pid;      /* CLONE_CHILD_SETTID */
    return pid;
}

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_BASE 7
#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_INTERP 3
#define PIE_BASE    0x0000000000400000UL   /* load address of a position-independent (ET_DYN) program */
#define INTERP_BASE 0x0000555500000000UL   /* load address of the dynamic loader named by PT_INTERP */

struct ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };

/* Map every PT_LOAD segment at bias+vaddr.  *hi receives the end of the highest segment and
 * *phdr_va the address of the program headers when they sit inside a loaded segment (AT_PHDR). */
static int load_segments(struct mm *nm, const uint8_t *img, size_t sz, const struct ehdr *eh,
                         uintptr_t bias, uintptr_t *hi, uintptr_t *phdr_va) {
    for (int i = 0; i < eh->phnum; i++) {
        const struct phdr *ph = (const void *)(img + eh->phoff + i * sizeof *ph);
        if (ph->type != PT_LOAD) continue;
        uintptr_t va = ph->vaddr + bias;
        if (va < PAGE_SIZE || va + ph->memsz > USER_TOP - STACK_SIZE * 4 || ph->filesz > ph->memsz ||
            (ph->filesz && ph->offset + ph->filesz > sz)) return -ENOEXEC;
        int r = mm_map(nm, va, ph->memsz);
        if (r) return r;
        if ((r = mm_copyout(nm, va, img + ph->offset, ph->filesz))) return r;
        if (va + ph->memsz > *hi) *hi = va + ph->memsz;
        if (eh->phoff >= ph->offset && eh->phoff < ph->offset + ph->filesz) *phdr_va = va + (eh->phoff - ph->offset);
    }
    return 0;
}

/* Load the ELF dynamic loader named by PT_INTERP (for musl: /lib/ld-musl-<arch>.so.1) at INTERP_BASE. */
static int load_interp(struct mm *nm, const char *path, uintptr_t *entry) {
    char abs[PATH_MAX];
    int r = vfs_resolve(path, 1, abs);
    if (r) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    if ((n->mode & S_IFMT) != S_IFREG) return -EACCES;
    if ((r = node_load(n))) return r;
    const uint8_t *img = n->data; size_t sz = n->size;
    const struct ehdr *eh = (const void *)img;
    if (sz < sizeof *eh || memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->type != ET_DYN || eh->machine != ELF_MACHINE || eh->phentsize != sizeof(struct phdr) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct phdr) > sz)
        return -ENOEXEC;
    uintptr_t hi = 0, phdr_va = 0;
    if ((r = load_segments(nm, img, sz, eh, INTERP_BASE, &hi, &phdr_va))) return r;
    *entry = INTERP_BASE + eh->entry;
    return 0;
}

static uint64_t rnd_state = 0x9E3779B97F4A7C15UL;
static uint64_t rnd(void) { rnd_state ^= ticks + 0x1234567; rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }

int proc_exec(struct proc *p, const char *path, char **argv, char **envp, struct trapframe *tf) {
    char abs[PATH_MAX];
    int r = vfs_resolve(path, 1, abs);
    if (r) return r;
    struct node *n = node_find(abs);
    if (!n) return -ENOENT;
    if ((n->mode & S_IFMT) != S_IFREG) return -EACCES;
    if ((r = node_load(n))) return r;
    const uint8_t *img = n->data; size_t sz = n->size;
    const struct ehdr *eh = (const void *)img;
    if (sz < sizeof *eh || memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        (eh->type != ET_EXEC && eh->type != ET_DYN) || eh->machine != ELF_MACHINE || eh->phentsize != sizeof(struct phdr) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct phdr) > sz)
        return -ENOEXEC;

    struct mm nm; memset(&nm, 0, sizeof nm);
    nm.root = arch_aspace_new();
    if (!nm.root) return -ENOMEM;
    uintptr_t bias = eh->type == ET_DYN ? PIE_BASE : 0;
    uintptr_t hi = 0, phdr_va = 0, interp_base = 0, entry = eh->entry + bias;
    for (int i = 0; i < eh->phnum; i++) {                /* PT_INTERP: start in the dynamic loader instead */
        const struct phdr *ph = (const void *)(img + eh->phoff + i * sizeof *ph);
        if (ph->type != PT_INTERP) continue;
        if (!ph->filesz || ph->filesz >= PATH_MAX || ph->offset + ph->filesz > sz) { r = -ENOEXEC; goto fail; }
        char ipath[PATH_MAX];
        memcpy(ipath, img + ph->offset, ph->filesz);
        ipath[ph->filesz] = 0;
        if ((r = load_interp(&nm, ipath, &entry))) goto fail;
        interp_base = INTERP_BASE;
    }
    if ((r = load_segments(&nm, img, sz, eh, bias, &hi, &phdr_va))) goto fail;
    if (!hi) { r = -ENOEXEC; goto fail; }
    if ((r = mm_map(&nm, USER_STACK_TOP - STACK_SIZE, STACK_SIZE))) goto fail;
    nm.brk_start = nm.brk_end = PGUP(hi);
    nm.mmap_next = MMAP_BASE;

    int argc = 0, envc = 0; size_t S = 0;
    while (argv && argv[argc]) S += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) S += strlen(envp[envc++]) + 1;
    const int naux = 15;
    size_t W = 1 + (argc + 1) + (envc + 1) + 2 * naux;
    uintptr_t top = USER_STACK_TOP, strb = top - S, rndp = (strb - 16) & ~15UL;
    uintptr_t sp = (rndp - W * 8) & ~15UL;
    size_t total = top - sp;
    uint8_t *buf = kmalloc(total);
    if (!buf) { r = -ENOMEM; goto fail; }
    memset(buf, 0, total);
    uint64_t *w = (uint64_t *)buf;
    uintptr_t sa = strb; size_t k = 0;
    w[k++] = argc;
    for (int i = 0; i < argc; i++) { w[k++] = sa; size_t l = strlen(argv[i]) + 1; memcpy(buf + (sa - sp), argv[i], l); sa += l; }
    w[k++] = 0;
    for (int i = 0; i < envc; i++) { w[k++] = sa; size_t l = strlen(envp[i]) + 1; memcpy(buf + (sa - sp), envp[i], l); sa += l; }
    w[k++] = 0;
    uint64_t r0 = rnd(), r1 = rnd();
    memcpy(buf + (rndp - sp), &r0, 8); memcpy(buf + (rndp - sp) + 8, &r1, 8);
    uint64_t aux[][2] = {
        {AT_PHDR, phdr_va}, {AT_PHENT, sizeof(struct phdr)}, {AT_PHNUM, eh->phnum}, {AT_PAGESZ, PAGE_SIZE},
        {AT_BASE, interp_base}, {AT_ENTRY, eh->entry + bias}, {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0},
        {AT_SECURE, 0}, {AT_RANDOM, rndp}, {AT_CLKTCK, HZ}, {AT_HWCAP, AUX_HWCAP}, {AT_NULL, 0},
    };
    for (int i = 0; i < naux; i++) { w[k++] = aux[i][0]; w[k++] = aux[i][1]; }
    r = mm_copyout(&nm, sp, buf, total);
    kfree(buf);
    if (r) goto fail;

    for (uintptr_t a = USER_STACK_TOP - STACK_SIZE; a < USER_STACK_TOP; a += PAGE_SIZE) arch_sync_icache(P2V(arch_translate(nm.root, a)), PAGE_SIZE);
    for (int i = 0; i < nm.nvma; i++)
        for (uintptr_t a = nm.vma[i].start; a < nm.vma[i].end; a += PAGE_SIZE) {
            uintptr_t pa = arch_translate(nm.root, a);
            if (pa) arch_sync_icache(P2V(pa), PAGE_SIZE);
        }

    struct mm *heap = kmalloc(sizeof *heap);
    if (!heap) { r = -ENOMEM; goto fail; }
    *heap = nm; heap->refs = 1;
    struct mm *old = p->mm;
    p->mm = heap;
    if (p == cur) arch_aspace_switch(heap->root);
    mm_put(old);
    for (int i = 0; i < NOFILE; i++)
        if (p->ft->f[i] && (p->cloexec >> i & 1)) { file_close(p->ft->f[i]); p->ft->f[i] = 0; }
    p->cloexec = 0;
    for (int i = 1; i < 65; i++) if (p->sigh[i] != 1) p->sigh[i] = 0;
    arch_tf_user(tf, entry, sp);
    arch_uctx_init(&p->uctx);
    if (p == cur) arch_uctx_restore(&p->uctx);
    const char *base = strrchr(abs, '/');
    strlcpy(p->name, base ? base + 1 : abs, sizeof p->name);
    return 0;
fail:
    mm_free(&nm);
    return r;
}

/* Start a process on a terminal: init (pid 1, sets init_pid) or an extra console shell (not init). */
static int spawn_process(const char *path, char **argv, char **envp, const char *console, int as_init) {
    struct proc *p = proc_alloc();
    if (!p) return -EAGAIN;
    p->mm = kmalloc(sizeof *p->mm);
    if (!p->mm) { fdtab_put(p->ft); p->ft = 0; p->state = P_UNUSED; return -ENOMEM; }
    memset(p->mm, 0, sizeof *p->mm); p->mm->refs = 1;
    p->mm->root = arch_aspace_new();
    p->pgid = p->pid;
    p->ft = fdtab_new();
    if (!p->ft) { p->state = P_UNUSED; return -ENOMEM; }
    for (int i = 0; i < 3; i++) {
        int r = vfs_open(console, 2, 0, &p->ft->f[i]);
        if (r) { for (int j = 0; j < i; j++) file_close(p->ft->f[j]); mm_put(p->mm); p->mm = 0; p->state = P_UNUSED; return r; }
    }
    if (p->ft->f[0]->type == F_NODE && p->ft->f[0]->node->dev == DEV_TTY && p->ft->f[0]->node->minor < TTY_ACTIVE_MINOR) {
        p->ctty = p->ft->f[0]->node->minor + 1;               /* the init console is its controlling terminal */
        tty_set_session(p->ft->f[0]->node->minor, p->pid);
    }
    struct trapframe tf; memset(&tf, 0, sizeof tf);
    int r = proc_exec(p, path, argv, envp, &tf);
    if (r) {
        for (int i = 0; i < 3; i++) file_close(p->ft->f[i]);
        mm_put(p->mm); p->mm = 0; p->state = P_UNUSED;
        return r;
    }
    p->tf = arch_kstack_setup((uintptr_t)p->kstack + KSTACK_SIZE, &p->ksp, &tf);
    strlcpy(p->name, as_init ? "init" : "sh", sizeof p->name);
    p->is_driver_loader = 1;
    p->state = P_RUNNABLE;
    if (as_init) init_pid = p->pid;
    sched_kick();
    return p->pid;
}

int proc_spawn_init(const char *path, char **argv, char **envp, const char *console) {
    return spawn_process(path, argv, envp, console, 1);
}

/* A second console shell (e.g. on the framebuffer terminal).  It is not init: its exit does not power the
 * system off, and it has no parent to wait for it (it is reaped when it exits). */
int proc_spawn_console(const char *path, char **argv, char **envp, const char *console) {
    return spawn_process(path, argv, envp, console, 0);
}
