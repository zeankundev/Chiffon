
#include "kernel.h"

extern char vectors[], trap_return[];
extern char __bss_end[];
extern uint64_t boot_l1[512];                          /* kernel direct map: one 1 GiB block per entry */

uintptr_t arch_kernel_end(void) { return V2P((uintptr_t)__bss_end); }

/* Make [pa, pa+len) reachable through P2V() with device (nGnRnE) attributes. Granularity is 1 GiB. */
int arch_map_device(uintptr_t pa, size_t len) {
    uintptr_t first = pa >> 30, last = (pa + len - 1) >> 30;
    if (last >= 512) return -1;
    for (uintptr_t g = first; g <= last; g++)
        if (!(boot_l1[g] & 1)) boot_l1[g] = (g << 30) | 0x401 | (3UL << 53);   /* AF | block | UXN | PXN, AttrIdx 0 */
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
    return 0;
}

void arch_init(void) {
    __asm__ volatile("msr vbar_el1,%0; isb" :: "r"((uint64_t)vectors));
    kprintf("arch: aarch64 vectors ready\n");
}

/* ---------------- traps ---------------- */
void irq_c(struct trapframe *tf) {
    board_irq(tf);                                    /* acknowledge + timer tick are the board's job */
    if (TF_FROM_USER(tf)) check_killed();
}
void sync_c(struct trapframe *tf) {
    uint64_t esr, far;
    __asm__ volatile("mrs %0,esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0,far_el1" : "=r"(far));
    uint32_t ec = esr >> 26;
    int user = TF_FROM_USER(tf);
    if (user && ec == 0x15) { syscall_dispatch(tf); return; }
    if (user) {
        if (ec == 0x20 || ec == 0x24) trap_user_fault(tf, "page fault", far, 11);
        trap_user_fault(tf, ec == 0x00 || ec == 0x0E ? "illegal instruction" : "exception", tf->pc, ec == 0x2C ? 8 : 4);
    }
    panic("kernel exception ec=%x esr=%lx pc=%p far=%p", ec, (unsigned long)esr, (void *)tf->pc, (void *)far);
}
void bad_c(struct trapframe *tf) {
    uint64_t esr; __asm__ volatile("mrs %0,esr_el1" : "=r"(esr));
    panic("unexpected exception esr=%lx pc=%p", (unsigned long)esr, (void *)tf->pc);
}

/* ---------------- idle / power ---------------- */
void arch_idle(void) { __asm__ volatile("msr daifclr,#2; wfi; msr daifset,#2" ::: "memory"); }
void arch_halt(void) { __asm__ volatile("msr daifset,#2"); for (;;) __asm__ volatile("wfi"); }

/* ---------------- virtual memory (TTBR0 = user, TTBR1 = kernel direct map) ---------------- */
#define PA_MASK 0x0000FFFFFFFFF000UL
#define PTE_TABLE 3UL
#define PTE_USER_PAGE (3UL | (1UL << 2) | (1UL << 6) | (3UL << 8) | (1UL << 10) | (1UL << 11) | (1UL << 53))
static uint8_t empty_l0[4096] __attribute__((aligned(4096)));
static uintptr_t cur_root = ~0UL;

uintptr_t arch_kernel_root(void) { return V2P(empty_l0); }
void arch_aspace_switch(uintptr_t root) {
    if (root == cur_root) return;
    cur_root = root;
    __asm__ volatile("msr ttbr0_el1,%0; isb; tlbi vmalle1; dsb ish; isb" :: "r"(root) : "memory");
}
uintptr_t arch_aspace_new(void) { return pmm_alloc(); }
void arch_aspace_free(uintptr_t root) {
    uint64_t *l0 = P2V(root);
    for (int i = 0; i < 512; i++) {
        if (!(l0[i] & 1)) continue;
        uint64_t *l1 = P2V(l0[i] & PA_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(l1[j] & 1)) continue;
            uint64_t *l2 = P2V(l1[j] & PA_MASK);
            for (int k = 0; k < 512; k++) if (l2[k] & 1) pmm_free(l2[k] & PA_MASK);
            pmm_free(l1[j] & PA_MASK);
        }
        pmm_free(l0[i] & PA_MASK);
    }
    pmm_free(root);
    if (cur_root == root) cur_root = ~0UL;
}
int arch_map(uintptr_t root, uintptr_t va, uintptr_t pa) {
    uint64_t *t = P2V(root);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t *e = &t[(va >> (12 + 9 * lvl)) & 511];
        if (!(*e & 1)) {
            uintptr_t np = pmm_alloc();
            if (!np) return -1;
            *e = np | PTE_TABLE;
        }
        t = P2V(*e & PA_MASK);
    }
    t[(va >> 12) & 511] = pa | PTE_USER_PAGE;
    return 0;
}
static uint64_t *walk(uintptr_t root, uintptr_t va) {
    uint64_t *t = P2V(root);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t e = t[(va >> (12 + 9 * lvl)) & 511];
        if (!(e & 1)) return 0;
        t = P2V(e & PA_MASK);
    }
    return &t[(va >> 12) & 511];
}
uintptr_t arch_translate(uintptr_t root, uintptr_t va) {
    uint64_t *e = walk(root, va);
    return (e && (*e & 1)) ? (*e & PA_MASK) : 0;
}
uintptr_t arch_unmap(uintptr_t root, uintptr_t va) {
    uint64_t *e = walk(root, va);
    if (!e || !(*e & 1)) return 0;
    uintptr_t pa = *e & PA_MASK;
    *e = 0;
    __asm__ volatile("dsb ishst; tlbi vaae1is,%0; dsb ish; isb" :: "r"(va >> 12) : "memory");
    return pa;
}
void arch_sync_icache(void *va, size_t n) {
    uintptr_t a = (uintptr_t)va & ~63UL, e = (uintptr_t)va + n;
    for (uintptr_t p = a; p < e; p += 64) __asm__ volatile("dc cvau,%0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish");
    for (uintptr_t p = a; p < e; p += 64) __asm__ volatile("ic ivau,%0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish; isb");
}

/* ---------------- contexts ---------------- */
void arch_set_kstack(uintptr_t top) { (void)top; }     /* SP_EL1 is reset by each eret path */
struct trapframe *arch_kstack_setup(uintptr_t top, uintptr_t *ksp, const struct trapframe *src) {
    struct trapframe *tf = (struct trapframe *)(top - sizeof *tf);
    *tf = *src;
    uint64_t *sp = (uint64_t *)tf - 12;                /* 96-byte arch_switch frame */
    for (int i = 0; i < 12; i++) sp[i] = 0;
    sp[11] = (uint64_t)trap_return;                    /* x30: ret lands in the eret path */
    *ksp = (uintptr_t)sp;
    return tf;
}
void arch_tf_user(struct trapframe *tf, uintptr_t entry, uintptr_t sp) {
    memset(tf, 0, sizeof *tf);
    tf->pc = entry; tf->sp = sp; tf->pstate = 0;        /* EL0t, interrupts enabled */
}
void arch_uctx_init(struct arch_uctx *u) { memset(u, 0, sizeof *u); }

