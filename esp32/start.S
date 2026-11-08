/* ESP32 entry point.  The ROM has set up the clocks and the UART and copied this image into RAM.
 * We set the stack, clear .bss and call kmain() with the call0 ABI (no register windows, so no
 * window-overflow exception handlers are needed). */
    .section .text.start, "ax"
    .global _start
    .type _start, @function
_start:
    movi    a1, _stack_top
    movi    a2, __bss_start
    movi    a3, __bss_end
    movi    a4, 0
1:  bgeu    a2, a3, 2f
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       1b
2:  call0   kmain
3:  j       3b
