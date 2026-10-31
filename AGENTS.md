# AGENTS.md — Chiffon Architecture & Contributor Guide

## 1. Architecture Overview
Chiffon is a Unix-like **hybrid kernel** (microkernel core + kernel-space core subsystems + user-space device driver servers reached through IPC).

- **Primary Architecture:** `aarch64` (QEMU `virt`, Cortex-A72, GICv2, PL011 UART). `x86_64` (QEMU PC, Multiboot) supported.
- **Microkernel Core:** Process scheduler, physical frame allocator (PMM), virtual memory management (VMM), IPC subsystem, and capability manager.
- **Hybrid Driver Model:**
  - Essential core services (block, TTY/VT console, base timers) operate in kernel space.
  - Peripheral device subsystems (such as the framebuffer display server `fbserver`) run as user-space servers in ring 3.
  - Drivers interact with hardware through explicit kernel capabilities (`CHI_CAP_MMIO`, `CHI_CAP_IRQ`).
  - Device services are reached only via IPC (`chiffon_ipc_call`).
  - A crash or termination of a user-space driver server does not crash the kernel; callers receive `-EPIPE` (-32) and the driver loader can restart the server.

## 2. Verified Subsystems & State Audit

| Subsystem | State | Details |
|---|---|---|
| **IPC** | Verified | Syscalls 1000–1006. Synchronous & asynchronous message passing. Measured round-trip latency: **88.5 µs** (~11,300 ops/sec). |
| **User-Space Drivers** | Verified | Driver loader (`drvloader`) and user-space framebuffer server (`fbserver`). Maps device MMIO via capabilities. |
| **Crash Resilience** | Verified | Server crash fails blocked and subsequent callers with `-EPIPE`; kernel survives intact. |
| **VFS & Storage** | Verified (ext read-only; changes logged) | Ext2/3/4 reader mounts read-only. Mutations (create, write, `mkdirat`, `unlinkat`, `renameat`, `symlinkat`, `chmod`) are recorded in a write-ahead log in the last 4 MiB of the disk (`kernel/persist.c`) and replayed at boot. Saved on close, `fsync`, `sync` and power-off. Reset by `make` rebuilding the image. Verified on aarch64 (virtio-blk) and x86_64 (ATA PIO): reboot, rename, unlink, compaction and a hard stop. |
| **Processes** | Verified (Single-CPU) | `fork`, `execve`, `wait4`, `exit`. Fixed limits: `NPROC = 32`, `NOFILE = 32`. Single-CPU execution (SMP locking planned for Phase 3/4). |
| **Virtual Memory** | Verified | Flat 4 GiB direct map (`MAXFRAMES = 1 << 20`). User space up to `0x00007FFFFFFFF000`. |

## 3. Capabilities & Syscall Interface

| Syscall | Number | Description |
|---|---|---|
| `CSYS_ipc_create` | 1000 | Create named IPC port |
| `CSYS_ipc_lookup` | 1001 | Lookup named IPC port |
| `CSYS_ipc_close`  | 1002 | Close IPC port |
| `CSYS_ipc_send`   | 1003 | Non-blocking or queued message send |
| `CSYS_ipc_recv`   | 1004 | Receive IPC message |
| `CSYS_ipc_call`   | 1005 | Synchronous request-response call |
| `CSYS_ipc_reply`  | 1006 | Reply to pending request |
| `CSYS_cap_grant`  | 1010 | Grant capability (`CHI_CAP_MMIO`, `CHI_CAP_IRQ`, `CHI_CAP_DRIVER`, `CHI_CAP_LOADER`) |
| `CSYS_dev_map`    | 1011 | Map device MMIO into user address space using capability |
| `CSYS_irq_claim`  | 1012 | Claim hardware IRQ |
| `CSYS_irq_wait`   | 1013 | Block waiting for hardware IRQ |
| `CSYS_dev_info`   | 1014 | Query hardware device info (`DEV_TYPE_FB`, `DEV_TYPE_UART`) |

## 4. Verification Commands

Run under QEMU (`make run`):
- `drvloader status`: View driver subsystem and hardware device status.
- `drvloader bench [n]`: Run IPC round-trip latency benchmark.
- `drvloader test-crash`: Run Phase 0 crash resilience test (verifies `-EPIPE` and kernel survival).
- `drvloader start fb`: Start the user-space framebuffer server.
- `ipcdemo`: Demonstrate client-to-server IPC and graphics operations.
- `fsutil write /tmp/a hello`, then `poweroff` and boot again: `cat /tmp/a` shows the change (persistence check). `fsutil fill 3000` forces a log compaction.
