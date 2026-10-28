/* Boot code runs at its physical address; the kernel proper lives in the
 * top 2 GiB (higher half) and is loaded physically right after it. */
ENTRY(_start)
KOFF = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 0x100000;
    __load_start = .;
    .boot : {
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }
    . = ALIGN(4096);
    . += KOFF;

    .text : AT(ADDR(.text) - KOFF) { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KOFF) { *(.rodata .rodata.*) . = ALIGN(4096); *(.initrd) }
    .data : AT(ADDR(.data) - KOFF) { *(.data .data.*) }
    __load_end = LOADADDR(.data) + SIZEOF(.data);
    .bss : AT(ADDR(.bss) - KOFF) {
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4096);
    }
    __bss_end = LOADADDR(.bss) + SIZEOF(.bss);
    __kernel_end_phys = __bss_end;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
