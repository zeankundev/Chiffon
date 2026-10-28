/* x86_64: higher-half start, interrupt/syscall entry, context switch. */
    .text
    .code64
    .globl start64
start64:
    mov $kboot_stack_top, %rsp
    xor %ebp, %ebp
    call kmain                           /* rdi = multiboot info */
1:  hlt
    jmp 1b

.macro PUSHALL
    push %rax; push %rbx; push %rcx; push %rdx; push %rsi; push %rdi; push %rbp
    push %r8;  push %r9;  push %r10; push %r11; push %r12; push %r13; push %r14; push %r15
.endm
.macro POPALL
    pop %r15; pop %r14; pop %r13; pop %r12; pop %r11; pop %r10; pop %r9; pop %r8
    pop %rbp; pop %rdi; pop %rsi; pop %rdx; pop %rcx; pop %rbx; pop %rax
.endm

.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
isr\n:
.if (\n==8)||(\n>=10 && \n<=14)||(\n==17)||(\n==21)||(\n==29)||(\n==30)
    push $\n
.else
    push $0
    push $\n
.endif
    jmp isr_common
.endr

isr_common:
    PUSHALL
    cld
    mov %rsp, %rdi
    call trap_c
    .globl trap_return
trap_return:
    POPALL
    add $16, %rsp
    iretq

/* SYSCALL entry: build the same frame an interrupt would, return with iretq. */
    .globl syscall_entry
syscall_entry:
    mov %rsp, user_rsp_tmp(%rip)
    mov syscall_kstack(%rip), %rsp
    push $0x1B                           /* ss  */
    push user_rsp_tmp(%rip)              /* rsp */
    push %r11                            /* rflags */
    push $0x23                           /* cs  */
    push %rcx                            /* rip */
    push $0                              /* err */
    push $0x80                           /* trapno */
    PUSHALL
    mov %rsp, %rdi
    call syscall_dispatch
    jmp trap_return

    .globl arch_switch
arch_switch:                             /* (uintptr_t *old_sp, uintptr_t new_sp) */
    push %rbp; push %rbx; push %r12; push %r13; push %r14; push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15; pop %r14; pop %r13; pop %r12; pop %rbx; pop %rbp
    ret

    .section .rodata
    .balign 8
    .globl isr_table
isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .quad isr\n
.endr

    .bss
    .balign 16
kboot_stack: .space 16384
kboot_stack_top:
