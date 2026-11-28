# Chiffon Production Readiness Roadmap

This is a readiness checklist, not a feature wish list. Chiffon is **not production-ready** until every **P0 release gate** is completed, independently reviewed, and tested on supported hardware. Mark an item complete only with a committed test or other repeatable evidence. A QEMU boot or a feature existing in source is not sufficient proof of safety.

## Current baseline

The repository describes Chiffon as a Unix-like hybrid kernel for AArch64 and x86_64. Current project notes report IPC, user-space framebuffer service crash handling, read-only ext filesystem access, and a write-ahead persistence log. The kernel now has a global big kernel lock, per-CPU state, AArch64 PSCI startup, and x86_64 boot-CPU-only execution. These features do **not** establish full SMP safety or production security. The supplied project audit reports single-CPU process verification, with current verification pending. See [CPU startup](docs/SMP.md), [the threat model](docs/THREAT_MODEL.md), [device trust](docs/DMA.md), [page permissions](docs/PAGE_PROTECTION.md), and [persistence semantics](docs/STORAGE_DURABILITY.md) for source-level changes and outstanding checks.

## P0 — Required before any production claim

### Memory safety and privilege boundaries

- [x] Establish a threat model and supported security boundary: user processes, kernel, device servers, DMA-capable devices, boot chain, and physical attacker assumptions. [Threat model at `e0ba2aa`](docs/THREAT_MODEL.md) documents the single-vCPU, trusted-host QEMU scope for AArch64 virt and x86_64 PC.
- [ ] Audit every user-pointer path (syscalls, IPC, `ioctl`, process loading, device mapping): validate lengths, overflow, alignment, permissions, and mapping lifetime; copy data safely so a process cannot change it during validation/use.
- [ ] Audit every parser and binary boundary (ELF, initramfs, FDT/DTB, Multiboot data, filesystem metadata, network packets, USB descriptors, virtio rings) for bounds checks and integer overflow.
- [ ] Make page permissions enforceable: user/kernel separation, read-only executable text, non-executable data and stacks, guard pages, and no writable-executable mappings by default. Document architecture-specific protections and fallback behavior.
- [ ] Audit allocation, reference counts, object lifetime, and error unwinding across process exit, `fork`/`exec`, IPC teardown, VFS, capabilities, and driver restart. Add hardened allocator checks for double-free, use-after-free, and corrupted metadata where practical.
- [ ] Remove or document unsafe interfaces; enforce least privilege for capability grant, MMIO mapping, IRQ ownership, process control, and service registration. Test that unauthorized callers are denied.
- [ ] Add kernel hardening options appropriate to each architecture (stack protection, address-space randomization where feasible, control-flow protections where supported, kernel address hiding, and hardened compiler/linker flags). Record any protection that cannot yet be provided.
- [ ] Review boot and update trust: define secure boot/signing, rollback protection, and recovery strategy for any real deployment that needs resistance to modified boot images.
- [ ] Run static analysis, compiler sanitizers on host-testable code, fuzzing for parsers/syscalls, and a focused security review. Triage and fix all critical/high findings before release.

### DMA isolation and device security

- [ ] Implement and validate IOMMU support for supported platforms (Intel VT-d/AMD-Vi on x86_64; SMMU where present on AArch64), including safe firmware-table discovery and fault handling.
- [ ] Put devices behind isolated DMA domains before allowing bus mastering; restrict each device to explicitly mapped buffers, revoke mappings on teardown, and test malicious/out-of-range DMA behavior.
- [x] Until isolation is implemented, disable bus mastering for untrusted devices or clearly limit support to trusted/emulated devices; do not claim driver crash isolation protects against DMA. [DMA policy at `26ae17d`](docs/DMA.md) limits both targets to trusted devices and host backends. This is an operator-enforced deployment restriction.
- [ ] Audit DMA address translation, cache coherency, barriers, ring ownership, and buffer lifetime in all virtio, USB, network, storage, and display drivers.
- [ ] Validate MMIO/IRQ capabilities against device ownership and address ranges; prevent overlapping or out-of-range grants and stale capabilities after process exit/restart.

### SMP correctness and concurrency

- [ ] Reconcile SMP implementation and claims: document which architectures/boards actually start multiple CPUs and which remain single CPU by default. Fix `this_cpu()` and per-CPU setup so CPU identity is reliable on every supported target.
- [ ] Audit the big kernel lock handoff, interrupt masking, exception entry/return, scheduler, sleep/wakeup, and idle/IPI paths for deadlocks, lost wakeups, reentrancy, and lock leaks.
- [ ] Protect or make per-CPU all shared state: PMM/VMM, process tables, address spaces and TLBs, IPC ports, capability tables, VFS and persistence, TTY/input, timers, driver state, and network stacks. Define lock order and IRQ-safe rules.
- [ ] Add TLB shootdown and address-space switch coordination before concurrent address-space use. Ensure process memory is not freed while another CPU can still access it.
- [ ] Validate atomic operations and memory ordering for both architectures; replace ad hoc busy-wait locks (including network locks) with correct primitives and bounded/fair behavior where needed.
- [ ] Add race and stress tests for CPU hot paths, process creation/exit, IPC teardown, storage sync, and interrupt load. Test 1, 2, and maximum supported CPUs repeatedly on QEMU and available real hardware.
- [ ] Define behavior for CPU startup failure, CPU shutdown, and partial CPU availability. Do not advertise a CPU count above the number successfully initialized and tested.

### Boot, memory, and reliability

- [ ] Parse firmware/boot memory maps and reserve kernel, bootloader data, MMIO, firmware, and initramfs ranges correctly; reject malformed or overlapping maps safely.
- [ ] Make PMM/VMM capacity follow detected usable memory rather than a silent fixed ceiling. Test low-memory operation, fragmentation, allocation exhaustion, and free/reuse across supported RAM sizes.
- [ ] Test page table isolation and permissions, mapping/unmapping, copy-on-write or documented non-support, guard pages, exhaustion, and invalid user faults on both architectures.
- [ ] Add watchdog or bounded recovery for stuck devices and kernel services; define panic, reboot, crash log, and safe-mode behavior. A panic must not silently corrupt persistent data.
- [ ] Verify timer monotonicity, wall-clock setup, interrupt routing, power-off/reset, and recovery from missing or malformed firmware/device information.
- [ ] Define supported hardware and boot configurations, then test each listed configuration on real hardware or a justified emulator. Do not equate QEMU-only support with general PC/ARM support.

### Storage and data integrity

- [ ] Specify the persistence log’s durability guarantees and failure model. Verify ordering, torn writes, CRC handling, log-full behavior, compaction interruption, flush/barrier semantics, and recovery after forced power loss.
- [ ] Ensure syscall success means the promised data is durable; propagate all write, flush, and metadata errors instead of logging and continuing as if saved.
- [ ] Add filesystem consistency checks and recovery tooling. Define supported filesystem features precisely; keep unsupported ext journaling/features read-only and reject unsafe mounts.
- [ ] Test disk-full conditions, bad blocks/device errors, concurrent writers, abrupt reset at every persistence stage, and repeated recovery. Keep backups/recovery guidance for deployment.
- [ ] Review permission, ownership, link, rename, and mount semantics against the chosen POSIX target. Prevent path traversal and cross-mount escape.

### Release evidence and operations

- [ ] Create automated host tests, in-kernel self-tests, syscall ABI checks, fuzz targets, and QEMU boot/integration tests for both architectures. Tests must have timeouts and reliable pass/fail markers.
- [ ] Add CI for clean builds, warnings-as-errors, static analysis, tests, and supported QEMU configurations. Preserve logs and artifacts for failures.
- [ ] Establish a release process: versioned ABI, changelog, reproducible build inputs, image checksums/signatures as required, known-issue list, upgrade/recovery instructions, and rollback procedure.
- [ ] Add diagnostics suitable for deployment: persistent crash reason, useful logs, health/status reporting, and a way to recover without losing user data.
- [ ] Complete an independent code/security review and close all release-blocking findings. Publish the exact tested architectures, boards, device set, and limits.

## P1 — Required for a practical general-purpose Unix-like system

### POSIX and application compatibility

- [ ] Define the intended POSIX/Linux compatibility target and maintain a syscall/ABI support table generated from the implementation.
- [ ] Complete and test signal masks, handlers and return, process groups/sessions, terminal job control, termios behavior, file descriptor semantics, pipes, poll/select, and error codes.
- [ ] Implement threads (`clone` semantics), futexes, robust thread exit, and synchronization semantics before claiming multithreaded application support.
- [ ] Add local sockets and then a documented network socket API; validate timeout, nonblocking, partial I/O, and shutdown behavior.
- [ ] Build, run, and test a useful software set (BusyBox shell, core tools, editor, compiler, and test suite) on both architectures. Document unsupported calls and programs.
- [ ] Raise process, file, memory, and IPC limits only after resource accounting, limits, and exhaustion handling are in place.

### C library and self-hosting

- [ ] Port a maintained libc, with glibc as a long-term compatibility target if resource and ABI scope justify it; first establish a complete, tested libc baseline such as musl if that better fits the project.
- [ ] Implement the required kernel ABI, TLS/thread support, signals, clocks, mmap, and filesystem semantics for that libc. Pass its test suite and application tests.
- [ ] Provide a complete native toolchain (compiler, assembler, linker, binutils or equivalent, headers, libc, build tools, shell, utilities) with reproducible package/build recipes.
- [ ] Build the kernel and userland inside Chiffon from source, then use the resulting compiler to rebuild them again. Verify outputs and document bootstrap stages.
- [ ] Add package installation/removal, dependency tracking, system configuration, and safe upgrade/rollback suitable for the intended users.

### System services and desktop

- [ ] Define a service manager contract (dependency ordering, supervision, restart policy, logs, shutdown, permissions, and service isolation). Implement a systemd-compatible environment only if Linux/systemd compatibility is a project goal; otherwise document the Chiffon service manager target.
- [ ] Implement stable graphics/input APIs, accelerated or usable display drivers, windowing/compositor, fonts, clipboard, and desktop settings.
- [ ] Complete audio, USB input/storage, networking, power management, battery, display modes, and common device-driver support for the actual supported hardware list.
- [ ] Add user accounts, login/session management, access control, configuration, and recovery flows for desktop use.
- [ ] Validate the desktop with realistic applications and sustained workloads, not just a framebuffer demo.

### Storage and networking breadth

- [ ] Decide whether ext is writable and journaled or retain the current write-ahead overlay design; document limits and provide recovery/backup tools.
- [ ] Add a native filesystem or robust writable filesystem with permissions, quotas/limits, atomic rename, crash recovery, and large-file testing.
- [ ] Add network protocol coverage, DNS behavior, firewall policy, network configuration, and secure remote access only with a reviewed security model.

## P2 — Product polish and long-term maintenance

- [ ] Define stable release support windows, ABI policy, deprecation rules, and architecture/board support tiers.
- [ ] Add performance and power benchmarks with baselines for boot, IPC, scheduling, storage, networking, graphics, and memory use.
- [ ] Improve portability and diagnostics, developer documentation, API references, driver authoring guide, and contribution/review workflow.
- [ ] Add localization, accessibility, installer, upgrade UI, and desktop polish if Chiffon targets end users.
- [ ] Track upstream security fixes for imported code and dependencies; schedule regular reviews and release updates.

## Production release gate

Do not call Chiffon production-ready until:

- [ ] Every P0 checklist item is complete or explicitly shown not to apply by the published threat model and supported hardware scope.
- [ ] Critical and high severity security and data-loss issues are fixed, with independent review evidence.
- [ ] Automated test and CI results pass for every advertised architecture and configuration, including SMP stress and storage power-loss recovery where supported.
- [ ] The published compatibility, hardware, filesystem, memory, CPU, and security limits match repeatable test results.
- [ ] A user can install/boot, update, diagnose failures, recover, and restore data using documented procedures.
- [ ] Project maintainers commit to handling security reports and shipping fixes for the supported release period.

## Evidence rule

For each checked box, record the test name or artifact, commit, architecture/device, configuration, and result in the relevant documentation or CI. Keep **implemented**, **tested**, and **supported** as separate claims. Revisit this roadmap whenever kernel security boundaries or supported hardware change.
