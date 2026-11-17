# Chiffon on ESP32 (Xtensa LX6, ESP32-D0WD-V3: 520 KB SRAM, no external PSRAM).
#
#   make            build chiffon.elf, the ESP image chiffon.bin, and flash.bin (4 MiB, image at 0x1000)
#   make run        boot flash.bin in Espressif's QEMU (set ESP_QEMU to its qemu-system-xtensa)
#   make flash PORT=/dev/ttyUSB0   write the image to a real board
#
# Needs the Espressif toolchain (xtensa-esp32-elf-gcc) and esptool.  Set ESP_TOOLS to the directory
# that holds the toolchain's bin/ (or put it on PATH), and ESPTOOL to the esptool command.
ESP_TOOLS ?=
TC    := $(if $(ESP_TOOLS),$(ESP_TOOLS)/)xtensa-esp32-elf-
CC    := $(TC)gcc
OBJCOPY := $(TC)objcopy
SIZE  := $(TC)size
ESPTOOL ?= esptool.py
ESP_QEMU ?= qemu-system-xtensa
PORT  ?= /dev/ttyUSB0

B := ../../build/esp32
SHARED := ../../kernel
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables \
          -mabi=call0 -mlongcalls -mtext-section-literals \
          -DCHI_TINY=1 -DMAX_CPUS=1 -DCHI_MAXFRAMES=1024 \
          -Iinclude -I../../include
LDFLAGS := -nostdlib -static -T linker.ld

SRC_C := main.c divdi.c flash.c ext2.c elf.c $(SHARED)/lib.c $(SHARED)/mm.c
OBJ   := $(patsubst %.c,$(B)/%.o,$(notdir $(SRC_C))) $(B)/start.o
OBJ   := $(sort $(OBJ) $(B)/flash_S.o)

.PHONY: all run flash clean
all: $(B)/chiffon.bin $(B)/flash.bin $(B)/fs.img $(PROGS)
	@$(SIZE) $(B)/chiffon.elf

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/lib.o: $(SHARED)/lib.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/mm.o: $(SHARED)/mm.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/start.o: start.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/chiffon.elf: $(OBJ) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

# ESP image: the format the ROM expects at flash offset 0x1000 (the second-stage bootloader slot)
$(B)/chiffon.bin: $(B)/chiffon.elf
	$(ESPTOOL) --chip esp32 elf2image --flash_mode dio --flash_freq 40m --flash_size 4MB -o $@ $<

# User programs (programs/*.c) are static Xtensa ELF files for the fixed addresses in prog.ld.
PROG_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
               -mabi=call0 -mlongcalls -mtext-section-literals -Iprograms
PROGS := $(patsubst programs/%.c,$(B)/progs/%,$(wildcard programs/*.c))
$(B)/progs/%: programs/%.c programs/prog_start.S programs/prog.ld programs/api.h
	@mkdir -p $(dir $@)
	$(CC) $(PROG_CFLAGS) -c programs/prog_start.S -o $(B)/progs/prog_start.o
	$(CC) $(PROG_CFLAGS) -c $< -o $(B)/progs/$*.o
	$(CC) -nostdlib -static -T programs/prog.ld -o $@ $(B)/progs/prog_start.o $(B)/progs/$*.o
	$(SIZE) $@

# Root filesystem: rootfs/ plus the compiled programs in bin/, as an ext4 image (mke2fs -d).
# It sits at flash 0x200000 (1 MiB).
FS_TYPE ?= ext4
$(B)/fs.img: $(shell find rootfs -type f) $(PROGS)
	@mkdir -p $(dir $@)
	rm -rf $(B)/rootfs && cp -r rootfs $(B)/rootfs && mkdir -p $(B)/rootfs/bin
	for p in $(PROGS); do cp $$p $(B)/rootfs/bin/; done
	rm -f $@
	truncate -s 1M $@
	mke2fs -q -t $(FS_TYPE) -O ^has_journal,^64bit,^metadata_csum -b 1024 -L chiffon -d $(B)/rootfs $@

$(B)/flash.bin: $(B)/chiffon.bin $(B)/fs.img
	$(ESPTOOL) --chip esp32 merge_bin -o $@ --fill-flash-size 4MB 0x1000 $(B)/chiffon.bin 0x200000 $(B)/fs.img

run: $(B)/flash.bin
	$(ESP_QEMU) -M esp32 -nographic -no-reboot -drive file=$(B)/flash.bin,if=mtd,format=raw

flash: $(B)/chiffon.bin
	$(ESPTOOL) --chip esp32 --port $(PORT) --baud 921600 write_flash 0x1000 $(B)/chiffon.bin

clean:
	rm -rf $(B)

$(B)/flash_S.o: flash.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/elf.o: elf.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
