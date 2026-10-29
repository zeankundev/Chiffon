/* PCI configuration space, mechanism #1 (ports 0xCF8/0xCFC).  Enough to find devices by ID. */
#include "kernel.h"
#include "drivers/x86io.h"
#include "drivers/x86_platform.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC
#define MAXDEV 32
static struct pci_dev devs[MAXDEV];
static int ndev;

static inline uint32_t cfg_addr(uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    return 0x80000000u | (uint32_t)b << 16 | (uint32_t)d << 11 | (uint32_t)f << 8 | (off & 0xFC);
}
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off) { outl(CFG_ADDR, cfg_addr(b, d, f, off)); return inl(CFG_DATA); }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v) { outl(CFG_ADDR, cfg_addr(b, d, f, off)); outl(CFG_DATA, v); }

int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out) {
    for (int i = 0; i < ndev; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) { *out = devs[i]; return 0; }
    return -1;
}
uint32_t pci_bar(const struct pci_dev *d, int n) {
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, 0x10 + 4 * n);
    return (v & 1) ? (v & ~3u) : (v & ~0xFu);
}

static int probe(void) {
    outl(CFG_ADDR, 0x80000000u);
    return inl(CFG_ADDR) == 0x80000000u ? 0 : -1;       /* does the address register read back? */
}
static int init(void) {
    for (int bus = 0; bus < 8; bus++)                    /* bus 0 plus the first few bridged buses */
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
                if (ndev < MAXDEV) devs[ndev++] = (struct pci_dev){ bus, dev, fn, id & 0xFFFF, id >> 16, pci_read32(bus, dev, fn, 8) };
                if (!fn && !(pci_read32(bus, dev, fn, 0xC) & 0x800000)) break;      /* not multi-function */
            }
    for (int i = 0; i < ndev; i++)
        kprintf("pci: %02x:%02x.%x %04x:%04x class %04x\n", devs[i].bus, devs[i].dev, devs[i].fn, devs[i].vendor, devs[i].device, devs[i].class_rev >> 16);
    return 0;
}
static struct kdriver drv = { "pci", "PCI config mechanism #1", DRV_BUS, 0, probe, init, 0, 0 };
KDRIVER(drv);
