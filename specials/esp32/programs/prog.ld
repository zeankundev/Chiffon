/* Program layout for ESP32 user programs (see elf.c).  The loader copies segments to these fixed
 * addresses, which lie outside the kernel's own memory: code in IRAM above the kernel's text, data
 * in the top of DRAM (the kernel's heap stops below it). */
OUTPUT_ARCH(xtensa)
ENTRY(_start)
MEMORY
{
  pgtext (RX) : ORIGIN = 0x40090000, LENGTH = 0x8000
  pgdata (RW) : ORIGIN = 0x3FFD4000, LENGTH = 0x8000
}
SECTIONS
{
  .text : { *(.text.start) *(.text .text.*) *(.literal .literal.*) . = ALIGN(4); } > pgtext
  .rodata : { *(.rodata .rodata.*) . = ALIGN(4); } > pgdata
  .data : { *(.data .data.*) . = ALIGN(4); } > pgdata
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) . = ALIGN(4); } > pgdata
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.xtensa.info) }
}
