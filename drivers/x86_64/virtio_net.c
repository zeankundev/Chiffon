/* virtio-net over PCI (x86_64), polled. Supports the modern transport (version 2) used by
 * QEMU when booting with BIOS/UEFI. Configuration: -device virtio-net-pci.
 * Two virtqueues (0 = receive, 1 = transmit), four descriptors each, one 1536-byte buffer
 * per descriptor. Only MAC feature negotiated: no checksum or GSO offload. */

#include "kernel.h"
#include "net.h"
#include "drivers/x86_platform.h"

#define VNQ     4
#define VN_BUF  1536
#define VIRTIO_PCI_ID_VENDOR 0x1AF4
#define VIRTIO_PCI_ID_NET    0x1000
#define VIRTIO_F_MAC         (1u << 5)
/* VIRTIO_F_VERSION_1 is bit 0 of feature word 1, checked directly in init() */

/* PCI BAR layout (modern transport) */
#define BAR_COMMON 0   /* BAR 0: common config */
#define BAR_NOTIFY 2   /* BAR 2: queue notify */
#define BAR_ISR    4   /* BAR 4: ISR (interrupt status) */

/* Common config structure (BAR 0) */
#define VIRTIO_PCI_COMMON_DFSELECT   0x00
#define VIRTIO_PCI_COMMON_DF         0x04
#define VIRTIO_PCI_COMMON_MSIX_CAP   0x08
#define VIRTIO_PCI_COMMON_NUMQ       0x0C
#define VIRTIO_PCI_COMMON_STATUS     0x12
#define VIRTIO_PCI_COMMON_CFGGENERATION 0x13
#define VIRTIO_PCI_COMMON_QSEL       0x22
#define VIRTIO_PCI_COMMON_QSIZE      0x24
#define VIRTIO_PCI_COMMON_QMSIX_VECTOR 0x26
#define VIRTIO_PCI_COMMON_QENABLE    0x28
#define VIRTIO_PCI_COMMON_QNOTIFY_OFF 0x2A
#define VIRTIO_PCI_COMMON_QDESCTBL   0x30
#define VIRTIO_PCI_COMMON_QAVAIL     0x40
#define VIRTIO_PCI_COMMON_QUSED      0x48

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
static uintptr_t bar_common, bar_notify, bar_isr;
static uint16_t rx_av, rx_used, tx_av, tx_used;
static struct netdev nd;
static uint8_t nd_mac[6];

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

/* Configure and enable one virtqueue (modern transport). */
static int setup_queue(int qi) {
	write_u16(bar_common + VIRTIO_PCI_COMMON_QSEL, qi);
	mfence();

	uint16_t qsz = read_u16(bar_common + VIRTIO_PCI_COMMON_QSIZE);
	if (qsz < VNQ) return -1;

	write_u16(bar_common + VIRTIO_PCI_COMMON_QSIZE, VNQ);
	mfence();

	uint64_t q = V2P((uintptr_t)&vq[qi]);

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

	q->desc[0].addr = V2P((uintptr_t)b);
	q->desc[0].len = (uint32_t)(12 + len);
	q->desc[0].flags = 0;
	q->desc[0].next = 0;

	q->avail_ring[tx_av % VNQ] = 0;
	mfence();
	tx_av++;
	q->avail_idx = tx_av;
	mfence();

	write_u16(bar_notify + 0, 1);  /* notify queue 1 */

	for (long spin = 0; q->used_idx == tx_used; spin++) {
		mfence();
		if (spin > 50000000L) return -EIO;
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
		write_u16(bar_notify + 0, 0);  /* notify queue 0 */
	}
	return n;
}

static int probe(void) {
	static const struct pci_id ids[] = {
		{ VIRTIO_PCI_ID_VENDOR, VIRTIO_PCI_ID_NET }
	};
	if (pci_find_any(ids, 1, &pdev)) return -1;
	return 0;
}

static int init(void) {
	pci_enable(&pdev, 3);  /* memory and bus master on */

	uint32_t bar0 = pci_bar(&pdev, 0);
	uint32_t bar2 = pci_bar(&pdev, 2);
	uint32_t bar4 = pci_bar(&pdev, 4);

	if (!bar0 || !bar2 || !bar4) return -1;

	bar_common = (uintptr_t)P2V(bar0);
	bar_notify = (uintptr_t)P2V(bar2);
	bar_isr = (uintptr_t)P2V(bar4);

	/* Map device memory as uncacheable */
	arch_map_device(bar0 & ~0xFFF, 4096);
	arch_map_device(bar2 & ~0xFFF, 4096);
	arch_map_device(bar4 & ~0xFFF, 4096);

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

	if (!(f1 & 1)) return -1;  /* VERSION_1 is bit 0 of word 1 (bit 32 overall) */

	/* Acknowledge features back */
	write_u32(bar_common + VIRTIO_PCI_COMMON_DFSELECT, 0);
	mfence();
	write_u32(bar_common + VIRTIO_PCI_COMMON_DF, f0 & VIRTIO_F_MAC);
	mfence();

	write_u32(bar_common + VIRTIO_PCI_COMMON_DFSELECT, 1);
	mfence();
	write_u32(bar_common + VIRTIO_PCI_COMMON_DF, 1);  /* VERSION_1 bit */
	mfence();

	/* Signal features OK */
	write_u8(bar_common + VIRTIO_PCI_COMMON_STATUS,
			 VIRTIO_PCI_STATUS_ACKNOWLEDGE | VIRTIO_PCI_STATUS_DRIVER | VIRTIO_PCI_STATUS_FEATURES_OK);
	mfence();

	if (!(read_u8(bar_common + VIRTIO_PCI_COMMON_STATUS) & VIRTIO_PCI_STATUS_FEATURES_OK)) {
		return -1;
	}

	/* Setup queues */
	if (setup_queue(0) || setup_queue(1)) return -1;

	/* Read MAC address from device config (offset 0 in device config region) */
	uintptr_t cfg_base = bar_common + 0x100;  /* device-specific config at offset 0x100 */
	for (int i = 0; i < 6; i++) {
		nd_mac[i] = read_u8(cfg_base + i);
	}

	/* Initialize receive buffers */
	for (int i = 0; i < VNQ; i++) {
		vq[0].desc[i].addr = V2P((uintptr_t)vbuf[i]);
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
	write_u16(bar_notify + 0, 0);

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
	probe, init, 0, 0
};
KDRIVER(drv);
