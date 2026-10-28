#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arch_defs.h"

#define PAGE_SIZE 4096UL
#define P2V(pa) ((void *)((uintptr_t)(pa) + DMAP_BASE))
#define V2P(va) ((uintptr_t)(va) - DMAP_BASE)

void arch_init(void);
void arch_idle(void);
void arch_halt(void) __attribute__((noreturn));
uintptr_t arch_kernel_end(void);
int arch_map_device(uintptr_t pa, size_t len);

uintptr_t arch_kernel_root(void);
uintptr_t arch_aspace_new(void);
void arch_aspace_free(uintptr_t aspace);
void arch_aspace_switch(uintptr_t aspace);
int arch_map(uintptr_t root, uintptr_t va, uintptr_t pa);
uintptr_t arch_unmap(uintptr_t root, uintptr_t va);
uintptr_t arch_translate(uintptr_t root, uintptr_t va);
void arch_sync_icache(void *va, size_t n);

void arch_set_kstack(uintptr_t top);
void arch_switch(uintptr_t *old_sp, uintptr_t new_sp);
struct trapframe *arch_kstack_setup(uintptr_t kstack_top, uintptr_t *ksp, const struct trapframe *src);
void arch_tf_user(struct trapframe *tf, uintptr_t entry, uintptr_t sp);
void arch_uctx_init(struct arch_uctx *u);
void arch_uctx_save(struct arch_uctx *u);
void arch_uctx_restore(struct arch_uctx *u);
