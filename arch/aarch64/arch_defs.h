#pragma once
#include <stdint.h>
#define ARCH_AARCH64 1
#define ARCH_NAME "aarch64"
#define ELF_MACHINE 183
#define DMAP_BASE 0xFFFF000000000000UL
#define USER_TOP  0x0000800000000000UL
#define AUX_HWCAP 3                      /* HWCAP_FP | HWCAP_ASIMD */

/* Layout must match SAVE/RESTORE in entry.S (304 bytes) */
struct trapframe {
    uint64_t x[31];
    uint64_t sp, pc, pstate, esr, far, pad[2];
};
struct arch_uctx {
    uint64_t q[64] __attribute__((aligned(16)));    /* v0-v31 */
    uint32_t fpcr, fpsr;
    uint64_t tpidr;                                 /* TLS pointer */
};
#define TF_SYSNO(tf) ((long)(tf)->x[8])
#define TF_A0(tf) ((long)(tf)->x[0])
#define TF_A1(tf) ((long)(tf)->x[1])
#define TF_A2(tf) ((long)(tf)->x[2])
#define TF_A3(tf) ((long)(tf)->x[3])
#define TF_A4(tf) ((long)(tf)->x[4])
#define TF_A5(tf) ((long)(tf)->x[5])
#define TF_RET(tf) ((tf)->x[0])
#define TF_SP(tf) ((tf)->sp)
#define TF_PC(tf) ((tf)->pc)
#define TF_FROM_USER(tf) (((tf)->pstate & 0xF) == 0)
