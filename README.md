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

## Contributing with AI
Yes, you are **allowed and encouraged** to deliver AI-generated code changes. Please verify and document implementation details in [AGENTS.md](AGENTS.md) and [PLAN.md](PLAN.md).
