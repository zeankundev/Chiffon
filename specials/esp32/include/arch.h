/* ESP32 arch interface.  Shadows arch/<name>/arch.h for this build (see specials/esp32/Makefile). */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "arch_defs.h"

#define PAGE_SIZE 4096UL
#define P2V(pa) ((void *)(uintptr_t)(pa))  /* identity mapping */
#define V2P(va) ((uintptr_t)(va))

void arch_halt(void) __attribute__((noreturn));
void arch_idle(void);
