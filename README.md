# Chiffon
A Unix-like **hybrid kernel** (microkernel core with kernel-space essential services and user-space device servers reached through IPC) + operating system for aarch64 and x86_64.

## How it works
Chiffon implements a hybrid kernel model:
- **Core Kernel**: Scheduler, PMM, VMM, IPC message passing, and capability management.
- **Kernel-space drivers**: Core essentials (block devices, TTY/VT console, base timers).
- **User-space device drivers**: Peripheral subsystems (such as the framebuffer display server `/bin/fbserver`) run as isolated user processes in ring 3.
- **Capabilities**: Device servers receive fine-grained hardware capabilities (`CHI_CAP_MMIO` for MMIO mapping, `CHI_CAP_IRQ` for hardware interrupts).
- **Crash Resilience**: If a user-space driver server crashes or is killed, the kernel does not panic. Blocked and subsequent callers receive `-EPIPE` (-32), allowing the driver loader (`/bin/drvloader`) to recover or restart the driver.

## Building & Running
```bash
make run      # Standard boot: serial console (UART)
make run-gfx  # Graphical boot: framebuffer window + keyboard
```

### In-Guest Driver Loader & Tests
Inside the Chiffon shell (`hello-init#`):
```bash
drvloader status      # View running driver services and hardware devices
drvloader bench 1000  # Run IPC round-trip latency benchmark
drvloader test-crash  # Run Phase 0 crash resilience verification
drvloader start fb    # Start the user-space framebuffer server
ipcdemo               # Run IPC client demo (connects to user-space fb service)
```

## System Notes
- **Filesystem**: Ext2/3/4 filesystem reader mounts read-only. Mutating syscalls (`mkdirat`, `unlinkat`, `renameat`, `symlinkat`, `chmod`) operate on the in-memory VFS only and are not persisted to disk.
- **Concurrency**: Kernel currently assumes a single CPU; SMP locking is scheduled for Phase 3/4.
- **Fixed Limits**: `NPROC = 32`, `NOFILE = 32`.

## Raspberry Pi Zero 2W (headless, USB console)
No SD card and no USB-UART adapter. The kernel boots from its embedded initramfs (`env/rpi`), and the console is a CDC-ACM serial port on the board's **DATA** port (use the port next to the PWR port, not PWR):
```bash
make ENV=rpi                                   # kernel + initramfs for rpi-zero2w
make ENV=rpi bootdir FIRMWARE=/path/to/firmware/boot   # stage kernel8.img, config.txt, firmware
sudo rpiboot ...                               # serve the staged directory (see usbboot's README for -d)
```
The Pi shows up as a serial port: `COMx` on Windows (usbser.sys, built in), `/dev/ttyACMn` on Linux, at any baud rate. The driver is in `device/aarch64/rpi-zero2w/usb_dwc2.c`: a polled DWC2 slave-mode gadget, so throughput is limited by the 100 Hz tick.

## Contributing with AI
Yes, you are **allowed and encouraged** to deliver AI-generated code changes. Please verify and document implementation details in [AGENTS.md](AGENTS.md) and [PLAN.md](PLAN.md).
