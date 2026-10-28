/* ARMv8 generic timer, EL1 physical timer (PPI 30 on most systems) */
#pragma once
#include <stdint.h>
static uint64_t armtimer_interval;
static inline void armtimer_reload(void) {
    __asm__ volatile("msr cntp_tval_el0,%0" :: "r"(armtimer_interval));
    __asm__ volatile("msr cntp_ctl_el0,%0" :: "r"((uint64_t)1));
}
static inline void armtimer_start(unsigned hz) {
    uint64_t f; __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(f));
    armtimer_interval = f / hz;
    armtimer_reload();
}
