/* Program entry: the loader calls _start(api) with the call0 ABI.  prog_main() is a C function
 * that is also call0, which clobbers a0, so keep the loader's return address on the stack. */
    .section .text.start, "ax"
    .global _start
    .type _start, @function
_start:
    addi    a1, a1, -16
    s32i    a0, a1, 0
    call0   prog_main
    l32i    a0, a1, 0
    addi    a1, a1, 16
    ret
