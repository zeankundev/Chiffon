<img width="1396" height="404" alt="image" src="https://github.com/user-attachments/assets/3a527007-f556-43cf-86a2-f10c9f621caa" />

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
To build the `demo` environment, only run this.
```bash
make run      # Standard boot: serial console (UART)
make run-gfx  # Graphical boot: framebuffer window + keyboard
```

To build your own environment, either run any of these commands.
```bash
make run ENV=<my-env>     # Standard boot: serial console (UART)
make run-gfx ENV=<my-env>  # Graphical boot: framebuffer window + keyboard
```

## Bootable x86_64 ISO on Windows

`make iso` creates `build/<environment>/x86_64-<device>/chiffon-x86_64.iso`.
It is a BIOS El Torito CD image, loaded by GRUB via Multiboot 1. The target
embeds the selected environment as an initramfs, so no separate disk image is
needed to boot it.

The project is being used from Windows, but GRUB's ISO authoring tools are
Linux tools. Run the following once from an Ubuntu WSL terminal (not PowerShell):

```bash
sudo apt update
sudo apt install -y grub-pc-bin grub-common xorriso make gcc binutils cpio
cd /mnt/c/Users/Zean/Chiffon
make ENV=demo ARCH=x86_64 iso
```

Test the resulting ISO from the same WSL terminal (install `qemu-system-x86`
if necessary):

```bash
qemu-system-x86_64 -m 256 -cdrom build/demo/x86_64-qemu/chiffon-x86_64.iso -serial stdio -no-reboot
```

This currently supports **x86_64 (x64)** only. Chiffon has no i386/32-bit x86
kernel target yet, so `ARCH=x86` cannot produce an ISO. The ISO itself uses a
standard BIOS El Torito boot image and can boot the 64-bit kernel on a
BIOS-compatible x64 PC or QEMU.

## Contributing with AI
Yes, you are **allowed and encouraged** to deliver AI-generated code changes. Please verify and document implementation details in [AGENTS.md](AGENTS.md) and [PLAN.md](PLAN.md).
