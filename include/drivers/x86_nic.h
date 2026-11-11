/* Helpers shared by the x86 PCI Ethernet drivers (e1000, virtio-net, ...).
 *
 * Every NIC driver on x86 needs the same few things, and getting them subtly different is how a
 * driver works in QEMU and fails on VirtualBox or a real board.  They live here once:
 *   - DMA addresses of kernel buffers (the image is linked in the higher half),
 *   - a delay that works before interrupts and before any process exist (boot),
 *   - mapping a memory BAR, 32 or 64 bit, as uncacheable device memory,
 *   - the PCI command bits a polled NIC wants,
 *   - the "look again for a NIC once a second" throttle used for hotplug. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "x86_platform.h"

uintptr_t nic_dma_addr(const void *va);                 /* bus address of a kernel static or direct-map buffer */
void      nic_udelay(unsigned us);                      /* busy-wait, valid at any time after board_init() */
int       nic_map_bar(const struct pci_dev *d, int bar, size_t len, volatile void **va);   /* 0 = mapped */
void      nic_pci_prepare(const struct pci_dev *d);     /* memory decode + bus master on, INTx off (we poll) */
int       nic_hotplug_due(uint64_t *next_scan);         /* 1 about once a second; also rescans PCI */
