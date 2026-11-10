# Board knobs for x86_64/generic (all optional). Variables are plain make syntax.
# QEMU_CMD is used by `make run`; $(KERNEL) is the kernel image, $(DISKARGS) the disk options.
MEM ?= 256            # guest RAM in MiB (env chiffon.conf may set MEM=1 for the tiny profile)
QEMU_CMD = qemu-system-x86_64 -m $(MEM) -kernel $(KERNEL) -nographic -no-reboot $(DISKARGS) -smp 4
# `make run-gfx`: QEMU window with the std VGA (bochs dispi) + PS/2 keyboard; the serial console stays on this terminal
QEMU_GFX_CMD = qemu-system-x86_64 -m $(MEM) -kernel $(KERNEL) -vga std -serial stdio -no-reboot $(DISKARGS) -smp 4
DISKARGS_FMT = -drive file=$(DISK),format=raw,if=ide
