# CPU startup and kernel serialization

On AArch64 QEMU virt, the device tree lists candidate CPUs and PSCI starts
secondaries with a logical CPU ID in the startup context. CPU 0 is the
boot CPU. Each secondary uses its ID to select a private boot stack and
installs its CPU record in `TPIDR_EL1` before entering the scheduler.
`this_cpu()` reads that register. The board records each CPU's GICv2 target
mask separately from its logical ID.

`ncpus` counts CPUs that have joined the scheduler. The boot CPU starts at
one. A secondary increments the count with the big kernel lock (BKL) held,
after installing its exception vectors and initializing its interrupt
interface and timer. Startup requests can fail or complete in any order.
The online flags identify populated slots even when IDs have gaps. The
scheduler checks all slots when sending a reschedule IPI. The count may
increase while early user processes are running.

On x86_64 only the boot CPU enters the kernel, and `this_cpu()` always
selects logical CPU 0. Firmware discovery may report additional processors.
`smp=on` leaves them parked and the online count remains one. Supporting
secondary CPUs requires a real-mode trampoline, per-CPU entry and context
state, interrupt and timer setup, and a serialization protocol for x86_64
kernel entry and exit. AArch64 boards without a startup hook also run on
the boot CPU only.

## Locking

On AArch64, interrupt masking precedes BKL acquisition. Boot, user
exception entry, and interrupts from an idle CPU acquire the BKL before
accessing shared kernel state. The scheduler and process transfer ownership
across their context switch. The kernel releases the BKL before returning
to user mode or enabling interrupts to idle. A secondary installs its
CPU-local pointer and vector base with interrupts masked before acquiring
the BKL. The x86_64 boot CPU relies on local interrupt masking, so the BKL
operations are empty on that target.

Shared scheduler and online CPU state are accessed under the BKL. Acquire
any subsystem serialization inside it. A subsystem lock whose owner needs
to resume through the scheduler must use a wait path that lets the scheduler
run. Spinning under the BKL for another context that needs it to make
progress deadlocks.

This model allows concurrent user execution on AArch64 and serializes
shared kernel work. Parallel kernel execution, CPU hotplug and recovery
from a CPU failure after it joins the scheduler are outside this model.
Changes that let shared kernel work run outside the BKL require a separate
audit of memory management, process ownership, IPC, and drivers.

## Runtime acceptance

The hybrid integration suites use one vCPU per guest. They cover user-server restart while the kernel retains TTY, storage and networking. They provide no multi-CPU stress measurements or cross-CPU teardown evidence. [Integration evidence](RELEASE_EVIDENCE.md)

- AArch64 QEMU virt with one vCPU should stay at one online CPU.
- With `-smp 4` on the full profile, each initialized secondary should
  announce a distinct logical ID once, and the last announcement should
  report four online CPUs. With the tiny profile, the limit is two.
- When a secondary fails to join, the online count should include only the
  boot CPU and the secondaries that announce successful initialization.
  Runnable processes and timer interrupts should continue to make progress.
- On x86_64 QEMU with several vCPUs and `smp=on`, the kernel should report
  one online CPU and complete boot. Per-CPU queries should return logical
  CPU 0.
