#include "kernel.h"

struct proc ptable[NPROC];
volatile uint64_t ticks;
static int next_pid = 1;
int init_pid = 1;
static uint8_t kstacks[NPROC][KSTACK_SIZE] __attribute__((aligned(16)));

#define PGDN(x) ((x) & ~(PAGE_SIZE - 1))
#define PGUP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

static struct vma *vma_left(struct mm *mm, uintptr_t s, int dev) {
    for (int i = 0; i < mm->nvma; i++)
        if (mm->vma[i].end == s && mm->vma[i].dev == dev) return &mm->vma[i];
    return 0;
}
static int vma_kind(struct mm *mm, uintptr_t a) {
    for (int i = 0; i < mm->nvma; i++) if (a >= mm->vma[i].start && a < mm->vma[i].end) return mm->vma[i].dev;
    return 0;
}
static int map_pages(struct mm *mm, uintptr_t va, size_t len, uintptr_t phys, int dev, int prot) {
    if (!len || va < PAGE_SIZE || va >= USER_TOP || len > USER_TOP - va ||
        (prot & ~(VM_READ | VM_WRITE | VM_EXEC))) return -EINVAL;
    if ((prot & VM_EXEC) && ((prot & VM_WRITE) || dev)) return -EACCES;
    uintptr_t s = PGDN(va), e = PGUP(va + len);
    uintptr_t stack_base = USER_STACK_TOP - STACK_SIZE;
    if (s < stack_base && e > stack_base - PAGE_SIZE) return -EINVAL;
    struct vma *left = vma_left(mm, s, dev);
    if (!left && mm->nvma >= NVMA) return -ENOMEM;
    if (!dev && (e - s) / PAGE_SIZE > pmm_free_count()) return -ENOMEM;
    for (uintptr_t a = s; a < e; a += PAGE_SIZE)
        if (arch_translate(mm->root, a)) return -ENOMEM;
    uintptr_t a;
    for (a = s; a < e; a += PAGE_SIZE) {
        uintptr_t pa = dev ? phys + (a - s) : pmm_alloc();
        if (!dev && !pa) break;
        int r = dev ? arch_map_device_user(mm->root, a, pa, prot) : arch_map(mm->root, a, pa, prot);
        if (r) { if (!dev) pmm_free(pa); break; }
    }
    if (a != e) {
        for (uintptr_t u = s; u < a; u += PAGE_SIZE) {
            uintptr_t pa = arch_unmap(mm->root, u);
            if (!dev) pmm_free(pa);
        }
        return -ENOMEM;
    }
    if (left) left->end = e;
    else mm->vma[mm->nvma++] = (struct vma){s, e, dev};
    return 0;
}
int mm_map_prot(struct mm *mm, uintptr_t va, size_t len, int prot) {
    return map_pages(mm, va, len, 0, 0, prot);
}
int mm_map(struct mm *mm, uintptr_t va, size_t len) {
    return mm_map_prot(mm, va, len, VM_READ | VM_WRITE);
}
/* Map physical device memory at va. These pages are never handed to the page allocator. */
int mm_map_dev_prot(struct mm *mm, uintptr_t va, size_t len, uintptr_t pa, int prot, int writable) {
    if (!pa || ((pa | va) & (PAGE_SIZE - 1)) || len > USER_TOP || PGUP(len) > UINTPTR_MAX - pa)
        return -EINVAL;
    if (!writable && (prot & VM_WRITE)) return -EACCES;
    return map_pages(mm, va, len, pa, writable ? VMA_DEVICE_RW : VMA_DEVICE_RO, prot);
}
int mm_map_dev(struct mm *mm, uintptr_t va, size_t len, uintptr_t pa) {
    return mm_map_dev_prot(mm, va, len, pa, VM_READ | VM_WRITE, 1);
}
int mm_protect(struct mm *mm, uintptr_t va, size_t len, int prot) {
    if ((va & (PAGE_SIZE - 1)) || va >= USER_TOP || len > USER_TOP - va ||
        (prot & ~(VM_READ | VM_WRITE | VM_EXEC))) return -EINVAL;
    if ((prot & (VM_WRITE | VM_EXEC)) == (VM_WRITE | VM_EXEC)) return -EACCES;
    uintptr_t end = PGUP(va + len);
    for (uintptr_t a = va; a < end; a += PAGE_SIZE) {
        if (!arch_translate(mm->root, a)) return -ENOMEM;
        int kind = vma_kind(mm, a);
        if ((prot & VM_EXEC) && kind != VMA_RAM) return -EACCES;
        if ((prot & VM_WRITE) && kind == VMA_DEVICE_RO) return -EACCES;
    }
    for (uintptr_t a = va; a < end; a += PAGE_SIZE) {
        if ((prot & VM_EXEC) && !(arch_page_prot(mm->root, a) & VM_EXEC))
            arch_sync_icache(P2V(arch_translate(mm->root, a)), PAGE_SIZE);
        arch_protect(mm->root, a, prot);
    }
    return 0;
}
int mm_unmap(struct mm *mm, uintptr_t va, size_t len) {
    if (!len || (va & (PAGE_SIZE - 1)) || va >= USER_TOP || len > USER_TOP - va) return -EINVAL;
    uintptr_t s = va, e = PGUP(va + len);
    int slots = mm->nvma;
    for (int i = 0; i < mm->nvma; i++) {
        const struct vma *v = &mm->vma[i];
        if (s <= v->start && e >= v->end) slots--;
        else if (s > v->start && e < v->end) slots++;
    }
    if (slots > NVMA) return -ENOMEM;
    for (int i = 0; i < mm->nvma; i++) {
        struct vma *v = &mm->vma[i];
        if (v->end <= s || v->start >= e) continue;
        uintptr_t start = s > v->start ? s : v->start;
        uintptr_t end = e < v->end ? e : v->end;
        for (uintptr_t a = start; a < end; a += PAGE_SIZE) {
            uintptr_t pa = arch_unmap(mm->root, a);
            if (pa && !v->dev) pmm_free(pa);
        }
        if (v->start >= s && v->end <= e) { mm->vma[i--] = mm->vma[--mm->nvma]; }
        else if (v->start >= s) v->start = e;
        else if (v->end <= e) v->end = s;
        else { mm->vma[mm->nvma++] = (struct vma){e, v->end, v->dev}; v->end = s; }
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
static int mm_access(struct mm *mm, uintptr_t va, size_t len, int prot) {
    if (!len) return 1;
    if (!mm || va >= USER_TOP || len > USER_TOP - va) return 0;
    for (uintptr_t a = PGDN(va); a < va + len; a += PAGE_SIZE)
        if (!(arch_page_prot(mm->root, a) & prot)) return 0;
    return 1;
}
int mm_user_write(struct mm *mm, uintptr_t va, size_t len) {
    return mm_access(mm, va, len, VM_WRITE);
}
/* Initialize pages freshly allocated by exec or file-backed mmap. */
int mm_load_bytes(struct mm *mm, uintptr_t va, const void *src, size_t len) {
    if (len && (va >= USER_TOP || len > USER_TOP - va)) return -EFAULT;
    const uint8_t *s = src;
    while (len) {
        uintptr_t pa = arch_translate(mm->root, PGDN(va));
        if (!pa) return -EFAULT;
        size_t off = va & (PAGE_SIZE - 1), n = PAGE_SIZE - off;
        if (n > len) n = len;
        memcpy((uint8_t *)P2V(pa) + off, s, n);
        if (arch_page_prot(mm->root, PGDN(va)) & VM_EXEC)
            arch_sync_icache((uint8_t *)P2V(pa) + off, n);
        va += n; s += n; len -= n;
    }
    return 0;
}
int mm_copyout(struct mm *mm, uintptr_t va, const void *src, size_t len) {
    if (!mm_user_write(mm, va, len)) return -EFAULT;
    return mm_load_bytes(mm, va, src, len);
}
int uok(uintptr_t va, size_t len) {
    return mm_access(cur->mm, va, len, VM_READ);
}
int uwok(uintptr_t va, size_t len) {
    return mm_user_write(cur->mm, va, len);
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
int proc_may_control(const struct proc *target) {
    if (target->tgid == cur->tgid || cur->pid == init_pid || cur->is_driver_loader) return 1;
    return !target->is_driver_loader && !target->driver_bootstrap &&
           !target->is_fb_server && !target->ncaps;
}
int proc_kill(int pid, int sig) {
    if (sig < 0 || sig > 64) return -EINVAL;
    int found = 0, allowed = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || p->state == P_EMBRYO || p->state == P_ZOMBIE) continue;
        if (pid > 0 ? p->tgid == pid : pid == 0 ? p->pgid == cur->pgid : pid == -1 ? p->pid != init_pid : p->pgid == -(long)pid) {
            found = 1;
            if (!proc_may_control(p)) continue;
            allowed = 1;
            proc_signal(p, sig);
        }
    }
    return allowed ? 0 : found ? -EPERM : -ESRCH;
}
/* tgkill(tgid, tid, sig): one thread. */
int proc_kill_tid(int tgid, int tid, int sig) {
    if (sig < 0 || sig > 64 || tgid <= 0 || tid <= 0) return -EINVAL;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED && p->state != P_EMBRYO && p->state != P_ZOMBIE && p->pid == tid && p->tgid == tgid) {
            if (!proc_may_control(p)) return -EPERM;
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
    cm->brk_start = pm->brk_start; cm->brk_end = pm->brk_end; cm->mmap_next = pm->mmap_next;
    for (int i = 0; i < pm->nvma; i++) {
        if (pm->vma[i].dev) continue;
        cm->vma[cm->nvma++] = pm->vma[i];
        for (uintptr_t a = pm->vma[i].start; a < pm->vma[i].end; a += PAGE_SIZE) {
            uintptr_t src = arch_translate(pm->root, a);
            if (!src || arch_translate(cm->root, a)) continue;
            int prot = arch_page_prot(pm->root, a);
            uintptr_t dst = pmm_alloc();
            if (!dst) goto fork_fail;
            memcpy(P2V(dst), P2V(src), PAGE_SIZE);
            if (prot & VM_EXEC) arch_sync_icache(P2V(dst), PAGE_SIZE);
            if (arch_map(cm->root, a, dst, prot)) { pmm_free(dst); goto fork_fail; }
        }
    }
    for (int i = 0; i < NOFILE; i++) if (cur->ft->f[i]) c->ft->f[i] = file_dup(cur->ft->f[i]);
    c->cloexec = cur->cloexec;
    strlcpy(c->cwd, cur->cwd, sizeof c->cwd);
    memcpy(c->sigh, cur->sigh, sizeof c->sigh);
    memcpy(c->name, cur->name, sizeof c->name);
    c->umask = cur->umask; c->pgid = cur->pgid; c->ppid = cur->pid;
    c->ctty = cur->ctty;
    c->driver_bootstrap = cur->is_driver_loader;
    arch_uctx_save(&cur->uctx);
    c->uctx = cur->uctx;
    c->tf = arch_kstack_setup((uintptr_t)c->kstack + KSTACK_SIZE, &c->ksp, tf);
    TF_RET(c->tf) = 0;
    c->state = P_RUNNABLE;
    sched_kick();
    return c->pid;
fork_fail:
    mm_put(cm); c->mm = 0;
    fdtab_put(c->ft); c->ft = 0;
    c->state = P_UNUSED;
    return -ENOMEM;
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
        mm_copyout(p->mm, p->clear_child_tid, &zero, 4);
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
                if (ustatus && mm_copyout(cur->mm, ustatus, &q->exit_status, sizeof q->exit_status)) return -EFAULT;
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
    if (cur->is_driver_loader || cur->driver_bootstrap || cur->ncaps) return -EPERM;
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
    if (flags & 0x1000000) mm_copyout(cur->mm, ctid, &pid, sizeof pid); /* CLONE_CHILD_SETTID */
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
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define PIE_BASE    0x0000000000400000UL   /* load address of a position-independent (ET_DYN) program */
#define INTERP_BASE 0x0000555500000000UL   /* load address of the dynamic loader named by PT_INTERP */

struct ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };

static int elf_header_ok(const struct ehdr *eh, size_t sz) {
    return sz >= sizeof *eh && !memcmp(eh->ident, "\177ELF", 4) && eh->ident[4] == 2 &&
           eh->ident[5] == 1 && eh->ident[6] == 1 && eh->version == 1 &&
           eh->ehsize == sizeof *eh && eh->machine == ELF_MACHINE &&
           eh->phentsize == sizeof(struct phdr) && eh->phoff >= sizeof *eh &&
           !(eh->phoff % _Alignof(struct phdr)) && eh->phoff <= sz && eh->phnum &&
           eh->phnum <= (sz - eh->phoff) / sizeof(struct phdr);
}

/* Map every PT_LOAD segment at bias+vaddr.  *hi receives the end of the highest segment and
 * *phdr_va the address of the program headers when they sit inside a loaded segment (AT_PHDR). */
static int load_segments(struct mm *nm, const uint8_t *img, size_t sz, const struct ehdr *eh,
                         uintptr_t bias, uintptr_t *hi, uintptr_t *phdr_va) {
    uintptr_t limit = USER_STACK_TOP - STACK_SIZE * 4;
    if (eh->entry >= limit - bias) return -ENOEXEC;
    uintptr_t entry = eh->entry + bias;
    int have_entry = 0;
    size_t phsize = eh->phnum * sizeof(struct phdr);
    for (int i = 0; i < eh->phnum; i++) {
        const struct phdr *ph = (const void *)(img + eh->phoff + i * sizeof *ph);
        if (ph->type != PT_LOAD) continue;
        if (ph->vaddr >= limit - bias || ph->filesz > ph->memsz || (ph->filesz &&
            (ph->offset > sz || ph->filesz > sz - ph->offset))) return -ENOEXEC;
        if (ph->align > 1 && ((ph->align & (ph->align - 1)) ||
            ((ph->vaddr ^ ph->offset) & (ph->align - 1)))) return -ENOEXEC;
        if (!ph->memsz) continue;
        uintptr_t va = ph->vaddr + bias;
        if (va < PAGE_SIZE || va < *hi || ph->memsz > limit - va ||
            (ph->flags & (PF_W | PF_X)) == (PF_W | PF_X)) return -ENOEXEC;
        uintptr_t end = va + ph->memsz;
        int prot = ((ph->flags & PF_R) ? VM_READ : 0) | ((ph->flags & PF_W) ? VM_WRITE : 0) |
                   ((ph->flags & PF_X) ? VM_EXEC : 0);
        uintptr_t first = PGDN(va);
        int r;
        if (*hi > first) {
            /* Adjacent segments may occupy different bytes of the same page. */
            int shared = prot | arch_page_prot(nm->root, first);
            if ((shared & (VM_WRITE | VM_EXEC)) == (VM_WRITE | VM_EXEC)) return -ENOEXEC;
            if ((r = mm_protect(nm, first, PAGE_SIZE, shared))) return r;
            first += PAGE_SIZE;
        }
        for (uintptr_t a = first; a < end; a += PAGE_SIZE)
            if (arch_translate(nm->root, a)) return -ENOEXEC;
        if (first < end && (r = mm_map_prot(nm, first, end - first, prot))) return r;
        if (ph->filesz && (r = mm_load_bytes(nm, va, img + ph->offset, ph->filesz))) return r;
        if (end > *hi) *hi = end;
        if ((prot & VM_EXEC) && entry >= va && entry < end) have_entry = 1;
        if ((prot & VM_READ) && eh->phoff >= ph->offset &&
            eh->phoff - ph->offset <= ph->filesz && phsize <= ph->filesz - (eh->phoff - ph->offset))
            *phdr_va = va + (eh->phoff - ph->offset);
    }
    return have_entry ? 0 : -ENOEXEC;
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
    if (!elf_header_ok(eh, sz) || eh->type != ET_DYN) return -ENOEXEC;
    uintptr_t hi = 0, phdr_va = 0;
    if ((r = load_segments(nm, img, sz, eh, INTERP_BASE, &hi, &phdr_va))) return r;
    *entry = INTERP_BASE + eh->entry;
    return 0;
}

extern const uint8_t _driver_loader_start[], _driver_loader_end[];
extern const uint8_t _hid_server_start[], _hid_server_end[];
extern const uint8_t _fb_server_start[], _fb_server_end[];

/* #!interp [optarg]: run the interpreter with the script path as the next argument. */
static int proc_exec_script(struct proc* p, const char* script, const uint8_t* img, size_t sz,
    char** argv, char** envp, struct trapframe* tf) {
    static int depth;                       /* nested #! lines: stop at a small limit */
    if (depth >= 4) return -ENOEXEC;

    char* line = kmalloc(PATH_MAX);
    if (!line) return -ENOMEM;
    size_t n = 0;
    while (n < sz && n < PATH_MAX - 1 && img[n] != '\n') { line[n] = img[n]; n++; }
    line[n] = 0;

    char* s = line + 2;                     /* skip "#!" */
    while (*s == ' ' || *s == '\t') s++;
    char* interp = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    char* optarg = 0;
    if (*s) {
        *s++ = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s) optarg = s;
    }
    if (!*interp) { kfree(line); return -ENOEXEC; }

    int argc = 0;
    while (argv && argv[argc]) argc++;
    char** nv = kmalloc(sizeof(char*) * (argc + 4));
    if (!nv) { kfree(line); return -ENOMEM; }
    int k = 0;
    nv[k++] = interp;
    if (optarg) nv[k++] = optarg;
    nv[k++] = (char*)script;
    for (int i = 1; i < argc; i++) nv[k++] = argv[i];
    nv[k] = 0;

    depth++;
    int r = proc_exec(p, interp, nv, envp, tf);
    depth--;
    kfree(nv);
    kfree(line);
    return r;
}

int proc_exec(struct proc *p, const char *path, char **argv, char **envp, struct trapframe *tf) {
    char abs[PATH_MAX];
    int r;
    int boot_loader = p != cur && p->is_driver_loader;
    int fb_server = p->driver_bootstrap && !strcmp(path, "/bin/fbserver");
    const uint8_t *img;
    size_t sz;
    int hid_server = p->driver_bootstrap && !strcmp(path, "/bin/hidserver");
    if (hid_server) {
        img = _hid_server_start;
        sz = (uintptr_t)_hid_server_end - (uintptr_t)img;
        strlcpy(abs, path, sizeof abs);
    } else if (boot_loader || fb_server) {
        const uint8_t *end = boot_loader ? _driver_loader_end : _fb_server_end;
        img = boot_loader ? _driver_loader_start : _fb_server_start;
        sz = (uintptr_t)end - (uintptr_t)img;
        strlcpy(abs, path, sizeof abs);
    } else {
        r = vfs_resolve(path, 1, abs);
        if (r) return r;
        struct node *n = node_find(abs);
        if (!n) return -ENOENT;
        if ((n->mode & S_IFMT) != S_IFREG) return -EACCES;
        if ((r = node_load(n))) return r;
        img = n->data;
        sz = n->size;
    }
    if (!boot_loader && !fb_server && !hid_server && sz >= 2 && img[0] == '#' && img[1] == '!')
        return proc_exec_script(p, abs, img, sz, argv, envp, tf);
    const struct ehdr *eh = (const void *)img;
    if (!elf_header_ok(eh, sz) || (eh->type != ET_EXEC && eh->type != ET_DYN)) return -ENOEXEC;

    struct mm nm; memset(&nm, 0, sizeof nm);
    nm.root = arch_aspace_new();
    if (!nm.root) return -ENOMEM;
    uintptr_t bias = eh->type == ET_DYN ? PIE_BASE : 0;
    uintptr_t hi = 0, phdr_va = 0, interp_base = 0;
    if ((r = load_segments(&nm, img, sz, eh, bias, &hi, &phdr_va))) goto fail;
    uintptr_t entry = eh->entry + bias;
    for (int i = 0; i < eh->phnum; i++) {                /* PT_INTERP: start in the dynamic loader instead */
        const struct phdr *ph = (const void *)(img + eh->phoff + i * sizeof *ph);
        if (ph->type != PT_INTERP) continue;
        if (interp_base || !phdr_va || ph->filesz < 2 || ph->filesz > PATH_MAX ||
            ph->offset > sz || ph->filesz > sz - ph->offset || img[ph->offset + ph->filesz - 1]) {
            r = -ENOEXEC;
            goto fail;
        }
        char ipath[PATH_MAX];
        memcpy(ipath, img + ph->offset, ph->filesz);
        if (strlen(ipath) != ph->filesz - 1) { r = -ENOEXEC; goto fail; }
        if ((r = load_interp(&nm, ipath, &entry))) goto fail;
        interp_base = INTERP_BASE;
    }
    if ((r = mm_map(&nm, USER_STACK_TOP - STACK_SIZE, STACK_SIZE))) goto fail;
    nm.brk_start = nm.brk_end = PGUP(hi);
    nm.mmap_next = MMAP_BASE;
    uint64_t placement;
    if (!hardware_random_word(&placement))
        nm.mmap_next += (placement & 0xffff) * PAGE_SIZE;

    int argc = 0, envc = 0; size_t S = 0;
    while (argv && argv[argc]) S += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) S += strlen(envp[envc++]) + 1;
    const int naux = 15;
    size_t W = 1 + (argc + 1) + (envc + 1) + 2 * naux;
    uintptr_t top = USER_STACK_TOP, strb = top - S, rndp = (strb - 16) & ~15UL;
    uintptr_t sp = (rndp - W * 8) & ~15UL;
    size_t total = top - sp;
    if (total > STACK_SIZE) { r = -ENOMEM; goto fail; }
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
    uint64_t random[2];
    int seeded = !hardware_random_word(&random[0]) && !hardware_random_word(&random[1]);
    if (seeded) memcpy(buf + (rndp - sp), random, sizeof random);
    uint64_t aux[][2] = {
        {AT_PHDR, phdr_va}, {AT_PHENT, sizeof(struct phdr)}, {AT_PHNUM, eh->phnum}, {AT_PAGESZ, PAGE_SIZE},
        {AT_BASE, interp_base}, {AT_ENTRY, eh->entry + bias}, {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0},
        {AT_SECURE, 0}, {seeded ? AT_RANDOM : 1 /* AT_IGNORE */, seeded ? rndp : 0},
        {AT_CLKTCK, HZ}, {AT_HWCAP, AUX_HWCAP}, {AT_NULL, 0},
    };
    for (int i = 0; i < naux; i++) { w[k++] = aux[i][0]; w[k++] = aux[i][1]; }
    r = mm_copyout(&nm, sp, buf, total);
    kfree(buf);
    if (r) goto fail;

    struct mm *heap = kmalloc(sizeof *heap);
    if (!heap) { r = -ENOMEM; goto fail; }
    *heap = nm; heap->refs = 1;
    struct mm *old = p->mm;
    p->mm = heap;
    if (p == cur) arch_aspace_switch(heap->root);
    mm_put(old);
    if (p->is_driver_loader || p->ncaps) ipc_proc_exit(p);
    dev_driver_cleanup(p);
    p->is_driver_loader = boot_loader;
    p->driver_bootstrap = 0;
    p->is_fb_server = fb_server;
    if (hid_server) p->caps[p->ncaps++] = (struct proc_cap){ CHI_CAP_DRIVER, DEV_TYPE_HID, 0 };
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
static int spawn_process(const char *path, char **argv, char **envp, const char *console, int as_init, int as_loader) {
    struct proc *p = proc_alloc();
    if (!p) return -EAGAIN;
    p->mm = kmalloc(sizeof *p->mm);
    if (!p->mm) { p->state = P_UNUSED; return -ENOMEM; }
    memset(p->mm, 0, sizeof *p->mm); p->mm->refs = 1;
    p->mm->root = arch_aspace_new();
    if (!p->mm->root) { mm_put(p->mm); p->mm = 0; p->state = P_UNUSED; return -ENOMEM; }
    p->pgid = p->pid;
    p->ft = fdtab_new();
    if (!p->ft) { mm_put(p->mm); p->mm = 0; p->state = P_UNUSED; return -ENOMEM; }
    for (int i = 0; i < 3; i++) {
        int r = vfs_open(console, 2, 0, &p->ft->f[i]);
        if (r) { fdtab_put(p->ft); p->ft = 0; mm_put(p->mm); p->mm = 0; p->state = P_UNUSED; return r; }
    }
    if (!as_loader && p->ft->f[0]->type == F_NODE && p->ft->f[0]->node->dev == DEV_TTY &&
        p->ft->f[0]->node->minor < TTY_ACTIVE_MINOR) {
        p->ctty = p->ft->f[0]->node->minor + 1;               /* the init console is its controlling terminal */
        tty_set_session(p->ft->f[0]->node->minor, p->pid);
    }
    struct trapframe tf; memset(&tf, 0, sizeof tf);
    p->is_driver_loader = as_loader;
    int r = proc_exec(p, path, argv, envp, &tf);
    if (r) {
        fdtab_put(p->ft); p->ft = 0;
        mm_put(p->mm); p->mm = 0; p->state = P_UNUSED;
        return r;
    }
    p->tf = arch_kstack_setup((uintptr_t)p->kstack + KSTACK_SIZE, &p->ksp, &tf);
    if (!as_loader) strlcpy(p->name, as_init ? "init" : "sh", sizeof p->name);
    p->state = P_RUNNABLE;
    if (as_init) init_pid = p->pid;
    sched_kick();
    return p->pid;
}

int proc_spawn_init(const char *path, char **argv, char **envp, const char *console) {
    int pid = spawn_process(path, argv, envp, console, 1, 0);
    if (pid > 0 && !cur) {
        int loader = proc_spawn_driver_loader(console);
        if (loader < 0) kprintf("driver loader startup failed (%d)\n", loader);
    }
    return pid;
}

int proc_spawn_driver_loader(const char *console) {
    if ((uintptr_t)_driver_loader_start == (uintptr_t)_driver_loader_end) return 0;
    char *argv[] = { "/bin/drvloader", "--service", 0 };
    return spawn_process(argv[0], argv, 0, console, 0, 1);
}

/* A second console shell (e.g. on the framebuffer terminal).  It is not init: its exit does not power the
 * system off, and it has no parent to wait for it (it is reaped when it exits). */
int proc_spawn_console(const char *path, char **argv, char **envp, const char *console) {
    return spawn_process(path, argv, envp, console, 0, 0);
}
