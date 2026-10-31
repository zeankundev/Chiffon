/* Raspberry Pi Zero 2W: USB OTG (DesignWare DWC2) device controller -> CDC-ACM serial console.
 *
 * The board's micro-USB data port enumerates as a CDC-ACM modem (USB class 02/02). Windows binds
 * its built-in usbser.sys and shows a COMx port; Linux shows /dev/ttyACMn. No USB-UART adapter.
 *
 * Design, kept deliberately small:
 *  - slave mode (the CPU moves data through the FIFOs), no DMA, USB interrupts masked. The driver
 *    is polled from the timer tick (KDRIVER poll hook, called via console_poll()). Throughput is
 *    therefore bounded by HZ, which is fine for a shell and a boot log.
 *  - full speed, 64-byte packets.  EP0 control, EP1 IN interrupt (0x81, notifications unused),
 *    EP2 IN bulk (0x82), EP2 OUT bulk (0x02).
 *  - Hardware is touched only from usb_init() and usb_poll(). The console hooks only move bytes
 *    through the software rings, with IRQs masked around ring updates, so kprintf() is safe from
 *    any context. Output produced before the host opens the port is kept (last TXQ bytes) and
 *    sent when it does. */
#include "kernel.h"
#include "console.h"
#include "driver.h"

#define USB_BASE   0x3F980000UL          /* BCM2837 USB OTG, peripheral bus (ARM view 0x3F...) */
#define FIFO_BASE  0x1000u               /* FIFO n is at FIFO_BASE + n * 0x1000 */

/* core registers */
#define GAHBCFG     0x008
#define GUSBCFG     0x00C
#define GRSTCTL     0x010
#define GINTSTS     0x014
#define GINTMSK     0x018
#define GRXSTSP     0x020
#define GRXFSIZ     0x024
#define GNPTXFSIZ   0x028
#define GNPTXSTS    0x02C
#define DIEPTXF(n)  (0x100u + 4u * (n))  /* periodic/IN FIFO size, n = 1..15 */
/* device registers */
#define DCFG        0x800
#define DCTL        0x804
#define DIEPCTL(n)  (0x900u + 0x20u * (n))
#define DIEPINT(n)  (0x908u + 0x20u * (n))
#define DIEPTSIZ(n) (0x910u + 0x20u * (n))
#define DTXFSTS(n)  (0x918u + 0x20u * (n))
#define DOEPCTL(n)  (0xB00u + 0x20u * (n))
#define DOEPTSIZ(n) (0xB10u + 0x20u * (n))

#define GRSTCTL_CSRST    (1u << 0)
#define GRSTCTL_RXFFLSH  (1u << 4)
#define GRSTCTL_TXFFLSH  (1u << 5)
#define GRSTCTL_AHBIDL   (1u << 31)
#define GINT_RXFLVL      (1u << 4)
#define GINT_USBRST      (1u << 12)
#define GINT_ENUMDONE    (1u << 13)
#define DCTL_SFTDISCON   (1u << 1)
#define EP_EPENA         (1u << 31)
#define EP_CNAK          (1u << 26)
#define EP_STALL         (1u << 21)
#define EP_USBAEP        (1u << 15)
#define EP_SETD0         (1u << 28)
#define EP_TYPE(t)       ((uint32_t)(t) << 18)   /* 2 = bulk, 3 = interrupt */
#define EP_TXFNUM(n)     ((uint32_t)(n) << 22)
#define TSIZ_ONE_PKT     (1u << 19)              /* PKTCNT = 1 */
#define INT_XFERC        1u

#define TXQ 8192                                 /* console output backlog (power of two) */
#define RXQ 1024                                 /* console input backlog */

static uint64_t usb_hz = 19200000;               /* CNTFRQ, read in usb_init() */

static inline uint64_t ctr(void) { uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static inline uint64_t us_ticks(unsigned us) { return (uint64_t)us * usb_hz / 1000000u; }
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, daif" : "=r"(f));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) { __asm__ volatile("msr daif, %0" :: "r"(f) : "memory"); }

static inline uint32_t rd(unsigned off) { return *(volatile uint32_t *)P2V(USB_BASE + off); }
static inline void wr(unsigned off, uint32_t v) { *(volatile uint32_t *)P2V(USB_BASE + off) = v; }
static inline volatile uint32_t *fifo(unsigned ep) { return (volatile uint32_t *)P2V(USB_BASE + FIFO_BASE + 0x1000u * ep); }

static void delay_us(unsigned us) { uint64_t e = ctr() + us_ticks(us); while (ctr() < e) { } }

/* wait until (reg & mask) == want; returns 1 on success, 0 on timeout */
static int wait_bits(unsigned off, uint32_t mask, uint32_t want, unsigned us) {
    uint64_t e = ctr() + us_ticks(us);
    while ((rd(off) & mask) != want)
        if (ctr() > e) return 0;
    return 1;
}

static void flush_txfifo(unsigned txnum) {       /* txnum 0x10 = all TX FIFOs */
    wr(GRSTCTL, (txnum << 6) | GRSTCTL_TXFFLSH);
    wait_bits(GRSTCTL, GRSTCTL_TXFFLSH, 0, 1000);
}
static void flush_rxfifo(void) {
    wr(GRSTCTL, GRSTCTL_RXFFLSH);
    wait_bits(GRSTCTL, GRSTCTL_RXFFLSH, 0, 1000);
}

/* ---- console rings (software only; hardware is driven from usb_poll) ---- */
static uint8_t txq[TXQ];
static uint32_t txh, txt;
static uint8_t rxq[RXQ];
static uint32_t rxh, rxt;
static int configured;
static int status_after_out;                     /* SET_LINE_CODING: ACK once its data arrived */
static uint8_t setup_buf[8];

static void con_write(int c) {
    uint64_t f = irq_save();
    if (txh - txt >= TXQ) txt++;                 /* full: drop the oldest byte, keep the newest */
    txq[txh++ % TXQ] = (uint8_t)c;
    irq_restore(f);
}
static struct console usb_console = { .name = "usb-cdc", .write = con_write, .flush = 0, .next = 0 };

int usb_cdc_getc(void) {                         /* called by board_console_getc() */
    uint64_t f = irq_save();
    int c = -1;
    if (rxh != rxt) c = rxq[rxt++ % RXQ];
    irq_restore(f);
    return c;
}

static void rxq_push(const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n && rxh - rxt < RXQ; i++) rxq[rxh++ % RXQ] = b[i];
}

/* ---- FIFO access (slave mode) ---- */
static void fifo_pop(uint8_t *dst, unsigned n, unsigned cap) {   /* always drains n bytes */
    volatile uint32_t *f = fifo(0);
    for (unsigned w = 0; w < (n + 3) / 4; w++) {
        uint32_t v = *f;
        for (unsigned b = 0; b < 4; b++) {
            unsigned i = w * 4 + b;
            if (dst && i < n && i < cap) dst[i] = (uint8_t)(v >> (8 * b));
        }
    }
}
static void fifo_push(unsigned ep, const uint8_t *src, unsigned n) {
    volatile uint32_t *f = fifo(ep);
    for (unsigned w = 0; w < (n + 3) / 4; w++) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; b++) {
            unsigned i = w * 4 + b;
            if (i < n) v |= (uint32_t)src[i] << (8 * b);
        }
        *f = v;
    }
}

/* ---- IN transfers (synchronous: start, then wait for XFERCOMPL) ---- */
static int in_start(unsigned ep, const uint8_t *buf, unsigned n) {
    uint32_t space = ep ? (rd(DTXFSTS(ep)) & 0xFFFFu) : (rd(GNPTXSTS) & 0xFFFFu);
    if (space < (n + 3) / 4) return 0;
    wr(DIEPINT(ep), rd(DIEPINT(ep)));           /* clear stale completions */
    wr(DIEPTSIZ(ep), TSIZ_ONE_PKT | n);
    wr(DIEPCTL(ep), rd(DIEPCTL(ep)) | EP_EPENA | EP_CNAK);
    fifo_push(ep, buf, n);
    return 1;
}
static int in_wait(unsigned ep, unsigned us) {
    uint64_t e = ctr() + us_ticks(us);
    for (;;) {
        if (rd(DIEPINT(ep)) & INT_XFERC) { wr(DIEPINT(ep), INT_XFERC); return 1; }
        if (ctr() > e) return 0;
    }
}

static void ep0_send(const uint8_t *p, unsigned n) {
    unsigned off = 0;
    do {
        unsigned c = n - off;
        if (c > 64) c = 64;
        if (!in_start(0, p ? p + off : 0, c)) return;
        if (!in_wait(0, 2000)) { flush_txfifo(0); return; }
        off += c;
    } while (off < n);
}
static void ep0_stall(void) { wr(DIEPCTL(0), rd(DIEPCTL(0)) | EP_STALL); }

/* ---- OUT endpoints ---- */
static void arm_ep0(void) {
    wr(DOEPTSIZ(0), TSIZ_ONE_PKT | 64u | (3u << 29));   /* one packet, room for 3 back-to-back SETUPs */
    wr(DOEPCTL(0), rd(DOEPCTL(0)) | EP_EPENA | EP_CNAK);
}
static void arm_bulk_out(void) {
    wr(DOEPTSIZ(2), TSIZ_ONE_PKT | 64u);
    wr(DOEPCTL(2), rd(DOEPCTL(2)) | EP_EPENA | EP_CNAK);
}

/* ---- descriptors (full speed, CDC-ACM with an interface association) ---- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02,           /* bcdUSB 2.00 */
    0xEF, 0x02, 0x01,            /* miscellaneous / common class / IAD */
    64,                          /* bMaxPacketSize0 */
    0x09, 0x12,                  /* idVendor 0x1209 (pid.codes, test PID) */
    0x01, 0x00,                  /* idProduct 0x0001 */
    0x00, 0x01,                  /* bcdDevice 1.00 */
    1, 2, 3,                     /* iManufacturer, iProduct, iSerialNumber */
    1                            /* one configuration */
};
static const uint8_t cfg_desc[75] = {
    9, 2, 75, 0, 2, 1, 0, 0x80, 50,                 /* configuration: 2 interfaces, bus powered, 100 mA */
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,             /* IAD: CDC control + data */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,             /* control interface: CDC ACM, AT commands */
    5, 0x24, 0x00, 0x10, 0x01,                      /* header functional, CDC 1.10 */
    5, 0x24, 0x01, 0x00, 1,                         /* call management (data on interface 1) */
    4, 0x24, 0x02, 0x02,                            /* ACM: line coding + serial state */
    5, 0x24, 0x06, 0, 1,                            /* union: control 0, slave 1 */
    7, 5, 0x81, 0x03, 8, 0, 16,                     /* EP1 IN interrupt, 8 bytes */
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,                   /* data interface: CDC data */
    7, 5, 0x82, 0x02, 64, 0, 0,                     /* EP2 IN bulk */
    7, 5, 0x02, 0x02, 64, 0, 0,                     /* EP2 OUT bulk */
};
static const char *str_tab[] = { 0, "Chiffon", "Chiffon serial console", "CHIFFON-ZERO2W-0001" };
static const uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

static unsigned make_string(unsigned idx, uint8_t *out) {
    if (idx == 0) { out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04; return 4; }   /* en-US */
    if (idx >= 4) return 0;
    unsigned n = 0;
    for (const char *s = str_tab[idx]; *s && n < 30; s++, n++) { out[2 + 2 * n] = (uint8_t)*s; out[3 + 2 * n] = 0; }
    out[0] = (uint8_t)(2 + 2 * n);
    out[1] = 3;
    return out[0];
}

/* ---- control requests ---- */
static void activate(void) {
    wr(DIEPCTL(1), 8 | EP_TYPE(3) | EP_TXFNUM(1) | EP_USBAEP | EP_SETD0);
    wr(DIEPCTL(2), 64 | EP_TYPE(2) | EP_TXFNUM(2) | EP_USBAEP | EP_SETD0);
    wr(DOEPCTL(2), 64 | EP_TYPE(2) | EP_USBAEP | EP_SETD0);
    configured = 1;
    arm_bulk_out();
}

static void std_request(uint8_t rq, uint16_t wv, uint16_t wl) {
    static uint8_t tmp[64];
    switch (rq) {
    case 0x00: {                                   /* GET_STATUS: self-powered = 0, no remote wakeup */
        uint8_t st[2] = { 0, 0 };
        ep0_send(st, wl < 2 ? wl : 2);
        break;
    }
    case 0x01: case 0x03:                          /* CLEAR_FEATURE / SET_FEATURE: accepted, not modelled */
        ep0_send(0, 0);
        break;
    case 0x05:                                     /* SET_ADDRESS: take effect after the status stage */
        ep0_send(0, 0);
        wr(DCFG, (rd(DCFG) & ~(0x7Fu << 4)) | ((uint32_t)(wv & 0x7Fu) << 4));
        break;
    case 0x06: {                                   /* GET_DESCRIPTOR */
        unsigned type = wv >> 8, idx = wv & 0xFF, n = 0;
        const uint8_t *d = 0;
        if (type == 1) { d = dev_desc; n = sizeof dev_desc; }
        else if (type == 2) { d = cfg_desc; n = sizeof cfg_desc; }
        else if (type == 3) { n = make_string(idx, tmp); d = tmp; }
        if (!n) { ep0_stall(); break; }            /* includes device_qualifier: full speed only */
        if (n > wl) n = wl;
        ep0_send(d, n);
        if (n < wl && (n % 64) == 0) ep0_send(0, 0);   /* ZLP so the host knows the data ended */
        break;
    }
    case 0x08: {                                   /* GET_CONFIGURATION */
        uint8_t c = configured ? 1 : 0;
        ep0_send(&c, wl < 1 ? wl : 1);
        break;
    }
    case 0x09:                                     /* SET_CONFIGURATION */
        if (wv == 1) activate();
        else if (wv == 0) configured = 0;
        else { ep0_stall(); break; }
        ep0_send(0, 0);
        break;
    case 0x0A: {                                   /* GET_INTERFACE: only alternate 0 exists */
        uint8_t z = 0;
        ep0_send(&z, wl < 1 ? wl : 1);
        break;
    }
    case 0x0B:                                     /* SET_INTERFACE: alternate 0 only */
        ep0_send(0, 0);
        break;
    default:
        ep0_stall();
        break;
    }
}

static void class_request(uint8_t rq, uint16_t wl) {
    switch (rq) {
    case 0x20:                                     /* SET_LINE_CODING: 7 bytes follow; ignored */
        if (wl) status_after_out = 1;              /* ACK is sent when the data packet arrives */
        else ep0_send(0, 0);
        break;
    case 0x21:                                     /* GET_LINE_CODING */
        ep0_send(line_coding, wl < 7 ? wl : 7);
        break;
    case 0x22: case 0x23:                          /* SET_CONTROL_LINE_STATE (DTR/RTS), SEND_BREAK */
        ep0_send(0, 0);
        break;
    default:
        ep0_stall();
        break;
    }
}

static void handle_setup(void) {
    const uint8_t *s = setup_buf;
    uint8_t rt = s[0], rq = s[1];
    uint16_t wv = (uint16_t)(s[2] | (s[3] << 8)), wl = (uint16_t)(s[6] | (s[7] << 8));
    wr(DIEPCTL(0), rd(DIEPCTL(0)) & ~EP_STALL);    /* a stall only lasts for one request */
    switch ((rt >> 5) & 3) {
    case 0: std_request(rq, wv, wl); break;
    case 1: class_request(rq, wl); break;
    default: ep0_stall(); break;
    }
}

/* ---- events ---- */
static void bus_reset(void) {
    configured = 0;
    status_after_out = 0;
    wr(DCFG, rd(DCFG) & ~(0x7Fu << 4));            /* back to address 0 */
    flush_txfifo(0x10);
    flush_rxfifo();
    wr(DIEPCTL(0), rd(DIEPCTL(0)) & ~EP_STALL);
    arm_ep0();
}

static void rx_service(void) {
    for (int guard = 0; guard < 128 && (rd(GINTSTS) & GINT_RXFLVL); guard++) {
        uint32_t s = rd(GRXSTSP);
        unsigned ep = s & 0xF, cnt = (s >> 4) & 0x7FF, pk = (s >> 17) & 0xF;
        switch (pk) {
        case 6:                                    /* SETUP data (8 bytes) */
            fifo_pop(setup_buf, cnt, sizeof setup_buf);
            break;
        case 4:                                    /* SETUP stage complete: request is in setup_buf */
            handle_setup();
            arm_ep0();
            break;
        case 2:                                    /* OUT data packet */
            if (ep == 2) {
                uint8_t b[64];
                fifo_pop(b, cnt, sizeof b);
                rxq_push(b, cnt < 64 ? cnt : 64);
            } else {
                fifo_pop(0, cnt, 0);
                if (ep == 0 && status_after_out) { status_after_out = 0; ep0_send(0, 0); }
            }
            break;
        case 3:                                    /* OUT transfer complete: re-arm the endpoint */
            if (ep == 2 && configured) arm_bulk_out();
            else if (ep == 0) arm_ep0();
            break;
        default:                                   /* global OUT NAK and anything else */
            if (cnt) fifo_pop(0, cnt, 0);
            break;
        }
    }
}

/* Bulk IN: one 64-byte packet per call, at most four per tick. If the host stops reading (port
 * closed) the backlog is dropped rather than stalling the kernel. */
static void tx_pump(void) {
    for (int k = 0; k < 4 && configured; k++) {
        unsigned n = txh - txt;
        if (!n) return;
        if (n > 64) n = 64;
        uint8_t pk[64];
        for (unsigned i = 0; i < n; i++) pk[i] = txq[(txt + i) % TXQ];
        if (!in_start(2, pk, n) || !in_wait(2, 2000)) {
            flush_txfifo(2);
            txt = txh;
            return;
        }
        txt += n;
    }
}

static void usb_poll(void) {
    uint64_t f = irq_save();
    uint32_t st = rd(GINTSTS);
    if (st & GINT_USBRST) { wr(GINTSTS, GINT_USBRST); bus_reset(); }
    if (st & GINT_ENUMDONE) wr(GINTSTS, GINT_ENUMDONE);
    rx_service();
    if (!(rd(DOEPCTL(0)) & EP_EPENA)) arm_ep0();
    if (configured && !(rd(DOEPCTL(2)) & EP_EPENA)) arm_bulk_out();
    tx_pump();
    irq_restore(f);
}

static int usb_init(void) {
    uint64_t hz = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    if (hz) usb_hz = hz;

    if (!wait_bits(GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 10000)) return -1;
    wr(GRSTCTL, GRSTCTL_CSRST);                    /* core soft reset */
    if (!wait_bits(GRSTCTL, GRSTCTL_CSRST, 0, 10000)) return -1;
    delay_us(10000);

    /* force device mode; internal UTMI+ PHY, 8-bit data path (USBTRDTIM = 9) */
    uint32_t u = rd(GUSBCFG);
    u &= ~((1u << 29) | (1u << 6));                /* FORCEHSTMODE, PHYSEL */
    u |= (1u << 30) | (9u << 10);                  /* FORCEDEVMODE, USBTRDTIM */
    wr(GUSBCFG, u);
    delay_us(50000);                               /* mode switch needs ~25 ms */

    wr(GAHBCFG, 0);                                /* no DMA; global USB interrupts stay masked (polled) */
    wr(GINTMSK, 0);
    wr(GINTSTS, 0xFFFFFFFFu);

    wr(DCFG, (rd(DCFG) & ~0x3u) | 1u);            /* full speed */

    /* FIFO map (words): RX 0..127, EP0 IN 128..191, EP1 IN 192..199, EP2 IN 200..263 */
    wr(GRXFSIZ, 128);
    wr(GNPTXFSIZ, (64u << 16) | 128u);
    wr(DIEPTXF(1), (8u << 16) | 192u);
    wr(DIEPTXF(2), (64u << 16) | 200u);
    flush_txfifo(0x10);
    flush_rxfifo();

    wr(DCTL, rd(DCTL) & ~DCTL_SFTDISCON);          /* connect: pull-up on */
    arm_ep0();
    console_register(&usb_console);                /* replays the boot log into the backlog */
    return 0;
}

static struct kdriver usb_dwc2_driver = {
    .name = "dwc2",
    .desc = "BCM2837 USB OTG: CDC-ACM serial console (polled)",
    .class = DRV_CHAR,
    .prio = 20,
    .init = usb_init,
    .poll = usb_poll,
};
KDRIVER(usb_dwc2_driver);
