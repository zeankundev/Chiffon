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

static uint64_t rng_state[2];
static int rng_seeded;

static uint64_t rng_timebase(void) {
#if ARCH_X86_64
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif ARCH_AARCH64
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return ticks;
#endif
}

static uint64_t splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* xorshift128+. NOT cryptographically secure on its own: used only when no hardware RNG exists. */
static uint64_t rng_fallback_word(void) {
    if (!rng_seeded) {
        uint64_t s = rng_timebase() ^ ((uint64_t)ticks << 32);
        rng_state[0] = splitmix64(&s);
        rng_state[1] = splitmix64(&s);
        rng_seeded = 1;
    }
    uint64_t s1 = rng_state[0];
    const uint64_t s0 = rng_state[1];
    rng_state[0] = s0;
    s1 ^= s1 << 23;
    rng_state[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return rng_state[1] + s0;
}

void kernel_random_fill(void* buf, size_t n) {
    uint8_t* p = buf;
    while (n) {
        uint64_t w;
        if (hardware_random_word(&w) != 0) w = rng_fallback_word();
        size_t k = n < 8 ? n : 8;
        memcpy(p, &w, k);
        p += k;
        n -= k;
    }
}
