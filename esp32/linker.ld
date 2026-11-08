/* ESP32 memory map (from ESP-IDF's esp32 memory.ld.in, without Bluetooth/WiFi reservations).
 *   iram0  0x40080000  128 KiB  instructions (SRAM0; the ROM copies .text here)
 *   dram0  0x3FFB0000  177 KiB  data, bss, stack and heap.  The top of this bank is used by the
 *                               ROM; ESP-IDF stops at 0x3FFDC200 and so do we.
 * Nothing is loaded from flash: the ROM copies every segment of the image into RAM, then jumps to
 * _start.  .bss is cleared by start.S. */
OUTPUT_ARCH(xtensa)
ENTRY(_start)

MEMORY
{
  iram0 (RX) : ORIGIN = 0x40080000, LENGTH = 0x20000
  dram0 (RW) : ORIGIN = 0x3FFB0000, LENGTH = 0x2C200
}

SECTIONS
{
  .text : {
    KEEP(*(.text.start))
    *(.text .text.*)
    *(.literal .literal.*)
    . = ALIGN(4);
  } > iram0

  .rodata : {
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > dram0

  .data : {
    *(.data .data.*)
    . = ALIGN(4);
  } > dram0

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > dram0

  .stack (NOLOAD) : {
    . = ALIGN(16);
    . += 8192;                         /* 8 KiB boot/kernel stack, grows down from _stack_top */
    _stack_top = .;
  } > dram0

  _heap_start = ALIGN(16);
  _heap_end = ORIGIN(dram0) + LENGTH(dram0);

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.xtensa.info) }
}
