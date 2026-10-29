/* Kernel-space driver model.
 *
 * A driver is a `struct kdriver` placed in the `.kdrivers` section with KDRIVER(); the linker
 * script collects them, so adding a driver is "drop a .c into drivers/<arch>/" -- no table to edit.
 * drivers_init() runs once from kmain(), ordered by (class, prio): buses first, then input,
 * displays, ...  `probe` answers "is the hardware there?" (0 = yes); `init` brings it up and
 * usually registers with a subsystem (fb_register(), irq_register(), console_register(), ...).
 * Subsystems expose their services to the rest of the system over IPC (see ipc.h). */
#pragma once
#include <stddef.h>

enum drv_class { DRV_BUS, DRV_INPUT, DRV_DISPLAY, DRV_BLOCK, DRV_CHAR, DRV_MISC };
enum drv_state { DRV_IDLE, DRV_ACTIVE, DRV_ABSENT, DRV_FAILED };

struct kdriver {
    const char *name;
    const char *desc;
    int class, prio;               /* lower prio value is initialised first inside a class */
    int (*probe)(void);            /* optional; 0 = hardware present */
    int (*init)(void);             /* optional; 0 = success */
    void (*poll)(void);            /* optional; called from the timer tick once ACTIVE */
    int state;
};
#define KDRIVER(d) static struct kdriver *const __kdrv_##d __attribute__((section(".kdrivers"), used)) = &(d)

void drivers_init(void);
void drivers_poll(void);
void driver_note(const char *name, const char *desc, int class);   /* list a board's built-in driver in /proc/drivers */
size_t drivers_dump(char *buf, size_t n);

/* interrupt routing for drivers (IRQ line numbers are board-defined: PIC IRQn on x86) */
#define NIRQ 32
int  irq_register(unsigned irq, void (*fn)(unsigned irq));
void irq_dispatch(unsigned irq);
