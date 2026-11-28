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
    uint64_t pending;
    uint64_t short_ptr;
    uint32_t short_resid;
    int short_seen;
    uint32_t seq; int code; uint64_t ptr; uint32_t resid;   /* last transfer event seen on this endpoint */
};
struct port {
    int attached, slot, speed, tries, faulted, quarantined;
    uint64_t next_try;
    uintptr_t devctx_pa, inctx_pa;
    uint8_t *devctx, *inctx;
    uintptr_t dma_pa; uint8_t *dma;                       /* control data, one page */
    uintptr_t bulk_pa[BULK_PAGES]; uint8_t *bulk[BULK_PAGES];
    uint64_t bulk_trb[BULK_PAGES];                        /* TRB addresses of the last bulk TD */
    struct ep ep[MAX_ENDPT];
};

static volatile uint8_t *cap, *op, *rt, *db;
static int ctx_sz, nports, slot_limit, ready;
static int controller_fault;
static uintptr_t dcbaa_pa; static uint64_t *dcbaa;
static struct ring cmd_ring, evt_ring;
static struct port ports[USB_MAX_PORTS + 1];          /* 1-based root port numbers */
static uint8_t slot_port[256];                        /* slot id -> root port (0 = none) */
static volatile int cmd_done; static int cmd_code; static uint32_t cmd_slot;
static uint64_t cmd_pending;

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
    if (!e->used || (e->periodic && param != e->pending)) return;
    if (e->short_ptr && param == e->short_ptr && (status >> 24) == 13) {
        e->short_seen = 1; e->short_resid = status & 0xFFFFFF;
        return;
    }
    e->seq++; e->code = (int)(status >> 24); e->ptr = param; e->resid = status & 0xFFFFFF;
    if (e->periodic && P->attached && !P->faulted) {
        if ((e->code != 1 && e->code != 13) || e->resid > e->mps) { P->faulted = 1; return; }
        usb_report(p, e->addr, e->buf, e->mps - e->resid);
        e->pending = ring_push(&e->ring, e->buf_pa, e->mps, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
        ring_db(P->slot, dci);                              /* keep one report outstanding */
    }
}

static void process_events(void) {
    unsigned n = 0;
    while (n < RING_TRBS) {
        struct trb *t = &evt_ring.v[evt_ring.enq];
        uint32_t c = t->control;
        if ((c & 1) != evt_ring.cycle) break;
        barrier();
        uint64_t param = t->param; uint32_t status = t->status;
        uint32_t type = (c >> 10) & 0x3F;
        if (type == EVT_TRANSFER) on_transfer(status, c, param);
        else if (type == EVT_COMMAND && param == cmd_pending) { cmd_code = (int)(status >> 24); cmd_slot = c >> 24; cmd_done = 1; }
        if (++evt_ring.enq == RING_TRBS) { evt_ring.enq = 0; evt_ring.cycle ^= 1; }
        n++;
    }
    if (n) w64(rt, IR0 + IR_ERDP, ((uint64_t)evt_ring.pa + 16u * evt_ring.enq) | 8);   /* EHB: event handled */
}

/* ---- synchronous commands and transfers ---- */
static int cmd_sync(uint32_t control, uint64_t param, uint32_t *slot_out) {
    if (!ready) return -EIO;
    cmd_done = 0;
    cmd_pending = ring_push(&cmd_ring, param, 0, control);
    barrier();
    ring_db(0, 0);
    for (long i = 0; i < WAIT_LIMIT && !cmd_done; i++) process_events();
    if (!cmd_done) { ready = 0; controller_fault = 1; return -ETIMEDOUT; }
    if (slot_out) *slot_out = cmd_slot;
    return cmd_code == 1 ? 0 : -EIO;
}

/* after a failed transfer the endpoint is halted: reset it and move its dequeue pointer past the failed TD */
static void recover_ep(struct port *P, struct ep *e) {
    uint32_t id = (uint32_t)P->slot << 24 | (uint32_t)e->dci << 16;
    if (cmd_sync((TRB_RESET_EP << 10) | id, 0, 0) ||
        cmd_sync((TRB_SET_TR_DEQUEUE << 10) | id, (uint64_t)(e->ring.pa + 16u * e->ring.enq) | e->ring.cycle, 0))
        P->faulted = 1;
}

static int wait_xfer(struct port *P, struct ep *e, uint32_t seq0) {
    for (long i = 0; i < WAIT_LIMIT; i++) {
        process_events();
        if (e->seq != seq0) return 0;
    }
    P->faulted = 1; /* Keep DMA buffers until the slot has been disabled. */
    return -ETIMEDOUT;
}

static uint8_t dci_of(uint8_t addr) { return (uint8_t)((addr & 0x7F) * 2 + ((addr & 0x80) ? 1 : 0)); }

int xhci_control(int p, const struct usb_setup *s, void *buf) {
    if (p < 1 || p > USB_MAX_PORTS || !s) return -EINVAL;
    struct port *P = &ports[p];
    struct ep *e = &P->ep[1];
    if (!ready || P->faulted || !P->attached || !e->used) return -ENODEV;
    int in = s->bmRequestType & 0x80;
    uint16_t len = s->wLength;
    if (len > PAGE_SIZE || (len && !buf)) return -EINVAL;
    if (len && !in) memcpy(P->dma, buf, len);
    uint64_t pkt; memcpy(&pkt, s, 8);
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(&e->ring, pkt, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    uint64_t data = 0;
    if (len) data = ring_push(&e->ring, P->dma_pa, len, (TRB_DATA << 10) | (in ? (1u << 16) | (1u << 2) : 0));
    uint64_t st = ring_push(&e->ring, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));
    e->short_ptr = in ? data : 0; e->short_seen = 0;
    uint32_t seq0 = e->seq;
    barrier();
    ring_db(P->slot, 1);
    int r = wait_xfer(P, e, seq0);
    if (r) return r;
    uint32_t actual = len;
    e->short_ptr = 0;
    if (e->short_seen) {
        if (e->short_resid > len) { P->faulted = 1; return -EIO; }
        actual -= e->short_resid;
    }
    if (e->code != 1 || e->ptr != st) { recover_ep(P, e); return -EIO; }
    if (actual && in) memcpy(buf, P->dma, actual);
    return (int)actual;
}

int xhci_bulk(int p, uint8_t addr, void *buf, uint32_t len) {
    if (p < 1 || p > USB_MAX_PORTS || !buf) return -EINVAL;
    struct port *P = &ports[p];
    uint8_t dci = dci_of(addr);
    if (dci < 2 || dci >= MAX_ENDPT) return -EINVAL;
    struct ep *e = &P->ep[dci];
    if (!ready || P->faulted || !P->attached || !e->used || !len || len > BULK_PAGES * PAGE_SIZE) return -EINVAL;
    int in = addr & 0x80;
    for (uint32_t page = 0; page < (len + PAGE_SIZE - 1) / PAGE_SIZE; page++) {
        if (P->bulk[page]) continue;
        P->bulk_pa[page] = pmm_alloc();
        if (!P->bulk_pa[page]) return -ENOMEM;
        P->bulk[page] = P2V(P->bulk_pa[page]);
    }
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
    /* Match the event to this TD before trusting its remaining-byte count. */
    uint32_t done = 0; int found = 0;
    for (int k = 0; k < i; k++)
        if (P->bulk_trb[k] == e->ptr) {
            if (e->resid > len) { P->faulted = 1; return -EIO; }
            done = len - e->resid;
            found = 1; break;
        }
    if (!found) { P->faulted = 1; return -EIO; }
    if (in) for (uint32_t off = 0; off < done; off += PAGE_SIZE)
        memcpy((uint8_t *)buf + off, P->bulk[off / PAGE_SIZE], done - off < PAGE_SIZE ? done - off : PAGE_SIZE);
    return (int)done;
}

int xhci_interrupt_start(int p, uint8_t addr) {
    if (p < 1 || p > USB_MAX_PORTS) return -EINVAL;
    struct port *P = &ports[p];
    uint8_t dci = dci_of(addr);
    if (dci < 2 || dci >= MAX_ENDPT) return -EINVAL;
    struct ep *e = &P->ep[dci];
    if (!ready || P->faulted || !P->attached || !e->used || !e->periodic || e->pending) return -EINVAL;
    e->pending = ring_push(&e->ring, e->buf_pa, e->mps, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
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
    c[4] = avg | ((type == 3 || type == 7) ? mps << 16 : 0);
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

static int ep_setup(struct port *P, struct ep *e, int dci, uint8_t addr, uint32_t mps, int periodic) {
    if (!e->ring.v) {
        uintptr_t pa = pmm_alloc();
        if (!pa) return -ENOMEM;
        ring_reset(&e->ring, pa);
    }
    else ring_reset(&e->ring, e->ring.pa);
    if (periodic && !e->buf) { e->buf_pa = pmm_alloc(); if (!e->buf_pa) return -ENOMEM; e->buf = P2V(e->buf_pa); }
    e->used = 1; e->periodic = periodic; e->dci = dci; e->addr = addr; e->mps = mps;
    e->seq = 0; e->code = 0; e->pending = 0; e->short_ptr = 0; e->short_seen = 0;
    (void)P;
    return 0;
}

/* ---- attach / detach ---- */
static int port_alloc(struct port *P) {
    if (!P->devctx) { P->devctx_pa = pmm_alloc(); if (!P->devctx_pa) return -ENOMEM; P->devctx = P2V(P->devctx_pa); }
    if (!P->inctx) { P->inctx_pa = pmm_alloc(); if (!P->inctx_pa) return -ENOMEM; P->inctx = P2V(P->inctx_pa); }
    if (!P->dma) { P->dma_pa = pmm_alloc(); if (!P->dma_pa) return -ENOMEM; P->dma = P2V(P->dma_pa); }
    return 0;
}

static int port_reset(int p) {
    uint32_t sc = port_sc(p);
    port_wr(p, (sc & PORT_PP) | PORT_PR);
    for (long i = 0; i < WAIT_LIMIT && (port_sc(p) & PORT_PR); i++) { }
    sc = port_sc(p);
    if (!(sc & PORT_CCS)) return -ENODEV;
    if ((sc & PORT_PR) || !(sc & PORT_PED)) return -EIO;
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
        if (!eps[i].mps || (intr && eps[i].mps > PAGE_SIZE)) return -EINVAL;
        struct ep *e = &P->ep[dci];
        if (ep_setup(P, e, dci, eps[i].addr, eps[i].mps, intr)) return -ENOMEM;
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
    P->attached = 0;
    usb_detach(p);
    for (int d = 1; d < MAX_ENDPT; d++) P->ep[d].used = 0;
    if (P->slot) {
        if (!ready || cmd_sync((TRB_DISABLE_SLOT << 10) | (uint32_t)P->slot << 24, 0, 0)) {
            P->quarantined = 1; /* Hardware may still own this port's DMA memory. */
            return;
        }
        dcbaa[P->slot] = 0;
        slot_port[P->slot] = 0;
        P->slot = 0;
    }
    /* Disable Slot completed; the controller has relinquished these frames. */
    for (int d = 1; d < MAX_ENDPT; d++) {
        if (P->ep[d].buf_pa) pmm_free(P->ep[d].buf_pa);
        if (P->ep[d].ring.pa) pmm_free(P->ep[d].ring.pa);
    }
    for (int i = 0; i < BULK_PAGES; i++)
        if (P->bulk_pa[i]) pmm_free(P->bulk_pa[i]);
    if (P->devctx_pa) pmm_free(P->devctx_pa);
    if (P->inctx_pa) pmm_free(P->inctx_pa);
    if (P->dma_pa) pmm_free(P->dma_pa);
    int tries = P->tries;
    uint64_t next_try = P->next_try;
    memset(P, 0, sizeof *P);
    P->tries = tries;
    P->next_try = next_try;
}

static int attach_port(int p) {
    struct port *P = &ports[p];
    if (port_reset(p)) return -ENODEV;
    P->speed = PORT_SPEED(port_sc(p));
    if (P->speed < USB_FS || P->speed > USB_SS) return -ENODEV;
    uint32_t slot = 0;
    if (cmd_sync(TRB_ENABLE_SLOT << 10, 0, &slot)) return -EIO;
    if (!slot || slot > (unsigned)slot_limit || slot_port[slot]) {
        ready = 0; controller_fault = 1;
        return -EIO;
    }
    P->slot = (int)slot;
    slot_port[slot] = (uint8_t)p;
    if (port_alloc(P)) { detach_port(p); return -ENOMEM; }
    memset(P->devctx, 0, PAGE_SIZE);
    memset(P->inctx, 0, PAGE_SIZE);
    dcbaa[slot] = P->devctx_pa;
    uint32_t mps0 = P->speed == USB_SS ? 512 : P->speed == USB_HS ? 64 : 8;
    if (ep_setup(P, &P->ep[1], 1, 0, mps0, 0)) { detach_port(p); return -ENOMEM; }
    uint32_t *icc = (uint32_t *)P->inctx;
    icc[1] = 0x3;                                             /* slot + EP0 */
    uint32_t *sl = ctx_at(P->inctx, 1);
    sl[0] = (uint32_t)P->speed << 20 | 1u << 27;
    sl[1] = (uint32_t)p << 16;
    write_ep_ctx(P->inctx, 1, 4, mps0, 0, P->ep[1].ring.pa | P->ep[1].ring.cycle, 8);
    if (cmd_sync((TRB_ADDRESS_DEVICE << 10) | (uint32_t)P->slot << 24, P->inctx_pa, 0)) {
        detach_port(p);
        return -EIO;
    }
    P->attached = 1;
    int r = usb_attach(p, P->speed);
    if (r || P->faulted || !(port_sc(p) & PORT_CCS)) {
        detach_port(p);
        return r ? r : -EIO;
    }
    return 0;
}

/* ---- root-port scan, called from the timer tick ---- */
static void poll(void) {
    if (controller_fault) {
        controller_fault = 0;
        for (int p = 1; p <= nports && p <= USB_MAX_PORTS; p++) {
            if (ports[p].attached || ports[p].slot) detach_port(p);
        }
    }
    if (!ready) return;
    process_events();
    for (int p = 1; p <= nports && p <= USB_MAX_PORTS; p++) {
        struct port *P = &ports[p];
        uint32_t sc = port_sc(p);
        if (sc & PORT_CSC) {
            port_wr(p, (sc & PORT_PP) | PORT_CSC);
            if (P->attached || P->slot) {
                detach_port(p);
                if (!(sc & PORT_CCS)) kprintf("[usb] port %d: disconnected\n", p);
            }
            P->tries = 0; P->next_try = 0;
        }
        if (!(sc & PORT_CCS)) { P->tries = 0; P->next_try = 0; if ((P->attached || P->slot) && !P->quarantined) { detach_port(p); kprintf("[usb] port %d: disconnected\n", p); } continue; }
        if (P->faulted && P->slot && !P->quarantined) detach_port(p);
        if (!ready) return;
        if (P->quarantined || P->attached || P->tries >= 3 || ticks < P->next_try) continue;
        P->tries++; P->next_try = ticks + 200;
        int r = attach_port(p);
        if (r) kprintf("[usb] port %d: enumeration failed (%d), retry %d\n", p, r, P->tries);
    }
    usb_poll();
}

/* ---- controller bring-up ---- */
static int probe(void) { struct pci_dev d; return pci_find_class(0x0C0330, &d) ? -1 : 0; }

static int bar_window(uint64_t size, uint64_t off, uint64_t len) {
    return off <= size && len <= size - off;
}

static int init(void) {
    struct pci_dev d;
    if (pci_find_class(0x0C0330, &d)) return -1;
    uint32_t cmd = pci_read32(d.bus, d.dev, d.fn, 0x04) & 0xFFFF;
    uint32_t lo = pci_read32(d.bus, d.dev, d.fn, 0x10);
    unsigned type = (lo >> 1) & 3;
    if ((lo & 1) || (type != 0 && type != 2)) return -1;
    uint32_t hi = type == 2 ? pci_read32(d.bus, d.dev, d.fn, 0x14) : 0;
    pci_write32(d.bus, d.dev, d.fn, 0x04, cmd & ~6u);
    pci_write32(d.bus, d.dev, d.fn, 0x10, ~0u);
    if (type == 2) pci_write32(d.bus, d.dev, d.fn, 0x14, ~0u);
    uint32_t masklo = pci_read32(d.bus, d.dev, d.fn, 0x10);
    uint32_t maskhi = type == 2 ? pci_read32(d.bus, d.dev, d.fn, 0x14) : ~0u;
    if (type == 2) pci_write32(d.bus, d.dev, d.fn, 0x14, hi);
    pci_write32(d.bus, d.dev, d.fn, 0x10, lo);
    uint64_t base = ((uint64_t)hi << 32) | (lo & ~0xFu);
    uint64_t mask = ((uint64_t)maskhi << 32) | (masklo & ~0xFu);
    uint64_t size = ~mask + 1;
    if (!base || !size || (size & (size - 1)) || (base & (size - 1)) ||
        base > UINTPTR_MAX - size || !bar_window(size, 0, 0x20)) return -1;
    pci_write32(d.bus, d.dev, d.fn, 0x04, (cmd & ~4u) | 2u);
    if (arch_map_device((uintptr_t)base, 0x20)) return -1;
    cap = P2V(base);
    uint8_t caplen = *(volatile uint8_t *)cap;
    uint32_t rtoff = r32(cap, CAP_RTSOFF) & ~0x1Fu;
    uint32_t dboff = r32(cap, CAP_DBOFF) & ~0x3u;
    uint32_t hcs1 = r32(cap, CAP_HCSPARAMS1), hcs2 = r32(cap, CAP_HCSPARAMS2), hcc1 = r32(cap, CAP_HCCPARAMS1);
    int slots = hcs1 & 0xFF;
    nports = hcs1 >> 24;
    ctx_sz = (hcc1 & 4) ? 64 : 32;
    if (!slots || !nports || caplen < 0x20 ||
        !bar_window(size, caplen, OP_PORT + 0x10u * (unsigned)nports) ||
        !bar_window(size, rtoff, IR0 + 0x20) ||
        !bar_window(size, dboff, 4u * ((unsigned)slots + 1)) ||
        arch_map_device((uintptr_t)base + caplen, OP_PORT + 0x10u * (unsigned)nports) ||
        arch_map_device((uintptr_t)base + rtoff, IR0 + 0x20) ||
        arch_map_device((uintptr_t)base + dboff, 4u * ((unsigned)slots + 1))) return -1;
    op = cap + caplen;
    rt = cap + rtoff;
    db = cap + dboff;
    /* halt, then reset */
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) & ~USBCMD_RS);
    for (long i = 0; i < WAIT_LIMIT && !(r32(op, OP_USBSTS) & USBSTS_HCH); i++) { }
    if (!(r32(op, OP_USBSTS) & USBSTS_HCH)) return -1;
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) | USBCMD_HCRST);
    for (long i = 0; i < WAIT_LIMIT && ((r32(op, OP_USBCMD) & USBCMD_HCRST) || (r32(op, OP_USBSTS) & USBSTS_CNR)); i++) { }
    if ((r32(op, OP_USBCMD) & USBCMD_HCRST) || (r32(op, OP_USBSTS) & USBSTS_CNR)) return -1;
    if (!(r32(op, 0x08) & 1)) return -1; /* PAGESIZE: controller must accept 4 KiB pages */
    uintptr_t dpa = 0, arr = 0, cmd_pa = 0, evt_pa = 0, erst = 0;
    uint32_t allocated = 0;
    uint64_t *a = 0;
    dpa = pmm_alloc();
    if (!dpa) goto alloc_fail;
    dcbaa_pa = dpa; dcbaa = P2V(dpa);
    /* scratchpad buffers the controller may require */
    uint32_t nscratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nscratch) {
        if (nscratch > PAGE_SIZE / 8) goto alloc_fail;
        arr = pmm_alloc();
        if (!arr) goto alloc_fail;
        a = P2V(arr);
        for (; allocated < nscratch; allocated++) {
            a[allocated] = pmm_alloc();
            if (!a[allocated]) goto alloc_fail;
        }
        dcbaa[0] = arr;
    }
    cmd_pa = pmm_alloc();
    if (!cmd_pa) goto alloc_fail;
    ring_reset(&cmd_ring, cmd_pa);
    evt_pa = pmm_alloc();
    if (!evt_pa) goto alloc_fail;
    ring_reset(&evt_ring, evt_pa);
    erst = pmm_alloc();
    if (!erst) goto alloc_fail;
    uint8_t *e = P2V(erst);
    *(uint64_t *)e = evt_ring.pa; *(uint32_t *)(e + 8) = RING_TRBS;
    w32(op, OP_CONFIG, (uint32_t)slots);
    w64(op, OP_CRCR, cmd_ring.pa | 1);
    w32(rt, IR0 + IR_ERSTSZ, 1);
    w64(rt, IR0 + IR_ERSTBA, erst);
    w64(rt, IR0 + IR_ERDP, evt_ring.pa);
    w64(op, OP_DCBAAP, dcbaa_pa);
    barrier();
    pci_write32(d.bus, d.dev, d.fn, 0x04, cmd | 6u);
    w32(op, OP_USBCMD, r32(op, OP_USBCMD) | USBCMD_RS);
    for (long i = 0; i < WAIT_LIMIT && (r32(op, OP_USBSTS) & USBSTS_HCH); i++) { }
    if (r32(op, OP_USBSTS) & USBSTS_HCH) {
        /* DMA ownership is uncertain after starting, so retain the frames. */
        pci_write32(d.bus, d.dev, d.fn, 0x04, (cmd & ~4u) | 2u);
        kprintf("xhci: controller did not start\n"); return -1;
    }
    for (int p = 1; p <= nports && p <= USB_MAX_PORTS; p++)
        if (!(port_sc(p) & PORT_PP)) port_wr(p, PORT_PP);     /* power the port */
    ready = 1;
    slot_limit = slots;
    kprintf("xhci: %d ports, %d slots, %u-byte contexts at %lx\n", nports, slots, ctx_sz, (unsigned long)base);
    return 0;
alloc_fail:
    if (erst) pmm_free(erst);
    if (evt_pa) pmm_free(evt_pa);
    if (cmd_pa) pmm_free(cmd_pa);
    while (allocated) pmm_free(a[--allocated]);
    if (arr) pmm_free(arr);
    if (dpa) pmm_free(dpa);
    dcbaa_pa = 0; dcbaa = 0;
    memset(&cmd_ring, 0, sizeof cmd_ring);
    memset(&evt_ring, 0, sizeof evt_ring);
    return -ENOMEM;
}

static struct kdriver drv = { "xhci", "USB host controller (xHCI): keyboards, mice, mass storage", DRV_BUS, 5, probe, init, poll, 0 };
KDRIVER(drv);
