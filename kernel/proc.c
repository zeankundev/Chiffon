#include "kernel.h"

struct proc ptable[NPROC];
struct proc *cur;
volatile uint64_t ticks;
static uintptr_t sched_ctx;
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
        if (!arch_translate(cur->mm.root, a)) return 0;
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

struct proc *proc_alloc(void) {
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != P_UNUSED) continue;
        memset(p, 0, sizeof *p);
        p->state = P_EMBRYO;
        p->pid = next_pid++;
        p->kstack = kstacks[i];
        p->umask = 022;
        strlcpy(p->cwd, "/", sizeof p->cwd);
        arch_uctx_init(&p->uctx);
        return p;
    }
    return 0;
}
void scheduler(void) {
    int last = 0;
    for (;;) {
        struct proc *n = 0;
        for (int i = 0; i < NPROC; i++) if (ptable[i].state == P_ZOMBIE && ptable[i].ppid == 0) ptable[i].state = P_UNUSED;
        for (int i = 1; i <= NPROC; i++) {
            struct proc *p = &ptable[(last + i) % NPROC];
            if (p->state == P_RUNNABLE) { n = p; last = p - ptable; break; }
        }
        if (!n) { arch_idle(); continue; }
        cur = n; n->state = P_RUNNING;
        arch_aspace_switch(n->mm.root);
        arch_set_kstack((uintptr_t)n->kstack + KSTACK_SIZE);
        arch_uctx_restore(&n->uctx);
        arch_switch(&sched_ctx, n->ksp);
        arch_uctx_save(&n->uctx);
        arch_aspace_switch(arch_kernel_root());
        cur = 0;
    }
}
static void sched(void) { arch_switch(&cur->ksp, sched_ctx); }
void yield(void) { cur->state = P_RUNNABLE; sched(); }
void sleep_on(void *chan) { cur->chan = chan; cur->state = P_SLEEPING; sched(); cur->chan = 0; }
void wakeup(void *chan) {
    for (int i = 0; i < NPROC; i++)
        if (ptable[i].state == P_SLEEPING && ptable[i].chan == chan) ptable[i].state = P_RUNNABLE;
}
void timer_tick(int from_user) {
    ticks++;
    console_poll();
    wakeup((void *)&ticks);
    if (from_user && cur && cur->state == P_RUNNING) yield();
}

void proc_signal(struct proc *p, int sig) {
    if (sig <= 0 || sig > 64) return;
    if (p->pid == init_pid && (sig == 10 || sig == 12 || sig == 15)) {
        kprintf("[chiffon] powering off\n");
        kernel_poweroff();
    }
    if (sig != 9 && p->sigh[sig]) return;
    switch (sig) {
    case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 28: return;
    }
    p->killed = sig;
    if (p->state == P_SLEEPING) p->state = P_RUNNABLE;
}
int proc_kill(int pid, int sig) {
    int found = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == P_UNUSED || p->state == P_EMBRYO || p->state == P_ZOMBIE) continue;
        if (pid > 0 ? p->pid == pid : pid == 0 ? p->pgid == cur->pgid : pid == -1 ? p->pid != init_pid : p->pgid == -pid) {
            found = 1; proc_signal(p, sig);
        }
    }
    return found ? 0 : -ESRCH;
}
void check_killed(void) { if (cur->killed) proc_exit(cur->killed); }

int proc_fork(struct trapframe *tf) {
    struct proc *c = proc_alloc();
    if (!c) return -EAGAIN;
    struct mm *pm = &cur->mm, *cm = &c->mm;
    cm->root = arch_aspace_new();
    if (!cm->root) { c->state = P_UNUSED; return -ENOMEM; }
    cm->nvma = pm->nvma; memcpy(cm->vma, pm->vma, sizeof pm->vma);
    cm->brk_start = pm->brk_start; cm->brk_end = pm->brk_end; cm->mmap_next = pm->mmap_next;
    for (int i = 0; i < pm->nvma; i++)
        for (uintptr_t a = pm->vma[i].start; a < pm->vma[i].end; a += PAGE_SIZE) {
            uintptr_t src = arch_translate(pm->root, a);
            if (!src || arch_translate(cm->root, a)) continue;
            if (pm->vma[i].dev) {
                if (arch_map(cm->root, a, src)) { mm_free(cm); c->state = P_UNUSED; return -ENOMEM; }
                continue;
            }
            uintptr_t dst = pmm_alloc();
            if (!dst) { mm_free(cm); c->state = P_UNUSED; return -ENOMEM; }
            memcpy(P2V(dst), P2V(src), PAGE_SIZE);
            arch_sync_icache(P2V(dst), PAGE_SIZE);
            arch_map(cm->root, a, dst);
        }
    for (int i = 0; i < NOFILE; i++) if (cur->fd[i]) c->fd[i] = file_dup(cur->fd[i]);
    c->cloexec = cur->cloexec;
    strlcpy(c->cwd, cur->cwd, sizeof c->cwd);
    memcpy(c->sigh, cur->sigh, sizeof c->sigh);
    memcpy(c->name, cur->name, sizeof c->name);
    c->umask = cur->umask; c->pgid = cur->pgid; c->ppid = cur->pid;
    c->is_driver_loader = cur->is_driver_loader;
    c->ncaps = cur->ncaps;
    memcpy(c->caps, cur->caps, sizeof cur->caps);
    arch_uctx_save(&cur->uctx);
    c->uctx = cur->uctx;
    c->tf = arch_kstack_setup((uintptr_t)c->kstack + KSTACK_SIZE, &c->ksp, tf);
    TF_RET(c->tf) = 0;
    c->state = P_RUNNABLE;
    return c->pid;
}

void proc_exit(int wstatus) {
    struct proc *p = cur;
    int was_init = (p->pid == init_pid);
    if (was_init) init_exited(wstatus);
    ipc_proc_exit(p);
    dev_driver_cleanup(p);
    for (int i = 0; i < NOFILE; i++) if (p->fd[i]) { file_close(p->fd[i]); p->fd[i] = 0; }
    arch_aspace_switch(arch_kernel_root());
    mm_free(&p->mm);
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &ptable[i];
        if (q->state != P_UNUSED && q->ppid == p->pid) { q->ppid = init_pid; if (q->state == P_ZOMBIE) for (int j = 0; j < NPROC; j++) if (ptable[j].pid == init_pid) wakeup(&ptable[j]); }
    }
    p->exit_status = wstatus;
    if (was_init) p->ppid = 0;
    p->state = P_ZOMBIE;
    for (int i = 0; i < NPROC; i++) if (ptable[i].pid == p->ppid && ptable[i].state != P_UNUSED) wakeup(&ptable[i]);
    sched();
    panic("zombie scheduled");
}

int proc_wait(int pid, uintptr_t ustatus, int options) {
    for (;;) {
        int have = 0;
        for (int i = 0; i < NPROC; i++) {
            struct proc *q = &ptable[i];
            if (q->state == P_UNUSED || q->state == P_EMBRYO || q->ppid != cur->pid) continue;
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

struct ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };

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
        eh->type != 2 || eh->machine != ELF_MACHINE || eh->phentsize != sizeof(struct phdr) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct phdr) > sz)
        return -ENOEXEC;

    struct mm nm; memset(&nm, 0, sizeof nm);
    nm.root = arch_aspace_new();
    if (!nm.root) return -ENOMEM;
    uintptr_t hi = 0, phdr_va = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const struct phdr *ph = (const void *)(img + eh->phoff + i * sizeof *ph);
        if (ph->type == 3) { r = -ENOEXEC; goto fail; }
        if (ph->type != 1) continue;
        if (ph->vaddr < PAGE_SIZE || ph->vaddr + ph->memsz > USER_TOP - STACK_SIZE * 4 || ph->filesz > ph->memsz ||
            (ph->filesz && ph->offset + ph->filesz > sz)) { r = -ENOEXEC; goto fail; }
        if ((r = mm_map(&nm, ph->vaddr, ph->memsz))) goto fail;
        if ((r = mm_copyout(&nm, ph->vaddr, img + ph->offset, ph->filesz))) goto fail;
        if (ph->vaddr + ph->memsz > hi) hi = ph->vaddr + ph->memsz;
        if (eh->phoff >= ph->offset && eh->phoff < ph->offset + ph->filesz) phdr_va = ph->vaddr + (eh->phoff - ph->offset);
    }
    if (!hi) { r = -ENOEXEC; goto fail; }
    if ((r = mm_map(&nm, USER_STACK_TOP - STACK_SIZE, STACK_SIZE))) goto fail;
    nm.brk_start = nm.brk_end = PGUP(hi);
    nm.mmap_next = MMAP_BASE;

    int argc = 0, envc = 0; size_t S = 0;
    while (argv && argv[argc]) S += strlen(argv[argc++]) + 1;
    while (envp && envp[envc]) S += strlen(envp[envc++]) + 1;
    const int naux = 14;
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
        {AT_ENTRY, eh->entry}, {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0}, {AT_SECURE, 0},
        {AT_RANDOM, rndp}, {AT_CLKTCK, HZ}, {AT_HWCAP, AUX_HWCAP}, {AT_NULL, 0},
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

    struct mm old = p->mm;
    p->mm = nm;
    if (p == cur) arch_aspace_switch(p->mm.root);
    mm_free(&old);
    for (int i = 0; i < NOFILE; i++)
        if (p->fd[i] && (p->cloexec >> i & 1)) { file_close(p->fd[i]); p->fd[i] = 0; }
    p->cloexec = 0;
    for (int i = 1; i < 65; i++) if (p->sigh[i] != 1) p->sigh[i] = 0;
    arch_tf_user(tf, eh->entry, sp);
    arch_uctx_init(&p->uctx);
    if (p == cur) arch_uctx_restore(&p->uctx);
    const char *base = strrchr(abs, '/');
    strlcpy(p->name, base ? base + 1 : abs, sizeof p->name);
    return 0;
fail:
    mm_free(&nm);
    return r;
}

int proc_spawn_init(const char *path, char **argv, char **envp, const char *console) {
    struct proc *p = proc_alloc();
    if (!p) return -EAGAIN;
    p->mm.root = arch_aspace_new();
    p->pgid = p->pid;
    for (int i = 0; i < 3; i++) {
        int r = vfs_open(console, 2, 0, &p->fd[i]);
        if (r) { for (int j = 0; j < i; j++) file_close(p->fd[j]); mm_free(&p->mm); p->state = P_UNUSED; return r; }
    }
    struct trapframe tf; memset(&tf, 0, sizeof tf);
    int r = proc_exec(p, path, argv, envp, &tf);
    if (r) {
        for (int i = 0; i < 3; i++) file_close(p->fd[i]);
        mm_free(&p->mm); p->state = P_UNUSED;
        return r;
    }
    p->tf = arch_kstack_setup((uintptr_t)p->kstack + KSTACK_SIZE, &p->ksp, &tf);
    strlcpy(p->name, "init", sizeof p->name);
    p->is_driver_loader = 1;
    p->state = P_RUNNABLE;
    init_pid = p->pid;
    return p->pid;
}
