# Chiffon on ESP32

A minimal boot of Chiffon on the ESP32-D0WD-V3 (Xtensa LX6, 520 KB SRAM, no PSRAM).

What it does: the ROM loads the image, the kernel starts the UART console, reports memory,
self-tests the heap (the same `kernel/mm.c` allocator the other ports use), and drops into the
interactive `hello-init#` shell matching generic Chiffon's `env/demo` init. On panic, an illegal
instruction exception (`ill`) is raised to crash the CPU.

What it does not do yet: interrupts, processes, the MMU-based memory model, filesystems, networking.
Those need an interrupt vector table, the register-window or call0 exception handlers, and a
scheduler written for this chip. Nothing here is verified on real hardware. The boot path was
verified in Espressif's ESP32 QEMU with the chip's real ROM.

## Build

You need two tools.

1. The Espressif Xtensa toolchain. Use the `xtensa-esp32-elf` variant. The generic
   `xtensa-esp-elf` build is big-endian, and the ESP32 is little-endian.
   Download `xtensa-esp-elf-<version>-x86_64-linux-gnu.tar.xz` from
   https://github.com/espressif/crosstool-NG/releases and unpack it. Put its `bin/` on `PATH`, or
   pass it as `ESP_TOOLS=/path/to/bin`.
2. esptool (`pip install esptool`, or a virtual environment if your system Python is locked).

```sh
make -C specials/esp32 ESP_TOOLS=/path/to/xtensa/bin ESPTOOL=esptool
```

This produces:

- `build/esp32/chiffon.elf` — the linked kernel. About 5 KB of code, 8 KB of BSS.
- `build/esp32/chiffon.bin` — the ESP image (`esptool elf2image`).
- `build/esp32/flash.bin` — a 4 MB image with `chiffon.bin` at offset `0x1000`, for QEMU.

## Run in emulation

Espressif publishes a QEMU build with an ESP32 machine:
https://github.com/espressif/qemu/releases (`qemu-xtensa-softmmu-...-x86_64-linux-gnu.tar.xz`).

```sh
make -C specials/esp32 run ESP_QEMU=/path/to/qemu-system-xtensa
```

Expected output (abridged):

```
Chiffon 0.4 (otohime, xtensa-esp32)
board: ESP32-D0WD-V3, 520 KiB SRAM, no PSRAM
mem: 167 KiB kernel heap above the 8 KiB stack (dram0 0x3FFB0000..0x3FFDC200 holds data, bss, stack, heap)
heap: self-test passed
boot complete; stack top 0x3ffb2350, heap 167 KiB
init: pid 1 is /sbin/init

Welcome to your simple Chiffon system! Type 'help' for a list of commands.
hello-init# 
```

## Flash a board

```sh
make -C specials/esp32 flash PORT=/dev/ttyUSB0 ESPTOOL=esptool
```

The command writes `chiffon.bin` to flash offset `0x1000`, the slot where the ESP32 ROM looks
for its second-stage bootloader. **This replaces any ESP-IDF bootloader on the board.** To restore
ESP-IDF later, flash its bootloader back to `0x1000` (or reflash the whole chip).

Reset the board after flashing. The console runs at 115200 baud on the UART0 pins that the ROM
uses (GPIO1 TX and GPIO3 RX on most boards).

## Memory layout

| Region | Address | Size | Use |
|---|---|---|---|
| IRAM | `0x40080000` | 128 KiB | code (the ROM copies it from flash) |
| DRAM | `0x3FFB0000`–`0x3FFDC200` | 177 KiB | data, bss, 8 KiB stack, heap |

The second 128 KiB data bank (`0x3FFE0000`–`0x40000000`) is not used yet. Adding it to the heap
is a small change in `linker.ld` and `main.c`.

## Next steps

- Install an exception vector table, so faults print a message instead of resetting.
- Enable the timer interrupt for a real tick, and replace the busy-wait heartbeat.
- Add a second heap over the unused data bank.
- Run the same kernel core on real hardware and record the boot log in `docs/`.

## Filesystem

The root filesystem is an ext2/3/4 image at flash offset `0x200000` (1 MiB). It is built from
`rootfs/` with `mke2fs -d` (e2fsprogs 1.43 or newer), together with the compiled programs in
`bin/`. The build rebuilds it whenever a file changes. Use `make FS_TYPE=ext2` for ext2.

The reader (`ext2.c`) is read-only. It supports ext2, ext3, and ext4: extent trees and classic
direct and indirect blocks, any block size up to 4 KiB, and 32- and 64-byte group descriptors.
It does not replay the journal, check checksums, or handle triple-indirect blocks.

The flash is read through the chip's flash MMU. `flash.c` maps 1 MiB of the flash into the
`0x3F400000` window with the ROM's own cache routines, which ESP-IDF's `spi_flash` also uses. In
QEMU the mapping was verified byte for byte. On real hardware, check the boot log for a fault when
the window is first read.

Shell commands: `ls`, `cat`, `cd`, `pwd`. Relative paths are taken from the working directory, and
`..` is supported.

## Programs

Any command that is not a builtin is looked up as `/bin/<name>`. The file must be a static
Xtensa ELF32 executable linked for the fixed addresses in `programs/prog.ld`:

- code in IRAM at `0x40090000`–`0x40098000`
- data in the top of DRAM at `0x3FFD4000`–`0x3FFDC000` (the kernel heap ends below it)

The loader (`elf.c`) copies the segments, zeroes `.bss`, and calls the entry point with a pointer
to the kernel interface in `programs/api.h`: `printf`, `putc`, `malloc`, `free`, and the cycle
counter. Programs are built with the same toolchain and flags (`-mabi=call0`), and link no libc.
`programs/hello.c` is the example.

Not supported yet: processes and `fork`, syscalls, IPC, and file writes. This means the Chiffon
userland programs in `env/` (`ipcdemo`, `fbdemo`, and so on) cannot run on the ESP32 as they are.
Those programs need the process model, which this port does not have.
