/* Driver registry, ordered bring-up and IRQ routing.  See include/driver.h. */
#include "kernel.h"

extern struct kdriver *const __kdrivers_start[], *const __kdrivers_end[];

#define MAXDRV 32
static struct kdriver *order[MAXDRV];
static int ndrv;
static struct kdriver builtin[8];
static int nbuiltin;

static const char *class_name(int c) {
    static const char *n[] = { "bus", "input", "display", "block", "char", "misc" };
    return c >= 0 && c < 6 ? n[c] : "?";
}
static const char *state_name(int s) {
    static const char *n[] = { "idle", "active", "absent", "failed" };
    return s >= 0 && s < 4 ? n[s] : "?";
}

void driver_note(const char *name, const char *desc, int class) {
    if (nbuiltin >= 8) return;
    struct kdriver *d = &builtin[nbuiltin++];
    d->name = name; d->desc = desc; d->class = class; d->state = DRV_ACTIVE;
}

void drivers_init(void) {
    for (struct kdriver *const *p = __kdrivers_start; p < __kdrivers_end && ndrv < MAXDRV; p++) order[ndrv++] = *p;
    for (int i = 1; i < ndrv; i++) {                                   /* insertion sort by (class, prio) */
        struct kdriver *d = order[i]; int j = i - 1;
        while (j >= 0 && (order[j]->class * 1000 + order[j]->prio) > (d->class * 1000 + d->prio)) { order[j + 1] = order[j]; j--; }
        order[j + 1] = d;
    }
    for (int i = 0; i < ndrv; i++) {
        struct kdriver *d = order[i];
        if (d->probe && d->probe()) { d->state = DRV_ABSENT; continue; }
        if (d->init && d->init()) { d->state = DRV_FAILED; kprintf("driver: %s failed to initialise\n", d->name); continue; }
        d->state = DRV_ACTIVE;
        kprintf("driver: %s (%s)\n", d->name, d->desc);
    }
}

void drivers_poll(void) {
    for (int i = 0; i < ndrv; i++)
        if (order[i]->state == DRV_ACTIVE && order[i]->poll) order[i]->poll();
}

size_t drivers_dump(char *buf, size_t n) {
    size_t o = 0;
    o += ksnprintf(buf + o, n - o, "%-12s %-8s %-7s %s\n", "NAME", "CLASS", "STATE", "DESCRIPTION");
    for (int i = 0; i < nbuiltin && o < n; i++)
        o += ksnprintf(buf + o, n - o, "%-12s %-8s %-7s %s\n", builtin[i].name, class_name(builtin[i].class), state_name(builtin[i].state), builtin[i].desc);
    for (int i = 0; i < ndrv && o < n; i++)
        o += ksnprintf(buf + o, n - o, "%-12s %-8s %-7s %s\n", order[i]->name, class_name(order[i]->class), state_name(order[i]->state), order[i]->desc);
    return o < n ? o : n - 1;
}

/* ---- IRQ routing ---- */
static void (*irq_fn[NIRQ])(unsigned);
int irq_register(unsigned irq, void (*fn)(unsigned)) {
    if (irq >= NIRQ || irq_fn[irq]) return -EINVAL;
    irq_fn[irq] = fn;
    return 0;
}
void irq_dispatch(unsigned irq) { if (irq < NIRQ && irq_fn[irq]) irq_fn[irq](irq); }
