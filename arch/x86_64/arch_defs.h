#pragma once
#include <stdint.h>
#define ARCH_X86_64 1
#define ARCH_NAME "x86_64"
#define ELF_MACHINE 62
#define DMAP_BASE 0xFFFF800000000000UL
#define USER_TOP  0x0000800000000000UL
#define AUX_HWCAP 0

/* Layout must match the push order in entry.S */
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t trapno, err;
    uint64_t rip, cs, rflags, rsp, ss;
};
struct arch_uctx {
    uint8_t fx[512] __attribute__((aligned(16)));   /* fxsave area */
    uint64_t fs_base;                               /* TLS pointer (arch_prctl) */
};
#define TF_SYSNO(tf) ((long)(tf)->rax)
#define TF_A0(tf) ((long)(tf)->rdi)
#define TF_A1(tf) ((long)(tf)->rsi)
#define TF_A2(tf) ((long)(tf)->rdx)
#define TF_A3(tf) ((long)(tf)->r10)
#define TF_A4(tf) ((long)(tf)->r8)
#define TF_A5(tf) ((long)(tf)->r9)
#define TF_RET(tf) ((tf)->rax)
#define TF_SP(tf) ((tf)->rsp)
#define TF_PC(tf) ((tf)->rip)
#define TF_FROM_USER(tf) (((tf)->cs & 3) == 3)
