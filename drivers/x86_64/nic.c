/* Shared helpers for the x86 PCI Ethernet drivers.  See include/drivers/x86_nic.h. */
#include "kernel.h"
#include "drivers/x86io.h"
#include "drivers/x86_nic.h"

#define KERNEL_VMA 0xFFFFFFFF80000000UL

uintptr_t nic_dma_addr(const void *p) {
    uintptr_t va = (uintptr_t)p;
    if (va >= KERNEL_VMA) return va - KERNEL_VMA;        /* statics in the higher-half kernel image */
    return V2P(va);                                      /* direct-map allocations */
}

/* Timing source: the 8254 PIT counter (always running, 1.193182 MHz, programmed by board_init()).
 * The tick counter cannot be used because it does not advance with interrupts off or before the
 * scheduler runs.  If the counter turns out not to move (an unusual machine), fall back to ISA
 * port 0x80 writes, which take about a microsecond each. */
#define PIT_HZ      1193182u
#define PIT_RELOAD  (PIT_HZ / HZ)

static uint16_t pit_read(void) {
    outb(0x43, 0x00);                                    /* latch channel 0 */
    uint8_t lo = inb(0x40);
    uint8_t hi = inb(0x40);
    return (uint16_t)(lo | (uint16_t)hi << 8);
}

static void delay_port80(unsigned us) { for (unsigned i = 0; i < us; i++) outb(0x80, 0); }

void nic_udelay(unsigned us) {
    if (!us) return;
    uint64_t want = (uint64_t)us * PIT_HZ / 1000000u + 1;       /* PIT counts to wait for */
    uint64_t seen = 0;
    uint16_t last = pit_read();
    unsigned stuck = 0;
    while (seen < want) {
        uint16_t now = pit_read();
        if (now == last) {
            if (++stuck > 100000u) { delay_port80(us); return; }   /* counter is not running */
            continue;
        }
        stuck = 0;
        if (last >= now) seen += (uint16_t)(last - now);
        else seen += (uint64_t)last + (PIT_RELOAD > now ? PIT_RELOAD - now : 0);   /* the counter reloaded */
        last = now;
    }
}

int nic_map_bar(const struct pci_dev *d, int bar, size_t len, volatile void **va) {
    if (bar < 0 || bar > 5 || !len) return -1;
    uint8_t off = (uint8_t)(0x10 + 4 * bar);
    uint32_t raw = pci_read32(d->bus, d->dev, d->fn, off);
    if (raw & 1) return -1;                              /* an I/O port BAR, not memory */
    uint64_t pa = raw & ~0xFull;
    if (((raw >> 1) & 3) == 2) {                         /* 64-bit BAR: the next dword holds the high half */
        if (bar == 5) return -1;
        pa |= (uint64_t)pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4)) << 32;
    }
    if (!pa) return -1;                                  /* firmware never assigned it */
    uint64_t end = pa + len;
    if (end < pa || end > (1ULL << 39)) return -1;       /* outside the 512 GiB direct map */
    uintptr_t page = (uintptr_t)(pa & ~0xFFFull);
    uintptr_t span = (uintptr_t)(((end + 0xFFF) & ~0xFFFull) - page);
    if (arch_map_device(page, span)) return -1;
    *va = (volatile void *)P2V((uintptr_t)pa);
    return 0;
}

void nic_pci_prepare(const struct pci_dev *d) {
    pci_enable(d, 0x2 | 0x4 | 0x400);                    /* memory decode, bus master, INTx disable */
}

int nic_hotplug_due(uint64_t *next_scan) {
    if (ticks < *next_scan) return 0;
    *next_scan = ticks + HZ;
    pci_rescan();
    return 1;
}
