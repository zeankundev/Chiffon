/* ESP32 (Xtensa LX6, little-endian, call0 ABI).  Minimal port: no MMU, no user mode, no interrupts
 * yet, so the process-related layouts are placeholders that keep the shared headers compiling. */
#pragma once
#include <stdint.h>
#define ARCH_XTENSA 1
#define ARCH_NAME "xtensa-esp32"
#define DMAP_BASE 0UL                       /* physical == virtual: there is no MMU */
#define USER_TOP  0UL                       /* no user address space */
#define AUX_HWCAP 0

struct trapframe { uint32_t a[16]; uint32_t pc, ps, sar; };
struct arch_uctx { uint32_t regs[16]; };
