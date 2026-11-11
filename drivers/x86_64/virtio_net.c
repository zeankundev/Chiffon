/* virtio-net over PCI (x86_64), polled. Supports the modern transport (version 2) used by
 * QEMU when booting with BIOS/UEFI. Configuration: -device virtio-net-pci.
 * Two virtqueues (0 = receive, 1 = transmit), four descriptors each, one 1536-byte buffer
 * per descriptor. Only MAC feature negotiated: no checksum or GSO offload. */

#include "kernel.h"
#include "net.h"
#include "drivers/x86_nic.h"

#define VNQ     4
#define VN_BUF  1536
#define VIRTIO_PCI_ID_VENDOR 0x1AF4
#define VIRTIO_PCI_ID_NET_MODERN 0x1041 /* Virtio 1.0 network device (current QEMU default) */
#define VIRTIO_PCI_ID_NET_LEGACY 0x1000 /* Transitional/legacy network device */
#define VIRTIO_F_MAC         (1u << 5)
/* VIRTIO_F_VERSION_1 is bit 0 of feature word 1, checked directly in init() */

#define PCI_CAP_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_DEVICE 4

/* Virtio PCI common configuration structure. */
#define VIRTIO_PCI_COMMON_DFSELECT   0x00
#define VIRTIO_PCI_COMMON_DF         0x04
#define VIRTIO_PCI_COMMON_GFSELECT   0x08
#define VIRTIO_PCI_COMMON_GF         0x0C
#define VIRTIO_PCI_COMMON_NUMQ       0x12
#define VIRTIO_PCI_COMMON_STATUS     0x14
#define VIRTIO_PCI_COMMON_CFGGENERATION 0x15
#define VIRTIO_PCI_COMMON_QSEL       0x16
#define VIRTIO_PCI_COMMON_QSIZE      0x18
#define VIRTIO_PCI_COMMON_QMSIX_VECTOR 0x1A
#define VIRTIO_PCI_COMMON_QENABLE    0x1C
#define VIRTIO_PCI_COMMON_QNOTIFY_OFF 0x1E
#define VIRTIO_PCI_COMMON_QDESCTBL   0x20
#define VIRTIO_PCI_COMMON_QAVAIL     0x28
#define VIRTIO_PCI_COMMON_QUSED      0x30

#define VIRTIO_PCI_STATUS_ACKNOWLEDGE 1
#define VIRTIO_PCI_STATUS_DRIVER 2
#define VIRTIO_PCI_STATUS_DRIVER_OK 4
#define VIRTIO_PCI_STATUS_FEATURES_OK 8

struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vqueue {
	struct vdesc desc[VNQ];
	uint16_t avail_flags, avail_idx, avail_ring[VNQ];
	uint8_t pad[4096 - 64 - 4 - 2 * VNQ];
	uint16_t used_flags, used_idx;
	struct { uint32_t id, len; } used_ring[VNQ];
} __attribute__((aligned(4096)));

static struct vqueue vq[2];
static uint8_t vbuf[2 * VNQ][VN_BUF] __attribute__((aligned(16)));
static struct pci_dev pdev;
static uintptr_t bar_common, bar_notify, bar_device;
static uint32_t common_len, notify_len, device_len, notify_mult;
static uintptr_t notify_queue[2];
static uint16_t rx_av, rx_used, tx_av, tx_used;
static struct netdev nd;
static uint8_t nd_mac[6];
static int device_attached = 0; /* track if device has been successfully attached */

static int init(void);

static inline uint8_t read_u8(uintptr_t addr) {
	return *(volatile uint8_t *)addr;
}

static inline uint16_t read_u16(uintptr_t addr) {
	return *(volatile uint16_t *)addr;
}

static inline uint32_t read_u32(uintptr_t addr) {
	return *(volatile uint32_t *)addr;
}

static inline uint64_t read_u64(uintptr_t addr) {
	return *(volatile uint64_t *)addr;
}

static inline void write_u8(uintptr_t addr, uint8_t v) {
	*(volatile uint8_t *)addr = v;
}

static inline void write_u16(uintptr_t addr, uint16_t v) {
	*(volatile uint16_t *)addr = v;
}

static inline void write_u32(uintptr_t addr, uint32_t v) {
	*(volatile uint32_t *)addr = v;
}

static inline void write_u64(uintptr_t addr, uint64_t v) {
	*(volatile uint64_t *)addr = v;
}

static void mfence(void) {
	__asm__ volatile("mfence" ::: "memory");
}

/* Kernel globals live in the higher-half image, not at the direct-map alias (shared helper). */
static uintptr_t dma_addr(uintptr_t va) { return nic_dma_addr((const void *)va); }

static uint8_t pci_cfg8(uint8_t off) {
	uint32_t v = pci_read32(pdev.bus, pdev.dev, pdev.fn, off & ~3u);
	return (uint8_t)(v >> ((off & 3u) * 8));
}

static uint16_t pci_cfg16(uint8_t off) {
	uint32_t v = pci_read32(pdev.bus, pdev.dev, pdev.fn, off & ~3u);
	return (uint16_t)(v >> ((off & 2u) * 8));
}

static uint32_t pci_cfg32(uint8_t off) {
	return pci_read32(pdev.bus, pdev.dev, pdev.fn, off);
}

/* Resolve the virtio PCI vendor capabilities instead of assuming fixed BARs. */
static int map_region(uint8_t bar, uint32_t off, uint32_t len, uintptr_t *va) {
	if (bar > 5 || !len) return -1;
	uint32_t raw_bar = pci_read32(pdev.bus, pdev.dev, pdev.fn, (uint8_t)(0x10 + bar * 4));
	if ((raw_bar & 1) || !(raw_bar & ~0xFu)) return -1; /* modern transport is MMIO */
	uint64_t phys = (uint64_t)pci_bar(&pdev, bar) + off;
	uint64_t end = phys + len;
	if (end > 0x100000000ULL || end < phys) return -1;
	uintptr_t page = (uintptr_t)phys & ~(uintptr_t)0xFFF;
	uintptr_t page_end = ((uintptr_t)end + 0xFFF) & ~(uintptr_t)0xFFF;
	if (arch_map_device(page, page_end - page)) return -1;
	*va = (uintptr_t)P2V((uintptr_t)phys);
	return 0;
}

static int find_virtio_regions(void) {
	uint8_t cap = pci_cfg8(0x34);
	int found_common = 0, found_notify = 0, found_device = 0;
	for (int hops = 0; cap >= 0x40 && hops < 48; hops++) {
		uint8_t id = pci_cfg8(cap), next = pci_cfg8((uint8_t)(cap + 1));
		if (id == PCI_CAP_VENDOR) {
			uint8_t len = pci_cfg8((uint8_t)(cap + 2));
			uint8_t type = pci_cfg8((uint8_t)(cap + 3));
			uint8_t bar = pci_cfg8((uint8_t)(cap + 4));
			uint32_t off = pci_cfg32((uint8_t)(cap + 8));
			uint32_t size = pci_cfg32((uint8_t)(cap + 12));
			if (len >= 16 && type == VIRTIO_PCI_CAP_COMMON) {
				common_len = size;
				if (map_region(bar, off, size, &bar_common)) return -1;
				found_common = 1;
			} else if (len >= 20 && type == VIRTIO_PCI_CAP_NOTIFY) {
				notify_len = size;
				notify_mult = pci_cfg32((uint8_t)(cap + 16));
				if (map_region(bar, off, size, &bar_notify)) return -1;
				found_notify = 1;
			} else if (len >= 16 && type == VIRTIO_PCI_CAP_DEVICE) {
				device_len = size;
				if (map_region(bar, off, size, &bar_device)) return -1;
				found_device = 1;
			}
		}
		if (!next || next == cap) break;
		cap = next;
	}
	return found_common && found_notify && found_device && common_len >= 0x38 && device_len >= 6 ? 0 : -1;
}

/* Configure and enable one virtqueue (modern transport). */
static int setup_queue(int qi) {
	write_u16(bar_common + VIRTIO_PCI_COMMON_QSEL, qi);
	mfence();
	uint16_t notify_off = read_u16(bar_common + VIRTIO_PCI_COMMON_QNOTIFY_OFF);
	uint64_t noff = (uint64_t)notify_off * notify_mult;
	if (noff + 2 > notify_len) return -1;
	notify_queue[qi] = bar_notify + (uintptr_t)noff;

	uint16_t qsz = read_u16(bar_common + VIRTIO_PCI_COMMON_QSIZE);
	if (qsz < VNQ) return -1;

	write_u16(bar_common + VIRTIO_PCI_COMMON_QSIZE, VNQ);
	mfence();

	uint64_t q = dma_addr((uintptr_t)&vq[qi]);

	write_u64(bar_common + VIRTIO_PCI_COMMON_QDESCTBL, q);
	mfence();
	write_u64(bar_common + VIRTIO_PCI_COMMON_QAVAIL, q + 64);
	mfence();
	write_u64(bar_common + VIRTIO_PCI_COMMON_QUSED, q + 4096);
	mfence();

	write_u16(bar_common + VIRTIO_PCI_COMMON_QENABLE, 1);
	mfence();

	return 0;
}

static int vn_send(const uint8_t *frame, size_t len) {
	if (len > VN_BUF - 12) return -EINVAL;  /* 12-byte header on modern */

	struct vqueue *q = &vq[1];
	uint8_t *b = vbuf[VNQ];  /* transmit slot 0 */
	memset(b, 0, 12);        /* clear header */
	memcpy(b + 12, frame, len);

	q->desc[0].addr = dma_addr((uintptr_t)b);
	q->desc[0].len = (uint32_t)(12 + len);
	q->desc[0].flags = 0;
	q->desc[0].next = 0;

	q->avail_ring[tx_av % VNQ] = 0;
	mfence();
	tx_av++;
	q->avail_idx = tx_av;
	mfence();

	write_u16(notify_queue[1], 1);  /* notify queue 1 */

	for (long spin = 0; q->used_idx == tx_used; spin++) {
		mfence();
		if (spin > 5000000L) { kprintf("vnet: TX completion timeout (used=%u expected=%u)\n", q->used_idx, tx_used); return -EIO; }
	}
	tx_used++;
	return 0;
}

static int vn_poll(void) {
	struct vqueue *q = &vq[0];
	int n = 0;

	mfence();
	while (rx_used != q->used_idx) {
		uint16_t pos = rx_used % VNQ;
		uint32_t id = q->used_ring[pos].id;
		uint32_t len = q->used_ring[pos].len;

		if (id < VNQ && len > 12 && len <= VN_BUF) {
			net_input(vbuf[id] + 12, len - 12);
		}

		q->avail_ring[rx_av % VNQ] = (uint16_t)id;
		rx_av++;
		mfence();
		q->avail_idx = rx_av;
		rx_used++;
		n++;
	}

	if (n) {
		mfence();
		write_u16(notify_queue[0], 0);  /* notify queue 0 */
	}
	return n;
}

/* Hotplug detection: poll for virtio-net device if not yet attached.
 * This allows ethernet to be automatically connected when hotplugged or added after boot. */
static void vn_detect_hotplug(void) {
	static uint64_t next_scan;
	if (device_attached || !nic_hotplug_due(&next_scan)) return;
	init();          /* finds the device if one has appeared; attaches it and sets device_attached */
}

static int probe(void) {
	/* Keep the driver active when no NIC is present yet.  Its poll hook retries
	 * PCI discovery so a virtio-net device added after boot can be attached. */
	return 0;
}

static int init(void) {
	static const struct pci_id ids[] = {
		{ VIRTIO_PCI_ID_VENDOR, VIRTIO_PCI_ID_NET_MODERN },
		{ VIRTIO_PCI_ID_VENDOR, VIRTIO_PCI_ID_NET_LEGACY }
	};
	if (pci_find_any(ids, 2, &pdev)) return 0; /* deferred hotplug probe */
	nic_pci_prepare(&pdev);  /* memory decode, bus mastering, INTx off */

	if (find_virtio_regions()) {
		kprintf("vnet: missing/invalid virtio PCI capabilities\n");
		return -1;
	}

	/* Reset device and acknowledge */
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS, 0);
	mfence();
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS, VIRTIO_PCI_STATUS_ACKNOWLEDGE);
	mfence();
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS,
			 VIRTIO_PCI_STATUS_ACKNOWLEDGE | VIRTIO_PCI_STATUS_DRIVER);
	mfence();

	/* Negotiate features: only MAC */
	write_u32(bar_common + VIRTIO_PCI_COMMON_DFSELECT, 0);  /* select feature word 0 */
	mfence();
	uint32_t f0 = read_u32(bar_common + VIRTIO_PCI_COMMON_DF);

	write_u32(bar_common + VIRTIO_PCI_COMMON_DFSELECT, 1);  /* select feature word 1 */
	mfence();
	uint32_t f1 = read_u32(bar_common + VIRTIO_PCI_COMMON_DF);

	if (!(f1 & 1)) { kprintf("vnet: device lacks VERSION_1 (features %x)\n", f1); return -1; }

	/* Acknowledge features back */
	write_u32(bar_common + VIRTIO_PCI_COMMON_GFSELECT, 0);
	mfence();
	write_u32(bar_common + VIRTIO_PCI_COMMON_GF, f0 & VIRTIO_F_MAC);
	mfence();

	write_u32(bar_common + VIRTIO_PCI_COMMON_GFSELECT, 1);
	mfence();
	write_u32(bar_common + VIRTIO_PCI_COMMON_GF, 1);  /* VERSION_1 bit */
	mfence();
	write_u32(bar_common + VIRTIO_PCI_COMMON_GFSELECT, 0);

	/* Signal features OK */
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS,
			 VIRTIO_PCI_STATUS_ACKNOWLEDGE | VIRTIO_PCI_STATUS_DRIVER | VIRTIO_PCI_STATUS_FEATURES_OK);
	mfence();

	if (!(read_u8(bar_common + VIRTIO_PCI_COMMON_STATUS) & VIRTIO_PCI_STATUS_FEATURES_OK)) {
		kprintf("vnet: device rejected negotiated features (status %x)\n", read_u8(bar_common + VIRTIO_PCI_COMMON_STATUS));
		return -1;
	}

	/* Setup queues */
	if (setup_queue(0)) { kprintf("vnet: RX queue setup failed\n"); return -1; }
	if (setup_queue(1)) { kprintf("vnet: TX queue setup failed\n"); return -1; }

	/* Read MAC address from device config (offset 0 in device config region) */
	for (int i = 0; i < 6; i++) nd_mac[i] = read_u8(bar_device + i);

	/* Initialize receive buffers */
	for (int i = 0; i < VNQ; i++) {
		vq[0].desc[i].addr = dma_addr((uintptr_t)vbuf[i]);
		vq[0].desc[i].len = VN_BUF;
		vq[0].desc[i].flags = 2;  /* VIRTQ_DESC_F_WRITE */
		vq[0].desc[i].next = 0;
		vq[0].avail_ring[i] = (uint16_t)i;
	}

	rx_av = VNQ;
	mfence();
	vq[0].avail_idx = rx_av;
	mfence();

	/* Notify device to begin processing receive queue */
	write_u16(notify_queue[0], 0);

	/* Signal DRIVER_OK */
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS,
			 VIRTIO_PCI_STATUS_ACKNOWLEDGE | VIRTIO_PCI_STATUS_DRIVER | VIRTIO_PCI_STATUS_FEATURES_OK |
			 VIRTIO_PCI_STATUS_DRIVER_OK);
	mfence();

	/* Register network device */
	nd.name = "virtio-net (pci/modern)";
	memcpy(nd.mac, nd_mac, 6);
	nd.send = vn_send;
	nd.poll = vn_poll;

	if (net_attach(&nd) != 0) return -1;

	device_attached = 1;

	kprintf("vnet: initialized, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		nd_mac[0], nd_mac[1], nd_mac[2], nd_mac[3], nd_mac[4], nd_mac[5]);

	return 0;
}

/* Priority 5: ahead of all other display drivers on x86_64 */
static struct kdriver drv = {
	"virtio-net-pci",
	"virtio-net PCI network device (QEMU/KVM/Hyper-V)",
	DRV_NET,
	5,
	probe, init, vn_detect_hotplug, 0
};
KDRIVER(drv);
