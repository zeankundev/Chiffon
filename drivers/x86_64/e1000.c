/* Intel PRO/1000 (8254x family) PCI/PCI-X Ethernet: VirtualBox's default 82540EM, QEMU's `e1000`,
 * VMware, and the real copper adapters of that generation.  Polled, legacy descriptor rings, no
 * checksum or segmentation offload.
 *
 * What a real chip (and VirtualBox) needs that QEMU forgives:
 *   - after reset the link must be asked to come up (CTRL.SLU, auto speed detection) and the PHY
 *     must be running; QEMU's model is simply "up", the others stay down until told;
 *   - the link takes seconds to negotiate, so the driver reports carrier through netdev.link()
 *     and the network stack waits for it before it sends DHCP;
 *   - the receive address must carry its valid bit and the multicast table must be cleared;
 *   - the reset needs real delays between steps (nic_udelay(): it works before interrupts run);
 *   - a 64-bit memory BAR must be read as 64 bit.
 * Not handled: fiber/SerDes parts, the IGP-PHY chips (82541/82547), the PCIe e1000e family
 * (82571..82574, ICH/PCH LAN: they need firmware handshakes), more than one adapter. */
#include "kernel.h"
#include "net.h"
#include "drivers/x86_nic.h"

#define E1000_VID       0x8086
#define RXN             32
#define TXN             32
#define BUF_SIZE        2048

#define REG_CTRL        0x0000
#define REG_STATUS      0x0008
#define REG_EERD        0x0014
#define REG_MDIC        0x0020
#define REG_ICR         0x00C0
#define REG_IMC         0x00D8
#define REG_RCTL        0x0100
#define REG_TCTL        0x0400
#define REG_TIPG        0x0410
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
#define REG_MTA         0x5200      /* 128 dwords */
#define REG_RAL         0x5400
#define REG_RAH         0x5404

#define CTRL_LRST       (1u << 3)
#define CTRL_ASDE       (1u << 5)
#define CTRL_SLU        (1u << 6)
#define CTRL_ILOS       (1u << 7)
#define CTRL_FRCSPD     (1u << 11)
#define CTRL_FRCDPX     (1u << 12)
#define CTRL_RST        (1u << 26)
#define CTRL_VME        (1u << 30)
#define CTRL_PHY_RST    (1u << 31)
#define STATUS_FD       (1u << 0)
#define STATUS_LU       (1u << 1)
#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)
#define RCTL_SECRC      (1u << 26)
#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)
#define RAH_AV          (1u << 31)
#define MDIC_READY      (1u << 28)
#define MDIC_ERROR      (1u << 30)
#define PHY_ADDR        1           /* the internal copper PHY of these parts */
#define PHY_CTRL        0
#define PHY_CTRL_PWRDN  (1u << 11)
#define PHY_CTRL_ANEN   (1u << 12)
#define PHY_CTRL_ANRST  (1u << 9)
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

/* Copper adapters of the 8254x generation that share this programming model. */
static const struct { uint16_t device; const char *name; } models[] = {
    { 0x100E, "Intel 82540EM (PRO/1000 MT Desktop)" },
    { 0x1015, "Intel 82540EM LOM" },
    { 0x1016, "Intel 82540EP LOM" },
    { 0x1017, "Intel 82540EP" },
    { 0x101E, "Intel 82540EP LP" },
    { 0x100F, "Intel 82545EM (PRO/1000 MT Server)" },
    { 0x1026, "Intel 82545GM" },
    { 0x1004, "Intel 82543GC" },
    { 0x1008, "Intel 82544EI" },
    { 0x100C, "Intel 82544GC" },
    { 0x100D, "Intel 82544GC LOM" },
    { 0x1010, "Intel 82546EB" },
    { 0x101D, "Intel 82546EB quad" },
    { 0x1079, "Intel 82546GB" },
    { 0x1099, "Intel 82546GB quad" },
    { 0x108A, "Intel 82546GB PCIe" },
};
#define NMODELS ((int)(sizeof models / sizeof models[0]))

static struct pci_dev pdev;
static volatile uint32_t *regs;
static struct e1000_rx_desc rx_ring[RXN] __attribute__((aligned(128)));
static struct e1000_tx_desc tx_ring[TXN] __attribute__((aligned(128)));
static uint8_t rx_buf[RXN][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buf[BUF_SIZE] __attribute__((aligned(16)));
static struct netdev nd;
static char nd_name[48];
static uint16_t rx_next, tx_next;
static int attached;

static inline uint32_t reg_read(uint32_t off) { return regs[off / 4]; }
static inline void reg_write(uint32_t off, uint32_t v) { regs[off / 4] = v; }
static inline void dma_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

static int e1000_link(void) {
    return regs && (reg_read(REG_STATUS) & STATUS_LU) ? 1 : 0;
}

static int e1000_rx_poll(void) {
    int count = 0;
    for (int budget = 0; budget < RXN; budget++) {
        struct e1000_rx_desc *d = &rx_ring[rx_next];
        dma_barrier();
        if (!(d->status & RX_DD)) break;
        uint16_t len = d->length;
        if ((d->status & RX_EOP) && !d->errors && len >= 14 && len <= BUF_SIZE)
            net_input(rx_buf[rx_next], len);
        d->status = 0;
        dma_barrier();
        reg_write(REG_RDT, rx_next);                    /* give the descriptor back to the chip */
        rx_next = (rx_next + 1) % RXN;
        count++;
    }
    return count;
}

static int wait_tx_done(const struct e1000_tx_desc *d) {
    for (unsigned i = 0; i < 20000u; i++) {             /* fast path: the chip is usually done at once */
        dma_barrier();
        if (d->status & TX_DD) return 0;
    }
    for (unsigned i = 0; i < 2000u; i++) {              /* then up to ~100 ms: a hypervisor may be slow */
        nic_udelay(50);
        dma_barrier();
        if (d->status & TX_DD) return 0;
    }
    return -EIO;
}

static int e1000_send(const uint8_t *frame, size_t len) {
    if (len > BUF_SIZE || len < 14) return -EINVAL;
    if (!e1000_link()) return -ENETDOWN;                /* a stalled ring would wedge later sends */
    struct e1000_tx_desc *d = &tx_ring[tx_next];
    if (wait_tx_done(d)) return -EIO;                   /* previous frame never left */
    memcpy(tx_buf, frame, len);
    d->addr = nic_dma_addr(tx_buf);
    d->length = (uint16_t)len;
    d->cso = 0;
    d->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    dma_barrier();
    tx_next = (tx_next + 1) % TXN;
    reg_write(REG_TDT, tx_next);
    return wait_tx_done(d);
}

/* ---- PHY (MDIC) ---- */
static int phy_read(unsigned reg, uint16_t *out) {
    reg_write(REG_MDIC, (reg << 16) | (PHY_ADDR << 21) | (2u << 26));
    for (unsigned i = 0; i < 640u; i++) {
        nic_udelay(10);
        uint32_t v = reg_read(REG_MDIC);
        if (v & MDIC_ERROR) return -1;
        if (v & MDIC_READY) { *out = (uint16_t)v; return 0; }
    }
    return -1;
}
static int phy_write(unsigned reg, uint16_t val) {
    reg_write(REG_MDIC, val | (reg << 16) | (PHY_ADDR << 21) | (1u << 26));
    for (unsigned i = 0; i < 640u; i++) {
        nic_udelay(10);
        uint32_t v = reg_read(REG_MDIC);
        if (v & MDIC_ERROR) return -1;
        if (v & MDIC_READY) return 0;
    }
    return -1;
}
/* Firmware or an earlier OS may have powered the PHY down or turned auto-negotiation off.  Only
 * touch it in that case, so a PHY that is already running keeps its link. */
static void phy_wake(void) {
    uint16_t ctl;
    if (phy_read(PHY_CTRL, &ctl)) return;               /* no readable PHY: leave it alone */
    if ((ctl & PHY_CTRL_PWRDN) || !(ctl & PHY_CTRL_ANEN))
        phy_write(PHY_CTRL, (uint16_t)((ctl & ~PHY_CTRL_PWRDN) | PHY_CTRL_ANEN | PHY_CTRL_ANRST));
}

static int mac_valid(const uint8_t m[6]) {
    int any = 0;
    for (int i = 0; i < 6; i++) any |= m[i];
    return any && !(m[0] & 1);
}

static int read_mac(uint8_t mac[6]) {
    uint32_t lo = reg_read(REG_RAL), hi = reg_read(REG_RAH);    /* loaded from the EEPROM by the reset */
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (i * 8));
    mac[4] = (uint8_t)hi;
    mac[5] = (uint8_t)(hi >> 8);
    if (mac_valid(mac)) return 0;

    /* EEPROM words 0..2 hold the MAC when RAL/RAH were not initialized. */
    for (int i = 0; i < 3; i++) {
        reg_write(REG_EERD, 1u | ((uint32_t)i << 8));
        uint32_t value = 0;
        unsigned spin;
        for (spin = 0; spin < 1000u; spin++) {
            value = reg_read(REG_EERD);
            if (value & (1u << 4)) break;
            nic_udelay(10);
        }
        if (spin == 1000u) return -1;
        mac[i * 2] = (uint8_t)(value >> 16);
        mac[i * 2 + 1] = (uint8_t)(value >> 24);
    }
    return mac_valid(mac) ? 0 : -1;
}

static int find_adapter(const char **name) {
    for (int i = 0; i < NMODELS; i++)
        if (!pci_find(E1000_VID, models[i].device, &pdev)) { *name = models[i].name; return 0; }
    return -1;
}

static int e1000_init(void) {
    const char *name;
    if (find_adapter(&name)) return 0;                  /* none present (yet): not an error */

    nic_pci_prepare(&pdev);
    volatile void *base;
    if (nic_map_bar(&pdev, 0, 0x20000, &base)) {
        kprintf("e1000: %02x:%02x.%x: register BAR missing or unmappable\n", pdev.bus, pdev.dev, pdev.fn);
        return -1;
    }
    regs = (volatile uint32_t *)base;

    /* Quiesce, then reset the MAC.  The delays are the ones the hardware manual asks for. */
    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_RCTL, 0);
    reg_write(REG_TCTL, TCTL_PSP);
    (void)reg_read(REG_STATUS);                         /* flush the posted writes */
    nic_udelay(10000);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    nic_udelay(5000);
    unsigned spin;
    for (spin = 0; (reg_read(REG_CTRL) & CTRL_RST) && spin < 1000u; spin++) nic_udelay(100);
    if (reg_read(REG_CTRL) == 0xFFFFFFFFu) {            /* reads of a dead or absent device */
        kprintf("e1000: device does not respond after reset\n");
        regs = 0;
        return -1;
    }
    nic_udelay(20000);                                  /* EEPROM autoload (RAL/RAH, config) */
    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);

    /* Ask for link: set link up, follow the PHY's speed/duplex, no forced values. */
    uint32_t ctrl = reg_read(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_ILOS | CTRL_FRCSPD | CTRL_FRCDPX | CTRL_VME | CTRL_PHY_RST);
    reg_write(REG_CTRL, ctrl);
    nic_udelay(1000);
    phy_wake();

    uint8_t mac[6];
    if (read_mac(mac)) { kprintf("e1000: no valid MAC address\n"); regs = 0; return -1; }
    for (int i = 0; i < 128; i++) reg_write(REG_MTA + 4 * i, 0);            /* no multicast */
    reg_write(REG_RAL, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    reg_write(REG_RAH, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | RAH_AV);

    memset(rx_ring, 0, sizeof rx_ring);
    memset(tx_ring, 0, sizeof tx_ring);
    for (int i = 0; i < RXN; i++) rx_ring[i].addr = nic_dma_addr(rx_buf[i]);
    for (int i = 0; i < TXN; i++) tx_ring[i].status = TX_DD;
    rx_next = tx_next = 0;
    dma_barrier();

    uint64_t rx_pa = nic_dma_addr(rx_ring);
    uint64_t tx_pa = nic_dma_addr(tx_ring);
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
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);                   /* 2048-byte buffers */
    (void)reg_read(REG_STATUS);

    ksnprintf(nd_name, sizeof nd_name, "%s", name);
    nd.name = nd_name;
    memcpy(nd.mac, mac, sizeof mac);
    nd.send = e1000_send;
    nd.poll = e1000_rx_poll;
    nd.link = e1000_link;
    if (net_attach(&nd)) { regs = 0; return -1; }
    attached = 1;
    kprintf("e1000: %s at %02x:%02x.%x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", name,
            pdev.bus, pdev.dev, pdev.fn, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            e1000_link() ? "up" : "negotiating");
    return 0;
}

static int probe(void) { return 0; }

static void poll(void) {
    static uint64_t next_scan;
    if (attached || !nic_hotplug_due(&next_scan)) return;
    e1000_init();
}

static struct kdriver drv = {
    "e1000", "Intel PRO/1000 (8254x) PCI Ethernet (VirtualBox, QEMU, real adapters)",
    DRV_NET, 10, probe, e1000_init, poll, 0
};
KDRIVER(drv);
