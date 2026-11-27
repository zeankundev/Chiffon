/* SMP: per-CPU state, the big kernel lock, IPIs, and secondary CPU bring-up.  See include/smp.h. */
#include "kernel.h"

#if ARCH_AARCH64
extern char secondary_start[];            /* boot.S: physical entry point for PSCI CPU_ON */
extern char vectors[];
#endif

struct cpu cpus[MAX_CPUS];
int ncpus = 1;
#if ARCH_AARCH64
static volatile char klock_word;
#endif

/* Weak defaults: boards without SMP support leave the other CPUs parked (single-CPU operation). */
__attribute__((weak)) int board_smp_start(uintptr_t entry_pa) { (void)entry_pa; return 0; }
__attribute__((weak)) void board_secondary_init(int id) { (void)id; }
__attribute__((weak)) void board_ipi_send(int cpu) { (void)cpu; }

struct cpu *this_cpu(void) {
#if ARCH_AARCH64
    uintptr_t p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return (struct cpu *)p;
#else
    /* x86_64 only enters the kernel on the boot CPU. */
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

/* AArch64 acquires the BKL with an acquire exclusive load and releases it with a release store.
 * x86_64 remains single-CPU and its entry paths do not participate in this protocol. */
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
#endif
}

void klock_leave(void) {
#if ARCH_AARCH64
    __asm__ volatile("stlrb wzr, [%0]" :: "r"(&klock_word) : "memory");
#endif
}

void sched_kick(void) {
    struct cpu *me = this_cpu();
    for (int i = 0; i < MAX_CPUS; i++) {
        struct cpu *c = &cpus[i];
        if (c != me && c->online && c->idle) board_ipi_send(c->id);
    }
}

void smp_init(void) {
#if ARCH_AARCH64
    int n = board_smp_start(V2P(secondary_start));
    if (n) kprintf("smp: requested startup for %d secondary CPU%s\n", n, n > 1 ? "s" : "");
#elif ARCH_X86_64
    extern void lapic_init(void);
    lapic_init();
    board_smp_start(0);
#endif
}

/* First code a secondary CPU runs with the MMU on (boot.S jumps here with the CPU index in x0). */
void secondary_main(uint64_t id) {
#if ARCH_AARCH64
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&cpus[id]));
    __asm__ volatile("msr vbar_el1, %0; msr cntkctl_el1, %1; isb" :: "r"((uint64_t)vectors), "r"((uint64_t)2));
    klock_enter();
    board_secondary_init((int)id);
    cpus[id].online = 1;
    ncpus++;
    kprintf("cpu %lu online (%d CPUs online)\n", (unsigned long)id, ncpus);
    scheduler();
#else
    (void)id;
#endif
    arch_halt();
}
