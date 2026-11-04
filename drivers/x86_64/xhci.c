/* xHCI host controller (USB 2 and 3 root ports) for x86_64 PCs and QEMU's qemu-xhci.
 *
 * Polled, not interrupt-driven.  Each timer tick drains the event ring, then scans the root ports.  A port
 * whose connect-status-change bit is set is attached (reset, enable slot, address device, then usb.c
 * enumerates the device) or detached.  Synchronous steps spin on the event ring with a fixed bound:
 * ticks do not advance inside an interrupt, so time is not used for timeouts.
 *
 * Ring discipline: a TRB's control word is written last, after a full barrier, so the controller never
 * sees a half-written TRB.  Each ring ends in a Link TRB that toggles the cycle bit. */
#include "kernel.h"
#include "drivers/x86_platform.h"
#include "drivers/usb.h"

#define ETIMEDOUT  110
#define WAIT_LIMIT 3000000L            /* poll iterations for one synchronous step */
#define BULK_PAGES 8                   /* 32 KiB per bulk transfer */
#define RING_TRBS  256
#define MAX_ENDPT  32

/* capability registers */
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18
/* operational registers (relative to CAPLENGTH) */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR   0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORT   0x400
#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define USBSTS_CNR   (1u << 11)
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR  (1u << 4)
#define PORT_PP  (1u << 9)
#define PORT_CSC (1u << 17)
#define PORT_PRC (1u << 21)
#define PORT_SPEED(sc) (((sc) >> 10) & 0xF)
/* interrupter 0 (runtime registers) */
#define IR0       0x20
#define IR_ERSTSZ 0x08
#define IR_ERSTBA 0x10
#define IR_ERDP   0x18

/* TRB types */
#define TRB_NORMAL 1
#define TRB_SETUP  2
#define TRB_DATA   3
#define TRB_STATUS 4
#define TRB_LINK   6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_RESET_EP 14
#define TRB_SET_TR_DEQUEUE 16
#define EVT_TRANSFER 32
#define EVT_COMMAND 33

struct trb { uint64_t param; uint32_t status, control; };
struct ring { uintptr_t pa; struct trb *v; unsigned enq; uint32_t cycle; };

struct ep {
    int used, periodic, dci;
    uint8_t addr;
    struct ring ring;
    uintptr_t buf_pa; uint8_t *buf;      /* periodic IN: one packet buffer */
    uint32_t mps;
    uint32_t seq; int code; uint64_t ptr; uint32_t resid;   /* last transfer event seen on this endpoint */
};
struct port {
    int attached, slot, speed, tries;
    uint64_t next_try;
    uintptr_t devctx_pa, inctx_pa;
    uint8_t *devctx, *inctx;
    uintptr_t dma_pa; uint8_t *dma;                       /* control data, one page */
    uintptr_t bulk_pa[BULK_PAGES]; uint8_t *bulk[BULK_PAGES];
    uint64_t bulk_trb[BULK_PAGES];                        /* TRB addresses of the last bulk TD */
    struct ep ep[MAX_ENDPT];
};

static volatile uint8_t *cap, *op, *rt, *db;
static int ctx_sz, nports, ready;
static uintptr_t dcbaa_pa; static uint64_t *dcbaa;
static struct ring cmd_ring, evt_ring;
static struct port ports[USB_MAX_PORTS + 1];          /* 1-based root port numbers */
static uint8_t slot_port[256];                        /* slot id -> root port (0 = none) */
static volatile int cmd_done; static int cmd_code; static uint32_t cmd_slot;

static void barrier(void) { __asm__ volatile("mfence" ::: "memory"); }
static uint32_t r32(volatile uint8_t *b, unsigned off) { return *(volatile uint32_t *)(b + off); }
static void w32(volatile uint8_t *b, unsigned off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static void w64(volatile uint8_t *b, unsigned off, uint64_t v) { w32(b, off, (uint32_t)v); w32(b, off + 4, (uint32_t)(v >> 32)); }
static uint32_t port_sc(int p) { return r32(op, OP_PORT + 0x10u * (p - 1)); }
static void port_wr(int p, uint32_t v) { w32(op, OP_PORT + 0x10u * (p - 1), v); }
static void ring_db(int slot, int dci) { w32(db, 4u * slot, (uint32_t)dci); }

/* ---- rings ---- */
static void ring_reset(struct ring *r, uintptr_t pa) {
    r->pa = pa; r->v = P2V(pa);
    memset(r->v, 0, PAGE_SIZE);
    r->enq = 0; r->cycle = 1;
}
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    if (r->enq == RING_TRBS - 1) {                          /* the last slot is a Link back to the start */
        struct trb *l = &r->v[RING_TRBS - 1];
        l->param = r->pa; l->status = 0;
        barrier();
        l->control = (TRB_LINK << 10) | (1u << 1) | r->cycle;
        r->enq = 0; r->cycle ^= 1;
    }
    struct trb *t = &r->v[r->enq];
    t->param = param; t->status = status;
    barrier();
    t->control = control | r->cycle;
    uint64_t at = r->pa + 16u * r->enq;
    r->enq++;
    return at;
}

/* ---- event ring ---- */
static void on_transfer(uint32_t status, uint32_t control, uint64_t param) {
    int slot = control >> 24, dci = (control >> 16) & 0x1F;
    int p = slot_port[slot];
    if (!p || p > USB_MAX_PORTS || dci >= MAX_ENDPT) return;
    struct port *P = &ports[p];
    struct ep *e = &P->ep[dci];
    if (!e->used) return;
    e->seq++; e->code = (int)(status >> 24); e->ptr = param; e->resid = status & 0xFFFFFF;
    if (e->periodic && P->attached) {
        if (e->code == 1 || e->code == 13) usb_report(p, e->addr, e->buf, e->mps - e->resid);
        ring_push(&e->ring, e->buf_pa, e->mps, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
        ring_db(P->slot, dci);                              /* keep one report outstanding */
    }
}

static void process_events(void) {
    unsigned n = 0;
    for (;;) {
        struct trb *t = &evt_ring.v[evt_ring.enq];
        uint32_t c = t->control;
        if ((c & 1) != evt_ring.cycle) break;
        barrier();
        uint64_t param = t->param; uint32_t status = t->status;
        uint32_t type = (c >> 10) & 0x3F;
        if (type == EVT_TRANSFER) on_transfer(status, c, param);
        else if (type == EVT_COMMAND) { cmd_code = (int)(status >> 24); cmd_slot = c >> 24; cmd_done = 1; }
        if (++evt_ring.enq == RING_TRBS) { evt_ring.enq = 0; evt_ring.cycle ^= 1; }
        n++;
    }
    if (n) w64(rt, IR0 + IR_ERDP, ((uint64_t)evt_ring.pa + 16u * evt_ring.enq) | 8);   /* EHB: event handled */
}

/* ---- synchronous commands and transfers ---- */
static int cmd_sync(uint32_t control, uint64_t param, uint32_t *slot_out) {
    cmd_done = 0;
    ring_push(&cmd_ring, param, 0, control);
    barrier();
    ring_db(0, 0);
    for (long i = 0; i < WAIT_LIMIT && !cmd_done; i++) process_events();
    if (!cmd_done) return -ETIMEDOUT;
    if (slot_out) *slot_out = cmd_slot;
    return cmd_code == 1 ? 0 : -EIO;
}

/* after a failed transfer the endpoint is halted: reset it and move its dequeue pointer past the failed TD */
static void recover_ep(struct port *P, struct ep *e) {
    uint32_t id = (uint32_t)P->slot << 24 | (uint32_t)e->dci << 16;
    cmd_sync((TRB_RESET_EP << 10) | id, 0, 0);
    cmd_sync((TRB_SET_TR_DEQUEUE << 10) | id, (uint64_t)(e->ring.pa + 16u * e->ring.enq) | e->ring.cycle, 0);
}

static int wait_xfer(struct port *P, struct ep *e, uint32_t seq0) {
    for (long i = 0; i < WAIT_LIMIT; i++) {
        process_events();
        if (e->seq != seq0) return 0;
    }
    return -ETIMEDOUT;
}

static uint8_t dci_of(uint8_t addr) { return (uint8_t)((addr & 0x7F) * 2 + ((addr & 0x80) ? 1 : 0)); }

int xhci_control(int p, const struct usb_setup *s, void *buf) {
    struct port *P = &ports[p];
    struct ep *e = &P->ep[1];
    if (!P->attached || !e->used) return -ENODEV;
    int in = s->bmRequestType & 0x80;
    uint16_t len = s->wLength;
    if (len > PAGE_SIZE) return -EINVAL;
    if (len && !in) memcpy(P->dma, buf, len);
    uint64_t pkt; memcpy(&pkt, s, 8);
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(&e->ring, pkt, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len) ring_push(&e->ring, P->dma_pa, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    uint64_t st = ring_push(&e->ring, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));
    uint32_t seq0 = e->seq;
    barrier();
    ring_db(P->slot, 1);
    int r = wait_xfer(P, e, seq0);
    if (r) return r;
    if (e->code != 1 || e->ptr != st) { recover_ep(P, e); return -EIO; }
    if (len && in) memcpy(buf, P->dma, len);
    return len;
}

int xhci_bulk(int p, uint8_t addr, void *buf, uint32_t len) {
    struct port *P = &ports[p];
    uint8_t dci = dci_of(addr);
    struct ep *e = &P->ep[dci];
    if (!P->attached || !e->used || dci < 2 || !len || len > BULK_PAGES * PAGE_SIZE) return -EINVAL;
    int in = addr & 0x80;
    if (!in) for (uint32_t off = 0; off < len; off += PAGE_SIZE)
        memcpy(P->bulk[off / PAGE_SIZE], (uint8_t *)buf + off, len - off < PAGE_SIZE ? len - off : PAGE_SIZE);
    uint32_t left = len; int i = 0;
    while (left) {
        uint32_t n = left > PAGE_SIZE ? PAGE_SIZE : left;
        int last = (left == n);
        P->bulk_trb[i] = ring_push(&e->ring, P->bulk_pa[i], n,
                                   (TRB_NORMAL << 10) | (last ? (1u << 5) : (1u << 4)) | (in ? (1u << 2) : 0));
        left -= n; i++;
    }
    uint32_t seq0 = e->seq;
    barrier();
    ring_db(P->slot, dci);
    int r = wait_xfer(P, e, seq0);
    if (r) return r;
    if (e->code != 1 && e->code != 13) { recover_ep(P, e); return -EIO; }
    /* which TRB finished?  A short packet (13) ends the transfer early; the residue tells how much moved */
    uint32_t done = len;
    for (int k = 0; k < i; k++)
        if (P->bulk_trb[k] == e->ptr) {
            uint32_t n = (k == i - 1) ? (len - k * PAGE_SIZE) : PAGE_SIZE;
            done = k * PAGE_SIZE + (n - e->resid);
            break;
        }
    if (in) for (uint32_t off = 0; off < done; off += PAGE_SIZE)
        memcpy((uint8_t *)buf + off, P->bulk[off / PAGE_SIZE], done - off < PAGE_SIZE ? done - off : PAGE_SIZE);
    return (int)done;
}

int xhci_interrupt_start(int p, uint8_t addr) {
    struct port *P = &ports[p];
    uint8_t dci = dci_of(addr);
    struct ep *e = &P->ep[dci];
    if (!P->attached || !e->used || !e->periodic) return -EINVAL;
    ring_push(&e->ring, e->buf_pa, e->mps, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    barrier();
    ring_db(P->slot, dci);
    return 0;
}

/* ---- device contexts ---- */
static uint32_t *ctx_at(uint8_t *base, int idx) { return (uint32_t *)(base + (unsigned)idx * ctx_sz); }

static void write_ep_ctx(uint8_t *ictx, int dci, uint32_t type, uint32_t mps, uint32_t interval, uint64_t trdp, uint32_t avg) {
    uint32_t *c = ctx_at(ictx, dci + 1);
    c[0] = interval << 16;
    c[1] = (3u << 1) | (type << 3) | (mps << 16);             /* CErr = 3 */
    c[2] = (uint32_t)trdp; c[3] = (uint32_t)(trdp >> 32);
    c[4] = avg;
}

static uint32_t ep_interval(int speed, uint8_t bint, int intr) {
    if (!intr) return 0;
    if (speed == USB_HS || speed == USB_SS) return bint ? (uint32_t)(bint - 1) : 0;   /* exponent */
    if (bint < 1) bint = 1;                                                           /* FS/LS: frames */
    uint32_t n = 0;
    while ((2u << n) <= bint) n++;
    n += 3;
    return n > 15 ? 15 : n;
}

static void ep_setup(struct port *P, struct ep *e, int dci, uint8_t addr, uint32_t mps, int periodic) {
    if (!e->ring.v) ring_reset(&e->ring, pmm_alloc());
    else ring_reset(&e->ring, e->ring.pa);
    if (periodic && !e->buf) { e->buf_pa = pmm_alloc(); e->buf = P2V(e->buf_pa); }
    e->used = 1; e->periodic = periodic; e->dci = dci; e->addr = addr; e->mps = mps;
    e->seq = 0; e->code = 0;
    (void)P;
}

/* ---- attach / detach ---- */
static void port_alloc(struct port *P) {
    P->devctx_pa = pmm_alloc(); P->devctx = P2V(P->devctx_pa);
    P->inctx_pa = pmm_alloc(); P->inctx = P2V(P->inctx_pa);
    P->dma_pa = pmm_alloc(); P->dma = P2V(P->dma_pa);
    for (int i = 0; i < BULK_PAGES; i++) { P->bulk_pa[i] = pmm_alloc(); P->bulk[i] = P2V(P->bulk_pa[i]); }
}

static int port_reset(int p) {
    uint32_t sc = port_sc(p);
    port_wr(p, (sc & PORT_PP) | PORT_PR);
    for (long i = 0; i < WAIT_LIMIT && (port_sc(p) & PORT_PR); i++) { }
    sc = port_sc(p);
    if (!(sc & PORT_CCS)) return -ENODEV;
    if (!(sc & PORT_PED)) return -EIO;
    port_wr(p, (sc & PORT_PP) | PORT_PRC);                    /* acknowledge the reset */
    return 0;
}

int xhci_set_ep0_mps(int p, uint16_t mps) {
    struct port *P = &ports[p];
    if (!P->attached) return -ENODEV;
    uint32_t *icc = (uint32_t *)P->inctx;
    icc[0] = 0; icc[1] = 1u << 1;                             /* add EP0 only */
    write_ep_ctx(P->inctx, 1, 4, mps, 0, P->ep[1].ring.pa | P->ep[1].ring.cycle, 8);
    return cmd_sync((TRB_EVALUATE_CTX << 10) | (uint32_t)P->slot << 24, P->inctx_pa, 0);
}

int xhci_configure(int p, const struct usb_ep *eps, int n) {
    struct port *P = &ports[p];
    if (!P->attached) return -ENODEV;
    uint32_t add = 1u, maxdci = 1;
    uint32_t *icc = (uint32_t *)P->inctx;
    icc[0] = 0;
    for (int i = 0; i < n; i++) {
        uint8_t dci = dci_of(eps[i].addr);
        int type = eps[i].attr & 3;
        int intr = type == 3, bulk = type == 2;
        if (dci < 2 || dci >= MAX_ENDPT || !(intr || bulk)) continue;
        struct ep *e = &P->ep[dci];
        ep_setup(P, e, dci, eps[i].addr, eps[i].mps, intr);
        uint32_t ctype = intr ? ((eps[i].addr & 0x80) ? 7 : 3) : ((eps[i].addr & 0x80) ? 6 : 2);
        write_ep_ctx(P->inctx, dci, ctype, eps[i].mps, ep_interval(P->speed, eps[i].interval, intr),
                     e->ring.pa | e->ring.cycle, intr ? eps[i].mps : 3072);
        add |= 1u << dci;
        if (dci > maxdci) maxdci = dci;
    }
    uint32_t *slot = ctx_at(P->inctx, 1);
    slot[0] = (uint32_t)P->speed << 20 | maxdci << 27;
    slot[1] = (uint32_t)p << 16;
    icc[1] = add;
    return cmd_sync((TRB_CONFIGURE_EP << 10) | (uint32_t)P->slot << 24, P->inctx_pa, 0);
}

static void detach_port(int p) {
    struct port *P = &ports[p];
    usb_detach(p);
    for (int d = 1; d < MAX_ENDPT; d++) P->ep[d].used = 0;
    if (P->slot) {
        cmd_sync((TRB_DISABLE_SLOT << 10) | (uint32_t)P->slot << 24, 0, 0);
        dcbaa[P->slot] = 0;
        slot_port[P->slot] = 0;
        P->slot = 0;
    }
    P->attached = 0;
}

static int attach_port(int p) {
    struct port *P = &ports[p];
    if (!P->devctx) port_alloc(P);
    if (port_reset(p)) return -ENODEV;
    P->speed = PORT_SPEED(port_sc(p));
    if (P->speed < USB_FS || P->speed > USB_SS) return -ENODEV;
    uint32_t slot = 0;
    if (cmd_sync(TRB_ENABLE_SLOT << 10, 0, &slot) || !slot || slot > 255) return -EIO;
    P->slot = (int)slot;
    slot_port[slot] = (uint8_t)p;
    memset(P->devctx, 0, PAGE_SIZE);
    memset(P->inctx, 0, PAGE_SIZE);
    dcbaa[slot] = P->devctx_pa;
    uint32_t mps0 = P->speed == USB_SS ? 512 : P->speed == USB_HS ? 64 : 8;
    ep_setup(P, &P->ep[1], 1, 0, mps0, 0);
    uint32_t *icc = (uint32_t *)P->inctx;
    icc[1] = 0x3;                                             /* slot + EP0 */
    uint32_t *sl = ctx_at(P->inctx, 1);
    sl[0] = (uint32_t)P->speed << 20 | 1u << 27;
    sl[1] = (uint32_t)p << 16;
    write_ep_ctx(P->inctx, 1, 4, mps0, 0, P->ep[1].ring.pa | P->ep[1].ring.cycle, 8);
    if (cmd_sync((TRB_ADDRESS_DEVICE << 10) | (uint32_t)P->slot << 24, P->inctx_pa, 0)) {
        cmd_sync((TRB_DISABLE_SLOT << 10) | (uint32_t)P->slot << 24, 0, 0);
        dcbaa[slot] = 0; slot_port[slot] = 0; P->slot = 0;
        return -EIO;
    }
    P->attached = 1;
    usb_attach(p, P->speed);
    return 0;
}

/* ---- root-port scan, called from the timer tick ---- */
static void poll(void) {
    if (!ready) return;
    process_events();
    for (int p = 1; p <= nports && p <= USB_MAX_PORTS; p++) {
        struct port *P = &ports[p];
        uint32_t sc = port_sc(p);
        if (sc & PORT_CSC) { port_wr(p, (sc & PORT_PP) | PORT_CSC); P->tries = 0; }   /* a new connection event resets the retry budget */
        if (!(sc & PORT_CCS)) { if (P->attached) { detach_port(p); kprintf("[usb] port %d: disconnected\n", p); } continue; }
        if (P->attached || P->tries >= 3 || ticks < P->next_try) continue;
        P->tries++; P->next_try = ticks + 200;
        int r = attach_port(p);
        if (r) kprintf("[usb] port %d: enumeration failed (%d), retry %d\n", p, r, P->tries);
    }
    usb_poll();
}

/* ---- controller bring-up ---- */
static int probe(void) { struct pci_dev d; return pci_find_class(0x0C0330, &d) ? -1 : 0; }

static int init(void) {
    struct pci_dev d;
    if (pci_find_class(0x0C0330, &d)) return -1;
    uint32_t cmd = pci_read32(d.bus, d.dev, d.fn, 0x04);
    pci_write32(d.bus, d.dev, d.fn, 0x04, (cmd & 0xFFFF) | 0x6);          /* memory space + bus master */
    uint32_t lo = pci_read32(d.bus, d.dev, d.fn, 0x10), hi = pci_read32(d.bus, d.dev, d.fn, 0x14);
    if (hi) { kprintf("xhci: controller BAR above 4 GiB (not supported)\n"); return -1; }
    uintptr_t base = lo & ~0xFu;
    if (!base) return -1;
    cap = P2V(base);
    uint8_t caplen = *(volatile uint8_t *)cap;
    op = cap + caplen;
    rt = cap + (r32(cap, CAP_RTSOFF) & ~0x1Fu);
    db = cap + (r32(cap, CAP_DBOFF) & ~0x3u);
    uint32_t hcs1 = r32(cap, CAP_HCSPARAMS1), hcs2 = r32(cap, CAP_HCSPARAMS2), hcc1 = r32(cap, CAP_HCCPARAMS1);
    int slots = hcs1 & 0xFF;
    nports = hcs1 >> 24;
    ctx_sz = (hcc1 & 4) ? 64 : 32;
    if (!slots || !nports) return -1;
    /* halt, then reset */
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~USBCMD_RS);
    for (long i = 0; i < WAIT_LIMIT && !(r32(op, OP_USBSTS) & USBSTS_HCH); i++) { }
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) | USBCMD_HCRST);
    for (long i = 0; i < WAIT_LIMIT && ((r32(op, OP_USBCMD) & USBCMD_HCRST) || (r32(op, OP_USBSTS) & USBSTS_CNR)); i++) { }
    w32(op, OP_CONFIG, (uint32_t)slots);
    uintptr_t dpa = pmm_alloc(); dcbaa_pa = dpa; dcbaa = P2V(dpa);
    /* scratchpad buffers the controller may require */
    uint32_t nscratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nscratch) {
        uintptr_t arr = pmm_alloc();
        uint64_t *a = P2V(arr);
        for (uint32_t i = 0; i < nscratch && i < PAGE_SIZE / 8; i++) a[i] = pmm_alloc();
        dcbaa[0] = arr;
    }
    ring_reset(&cmd_ring, pmm_alloc());
    w64(op, OP_CRCR, cmd_ring.pa | 1);                          /* RCS = 1 */
    ring_reset(&evt_ring, pmm_alloc());
    uintptr_t erst = pmm_alloc();
    uint8_t *e = P2V(erst);
    *(uint64_t *)e = evt_ring.pa; *(uint32_t *)(e + 8) = RING_TRBS;
    w32(rt, IR0 + IR_ERSTSZ, 1);
    w64(rt, IR0 + IR_ERSTBA, erst);
    w64(rt, IR0 + IR_ERDP, evt_ring.pa);
    w64(op, OP_DCBAAP, dcbaa_pa);
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) | USBCMD_RS);
    for (long i = 0; i < WAIT_LIMIT && (r32(op, OP_USBSTS) & USBSTS_HCH); i++) { }
    if (r32(op, OP_USBSTS) & USBSTS_HCH) { kprintf("xhci: controller did not start\n"); return -1; }
    for (int p = 1; p <= nports && p <= USB_MAX_PORTS; p++)
        if (!(port_sc(p) & PORT_PP)) port_wr(p, PORT_PP);     /* power the port */
    ready = 1;
    kprintf("xhci: %d ports, %d slots, %u-byte contexts at %lx\n", nports, slots, ctx_sz, (unsigned long)base);
    return 0;
}

static struct kdriver drv = { "xhci", "USB host controller (xHCI): keyboards, mice, mass storage", DRV_BUS, 5, probe, init, poll, 0 };
KDRIVER(drv);
