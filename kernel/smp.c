/* SMP: per-CPU state, the big kernel lock, IPIs, and secondary CPU bring-up.  See include/smp.h. */
#include "kernel.h"

#if ARCH_AARCH64
extern char secondary_start[];            /* boot.S: physical entry point for PSCI CPU_ON */
extern char vectors[];
#endif

struct cpu cpus[MAX_CPUS];
int ncpus = 1;
static volatile char klock_word;

/* Weak defaults: boards without SMP support leave the other CPUs parked (single-CPU operation). */
__attribute__((weak)) int board_smp_start(uintptr_t entry_pa) { (void)entry_pa; return 0; }
__attribute__((weak)) void board_secondary_init(int id) { (void)id; }
__attribute__((weak)) void board_ipi_send(int cpu) { (void)cpu; }

struct cpu *this_cpu(void) {
#if ARCH_AARCH64
    uintptr_t p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return (struct cpu *)p;
#elif ARCH_X86_64
    /* On x86_64, we use GS.base (set via arch_prctl or at CPU init).
     * For now, use a global: the kernel will set cpus[cpu_id].id to the LAPIC ID. */
    uint32_t lapic_id;
    uintptr_t msr_val;
    __asm__ volatile("movl $0x1B, %%ecx; rdmsr; shrl $24, %%eax" : "=a"(lapic_id) : : "ecx", "edx");
    /* For simplicity in kernel early boot, just return &cpus[0]; full implementation
     * would use per-CPU storage via GS.base or per-LAPIC-ID table lookup. */
    for (int i = 0; i < ncpus; i++) {
        if (cpus[i].id == lapic_id) return &cpus[i];
    }
    return &cpus[0];
#else
    return &cpus[0];
#endif
}

void smp_bsp_init(void) {
#if ARCH_AARCH64
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&cpus[0]));
#endif
    for (int i = 0; i < MAX_CPUS; i++) cpus[i].id = i;
    cpus[0].online = 1;
    klock_enter();                        /* kmain runs under the lock until the scheduler starts */
}

/* The lock is a byte: 0 = free.  Written with exclusive load/store (ldaxrb/stxrb) for aarch64,
 * or cmpxchg for x86_64. GCC __atomic builtins are avoided since this is freestanding. */
void klock_enter(void) {
#if ARCH_AARCH64
    uint32_t v, fail;
    __asm__ volatile(
        "1: ldaxrb %w0, [%2]\n"
        "   cbnz   %w0, 2f\n"
        "   mov    %w0, #1\n"
        "   stxrb  %w1, %w0, [%2]\n"
        "   cbnz   %w1, 1b\n"
        "   b      3f\n"
        "2: yield\n"
        "   b      1b\n"
        "3:\n"
        : "=&r"(v), "=&r"(fail) : "r"(&klock_word) : "memory", "cc");
#elif ARCH_X86_64
    uint8_t v;
    do {
        __asm__ volatile("pause");
        __asm__ volatile("movb $0, %0; lock cmpxchgb %1, %0" : "=m"(klock_word), "=a"(v) : "a"(0), "b"(1) : "memory");
    } while (v != 0);
#endif
}

void klock_leave(void) {
#if ARCH_AARCH64
    __asm__ volatile("stlrb wzr, [%0]" :: "r"(&klock_word) : "memory");
#elif ARCH_X86_64
    __asm__ volatile("movb $0, %0" : "=m"(klock_word) : : "memory");
#endif
}

void sched_kick(void) {
    struct cpu *me = this_cpu();
    for (int i = 0; i < ncpus; i++) {
        struct cpu *c = &cpus[i];
        if (c != me && c->online && c->idle) board_ipi_send(c->id);
    }
}

void smp_init(void) {
#if ARCH_AARCH64
    int n = board_smp_start(V2P(secondary_start));
    ncpus = 1 + n;
    if (n) kprintf("smp: starting %d secondary CPU%s\n", n, n > 1 ? "s" : "");
#elif ARCH_X86_64
    extern void lapic_init(void);
    lapic_init();
    int n = board_smp_start(0);  /* entry_pa unused on x86_64 (we have fixed bootcode) */
    ncpus = 1 + n;
    if (n) kprintf("smp: starting %d secondary CPU%s\n", n, n > 1 ? "s" : "");
#endif
}

/* First code a secondary CPU runs with the MMU on (boot.S jumps here with the CPU index in x0). */
void secondary_main(uint64_t id) {
#if ARCH_AARCH64
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&cpus[id]));
    __asm__ volatile("msr vbar_el1, %0; msr cntkctl_el1, %1; isb" :: "r"((uint64_t)vectors), "r"((uint64_t)2));
    board_secondary_init((int)id);
#endif
    klock_enter();
    cpus[id].online = 1;
    kprintf("cpu %lu online\n", (unsigned long)id);
    scheduler();
}
