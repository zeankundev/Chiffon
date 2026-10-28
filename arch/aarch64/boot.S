/* aarch64 entry (EL1, MMU off, x0 = DTB physical address).
 * Everything here is position independent (adrp) until the MMU is on. */
/* Bit n of BOOT_DEVICE_GB_MASK set => physical GiB n is mapped as device memory, else as RAM.
 * Override from a board's board.mk (default: GiB 0 = peripherals, as on QEMU virt). */
#ifndef BOOT_DEVICE_GB_MASK
#define BOOT_DEVICE_GB_MASK 0x1
#endif
    .section .text.boot, "ax"
/* arm64 Linux "Image" header: lets QEMU, U-Boot and the Raspberry Pi firmware load this file
 * as a kernel and pass the device tree pointer in x0.  Entry is the first instruction. */
#ifndef IMAGE_TEXT_OFFSET
#define IMAGE_TEXT_OFFSET 0x80000
#endif
    b    _start                          /* code0 */
    .word 0                              /* code1 */
    .quad IMAGE_TEXT_OFFSET              /* text_offset from a 2 MiB aligned base */
    .quad __image_size                   /* effective image size incl. bss */
    .quad 0                              /* flags: little endian */
    .quad 0, 0, 0
    .word 0x644d5241                     /* magic "ARM\x64" */
    .word 0
    .globl _start
_start:
    mov x19, x0
    mov x1, #(3 << 20)                   /* CPACR_EL1.FPEN: FP/SIMD allowed */
    msr cpacr_el1, x1
    isb

    adrp x0, __bss_start
    add  x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add  x1, x1, :lo12:__bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:
    adrp x0, boot_l1
    add  x0, x0, :lo12:boot_l1
    adrp x1, boot_l0_hi
    add  x1, x1, :lo12:boot_l0_hi
    adrp x2, boot_l0_lo
    add  x2, x2, :lo12:boot_l0_lo
    orr  x3, x0, #3
    str  x3, [x1]                        /* TTBR1: VA 0xFFFF0000_00000000 + PA */
    str  x3, [x2]                        /* TTBR0: identity (temporary)         */
    mov  x4, #0x401                      /* device template: nGnRnE, AF, block */
    movk x4, #0x60, lsl #48              /*                  UXN | PXN         */
    mov  x5, #0x705                      /* normal template: WB, inner-sh, AF  */
    movk x5, #0x40, lsl #48              /*                  UXN               */
.irp gb, 0, 1, 2, 3
    mov  x6, #(\gb << 30)
.if ((BOOT_DEVICE_GB_MASK) >> \gb) & 1
    orr  x7, x4, x6
.else
    orr  x7, x5, x6
.endif
    str  x7, [x0, #(\gb * 8)]
.endr

    mov  x0, #0xFF00                     /* MAIR: attr0 = device, attr1 = normal WB */
    msr  mair_el1, x0
    mov  x0, #0x3510                     /* TCR: 48-bit VA both halves, 4K, WBWA, inner-sh */
    movk x0, #0xB510, lsl #16
    movk x0, #0x2, lsl #32
    msr  tcr_el1, x0
    msr  ttbr0_el1, x2
    msr  ttbr1_el1, x1
    isb
    dsb  sy
    tlbi vmalle1
    dsb  sy
    isb
    mrs  x0, sctlr_el1
    orr  x0, x0, #1                      /* M */
    orr  x0, x0, #(1 << 2)               /* C */
    orr  x0, x0, #(1 << 12)              /* I */
    orr  x0, x0, #(1 << 23)              /* SPAN: leave PAN alone */
    bic  x0, x0, #(1 << 19)              /* WXN off */
    bic  x0, x0, #(1 << 1)               /* A off */
    msr  sctlr_el1, x0
    isb
    ldr  x1, =high_entry
    br   x1

    .text
high_entry:
    ldr  x0, =boot_stack_top
    mov  sp, x0
    mov  x29, xzr
    mov  x0, x19
    bl   kmain
1:  wfi
    b 1b

    .bss
    .balign 4096
boot_l0_lo: .space 4096
boot_l0_hi: .space 4096
    .globl boot_l1
boot_l1:    .space 4096
    .balign 16
boot_stack: .space 16384
boot_stack_top:
