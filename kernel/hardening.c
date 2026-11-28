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

/* ---- Kernel random source ----
 * A ChaCha20 generator with fast key erasure.  The key is seeded from the hardware RNG (RDRAND on
 * x86_64, RNDR on aarch64) when one exists, and always from timing jitter.  Every 1024 blocks the
 * key is refreshed from the hardware RNG again when present.  Without a hardware RNG the generator
 * is only as strong as the jitter it is seeded with, which is weaker than real hardware entropy:
 * the kernel reports that once at first use.  This source feeds /dev/urandom, getrandom(2), AT_RANDOM
 * (the stack canary and pointer-mangling seeds of user programs) and therefore TLS key generation. */
#define RNG_RESEED_BLOCKS 1024u

static uint32_t rng_key[8];
static uint64_t rng_block;               /* block counter (64-bit, never repeats) */
static int rng_keyed, rng_warned;

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
static void chacha_qr(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = ROTL32(*d, 16);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 12);
    *a += *b; *d ^= *a; *d = ROTL32(*d, 8);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 7);
}
/* One ChaCha20 block: 256-bit key, 64-bit block counter, zero nonce. */
static void chacha_block(const uint32_t key[8], uint64_t ctr, uint32_t out[16]) {
    uint32_t in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                       (uint32_t)ctr, (uint32_t)(ctr >> 32), 0, 0};
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int r = 0; r < 10; r++) {
        chacha_qr(&x[0], &x[4], &x[8], &x[12]);  chacha_qr(&x[1], &x[5], &x[9], &x[13]);
        chacha_qr(&x[2], &x[6], &x[10], &x[14]); chacha_qr(&x[3], &x[7], &x[11], &x[15]);
        chacha_qr(&x[0], &x[5], &x[10], &x[15]); chacha_qr(&x[1], &x[6], &x[11], &x[12]);
        chacha_qr(&x[2], &x[7], &x[8], &x[13]);  chacha_qr(&x[3], &x[4], &x[9], &x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

/* Fold an entropy word into the key: key = PRF(key XOR input). */
static void rng_absorb(uint64_t w) {
    uint32_t blk[16];
    rng_key[0] ^= (uint32_t)w;         rng_key[1] ^= (uint32_t)(w >> 32);
    rng_key[2] ^= (uint32_t)(w * 0x9E3779B97F4A7C15ull);
    rng_key[3] ^= (uint32_t)((w * 0x9E3779B97F4A7C15ull) >> 32);
    chacha_block(rng_key, rng_block++, blk);
    memcpy(rng_key, blk, sizeof rng_key);
}

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

static int rng_hw_word(uint64_t *w) { return hardware_random_word(w) == 0; }

static void rng_seed(void) {
    uint64_t hw;
    int got_hw = 0;
    for (int i = 0; i < 4; i++) if (rng_hw_word(&hw)) { rng_absorb(hw); got_hw = 1; }
    /* Jitter: the cost of each loop varies with cache, interrupt and bus timing. */
    uint64_t x = 0x243F6A8885A308D3ull;
    for (int i = 0; i < 256; i++) {
        uint64_t t0 = rng_timebase();
        for (int k = 0; k < (int)(t0 & 31) + 1; k++) x = (x ^ (x << 13)) * 0x2545F4914F6CDD1Dull;
        rng_absorb(rng_timebase() - t0 + x);
    }
    rng_keyed = 1;
    if (!got_hw && !rng_warned) {
        rng_warned = 1;
        kprintf("[random] no hardware RNG: generator seeded from timing jitter only\n");
    }
}

void kernel_random_fill(void *buf, size_t n) {
    if (!rng_keyed) rng_seed();
    uint8_t *p = buf;
    while (n) {
        if (rng_block % RNG_RESEED_BLOCKS == 0 && rng_block) {
            uint64_t hw;
            if (rng_hw_word(&hw)) rng_absorb(hw);
        }
        uint32_t blk[16];
        chacha_block(rng_key, rng_block++, blk);
        memcpy(rng_key, blk, sizeof rng_key);            /* fast key erasure */
        size_t k = n < 32 ? n : 32;                       /* output: the other half of the block */
        memcpy(p, &blk[8], k);
        p += k;
        n -= k;
    }
}
