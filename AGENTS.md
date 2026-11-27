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
| **VFS & Storage** | Verified (ext read-only; changes logged) | Ext2/3/4 reader mounts read-only. Mutations (create, write, `mkdirat`, `unlinkat`, `renameat`, `symlinkat`, `chmod`) are recorded in a write-ahead log in the last 4 MiB of the disk (`kernel/persist.c`) and replayed at boot. The journal now uses sector-aligned CHIFLOG2 records and migrates CHIFLOG1 on snapshot. The following checks predate CHIFLOG2. Reset by `make` rebuilding the image. Verified on aarch64 (virtio-blk) and x86_64 (ATA PIO): reboot, rename, unlink, compaction and a hard stop. |
| **Processes** | Earlier single-CPU checks | `fork`, `execve`, `wait4`, `exit`. AArch64 QEMU requests secondary startup through PSCI and counts each secondary after local initialization. x86_64 runs on its boot CPU, including with `smp=on`. Current SMP changes await human checks. [CPU startup and locking](docs/SMP.md) |
| **Virtual Memory** | Source implementation, pending current checks | Boot memory ranges are clipped to the mapped window and PMM capacity. AArch64 and x86_64 now encode user page permissions and reject writable-executable user mappings. [Page protection](docs/PAGE_PROTECTION.md) |
| **Keyboard delivery** | Verified in QEMU (x86_64 PS/2, aarch64 virtio-input); test program not committed | Per-tty `CHI_KBDSETMODE` / `CHI_KBDGETMODE` (`include/uapi/chiffon_abi.h`). `CHI_KBDMODE_DIRECT` sends each key press/release as a `chi_input_event` record straight to the reader: no line buffer, no echo, no ^C/^\ signals. Alt+F1..F4 stay a kernel hotkey. The mode ends when the process that set it exits. Default is `CHI_KBDMODE_TTY` (unchanged line discipline). |

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
- Direct keyboard mode: a program opens `/dev/tty0`, calls `chiffon_ioctl(fd, CHI_KBDSETMODE, (void *)CHI_KBDMODE_DIRECT)`, then reads `struct chi_input_event` records. Type into the QEMU window (or `sendkey` on the monitor) and check that Ctrl-C arrives as a key record and no signal is sent.

## 5. Orchestrator Planning Guide

These instructions apply to any AI model or human coordinating work in this repository. Prefer a capable OpenAI model when one is available; otherwise use the best available model or human expertise. The repository instructions, task boundaries, and acceptance criteria are authoritative and must remain understandable and usable with any model. Do not rely on OpenAI-specific tools, features, or terminology to explain required work.

### Architecture is a project constraint

Chiffon is intentionally a **hybrid kernel**: a microkernel core, selected essential services in kernel space, and peripheral device servers in user space communicating through IPC and explicit capabilities. This is a design requirement, not a temporary midpoint or an invitation to replace the architecture.

- Preserve that division when planning and implementing changes. Keep scheduling, PMM, VMM, IPC, and capability enforcement in the kernel core; keep established essential services such as block, TTY/VT, and base timers in kernel space; keep peripheral services such as the framebuffer server in user space when the current design supports it.
- Do not propose or implement a wholesale move toward either a pure microkernel or a monolithic kernel. In particular, do not move essential kernel services to user space merely to make the system “more microkernel,” and do not absorb peripheral servers into the kernel merely to simplify calls or improve convenience.
- For a change that crosses a boundary, first inspect the current implementation and identify which side owns the behavior, how IPC and capabilities enforce access, and how failures are contained. Extend the existing boundary unless there is a specific, evidence-based reason to change it.
- A boundary change requires a concrete subsystem need, a comparison with a hybrid-preserving option, an impact analysis for both supported architectures and crash isolation, and an update to the architecture documentation. If the request does not clearly authorize an architectural redesign, keep the hybrid model and ask only if an unresolved choice would materially change the requested behavior.

### Plan and coordinate the work

- **Start from the requested outcome.** Inspect the relevant source, documentation, current git state, build targets, and existing verification before editing. Treat this guide as a map, not proof that every implementation detail remains current. Preserve unrelated user changes and identify them before touching overlapping files.
- **Make a concise plan for multi-step work.** State the affected files or subsystems, dependencies and order, architecture considerations, and observable completion criteria. Keep a one-line fix direct.
- **Delegate only independent work when useful and available.** Give each worker a bounded task with expected output, relevant paths, edit permissions, and constraints. Avoid overlapping edits. The orchestrator owns integration, conflict resolution, architecture consistency, final diff review, and the user-facing summary. When delegation is unavailable, use these as sequential checkpoints.
- **Keep scope controlled.** Prefer the smallest change that satisfies the request. Do not turn a local fix into an architectural rewrite. If evidence shows the requested approach conflicts with the hybrid design, explain the conflict and find a hybrid-preserving implementation or clarify the material tradeoff.
- **Plan for both supported architectures where relevant.** Preserve `aarch64` and `x86_64` behavior where practical. For architecture-specific work, state which target is affected and what remains to verify on the other target.
- **Verify proportionally.** Choose the narrowest useful build or runtime check, expanding to both architectures when changes cross shared kernel, driver, IPC, or hardware boundaries. Do not claim checks passed unless they were actually run; clearly report unavailable tools and untested targets.
- **Review before finishing.** Inspect the final diff for unintended edits, stale comments, error-path gaps, violations of the kernel/user-space division, and mismatches with the plan. Summarize what changed, why, files touched, and verification performed.
- **Handle uncertainty with evidence.** If this guide and code disagree, inspect current code and tests, then update documentation if needed. State reasonable assumptions and proceed; ask for clarification only when the unresolved choice would materially change behavior or scope.
