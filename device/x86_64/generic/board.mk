# Board knobs for x86_64/generic (all optional). Variables are plain make syntax.
# QEMU_CMD is used by `make run`; $(KERNEL) is the kernel image, $(DISKARGS) the disk options.
QEMU_CMD = qemu-system-x86_64 -m 256 -kernel $(KERNEL) -nographic -no-reboot $(DISKARGS)
# `make run-gfx`: QEMU window with the std VGA (bochs dispi) + PS/2 keyboard; the serial console stays on this terminal
QEMU_GFX_CMD = qemu-system-x86_64 -m 256 -kernel $(KERNEL) -vga std -serial stdio -no-reboot $(DISKARGS)
DISKARGS_FMT = -drive file=$(DISK),format=raw,if=ide
