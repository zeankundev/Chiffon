ENV ?= demo
ENVDIR := env/$(ENV)

HOSTARCH_RAW := $(shell uname -m)
ifneq ($(filter aarch64 arm64 armv8l,$(HOSTARCH_RAW)),)
  HOSTARCH := aarch64
else
  HOSTARCH := x86_64
endif
NEEDS_ENV := $(filter-out help new-env new-device clean,$(or $(MAKECMDGOALS),all))
ifneq ($(NEEDS_ENV),)
  ifeq ($(wildcard $(ENVDIR)/chiffon.conf),)
    $(error No environment 'env/$(ENV)/chiffon.conf'. Create one with:  make new-env NAME=$(ENV))
  endif
  include $(ENVDIR)/chiffon.conf
endif
ARCH   := $(strip $(or $(ARCH),auto))
ifeq ($(ARCH),auto)
  ARCH := $(HOSTARCH)
endif
DEVICE := $(strip $(or $(DEVICE),qemu))
FS     := $(strip $(or $(FS),ext4))
SIZE   := $(strip $(SIZE))
MBR    := $(strip $(MBR))
CROSS  ?= $(if $(filter-out $(HOSTARCH),$(ARCH)),$(ARCH)-linux-gnu-)

ifeq ($(filter $(ARCH),x86_64 aarch64),)
  $(error ARCH must be x86_64 or aarch64 (got '$(ARCH)'))
endif
ifeq ($(filter $(FS),ext2 ext3 ext4 initramfs),)
  $(error FS must be ext2, ext3, ext4 or initramfs (got '$(FS)'). Other filesystems need a kernel driver: docs/ADDING-A-FILESYSTEM.md)
endif

# ---- board: custom (env/<ENV>/device/<DEVICE>) wins over built-in (device/<ARCH>/<DEVICE>) ----
BOARD_DIR := $(firstword $(wildcard $(ENVDIR)/device/$(DEVICE)) $(wildcard device/$(ARCH)/$(DEVICE)))
ifneq ($(NEEDS_ENV),)
  ifeq ($(BOARD_DIR),)
    $(error No board '$(DEVICE)' for $(ARCH). Built in: $(notdir $(wildcard device/$(ARCH)/*)). Custom: make new-device ENV=$(ENV) DEVICE=$(DEVICE))
  endif
  -include $(BOARD_DIR)/board.mk
endif
ifeq ($(ARCH),aarch64)
  LOAD_ADDR ?= 0x40080000
  BOOT_DEVICE_GB_MASK ?= 0x1
endif

BUILD ?= build
B := $(BUILD)/$(ENV)/$(ARCH)-$(DEVICE)
# host helper (cpio writer): located/built lazily by tools/hosttool.sh; falls back to $$TMPDIR if BUILD is noexec
MKCPIO = $$(sh tools/hosttool.sh "$(BUILD)" "$(HOSTCC)")
ifeq ($(CC),cc)
  CC := $(CROSS)gcc
endif
ifeq ($(origin LD),default)
  LD := $(CROSS)ld
endif
OBJCOPY ?= $(CROSS)objcopy
HOSTCC ?= cc

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -ffreestanding -fno-builtin \
          -fno-pic -fno-pie -fno-stack-protector \
          $(shell $(CC) -fno-tree-loop-distribute-patterns -E -x c /dev/null -o /dev/null 2>/dev/null && echo -fno-tree-loop-distribute-patterns) \
          -fno-asynchronous-unwind-tables -mgeneral-regs-only -Iinclude -Iarch/$(ARCH) -I$(BOARD_DIR)
ifeq ($(ARCH),x86_64)
  CFLAGS += -mno-red-zone -mcmodel=kernel
  KEXT := bin
else
  CFLAGS += -mcmodel=small -mstrict-align -DBOOT_DEVICE_GB_MASK=$(BOOT_DEVICE_GB_MASK) \
            -DIMAGE_TEXT_OFFSET=$(shell printf '0x%x' $$(( $(LOAD_ADDR) & 0x1fffff )))
  KEXT := bin
endif
NOWARN := $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx-segments && echo --no-warn-rwx-segments)
LDFLAGS := -z noexecstack $(NOWARN) -nostdlib -static -no-pie -z max-page-size=4096 \
           $(if $(LOAD_ADDR),--defsym=LOAD_ADDR=$(LOAD_ADDR)) -T arch/$(ARCH)/linker.ld

KERNEL := $(B)/chiffon.$(KEXT)
DISKIMG := $(if $(filter initramfs,$(FS)),,$(B)/disk.img)
DISK    := $(DISKIMG)
DISKARGS = $(if $(DISK),$(DISKARGS_FMT))

SRC_K := $(wildcard kernel/*.c) $(wildcard arch/$(ARCH)/*.c) $(wildcard drivers/$(ARCH)/*.c)
SRC_S := $(wildcard arch/$(ARCH)/*.S) kernel/initrd.S
SRC_B := $(wildcard $(BOARD_DIR)/*.c) $(wildcard $(BOARD_DIR)/*.S)
OBJ := $(patsubst %.c,$(B)/%.o,$(SRC_K)) $(patsubst %.S,$(B)/%.o,$(SRC_S)) \
       $(patsubst $(BOARD_DIR)/%,$(B)/board/%.o,$(basename $(SRC_B)))
OBJ := $(sort $(OBJ))

# compiler for user programs (override e.g. UCC="clang --target=aarch64-linux-gnu")
UCC ?= $(CROSS)gcc
UCFLAGS := -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -no-pie -O2 \
           $(shell $(UCC) -fno-tree-loop-distribute-patterns -E -x c /dev/null -o /dev/null 2>/dev/null && echo -fno-tree-loop-distribute-patterns) \
           -Wno-unused-command-line-argument -Iuserland -Iinclude
USER_SRC := $(wildcard $(ENVDIR)/src/*.c)
USER_BIN := $(patsubst $(ENVDIR)/src/%.c,$(B)/userland/%,$(USER_SRC))
ENV_FILES := $(shell find $(ENVDIR)/rootfs -type f 2>/dev/null) $(wildcard $(ENVDIR)/build.sh)

.DEFAULT_GOAL := all
all: $(KERNEL) $(DISKIMG)
	@echo ""
	@echo "Built env '$(ENV)': $(ARCH) / $(DEVICE) / $(FS)"
	@echo "  kernel: $(KERNEL)$(if $(DISKIMG),\n  disk:   $(DISKIMG))"
	@echo "  run:    make run ENV=$(ENV)"

$(B)/chiffon.elf: $(OBJ) arch/$(ARCH)/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
$(B)/chiffon.bin: $(B)/chiffon.elf
	$(OBJCOPY) -O binary $< $@

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/board/%.o: $(BOARD_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/board/%.o: $(BOARD_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DINITRD_FILE='"$(abspath $(B)/initramfs.cpio)"' -c $< -o $@
$(B)/kernel/initrd.o: $(B)/initramfs.cpio

$(B)/userland/%: $(ENVDIR)/src/%.c userland/libchiffon.h
	@mkdir -p $(dir $@)
	$(UCC) $(UCFLAGS) -o $@ $<

# root filesystem tree = env rootfs/ overlay + compiled programs (+ optional env/<ENV>/build.sh hook)
$(B)/rootfs.stamp: $(USER_BIN) $(ENV_FILES) tools/mkrootfs.sh
	sh tools/mkrootfs.sh "$(ENVDIR)" "$(B)/rootfs" "$(B)/userland" "$(ARCH)" "$(CROSS)"
	@touch $@

# the kernel always embeds an initramfs; it is empty unless FS=initramfs
# rebuilt automatically when FS/SIZE/MBR/ARCH/DEVICE change
$(B)/config.stamp: FORCE
	@mkdir -p $(B); echo "$(ARCH) $(DEVICE) $(FS) $(SIZE) $(MBR)" > $@.new; if cmp -s $@.new $@; then rm -f $@.new; else mv $@.new $@; fi
FORCE:
$(B)/initramfs.cpio: $(B)/rootfs.stamp $(B)/config.stamp
	@mkdir -p $(B)/empty
	$(MKCPIO) $(if $(filter initramfs,$(FS)),$(B)/rootfs,$(B)/empty) $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
$(DISKIMG): $(B)/rootfs.stamp $(B)/config.stamp
	MKCPIO="$(MKCPIO)" sh tools/mkimage.sh -r "$(B)/rootfs" -o "$@" -f "$(FS)" -l "$(ENV)" $(if $(SIZE),-s $(SIZE)) $(if $(MBR),-p)

run: all
	@$(if $(QEMU_CMD),,$(error Board '$(DEVICE)' has no QEMU_CMD in $(BOARD_DIR)/board.mk; run the kernel image on your hardware))
	$(QEMU_CMD) $(if $(CMDLINE),-append "$(CMDLINE)")

# same machine with a graphical window: framebuffer console + PS/2 keyboard (serial stays on this terminal)
run-gfx: all
	@$(if $(QEMU_GFX_CMD),,$(error Board '$(DEVICE)' has no QEMU_GFX_CMD in $(BOARD_DIR)/board.mk))
	$(QEMU_GFX_CMD) $(if $(CMDLINE),-append "$(CMDLINE)")

new-env:
	@test -n "$(NAME)" || { echo "Usage: make new-env NAME=<name>"; exit 1; }
	@test ! -e env/$(NAME) || { echo "env/$(NAME) already exists"; exit 1; }
	cp -R env/demo env/$(NAME)
	@echo ""
	@echo "Created env/$(NAME) from the sample template."
	@echo " env/$(NAME)/chiffon.conf: edit this file to configure the environment (ARCH, DEVICE, FS, SIZE, MBR, CMDLINE)"
	@echo "  env/$(NAME)/src/*.c            your programs (src/init.c -> /sbin/init), see userland/libchiffon.h"
	@echo "  env/$(NAME)/rootfs/            your root filesystem overlay (files copied into the disk image, chiffon.ini picks the init)"
	@echo "Build and test: make ENV=$(NAME) run"
new-device:
	@test -n "$(DEVICE)" || { echo "Usage: make new-device DEVICE=<name>"; exit 1; }
	@test -d env/$(ENV) || { echo "no env/$(ENV): make new-env NAME=$(ENV) first"; exit 1; }
	@test ! -e env/$(ENV)/device/$(DEVICE) || { echo "env/$(ENV)/device/$(DEVICE) already exists"; exit 1; }
	@test -d device/$(ARCH)/$(or $(BASE),qemu) || { echo "no built-in board device/$(ARCH)/$(or $(BASE),qemu) to start from"; exit 1; }
	mkdir -p env/$(ENV)/device
	cp -R device/$(ARCH)/$(or $(BASE),qemu) env/$(ENV)/device/$(DEVICE)
	@sed -i 's|^DEVICE=.*|DEVICE=$(DEVICE)|; s|^ARCH=.*|ARCH=$(ARCH)|' env/$(ENV)/chiffon.conf
	@echo "Created env/$(ENV)/device/$(DEVICE)"
clean:
	@rm -rf $(BUILD)
help:
	@echo "Chiffon - a small Unix-like microkernel + OS (c) 2026 zeankun"
	@echo ""
	@echo "- make [ENV=demo] : build the kernel and disk image for the given environment (default: demo)"
	@echo "- make new-env NAME=<name> : create a new environment with the given name"
	@echo "- make new-device DEVICE=<name> : create a new device for the current environment"
	@echo "- make run [ENV=demo] : run the kernel in QEMU on the serial console (if supported by the board)"
	@echo "- make run-gfx [ENV=demo] : run the kernel in QEMU with a window (framebuffer console + keyboard)"
	@echo "- make clean : remove all build artifacts"
	@echo ""
	@echo "Environment configuration: env/<ENV>/chiffon.conf"
.PHONY: all run run-gfx new-env new-device clean help FORCE