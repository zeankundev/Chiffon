# Board knobs for x86_64/generic (all optional). Variables are plain make syntax.
# QEMU_CMD is used by `make run`; $(KERNEL) is the kernel image, $(DISKARGS) the disk options.
MEM ?= 256            # guest RAM in MiB (env chiffon.conf may set MEM=1 for the tiny profile)
# QEMU user networking provides DHCP and NAT through the host's active Internet connection
# (including Wi-Fi) without requiring a host bridge or elevated host permissions.
# NET_MODEL picks the emulated NIC so every driver can be tested without VirtualBox:
#   make run NET_MODEL=e1000        Intel 82540EM, VirtualBox's default adapter
#   make run NET_MODEL=e1000-82545em
#   make run                        virtio-net (default)
NET_MODEL ?= virtio-net-pci,disable-legacy=on
QEMU_NET_ARGS ?= -netdev user,id=n0 -device $(NET_MODEL),netdev=n0
QEMU_CMD = qemu-system-x86_64 -m $(MEM) -kernel $(KERNEL) -nographic -no-reboot $(QEMU_NET_ARGS) $(DISKARGS) -smp 4
# `make run-gfx`: QEMU window with the std VGA (bochs dispi) + PS/2 keyboard; the serial console stays on this terminal
QEMU_GFX_CMD = qemu-system-x86_64 -m $(MEM) -kernel $(KERNEL) -vga std -serial stdio -no-reboot $(QEMU_NET_ARGS) $(DISKARGS) -smp 4
DISKARGS_FMT = -drive file=$(DISK),format=raw,if=ide
