/* Intel PRO/1000 PCI Ethernet, used by VirtualBox's default 82540EM adapter.
 * Polled, legacy descriptor rings, no checksum or segmentation offload. */
#include "kernel.h"
#include "net.h"
#include "drivers/x86_platform.h"

#define E1000_VID       0x8086
#define E1000_82540EM   0x100E
#define E1000_82543GC   0x1004
#define E1000_82545EM   0x100F
#define RXN             32
#define TXN             32
#define BUF_SIZE        2048

#define REG_CTRL        0x0000
#define REG_STATUS      0x0008
#define REG_EERD        0x0014
#define REG_ICR         0x00C0
#define REG_IMC         0x00D8
#define REG_RCTL        0x0100
#define REG_TCTL        0x0400
#define REG_TIPG        0x0410
#define REG_RAL         0x5400
#define REG_RAH         0x5404
#define REG_RDBAL       0x2800
#define REG_RDBAH       0x2804
#define REG_RDLEN       0x2808
#define REG_RDH         0x2810
#define REG_RDT         0x2818
#define REG_TDBAL       0x3800
#define REG_TDBAH       0x3804
#define REG_TDLEN       0x3808
#define REG_TDH         0x3810
#define REG_TDT         0x3818

#define CTRL_RST        (1u << 26)
#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)
#define RCTL_SECRC      (1u << 26)
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)
#define RX_DD           1
#define RX_EOP          2
#define TX_DD           1
#define TX_CMD_EOP      1
#define TX_CMD_IFCS     2
#define TX_CMD_RS       8

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, command, status, css;
    uint16_t special;
} __attribute__((packed));

static struct pci_dev pdev;
static volatile uint32_t *regs;
static struct e1000_rx_desc rx_ring[RXN] __attribute__((aligned(16)));
static struct e1000_tx_desc tx_ring[TXN] __attribute__((aligned(16)));
static uint8_t rx_buf[RXN][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(16)));
static struct netdev nd;
static uint16_t rx_next, tx_next;
static int attached;

static inline uint32_t reg_read(uint32_t off) { return regs[off / 4]; }
static inline void reg_write(uint32_t off, uint32_t v) { regs[off / 4] = v; }
static inline void dma_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

static uintptr_t dma_addr(uintptr_t va) {
    const uintptr_t kernel_vma = 0xFFFFFFFF80000000UL;
    if (va >= kernel_vma) return va - kernel_vma;
    return V2P(va);
}

static const struct pci_id e1000_ids[] = {
    { E1000_VID, E1000_82540EM },
    { E1000_VID, E1000_82543GC },
    { E1000_VID, E1000_82545EM },
};

static int e1000_rx_poll(void) {
    int count = 0;
    for (;;) {
        struct e1000_rx_desc *d = &rx_ring[rx_next];
        dma_barrier();
        if (!(d->status & RX_DD)) break;
        uint16_t len = d->length;
        if ((d->status & RX_EOP) && !d->errors && len >= 14 && len <= BUF_SIZE)
            net_input(rx_buf[rx_next], len);
        d->status = 0;
        dma_barrier();
        reg_write(REG_RDT, rx_next);
        rx_next = (rx_next + 1) % RXN;
        count++;
    }
    return count;
}

static int e1000_send(const uint8_t *frame, size_t len) {
    if (len > BUF_SIZE || len < 14) return -EINVAL;
    struct e1000_tx_desc *d = &tx_ring[tx_next];
    for (unsigned spin = 0; !(d->status & TX_DD); spin++) {
        if (spin > 5000000u) return -EIO;
        dma_barrier();
    }
    memcpy(tx_buf, frame, len);
    d->addr = dma_addr((uintptr_t)tx_buf);
    d->length = (uint16_t)len;
    d->cso = 0;
    d->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    dma_barrier();
    tx_next = (tx_next + 1) % TXN;
    reg_write(REG_TDT, tx_next);
    for (unsigned spin = 0; !(d->status & TX_DD); spin++) {
        if (spin > 5000000u) return -EIO;
        dma_barrier();
    }
    return 0;
}

static int read_mac(uint8_t mac[6]) {
    uint32_t lo = reg_read(REG_RAL), hi = reg_read(REG_RAH);
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (i * 8));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
    int any = 0;
    for (int i = 0; i < 6; i++) any |= mac[i];
    if (any && !(mac[0] & 1)) return 0;

    /* EEPROM words 0..2 hold the MAC when RAL/RAH were not initialized. */
    for (int i = 0; i < 3; i++) {
        reg_write(REG_EERD, 1u | ((uint32_t)i << 8));
        unsigned spin;
        uint32_t value;
        for (spin = 0; spin < 1000000u; spin++) {
            value = reg_read(REG_EERD);
            if (value & (1u << 4)) break;
        }
        if (spin == 1000000u) return -1;
        mac[i * 2] = (uint8_t)(value >> 16);
        mac[i * 2 + 1] = (uint8_t)(value >> 24);
    }
    return (mac[0] & 1) ? -1 : 0;
}

static int e1000_init(void) {
    if (pci_find_any(e1000_ids, sizeof e1000_ids / sizeof e1000_ids[0], &pdev)) return 0;
    uint32_t raw_bar = pci_read32(pdev.bus, pdev.dev, pdev.fn, 0x10);
    uint32_t bar = pci_bar(&pdev, 0);
    if (!bar || (raw_bar & 1)) return -1;
    pci_enable(&pdev, 6); /* memory decode and bus mastering */
    if (arch_map_device(bar & ~0xFFFu, 0x20000)) return -1;
    regs = (volatile uint32_t *)P2V(bar);

    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (unsigned spin = 0; (reg_read(REG_CTRL) & CTRL_RST) && spin < 1000000u; spin++)
        __asm__ volatile("pause");
    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);

    memset(rx_ring, 0, sizeof rx_ring);
    memset(tx_ring, 0, sizeof tx_ring);
    for (int i = 0; i < RXN; i++) {
        rx_ring[i].addr = dma_addr((uintptr_t)rx_buf[i]);
        rx_ring[i].status = 0;
    }
    for (int i = 0; i < TXN; i++) tx_ring[i].status = TX_DD;
    rx_next = tx_next = 0;
    dma_barrier();

    uintptr_t rx_pa = dma_addr((uintptr_t)rx_ring);
    uintptr_t tx_pa = dma_addr((uintptr_t)tx_ring);
    reg_write(REG_RDBAL, (uint32_t)rx_pa);
    reg_write(REG_RDBAH, (uint32_t)(rx_pa >> 32));
    reg_write(REG_RDLEN, sizeof rx_ring);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RXN - 1);
    reg_write(REG_TDBAL, (uint32_t)tx_pa);
    reg_write(REG_TDBAH, (uint32_t)(tx_pa >> 32));
    reg_write(REG_TDLEN, sizeof tx_ring);
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 10 | (8 << 10) | (6 << 20));
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10 << 4) | (0x40 << 12));

    uint8_t mac[6];
    if (read_mac(mac)) { kprintf("e1000: failed to read MAC address\n"); return -1; }
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    nd.name = "Intel PRO/1000 (e1000)";
    memcpy(nd.mac, mac, sizeof mac);
    nd.send = e1000_send;
    nd.poll = e1000_rx_poll;
    if (net_attach(&nd)) return -1;
    attached = 1;
    kprintf("e1000: initialized, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int probe(void) { return 0; }

static void poll(void) {
    if (attached) return;
    static uint64_t next_scan;
    if (ticks < next_scan) return;
    next_scan = ticks + HZ;
    pci_rescan();
    if (!pci_find_any(e1000_ids, sizeof e1000_ids / sizeof e1000_ids[0], &pdev)) e1000_init();
}

static struct kdriver drv = {
    "e1000", "Intel PRO/1000 PCI Ethernet (VirtualBox and compatible)" ,
    DRV_NET, 10, probe, e1000_init, poll, 0
};
KDRIVER(drv);
