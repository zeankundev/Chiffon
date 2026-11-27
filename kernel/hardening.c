#include "kernel.h"

/* Available before heap, scheduler or device initialization. */
int hardware_random_word(uint64_t *out) {
#if ARCH_X86_64
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(c & (1u << 30))) return -ENOSYS;
    for (int i = 0; i < 10; i++) {
        unsigned char ok;
        uint64_t word;
        __asm__ volatile("rdrand %0; setc %1" : "=&r"(word), "=qm"(ok) :: "cc");
        if (ok) { *out = word; return 0; }
    }
#elif ARCH_AARCH64
    uint64_t features;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(features));
    if (!(features >> 60)) return -ENOSYS;
    for (int i = 0; i < 10; i++) {
        uint64_t word, ok;
        __asm__ volatile("mrs %0, S3_3_C2_C4_0; cset %1, ne"
                         : "=&r"(word), "=r"(ok) :: "cc");
        if (ok) { *out = word; return 0; }
    }
#else
    (void)out;
    return -ENOSYS;
#endif
    return -EAGAIN;
}

/* A terminator canary still detects accidental overwrites on CPUs without an RNG. */
uintptr_t __stack_chk_guard = 0xff0a0000UL;

__attribute__((no_stack_protector)) void stack_guard_init(void) {
    uint64_t word;
    if (!hardware_random_word(&word) && (word & ~0xffUL))
        __stack_chk_guard = word & ~0xffUL;
}

__attribute__((noreturn,no_stack_protector)) void __stack_chk_fail(void) {
    panic("kernel stack guard corrupted");
}
