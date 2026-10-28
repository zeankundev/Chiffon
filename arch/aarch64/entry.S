/* aarch64 exception vectors, context switch, FP/SIMD state. */
/* The kernel C code is built with -mgeneral-regs-only, which clang's assembler also applies to
 * .S files; this file saves/restores the user's FP/SIMD registers, so enable them explicitly. */
    .arch armv8-a+fp+simd
.macro SAVE
    sub  sp, sp, #304
    stp  x0, x1,   [sp, #0]
    stp  x2, x3,   [sp, #16]
    stp  x4, x5,   [sp, #32]
    stp  x6, x7,   [sp, #48]
    stp  x8, x9,   [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30,      [sp, #240]
    mrs  x21, sp_el0
    mrs  x22, elr_el1
    mrs  x23, spsr_el1
    str  x21, [sp, #248]
    stp  x22, x23, [sp, #256]
.endm

    .text
    .globl trap_return
trap_return:
    ldr  x21, [sp, #248]
    ldp  x22, x23, [sp, #256]
    msr  sp_el0, x21
    msr  elr_el1, x22
    msr  spsr_el1, x23
    ldp  x0, x1,   [sp, #0]
    ldp  x2, x3,   [sp, #16]
    ldp  x4, x5,   [sp, #32]
    ldp  x6, x7,   [sp, #48]
    ldp  x8, x9,   [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    ldr  x30,      [sp, #240]
    add  sp, sp, #304
    eret

sync_entry:
    SAVE
    mov  x0, sp
    bl   sync_c
    b    trap_return
irq_entry:
    SAVE
    mov  x0, sp
    bl   irq_c
    b    trap_return
bad_entry:
    SAVE
    mov  x0, sp
    bl   bad_c
    b    trap_return

    .section .text.vectors, "ax"
    .balign 2048
    .globl vectors
vectors:
    .macro VEC target
    .balign 128
    b \target
    .endm
    VEC bad_entry          /* cur EL, SP0: sync */
    VEC bad_entry          /*              irq  */
    VEC bad_entry          /*              fiq  */
    VEC bad_entry          /*              serr */
    VEC sync_entry         /* cur EL, SPx: sync */
    VEC irq_entry          /*              irq  */
    VEC bad_entry
    VEC bad_entry
    VEC sync_entry         /* lower EL, A64: sync */
    VEC irq_entry          /*                irq  */
    VEC bad_entry
    VEC bad_entry
    VEC bad_entry          /* lower EL, A32 */
    VEC bad_entry
    VEC bad_entry
    VEC bad_entry

    .text
    .globl arch_switch
arch_switch:                               /* (uintptr_t *old_sp, uintptr_t new_sp) */
    sub  sp, sp, #96
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    mov  x2, sp
    str  x2, [x0]
    mov  sp, x1
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    add  sp, sp, #96
    ret

    .globl arch_uctx_save
arch_uctx_save:
    stp q0, q1,   [x0, #0]
    stp q2, q3,   [x0, #32]
    stp q4, q5,   [x0, #64]
    stp q6, q7,   [x0, #96]
    stp q8, q9,   [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mrs x1, fpcr
    mrs x2, fpsr
    add x4, x0, #512
    stp w1, w2, [x4]
    mrs x3, tpidr_el0
    str x3, [x4, #8]
    ret

    .globl arch_uctx_restore
arch_uctx_restore:
    ldp q0, q1,   [x0, #0]
    ldp q2, q3,   [x0, #32]
    ldp q4, q5,   [x0, #64]
    ldp q6, q7,   [x0, #96]
    ldp q8, q9,   [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    add x4, x0, #512
    ldp w1, w2, [x4]
    msr fpcr, x1
    msr fpsr, x2
    ldr x3, [x4, #8]
    msr tpidr_el0, x3
    ret
