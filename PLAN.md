# PLAN.md — Chiffon roadmap and status audit

Baseline: commit `e948fe2` ("Fix keyboard stuffs on aarch64"), reviewed 2026-10-29.
Every status below was checked against the source. Where something could not be verified it is marked **unverified**.

## 1. Goal status

| Goal | Claimed by repo? | Actual state | Evidence | Gap |
|---|---|---|---|---|
| True hybrid kernel | Yes. README: "hybrid in the classic sense". | Monolithic today. Scheduler, MM, VFS, TTY, drivers and all filesystems run in ring 0. The IPC layer exists (syscalls 1000–1006, `kernel/ipc.c`), but no core subsystem depends on a user-space server. | README "Not (yet) done"; `include/uapi/chiffon_abi.h`; `kernel/ipc.c` (183 lines) | Large |
| Partial or workable POSIX | Implied | About 80 syscalls dispatched in `kernel/syscall.c`. Process model is `fork`/`execve`/`wait4` only. `CLONE_VM` and `CLONE_THREAD` return `-ENOSYS` (line 328). No sockets, no futex. `ioctl` exists but the termios coverage is **unverified**. | `kernel/syscall.c` | Medium. Not yet verified against BusyBox |
| Up to 16 cores | Not claimed as done | Not implemented. No atomics, no spinlocks, no IRQ-save primitives. `struct proc *cur` is a single global (`kernel/proc.c:4`). No MADT or LAPIC parsing on x86_64. On aarch64, secondary CPUs are never released: `arch/aarch64/boot.S` only runs `kmain` on the boot CPU. PSCI is used only for `SYSTEM_OFF`. `NPROC` is 32 and there is no `MAX_CPUS`. | `grep` for `lock`, `xchg`, `ldaxr`, `__sync_`, `__atomic`, `sipi`, `madt`, `cpu_on` returns nothing relevant | Very large |
| Up to 128 GB RAM | Not claimed as done | Not implemented. `MAXFRAMES (1u << 20)` = 4 GiB (`kernel/mm.c:3`). Bitmap is 128 KiB static. On x86_64, `arch/x86_64/boot.S` maps only 4 GiB (PD0..PD3) as identity and direct map. The aarch64 direct-map extent is **unverified**. No memory map is parsed for allocation. | `kernel/mm.c`, `arch/x86_64/boot.S:26` | Large |
| Consistent code quality | Partly | Strict warnings are enabled (`-Wall -Wextra`, `-std=gnu11`). There is no test suite, no CI, no formatter config, no static analyzer config. About 6,000 lines of C, headers and assembly across the repo; about 3,200 in `kernel/`. Style is mixed. | `Makefile`; `find` for tests and configs returns nothing | Medium |

## 2. Problems that mislead or can cause bugs

These are verified. Fix them in Phase 0 before adding features.

1. **Architecture name is inconsistent.** README says "hybrid kernel". `make help` says "a small Unix-like microkernel". The code is monolithic. Use one term everywhere. Until Phase 6 is done, use "monolithic kernel with an IPC layer (hybrid goal pending)".
2. **Broken pointer in the Makefile.** The `FS` check error says "Other filesystems need a kernel driver: docs/ADDING-A-FILESYSTEM.md". The `docs/` directory does not exist.
3. **Filesystems look fuller than they are.** The Makefile accepts `FS=ext2|ext3|ext4`. `kernel/ext.c` (194 lines) is a read-only reader. It does not replay the journal and prints `journal needs recovery, showing on-disk state`. Mutating syscalls (`mkdirat`, `unlinkat`, `renameat`, `symlinkat`, `chmod`) act on the in-memory VFS (`kernel/vfs.c`, `vfs_rename`, `node_unlink`). Nothing is written back to disk. This is not documented anywhere.
4. **Syscall tables and dispatch may disagree.** `arch/x86_64/sysnum.h` defines about 85 numbers and `arch/aarch64/sysnum.h` defines about 65. `kernel/syscall.c` has 80 `case` labels, and unmatched calls fall through to `-ENOSYS` (line 541). Some defined numbers are probably never implemented. **Not yet checked by script.** Phase 1 covers this.
5. **Single-CPU assumptions are silent.** Nothing in the code or docs says the kernel is unsafe with more than one CPU. Any future SMP bring-up will corrupt the scheduler, the VFS node table, the TTY and the IPC port table.
6. **Tiny fixed limits.** `NPROC` and `NOFILE` are both 32 (`include/kernel.h:60-61`). These are not bugs, but any POSIX shell pipeline with several jobs can hit them quickly. Raise them only with a memory budget.
7. **Previous `AGENTS.md` was untracked.** It was not committed. Some of its claims were not verified, for example that signal delivery works and that termios is covered. The replacement file only states what was checked.

## 3. Phases

Each phase has an exit criterion. Do not move to the next phase until the current one is met. Each phase must leave both `x86_64` and `aarch64` building and booting in QEMU.

### Phase 0 — True hybrid kernel (microkernel + most drivers in kernel space and some in userspace)
Define "true hybrid kernel" before starting. Proposed definition for this project: **at least one device subsystem runs as a user-space server reached only through IPC, and a crash of that server does not panic the kernel.** Use the framebuffer or console first, because they already have a clean IPC service boundary (`fb`, `console`).
- Add a user-space driver loader on top of IPC. Give each server a capability for its device MMIO and IRQ.
- Measure IPC round-trip latency and record the number in the repo.
- Kill the server in a test and check that the kernel survives and callers get `-EPIPE`.
- **Exit:** the crash test passes, and the latency is recorded in README.

### Phase 1 — Honest baseline (small, first)
- Pick one architecture term and apply it in README, `make help`, and AGENTS.md. aarch64 is highly preferred
- Refactor the entire code to be a true hybrid kernel (microkernel + few kernel space drivers)
- Fix or remove the `docs/ADDING-A-FILESYSTEM.md` reference.
- Document that ext is read-only and that VFS writes are in memory.
- Commit AGENTS.md and PLAN.md.
- Record a boot log for each arch and for `demo` as the reference.
- **Exit:** a new contributor can read README and find no claim the code contradicts.

### Phase 2 — Measurement (blocks everything after it)
- Add `make test`: host-side unit tests for pure code (`kernel/lib.c`, allocator logic in `mm.c` where separable, errno mapping).
- Add an in-kernel self-test mode (boot flag such as `selftest`) that runs IPC, VFS, and fork/exec checks and prints a fixed PASS/FAIL marker.
- Add a QEMU boot test that waits for the marker, with a timeout, for both arches.
- Add a script that compares `sysnum.h` against `syscall.c` and lists defined-but-unimplemented numbers.
- Add CI running the build for both arches and the QEMU test.
- Pick `clang-format` and `cppcheck` (or similar), add their configs, and reformat once in a single commit.
- **Exit:** CI is green on both arches, and the syscall audit output is committed.

### Phase 3 — Locking and per-CPU state (still one CPU)
- Add a spinlock with IRQ save and restore for both arches (`ldaxr/stlxr` on aarch64, `lock cmpxchg` on x86_64).
- Add a per-CPU structure. Put `cur` and the run queue into it. Use `GS` base on x86_64 and `TPIDR_EL1` on aarch64.
- Audit and document every global in `kernel/`: `ptable`, `cur`, `bm`, the VFS node table, TTY and FB state, the keyboard buffer, the IPC port table, block caches.
- Add lock-order rules to AGENTS.md.
- Add a debug mode that asserts a lock is held when a shared structure is touched.
- **Exit:** the self-test passes with the new locks, and every global has an owner and a lock or a note saying why none is needed.

### Phase 4 — SMP bring-up to 16 CPUs
- x86_64: parse RSDP → XSDT → MADT for LAPIC IDs. Start APs with INIT/SIPI and a real-mode trampoline. Give each AP its own stack.
- aarch64: read CPU nodes from the DTB. Start each CPU with PSCI `CPU_ON`, passing a per-CPU stack. Add a `MAX_CPUS` constant, default 16.
- Make the scheduler run on all CPUs. Keep it simple at first, with one run queue per CPU and no load balancing.
- Test with `-smp 16` on both arches.
- **Exit:** a stress test starts 64 processes and each one prints its CPU ID. Each CPU runs at least one process. The test is run under QEMU TCG and, where available, KVM.

### Phase 5 — 128 GB physical memory
- Parse the real memory map: multiboot or UEFI on x86_64, DTB `/memory` on aarch64.
- Replace the flat 4 GiB bitmap with one sized for 128 GiB. That is 2^25 frames, a 4 MiB bitmap. A two-level bitmap or buddy allocator also works.
- Extend the direct map to cover all usable RAM. On x86_64, 128 GiB at 2 MiB pages needs 65,536 PDEs, which is 128 page tables. Verify the aarch64 extent first (open question in section 1).
- Add a test that allocates and frees every frame.
- Raise `NPROC` and `NOFILE` only with an explicit memory budget written in the commit message.
- **Exit:** `make run ENV=... ` with `-m 128G` boots on both arches and the frame-exhaustion test passes. Record whether the run used KVM or TCG.

### Phase 6 — POSIX breadth (to "workable")
- `CLONE_VM` and `CLONE_THREAD` (threads), `futex`, and `set_robust_list` (already dispatched).
- `poll` and `select` across all fd types, including pipes and TTYs.
- Full termios ioctls on the TTY. Verify coverage before claiming it.
- Signal delivery with masks, `sigaction` handlers, and `sigreturn`. Verify with a test program.
- Job control: process groups and sessions (`setpgid` and `setsid` are dispatched; their behavior is unverified).
- Sockets: at least `AF_UNIX` stream sockets.
- Check each addition against `env/busybox` (sh with pipes, background jobs, Ctrl-C, `ls`, `vi`).
- **Exit:** BusyBox `sh` runs pipes and a background job and handles Ctrl-C, under the QEMU test.

### Phase 7 — Writable persistent storage (needed for "workable")
- Add ext2 write support first (simplest). Then decide whether ext4 journaling is needed, or whether `FS=ext4` should be documented as read-only.
- Until this is done, state in README that writes are not persisted.
- **Exit:** a file written in the guest survives a reboot under QEMU.

## 4. Open questions for the maintainer
- Is "true hybrid" the definition in Phase 6, or do you want a stricter one (for example, the scheduler in user space)?
- Is 16 CPUs under QEMU TCG acceptable for CI, or is KVM required for the SMP test?
- Should `FS=ext3`/`ext4` be kept in the Makefile until writes work, or hidden?
- The aarch64 direct-map extent is unverified. Confirm it before Phase 4.

## 5. Definition of done

| Goal | Done when |
|---|---|
| True hybrid | Phase 6 exit criterion met, and README and AGENTS.md say so with evidence |
| POSIX workable | Phase 5 exit criterion met under the BusyBox test |
| 16 cores | Phase 3 exit criterion met on both arches |
| 128 GB | Phase 4 exit criterion met on both arches |
| Code quality | Phase 1 CI green on both arches, and no new warnings or unverified claims |