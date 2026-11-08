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

B := ../build/esp32
SHARED := ../kernel
CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
          -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables \
          -mabi=call0 -mlongcalls -mtext-section-literals \
          -DCHI_TINY=1 -DMAX_CPUS=1 -DCHI_MAXFRAMES=1024 \
          -Iinclude -I../include
LDFLAGS := -nostdlib -static -T linker.ld

SRC_C := main.c divdi.c $(SHARED)/lib.c $(SHARED)/mm.c
OBJ   := $(patsubst %.c,$(B)/%.o,$(notdir $(SRC_C))) $(B)/start.o
OBJ   := $(sort $(OBJ))

.PHONY: all run flash clean
all: $(B)/chiffon.bin $(B)/flash.bin
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

$(B)/flash.bin: $(B)/chiffon.bin
	$(ESPTOOL) --chip esp32 merge_bin -o $@ --fill-flash-size 4MB 0x1000 $<

run: $(B)/flash.bin
	$(ESP_QEMU) -M esp32 -nographic -no-reboot -drive file=$(B)/flash.bin,if=mtd,format=raw

flash: $(B)/chiffon.bin
	$(ESPTOOL) --chip esp32 --port $(PORT) --baud 921600 write_flash 0x1000 $(B)/chiffon.bin

clean:
	rm -rf $(B)
