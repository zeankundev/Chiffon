/* Driver registry, ordered bring-up and IRQ routing.  See include/driver.h. */
#include "kernel.h"
#include "fb.h"

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

/* ---- Hybrid Capabilities and User-space Driver Backend ---- */

int cap_check(struct proc *p, uint32_t type, uintptr_t val1, size_t val2) {
    if (!p) return 0;
    if (p->pid == 1 || p->is_driver_loader) return 1;
    for (int i = 0; i < p->ncaps; i++) {
        if (p->caps[i].type == type) {
            if (type == CHI_CAP_MMIO) {
                if (val1 >= p->caps[i].base && val1 + val2 <= p->caps[i].base + p->caps[i].size) return 1;
            } else if (type == CHI_CAP_IRQ) {
                if (val1 == p->caps[i].base) return 1;
            } else if (type == CHI_CAP_DRIVER || type == CHI_CAP_LOADER) {
                return 1;
            }
        }
    }
    return 0;
}

long cap_grant(int pid, uint32_t type, uintptr_t val1, size_t val2) {
    if (!cur || (!cur->is_driver_loader && cur->pid != 1)) return -EPERM;
    struct proc *target = 0;
    for (int i = 0; i < NPROC; i++) {
        if (ptable[i].state != P_UNUSED && ptable[i].pid == pid) {
            target = &ptable[i];
            break;
        }
    }
    if (!target) return -ESRCH;
    if (type == CHI_CAP_LOADER) {
        target->is_driver_loader = 1;
        return 0;
    }
    if (target->ncaps >= MAX_PROC_CAPS) return -ENOMEM;
    target->caps[target->ncaps++] = (struct proc_cap){ .type = type, .base = val1, .size = val2 };
    return 0;
}

long dev_mmap_phys(uintptr_t phys, size_t len) {
    if (!cur) return -EINVAL;
    if (!cap_check(cur, CHI_CAP_MMIO, phys, len)) return -EPERM;
    if (!len) return -EINVAL;
    uintptr_t p_aligned = phys & ~(PAGE_SIZE - 1);
    size_t l_aligned = ((phys + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - p_aligned;
    if (arch_map_device(p_aligned, l_aligned)) return -EIO;
    if (cur->mm.mmap_next < MMAP_BASE) cur->mm.mmap_next = MMAP_BASE;
    uintptr_t va = cur->mm.mmap_next;
    if (va + l_aligned > USER_STACK_TOP - STACK_SIZE * 8) return -ENOMEM;
    cur->mm.mmap_next += l_aligned;
    int r = mm_map_dev(&cur->mm, va, l_aligned, p_aligned);
    if (r) return r;
    return (long)(va + (phys & (PAGE_SIZE - 1)));
}

static volatile int irq_pending[NIRQ];
static int irq_owner_pid[NIRQ];

static void user_irq_handler(unsigned irq) {
    if (irq < NIRQ) {
        irq_pending[irq] = 1;
        wakeup((void *)&irq_pending[irq]);
    }
}

long dev_irq_claim(unsigned irq) {
    if (irq >= NIRQ) return -EINVAL;
    if (!cur || !cap_check(cur, CHI_CAP_IRQ, irq, 0)) return -EPERM;
    if (irq_owner_pid[irq] && irq_owner_pid[irq] != cur->pid) return -EBUSY;
    int r = irq_register(irq, user_irq_handler);
    if (r && irq_owner_pid[irq] != cur->pid) return r;
    irq_owner_pid[irq] = cur->pid;
    return 0;
}

long dev_irq_wait(unsigned irq) {
    if (irq >= NIRQ) return -EINVAL;
    if (!cur || !cap_check(cur, CHI_CAP_IRQ, irq, 0)) return -EPERM;
    if (irq_owner_pid[irq] != cur->pid) return -EPERM;
    while (!irq_pending[irq]) {
        if (cur->killed) return -EINTR;
        sleep_on((void *)&irq_pending[irq]);
    }
    irq_pending[irq] = 0;
    return 0;
}

void dev_driver_cleanup(struct proc *p) {
    if (!p) return;
    for (unsigned i = 0; i < NIRQ; i++) {
        if (irq_owner_pid[i] == p->pid) {
            irq_owner_pid[i] = 0;
            irq_fn[i] = 0;
            irq_pending[i] = 0;
        }
    }
}

static uintptr_t virtual_fb_phys;
static size_t virtual_fb_size;

long dev_info_query(int type, void *uinfo) {
    if (!cur) return -EINVAL;
    switch (type) {
    case DEV_TYPE_FB: {
        if (!uok((uintptr_t)uinfo, sizeof(struct chi_dev_fb_info))) return -EFAULT;
        struct chi_dev_fb_info info;
        memset(&info, 0, sizeof info);
        struct fb_device *d = fb_primary();
        if (d) {
            info.phys = d->info.phys;
            info.size = d->info.size;
            info.width = d->info.width;
            info.height = d->info.height;
            info.pitch = d->info.pitch;
            info.bpp = d->info.bpp;
        } else {
            if (!virtual_fb_phys) {
                uint32_t w = 640, h = 480, pitch = w * 4;
                size_t sz = (size_t)pitch * h;
                uint8_t *vbuf = kmalloc(sz + PAGE_SIZE);
                if (vbuf) {
                    virtual_fb_phys = V2P((uintptr_t)vbuf);
                    virtual_fb_size = sz;
                }
            }
            info.phys = virtual_fb_phys;
            info.size = virtual_fb_size;
            info.width = 640;
            info.height = 480;
            info.pitch = 640 * 4;
            info.bpp = 32;
        }
        memcpy(uinfo, &info, sizeof info);
        return 0;
    }
    case DEV_TYPE_UART: {
        if (!uok((uintptr_t)uinfo, sizeof(struct chi_dev_uart_info))) return -EFAULT;
        struct chi_dev_uart_info info;
        memset(&info, 0, sizeof info);
#if defined(__aarch64__)
        info.phys = 0x09000000UL;
        info.size = 0x1000;
        info.irq = 33;
#else
        info.phys = 0x3F8;
        info.size = 8;
        info.irq = 4;
#endif
        memcpy(uinfo, &info, sizeof info);
        return 0;
    }
    }
    return -ENODEV;
}
