# Board knobs for aarch64 (all optional; plain make syntax).
#   LOAD_ADDR            physical address the kernel is loaded/linked at
#   BOOT_DEVICE_GB_MASK  bit n set => physical GiB n is mapped as device memory at boot (rest = RAM)
#   QEMU_CMD / DISKARGS_FMT  used by `make run`; leave out if the board cannot run under QEMU
LOAD_ADDR ?= 0x40080000
BOOT_DEVICE_GB_MASK = 0x1
MEM ?= 256            # guest RAM in MiB (env chiffon.conf may set MEM=1 for the tiny profile)
# user-mode network (QEMU's built-in NAT: DHCP at 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3)
QEMU_NET_ARGS ?= -netdev user,id=n0 -device virtio-net-device,netdev=n0
QEMU_CMD = qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m $(MEM) -kernel $(KERNEL) -nographic -no-reboot $(QEMU_NET_ARGS) $(DISKARGS)
# `make run-gfx`: ramfb framebuffer + virtio keyboard (typing in the QEMU window / VNC reaches the guest; serial also works)
QEMU_GFX_CMD = qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m $(MEM) -kernel $(KERNEL) -device ramfb -device virtio-keyboard-device -serial stdio -no-reboot $(QEMU_NET_ARGS) $(DISKARGS) -smp 4
DISKARGS_FMT = -drive file=$(DISK),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0
