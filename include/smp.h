/* Symmetric multiprocessing: per-CPU state, the big kernel lock and secondary CPU start-up.
 *
 * Locking model (the same one xv6 uses for its process table): the big kernel lock is held by
 * whichever context is running kernel code.  A process gives the lock to the next context when it
 * switches, so the scheduler never runs unlocked.  An idle CPU releases it while it waits (wfi) for
 * an interrupt.  Kernel mode runs with interrupts masked, so a lock holder cannot be re-entered by
 * its own interrupt.  Architectures without SMP (x86_64 for now) compile the lock away. */
#pragma once
#include "arch.h"
#include <stdint.h>

#define MAX_CPUS 16
#define SGI_RESCHED 1                     /* inter-processor interrupt: "a process became runnable" */

struct proc;
struct cpu {
    struct proc *proc;                    /* process running on this CPU (0 while in the scheduler) */
    uintptr_t sched_ctx;                  /* this CPU's scheduler stack pointer while a process runs */
    uintptr_t aspace;                     /* TTBR0 root this CPU has loaded (aarch64) */
    int id;                               /* 0 = boot CPU */
    int online;
    int idle;                             /* parked waiting for work: an IPI wakes it */
};
extern struct cpu cpus[MAX_CPUS];
extern int ncpus;                         /* the boot CPU plus every secondary CPU that was started */
struct cpu *this_cpu(void);
#define cur (this_cpu()->proc)            /* the process running on this CPU */

void smp_bsp_init(void);                  /* boot CPU: set up its per-CPU pointer (first thing in kmain) */
void klock_enter(void);
void klock_leave(void);
void sched_kick(void);                    /* a process became runnable: wake idle CPUs */
void smp_init(void);                      /* boot CPU: start the secondary CPUs */
void secondary_main(uint64_t id) __attribute__((noreturn));   /* entered from boot.S */

/* board hooks (weak defaults in smp.c; a board opts in by providing them) */
int  board_smp_start(uintptr_t entry_pa); /* start secondaries at entry_pa; returns how many started */
void board_secondary_init(int id);        /* per-CPU interrupt controller and timer setup */
void board_ipi_send(int cpu);
