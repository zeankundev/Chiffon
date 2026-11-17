/* rom_call1(fn, arg): call a windowed-ABI ROM function with one argument from call0 C code.
 * call8 rotates the register window; the callee returns with retw to this frame, so a0 (our
 * return address) is untouched and a plain ret is correct. */
    .section .text, "ax"
    .global rom_call1
    .type rom_call1, @function
rom_call1:
    mov     a10, a3
    callx8  a2
    ret
