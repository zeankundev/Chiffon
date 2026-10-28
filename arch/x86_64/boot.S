/* Multiboot1 entry: 32-bit protected mode -> long mode -> higher half.
 * Works with QEMU "-kernel", GRUB (multiboot), and anything multiboot-capable. */
    .set MB_MAGIC, 0x1BADB002
    .set MB_FLAGS, 0x00010003            /* page-align modules, want meminfo, address fields valid */

    .section .multiboot, "a"
    .balign 4
mb_header:
    .long MB_MAGIC
    .long MB_FLAGS
    .long -(MB_MAGIC + MB_FLAGS)
    .long mb_header                      /* header_addr  */
    .long __load_start                   /* load_addr    */
    .long __load_end                     /* load_end_addr*/
    .long __bss_end                      /* bss_end_addr */
    .long _start                         /* entry_addr   */

    .section .boot.text, "ax"
    .code32
    .globl _start
_start:
    cli
    mov $boot_stack_top, %esp
    mov %ebx, mbi_saved

    /* PD0..PD3: identity 4 GiB with 2 MiB pages (P|RW|PS) */
    mov $pd0, %edi
    mov $0x83, %eax
    mov $2048, %ecx
1:  mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b

    /* PDPT_LO[0..3] -> PD0..PD3 */
    mov $pdpt_lo, %edi
    mov $pd0, %eax
    or $3, %eax
    mov $4, %ecx
2:  mov %eax, (%edi)
    movl $0, 4(%edi)
    add $4096, %eax
    add $8, %edi
    loop 2b

    /* PDPT_HI[510] -> PD0, [511] -> PD1  (virtual -2GiB .. 0) */
    mov $pd0, %eax
    or $3, %eax
    mov %eax, pdpt_hi + 510*8
    add $4096, %eax
    mov %eax, pdpt_hi + 511*8

    /* PML4: [0] identity, [256] direct map, [511] kernel image */
    mov $pdpt_lo, %eax
    or $3, %eax
    mov %eax, boot_pml4
    mov %eax, boot_pml4 + 256*8
    mov $pdpt_hi, %eax
    or $3, %eax
    mov %eax, boot_pml4 + 511*8

    mov %cr4, %eax
    or $0x20, %eax                       /* PAE */
    mov %eax, %cr4
    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx                /* EFER.LME */
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80000001, %eax                 /* PG | PE */
    mov %eax, %cr0
    lgdt boot_gdt_ptr
    ljmp $0x08, $long_entry

    .code64
long_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    mov mbi_saved(%rip), %edi            /* multiboot info (physical) */
    movabs $start64, %rax
    jmp *%rax

    .section .boot.data, "aw"
    .balign 16
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF             /* 64-bit code */
    .quad 0x00CF92000000FFFF             /* data */
boot_gdt_ptr:
    .word 3*8 - 1
    .long boot_gdt
mbi_saved: .long 0
    .balign 4096
    .globl boot_pml4
boot_pml4: .space 4096
pdpt_lo:   .space 4096
pdpt_hi:   .space 4096
pd0:       .space 4096
pd1:       .space 4096
pd2:       .space 4096
pd3:       .space 4096
    .balign 16
boot_stack: .space 4096
boot_stack_top:
