# Board knobs for aarch64 (all optional; plain make syntax).
#   LOAD_ADDR            physical address the kernel is loaded/linked at
#   BOOT_DEVICE_GB_MASK  bit n set => physical GiB n is mapped as device memory at boot (rest = RAM)
#   QEMU_CMD / DISKARGS_FMT  used by `make run`; leave out if the board cannot run under QEMU
LOAD_ADDR = 0x40080000
BOOT_DEVICE_GB_MASK = 0x1
QEMU_CMD = qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256 -kernel $(KERNEL) -nographic -no-reboot $(DISKARGS)
# `make run-gfx`: ramfb framebuffer in a QEMU window; input stays on the serial console (this terminal)
QEMU_GFX_CMD = qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256 -kernel $(KERNEL) -device ramfb -serial stdio -no-reboot $(DISKARGS)
DISKARGS_FMT = -drive file=$(DISK),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0
