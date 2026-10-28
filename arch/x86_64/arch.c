
#include "kernel.h"

extern char boot_pml4[];
extern char trap_return[], syscall_entry[];
extern uint64_t isr_table[48];
uint64_t syscall_kstack, user_rsp_tmp;

static inline void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static inline uint64_t rdmsr(uint32_t m) { uint32_t l, h; __asm__ volatile("rdmsr" : "=a"(l), "=d"(h) : "c"(m)); return (uint64_t)h << 32 | l; }

uintptr_t arch_kernel_end(void) { extern char __kernel_end_phys[]; return (uintptr_t)__kernel_end_phys; }
/* x86_64: the first 4 GiB is already direct-mapped at boot; devices here use port I/O or low MMIO. */
int arch_map_device(uintptr_t pa, size_t len) { return pa + len <= 0x100000000UL ? 0 : -1; }

/* ---------------- GDT / TSS / IDT ---------------- */
struct tss { uint32_t r0; uint64_t rsp0, rsp1, rsp2, r1, ist[7], r2; uint16_t r3, iomap; } __attribute__((packed));
static struct tss tss;
static uint64_t gdt[7];
struct gate { uint16_t lo, sel; uint8_t ist, attr; uint16_t mid; uint32_t hi, zero; } __attribute__((packed));
static struct gate idt[256];

void arch_set_kstack(uintptr_t top) { tss.rsp0 = top; syscall_kstack = top; }

void arch_init(void) {
    gdt[1] = 0x00AF9A000000FFFFUL; gdt[2] = 0x00CF92000000FFFFUL;
    gdt[3] = 0x00CFF2000000FFFFUL; gdt[4] = 0x00AFFA000000FFFFUL;
    uint64_t b = (uint64_t)&tss; uint32_t lim = sizeof tss - 1;
    gdt[5] = (lim & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89UL << 40) | ((uint64_t)((lim >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
    gdt[6] = b >> 32;
    tss.iomap = sizeof tss;
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) gp = { sizeof gdt - 1, (uint64_t)gdt };
    __asm__ volatile("lgdt %0" :: "m"(gp));
    __asm__ volatile("pushq $0x08; lea 1f(%%rip),%%rax; pushq %%rax; lretq; 1:" ::: "rax", "memory");
    __asm__ volatile("mov %0,%%ds; mov %0,%%es; mov %0,%%ss" :: "r"((uint16_t)0x10));
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));

    for (int i = 0; i < 48; i++) {
        uint64_t a = isr_table[i];
        idt[i] = (struct gate){ a & 0xFFFF, 0x08, 0, 0x8E, (a >> 16) & 0xFFFF, a >> 32, 0 };
    }
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) ip = { sizeof idt - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(ip));

    /* SSE for userland (kernel itself is built without it) */
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0)); cr0 = (cr0 & ~4UL) | 2; __asm__ volatile("mov %0,%%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4)); cr4 |= 0x600; __asm__ volatile("mov %0,%%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");

    /* SYSCALL/SYSRET */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);
    wrmsr(0xC0000081, (0x10UL << 48) | (0x08UL << 32));
    wrmsr(0xC0000082, (uint64_t)syscall_entry);
    wrmsr(0xC0000084, 0x200 | 0x100 | 0x400);
    kprintf("arch: x86_64 GDT/IDT/SYSCALL ready\n");
}

/* ---------------- traps ---------------- */
void trap_c(struct trapframe *tf) {
    uint64_t n = tf->trapno;
    int user = TF_FROM_USER(tf);
    if (n >= 32 && n < 48) {                         /* external interrupt: the board handles it */
        board_irq(tf);
        if (user) check_killed();
        return;
    }
    if (user) {
        uint64_t cr2 = 0;
        if (n == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        trap_user_fault(tf, n == 14 ? "page fault" : n == 6 ? "illegal instruction" : "exception", n == 14 ? cr2 : tf->rip,
                        n == 0 ? 8 : n == 6 ? 4 : 11);
    }
    uint64_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    panic("kernel exception %lu err=%lx rip=%p cr2=%p rsp=%p", n, tf->err, (void *)tf->rip, (void *)cr2, (void *)tf->rsp);
}

/* ---------------- idle / power ---------------- */
void arch_idle(void) { __asm__ volatile("sti; hlt; cli"); }
void arch_halt(void) { __asm__ volatile("cli"); for (;;) __asm__ volatile("hlt"); }

/* ---------------- virtual memory (4-level paging) ---------------- */
#define PTE_ADDR 0x000FFFFFFFFFF000UL
uintptr_t arch_kernel_root(void) { return (uintptr_t)boot_pml4; }
static uintptr_t cur_root;
void arch_aspace_switch(uintptr_t root) {
    if (root == cur_root) return;
    cur_root = root;
    __asm__ volatile("mov %0,%%cr3" :: "r"(root) : "memory");
}
uintptr_t arch_aspace_new(void) {
    uintptr_t pa = pmm_alloc();
    if (!pa) return 0;
    memcpy((char *)P2V(pa) + 2048, (char *)P2V(arch_kernel_root()) + 2048, 2048);   /* share kernel half */
    return pa;
}
void arch_aspace_free(uintptr_t root) {
    uint64_t *l4 = P2V(root);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & 1)) continue;
        uint64_t *l3 = P2V(l4[i] & PTE_ADDR);
        for (int j = 0; j < 512; j++) {
            if (!(l3[j] & 1)) continue;
            uint64_t *l2 = P2V(l3[j] & PTE_ADDR);
            for (int k = 0; k < 512; k++) if (l2[k] & 1) pmm_free(l2[k] & PTE_ADDR);
            pmm_free(l3[j] & PTE_ADDR);
        }
        pmm_free(l4[i] & PTE_ADDR);
    }
    pmm_free(root);
}
int arch_map(uintptr_t root, uintptr_t va, uintptr_t pa) {
    uint64_t *t = P2V(root);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t *e = &t[(va >> (12 + 9 * lvl)) & 511];
        if (!(*e & 1)) {
            uintptr_t np = pmm_alloc();
            if (!np) return -1;
            *e = np | 7;
        }
        t = P2V(*e & PTE_ADDR);
    }
    t[(va >> 12) & 511] = pa | 7;                    /* P | RW | US */
    return 0;
}
static uint64_t *walk(uintptr_t root, uintptr_t va) {
    uint64_t *t = P2V(root);
    for (int lvl = 3; lvl > 0; lvl--) {
        uint64_t e = t[(va >> (12 + 9 * lvl)) & 511];
        if (!(e & 1)) return 0;
        t = P2V(e & PTE_ADDR);
    }
    return &t[(va >> 12) & 511];
}
uintptr_t arch_translate(uintptr_t root, uintptr_t va) {
    uint64_t *e = walk(root, va);
    return (e && (*e & 1)) ? (*e & PTE_ADDR) : 0;
}
uintptr_t arch_unmap(uintptr_t root, uintptr_t va) {
    uint64_t *e = walk(root, va);
    if (!e || !(*e & 1)) return 0;
    uintptr_t pa = *e & PTE_ADDR;
    *e = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    return pa;
}
void arch_sync_icache(void *va, size_t n) { (void)va; (void)n; }    /* coherent on x86 */

/* ---------------- contexts ---------------- */
struct trapframe *arch_kstack_setup(uintptr_t top, uintptr_t *ksp, const struct trapframe *src) {
    struct trapframe *tf = (struct trapframe *)(top - sizeof *tf);
    *tf = *src;
    uint64_t *sp = (uint64_t *)tf;
    *--sp = (uint64_t)trap_return;                   /* arch_switch "returns" into the iretq path */
    for (int i = 0; i < 6; i++) *--sp = 0;           /* rbp rbx r12-r15 */
    *ksp = (uintptr_t)sp;
    return tf;
}
void arch_tf_user(struct trapframe *tf, uintptr_t entry, uintptr_t sp) {
    memset(tf, 0, sizeof *tf);
    tf->rip = entry; tf->rsp = sp; tf->cs = 0x23; tf->ss = 0x1B; tf->rflags = 0x202;
}
void arch_uctx_init(struct arch_uctx *u) {
    memset(u, 0, sizeof *u);
    *(uint16_t *)u->fx = 0x37F;                      /* FCW */
    *(uint32_t *)(u->fx + 24) = 0x1F80;              /* MXCSR */
}
void arch_uctx_save(struct arch_uctx *u) { __asm__ volatile("fxsave (%0)" :: "r"(u->fx) : "memory"); }
void arch_uctx_restore(struct arch_uctx *u) {
    __asm__ volatile("fxrstor (%0)" :: "r"(u->fx) : "memory");
    wrmsr(0xC0000100, u->fs_base);
}

