/* Interfaces between the x86 PC drivers in drivers/x86_64/ and the generic board. */
#pragma once
#include <stdint.h>

struct pci_dev { uint8_t bus, dev, fn; uint16_t vendor, device; uint32_t class_rev; };
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
int      pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);       /* 0 = found */
uint32_t pci_bar(const struct pci_dev *d, int n);                               /* BAR n, flag bits masked off */

/* the board hands the Multiboot info block to this before anything else is initialised */
void mb_fb_boot_info(const uint32_t *mbi);
