/* Kernel runs in the upper half (TTBR1). LOAD_ADDR (physical load address) comes from the
 * board: --defsym=LOAD_ADDR=... (board.mk; default 0x40080000 = QEMU virt RAM + 512K). */
ENTRY(_start_phys)
KOFF = 0xFFFF000000000000;
SECTIONS
{
    . = KOFF + LOAD_ADDR;
    .text : AT(ADDR(.text) - KOFF) { KEEP(*(.text.boot)) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KOFF) { *(.rodata .rodata.*) . = ALIGN(4096); *(.initrd) }
    .data : AT(ADDR(.data) - KOFF) { *(.data .data.*) }
    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - KOFF) {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }
    _start_phys = _start - KOFF;
    __image_size = __bss_end - ADDR(.text);
    __kernel_end_phys = __bss_end - KOFF;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
