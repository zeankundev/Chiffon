/* Minimal IPv4 stack: Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP, DNS (A records) and a
 * stop-and-wait TCP client.  Polled, not interrupt driven.  One protocol operation runs at a time
 * (net_lock); blocked callers yield so the rest of the system keeps running.  The interface is
 * configured on first use (DHCP, falling back to the QEMU user-network addresses).
 *
 * Deliberately missing: server sockets, IP fragmentation and reassembly, TCP retransmission of
 * FIN, congestion control, TIME_WAIT, IPv6, and any capability check on the syscalls. */
#include "kernel.h"
#include "net.h"
#include "uapi/chiffon_abi.h"

#define ETH_IP   0x0800
#define ETH_ARP  0x0806
#define IP_ICMP  1
#define IP_TCP   6
#define IP_UDP   17
#define BCAST    0xFFFFFFFFu
#define EHOSTUNREACH 113
#define MS(ms) (((ms) + 9) / 10)            /* milliseconds -> ticks (HZ = 100) */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_MSS 1400
#define TCP_RXBUF 2048
#define TCP_CONNS 2

static const struct netdev *dev;
static uint8_t mac[6];
static uint32_t my_ip, my_mask, my_gw, my_dns;
static int configured, busy;
static uint16_t ip_id;
static uint8_t eth_buf[14 + 1500];               /* outgoing Ethernet frame */
static uint8_t ip_buf[1500];                     /* outgoing IP packet */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static uint32_t csum_acc(const void *p, size_t n, uint32_t acc) {
    const uint8_t *b = p;
    for (size_t i = 0; i + 1 < n; i += 2) acc += (uint32_t)b[i] << 8 | b[i + 1];
    if (n & 1) acc += (uint32_t)b[n - 1] << 8;
    return acc;
}
static uint16_t csum_fold(uint32_t acc) { while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16); return (uint16_t)~acc; }
static uint16_t csum(const void *p, size_t n) { return csum_fold(csum_acc(p, n, 0)); }
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

/* ---- pacing: never yield before the first process exists (boot) ---- */
static void net_pause(void) { if (cur) yield(); }
static void net_poll(void) { if (dev) dev->poll(); }
static void net_lock(void) { while (busy) net_pause(); busy = 1; }
static void net_unlock(void) { busy = 0; }

int net_attach(const struct netdev *d) {
    if (dev) return -EBUSY;
    dev = d;
    memcpy(mac, d->mac, 6);
    kprintf("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", d->name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
__attribute__((weak)) int board_net_probe(void) { return -ENODEV; }

/* ---- Ethernet and ARP ---- */
static int eth_out(const uint8_t *dst, uint16_t type, const uint8_t *pl, size_t n) {
    if (n > sizeof eth_buf - 14) return -EINVAL;
    memcpy(eth_buf, dst, 6); memcpy(eth_buf + 6, mac, 6); wr16(eth_buf + 12, type);
    memcpy(eth_buf + 14, pl, n);
    return dev->send(eth_buf, 14 + n);
}

struct arp_ent { uint32_t ip; uint8_t mac[6]; uint8_t valid; };
static struct arp_ent arp[8];
static int arp_next;

static int arp_find(uint32_t ip, uint8_t *m) {
    for (int i = 0; i < 8; i++) if (arp[i].valid && arp[i].ip == ip) { memcpy(m, arp[i].mac, 6); return 1; }
    return 0;
}
static void arp_put(uint32_t ip, const uint8_t *m) {
    for (int i = 0; i < 8; i++) if (arp[i].valid && arp[i].ip == ip) { memcpy(arp[i].mac, m, 6); return; }
    struct arp_ent *e = &arp[arp_next++ % 8];
    e->ip = ip; memcpy(e->mac, m, 6); e->valid = 1;
}
static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa, const uint8_t *eth_dst) {
    uint8_t p[28];
    wr16(p, 1); wr16(p + 2, ETH_IP); p[4] = 6; p[5] = 4; wr16(p + 6, op);
    memcpy(p + 8, mac, 6); wr32(p + 14, my_ip); memcpy(p + 18, tha, 6); wr32(p + 24, tpa);
    eth_out(eth_dst, ETH_ARP, p, sizeof p);
}
static void arp_input(const uint8_t *p, size_t n) {
    if (n < 28 || rd16(p + 2) != ETH_IP || p[4] != 6 || p[5] != 4) return;
    uint32_t spa = rd32(p + 14), tpa = rd32(p + 24);
    arp_put(spa, p + 8);
    if (rd16(p + 6) == 1 && my_ip && tpa == my_ip) arp_send(2, p + 8, spa, p + 8);
}
static int resolve(uint32_t ip, uint8_t *m) {
    static const uint8_t bcast[6] = { 255, 255, 255, 255, 255, 255 }, zero[6];
    if (ip == BCAST) { memcpy(m, bcast, 6); return 1; }
    if (arp_find(ip, m)) return 1;
    for (int t = 0; t < 3; t++) {
        arp_send(1, zero, ip, bcast);
        uint64_t until = ticks + MS(500);
        while (ticks < until) {
            net_poll();
            if (arp_find(ip, m)) return 1;
            net_pause();
        }
    }
    return 0;
}
/* Next-hop MAC for dst.  May poll, so call it before building any packet buffer. */
static int route(uint32_t dst, uint8_t *m) {
    uint32_t nh = (dst == BCAST || !my_mask || (my_ip & my_mask) == (dst & my_mask)) ? dst : my_gw;
    return resolve(nh, m) ? 0 : -EHOSTUNREACH;
}

/* ---- IPv4 ---- */
static int ip_out_to(const uint8_t *m, uint32_t dst, uint8_t proto, const uint8_t *pl, size_t n) {
    if (n > sizeof ip_buf - 20) return -EINVAL;
    uint8_t *h = ip_buf;
    h[0] = 0x45; h[1] = 0; wr16(h + 2, 20 + n); wr16(h + 4, ip_id++); wr16(h + 6, 0x4000);
    h[8] = 64; h[9] = proto; wr16(h + 10, 0); wr32(h + 12, my_ip); wr32(h + 16, dst);
    wr16(h + 10, csum(h, 20));
    memcpy(ip_buf + 20, pl, n);
    return eth_out(m, ETH_IP, ip_buf, 20 + n);
}

/* ---- ICMP echo ---- */
static uint16_t ping_id = 0x4348, ping_seq;
static int ping_got;

/* Microseconds from the ARM generic timer (the tick is only 10 ms).  Other architectures fall back
 * to the tick counter. */
static uint64_t now_us(void) {
#if defined(__aarch64__)
    uint64_t c, f;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return c / f * 1000000u + (c % f) * 1000000u / f;
#else
    return ticks * 10000u;
#endif
}
static uint8_t icmp_buf[1472];
static void icmp_input(uint32_t src, const uint8_t *p, size_t n, const uint8_t *smac) {
    if (n < 8) return;
    if (p[0] == 8 && n <= sizeof icmp_buf) {                       /* echo request: answer at once */
        memcpy(icmp_buf, p, n); icmp_buf[0] = 0; wr16(icmp_buf + 2, 0);
        wr16(icmp_buf + 2, csum(icmp_buf, n));
        ip_out_to(smac, src, IP_ICMP, icmp_buf, n);
    } else if (p[0] == 0 && rd16(p + 4) == ping_id && rd16(p + 6) == ping_seq) ping_got = 1;
}

/* ---- UDP (one outstanding receive at a time) ---- */
static struct { uint16_t port; uint8_t *buf; size_t cap, len; int got; uint32_t from; } uwant;
static uint8_t udp_buf[8 + 1472];
static void udp_input(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 8 || !uwant.port || uwant.got || rd16(p + 2) != uwant.port) return;
    size_t l = rd16(p + 4);
    if (l < 8 || l > n) return;
    size_t pl = l - 8 < uwant.cap ? l - 8 : uwant.cap;
    memcpy(uwant.buf, p + 8, pl);
    uwant.len = pl; uwant.from = src; uwant.got = 1;
}
static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *pl, size_t n) {
    if (n > sizeof udp_buf - 8) return -EINVAL;
    uint8_t m[6];
    int r = route(dst, m);
    if (r) return r;
    wr16(udp_buf, sport); wr16(udp_buf + 2, dport); wr16(udp_buf + 4, 8 + n); wr16(udp_buf + 6, 0);
    memcpy(udp_buf + 8, pl, n);
    return ip_out_to(m, dst, IP_UDP, udp_buf, 8 + n);
}
/* Wait up to timeout_ms for a datagram to `port`.  Returns its length, or -ETIMEDOUT. */
static long udp_wait(uint16_t port, uint8_t *buf, size_t cap, int timeout_ms, uint32_t *from) {
    uwant.port = port; uwant.buf = buf; uwant.cap = cap; uwant.got = 0;
    uint64_t until = ticks + MS(timeout_ms);
    while (!uwant.got && ticks < until) { net_poll(); if (!uwant.got) net_pause(); }
    long r = uwant.got ? (long)uwant.len : -ETIMEDOUT;
    if (from) *from = uwant.from;
    uwant.port = 0;
    return r;
}

/* ---- DHCP (RFC 2131, the parts a client needs) ---- */
static size_t dhcp_msg(uint8_t *b, uint8_t type, uint32_t xid, uint32_t want_ip, uint32_t server) {
    memset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6; wr32(b + 4, xid); wr16(b + 10, 0x8000);
    memcpy(b + 28, mac, 6);
    wr32(b + 236, 0x63825363);
    size_t o = 240;
    b[o++] = 53; b[o++] = 1; b[o++] = type;
    if (want_ip) { b[o++] = 50; b[o++] = 4; wr32(b + o, want_ip); o += 4; }
    if (server) { b[o++] = 54; b[o++] = 4; wr32(b + o, server); o += 4; }
    b[o++] = 55; b[o++] = 4; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 51;
    b[o++] = 255;
    return o;
}
struct dhcp_opts { uint8_t type; uint32_t mask, router, dns, server; };
static void dhcp_parse(const uint8_t *b, size_t n, struct dhcp_opts *d) {
    size_t o = 240;
    while (o < n) {
        uint8_t c = b[o];
        if (c == 255) break;
        if (c == 0) { o++; continue; }
        if (o + 1 >= n) break;
        uint8_t l = b[o + 1];
        if (o + 2 + l > n) break;
        const uint8_t *v = b + o + 2;
        if (c == 53 && l >= 1) d->type = v[0];
        else if (c == 1 && l >= 4) d->mask = rd32(v);
        else if (c == 3 && l >= 4) d->router = rd32(v);
        else if (c == 6 && l >= 4) d->dns = rd32(v);
        else if (c == 54 && l >= 4) d->server = rd32(v);
        o += 2 + l;
    }
}
static int dhcp_run(void) {
    static uint8_t msg[300], rep[300];
    uint32_t xid = (uint32_t)ticks * 2654435761u ^ rd32(mac + 2);
    struct dhcp_opts o = { 0, 0, 0, 0, 0 };
    uint32_t offered = 0;
    for (int t = 0; t < 3 && !offered; t++) {                       /* DISCOVER -> OFFER */
        size_t n = dhcp_msg(msg, 1, xid, 0, 0);
        udp_send(BCAST, 68, 67, msg, n);
        long r = udp_wait(68, rep, sizeof rep, 1000, 0);
        if (r < 240 || rd32(rep + 4) != xid || rep[0] != 2) continue;
        o = (struct dhcp_opts){ 0, 0, 0, 0, 0 };
        dhcp_parse(rep, r, &o);
        if (o.type == 2) offered = rd32(rep + 16);
    }
    if (!offered) return 0;
    for (int t = 0; t < 3; t++) {                                   /* REQUEST -> ACK */
        size_t n = dhcp_msg(msg, 3, xid, offered, o.server);
        udp_send(BCAST, 68, 67, msg, n);
        long r = udp_wait(68, rep, sizeof rep, 1000, 0);
        if (r < 240 || rd32(rep + 4) != xid || rep[0] != 2) continue;
        struct dhcp_opts a = { 0, 0, 0, 0, 0 };
        dhcp_parse(rep, r, &a);
        if (a.type != 5) continue;
        my_ip = offered;
        my_mask = a.mask ? a.mask : 0xFFFFFF00u;
        my_gw = a.router;
        my_dns = a.dns;
        return 1;
    }
    return 0;
}

static void kprint_ip(uint32_t ip) {
    kprintf("%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}
/* Bring the interface up.  Runs in the first process that uses the network. */
static int net_configure(void) {
    if (configured) return 0;
    if (!dev) return -ENODEV;
    net_lock();
    if (!configured) {
        if (!dhcp_run()) {
            my_ip = 0x0a00020f; my_mask = 0xFFFFFF00u; my_gw = 0x0a000202; my_dns = 0x0a000203;
            kprintf("net: no DHCP answer, using the QEMU user-network addresses\n");
        }
        configured = 1;
        kprintf("net: ip "); kprint_ip(my_ip);
        kprintf(" mask "); kprint_ip(my_mask);
        kprintf(" gw "); kprint_ip(my_gw);
        kprintf(" dns "); kprint_ip(my_dns); kprintf("\n");
    }
    net_unlock();
    return 0;
}

/* ---- DNS (A records over UDP) ---- */
static size_t dns_name_end(const uint8_t *r, size_t n, size_t o) {
    while (o < n) {
        uint8_t l = r[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2;
        o += 1 + l;
    }
    return n + 1;
}
/* Parse a DNS response (rep/n) and return the first IPv4 address in its answers.
 * 0 = found, -ENOENT = no A record or the server said no such name. */
int dns_parse_a(const uint8_t *rep, size_t n, uint32_t *out) {
    if (n < 12) return -ENOENT;
    if (rep[3] & 15) return -ENOENT;                    /* NXDOMAIN or server failure: no retry */
    size_t p = 12, end = n;
    uint16_t qd = rd16(rep + 4), an = rd16(rep + 6);
    for (uint16_t i = 0; i < qd; i++) p = dns_name_end(rep, end, p) + 4;
    for (uint16_t i = 0; i < an && p < end; i++) {
        p = dns_name_end(rep, end, p);
        if (p + 10 > end) break;
        uint16_t type = rd16(rep + p), rdl = rd16(rep + p + 8);
        p += 10;
        if (p + rdl > end) break;
        if (type == 1 && rdl == 4) { *out = rd32(rep + p); return 0; }
        p += rdl;
    }
    return -ENOENT;
}
static int dns_query(const char *name, uint32_t *out) {
    if (!my_dns) return -EHOSTUNREACH;
    static uint8_t q[300], rep[512];
    uint16_t id = (uint16_t)(ticks ^ 0x5a5a);
    wr16(q, id); wr16(q + 2, 0x0100); wr16(q + 4, 1); wr16(q + 6, 0); wr16(q + 8, 0); wr16(q + 10, 0);
    size_t o = 12;
    const char *s = name;
    while (*s) {
        size_t l = 0;
        while (s[l] && s[l] != '.') l++;
        if (l == 0 || l > 63 || o + l + 6 > sizeof q) return -EINVAL;
        q[o++] = (uint8_t)l; memcpy(q + o, s, l); o += l; s += l;
        if (*s == '.') s++;
    }
    q[o++] = 0; wr16(q + o, 1); wr16(q + o + 2, 1); o += 4;
    uint16_t sport = (uint16_t)(40000 + ticks % 20000);
    for (int t = 0; t < 3; t++) {
        int r = udp_send(my_dns, sport, 53, q, o);
        if (r) return r;
        long n = udp_wait(sport, rep, sizeof rep, 1000, 0);
        if (n < 12 || rd16(rep) != id || !(rep[2] & 0x80)) continue;
        return dns_parse_a(rep, (size_t)n, out);
    }
    return -ETIMEDOUT;
}

/* ---- TCP (client only, stop-and-wait) ---- */
enum { TS_FREE, TS_SYN_SENT, TS_ESTAB, TS_CLOSED };
struct tconn {
    uint8_t state, fin, err;
    uint16_t lport, rport, rxlen;
    uint32_t ip, snd_una, snd_nxt, rcv_nxt;
    uint8_t hwaddr[6];
    uint8_t rx[TCP_RXBUF];
};
static struct tconn tc[TCP_CONNS];
static uint8_t tcp_seg[24 + TCP_MSS];

static int tcp_out(struct tconn *c, uint8_t flags, uint32_t seq, const uint8_t *pl, size_t n, int with_mss) {
    size_t hl = with_mss ? 24 : 20;
    if (hl + n > sizeof tcp_seg) return -EINVAL;
    memset(tcp_seg, 0, hl);
    wr16(tcp_seg, c->lport); wr16(tcp_seg + 2, c->rport); wr32(tcp_seg + 4, seq);
    wr32(tcp_seg + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0);
    tcp_seg[12] = (uint8_t)((hl / 4) << 4); tcp_seg[13] = flags;
    wr16(tcp_seg + 14, (uint16_t)(TCP_RXBUF - c->rxlen));
    if (with_mss) { tcp_seg[20] = 2; tcp_seg[21] = 4; wr16(tcp_seg + 22, TCP_MSS); }
    if (n) memcpy(tcp_seg + hl, pl, n);
    uint8_t ph[12];
    wr32(ph, my_ip); wr32(ph + 4, c->ip); ph[8] = 0; ph[9] = IP_TCP; wr16(ph + 10, (uint16_t)(hl + n));
    wr16(tcp_seg + 16, csum_fold(csum_acc(tcp_seg, hl + n, csum_acc(ph, 12, 0))));
    return ip_out_to(c->hwaddr, c->ip, IP_TCP, tcp_seg, hl + n);
}
static struct tconn *tcp_find(uint32_t src, uint16_t sport, uint16_t dport) {
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tconn *c = &tc[i];
        if (c->state != TS_FREE && c->ip == src && c->rport == sport && c->lport == dport) return c;
    }
    return 0;
}
static void tcp_input(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 20) return;
    size_t hl = (size_t)(p[12] >> 4) * 4;
    if (hl < 20 || hl > n) return;
    struct tconn *c = tcp_find(src, rd16(p), rd16(p + 2));
    if (!c) return;
    uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
    uint8_t fl = p[13];
    const uint8_t *pl = p + hl;
    size_t pn = n - hl;
    if (fl & TCP_RST) { c->state = TS_CLOSED; c->err = 1; return; }
    if (c->state == TS_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1; c->snd_una = ack; c->state = TS_ESTAB;
            tcp_out(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
        }
        return;
    }
    if (c->state != TS_ESTAB) return;
    if ((fl & TCP_ACK) && seq_lt(c->snd_una, ack) && !seq_lt(c->snd_nxt, ack)) c->snd_una = ack;
    int reply = 0;
    if (pn) {
        if (seq == c->rcv_nxt) {
            size_t room = TCP_RXBUF - c->rxlen, take = pn < room ? pn : room;
            memcpy(c->rx + c->rxlen, pl, take);
            c->rxlen += (uint16_t)take; c->rcv_nxt += (uint32_t)take;
        }
        reply = 1;
    }
    if ((fl & TCP_FIN) && seq + (uint32_t)pn == c->rcv_nxt) { c->rcv_nxt++; c->fin = 1; reply = 1; }
    if (reply) tcp_out(c, TCP_ACK, c->snd_nxt, 0, 0, 0);
}
static long tcp_connect(uint32_t ip, uint16_t port) {
    int i = 0;
    while (i < TCP_CONNS && tc[i].state != TS_FREE) i++;
    if (i == TCP_CONNS) return -EMFILE;
    struct tconn *c = &tc[i];
    memset(c, 0, sizeof *c);
    c->ip = ip; c->rport = port; c->lport = (uint16_t)(49152 + ticks % 12000);
    if (route(ip, c->hwaddr)) return -EHOSTUNREACH;
    uint32_t iss = (uint32_t)ticks * 64000u + 1000u;
    c->snd_una = iss; c->snd_nxt = iss + 1; c->state = TS_SYN_SENT;
    for (int t = 0; t < 3 && c->state == TS_SYN_SENT; t++) {
        tcp_out(c, TCP_SYN, iss, 0, 0, 1);
        uint64_t until = ticks + MS(1000);
        while (c->state == TS_SYN_SENT && ticks < until) { net_poll(); if (c->state == TS_SYN_SENT) net_pause(); }
    }
    if (c->state != TS_ESTAB) { c->state = TS_FREE; return -ETIMEDOUT; }
    return i;
}
static long tcp_send(struct tconn *c, const uint8_t *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        if (c->state != TS_ESTAB) return -EPIPE;
        size_t chunk = n - done < TCP_MSS ? n - done : TCP_MSS;
        uint32_t seq = c->snd_nxt, target = seq + (uint32_t)chunk;
        c->snd_nxt = target;
        int tries = 0;
        uint64_t until = 0;
        for (;;) {
            if (tries == 0 || ticks >= until) {
                if (tries++ == 5) { c->state = TS_CLOSED; return -ETIMEDOUT; }
                tcp_out(c, TCP_PSH | TCP_ACK, seq, buf + done, chunk, 0);
                until = ticks + MS(1000);
            }
            net_poll();
            if (!seq_lt(c->snd_una, target)) break;
            if (c->state != TS_ESTAB) return -EPIPE;
            net_pause();
        }
        done += chunk;
    }
    return (long)n;
}
static long tcp_recv(struct tconn *c, uint8_t *buf, size_t cap, int timeout_ms) {
    uint64_t until = ticks + MS(timeout_ms);
    while (!c->rxlen && !c->fin && c->state == TS_ESTAB && ticks < until) { net_poll(); if (!c->rxlen && !c->fin) net_pause(); }
    if (!c->rxlen) {
        if (c->fin || c->state != TS_ESTAB) return 0;
        return c->err ? -EPIPE : -ETIMEDOUT;
    }
    size_t n = c->rxlen < cap ? c->rxlen : cap;
    memcpy(buf, c->rx, n);
    memmove(c->rx, c->rx + n, c->rxlen - n);
    c->rxlen -= (uint16_t)n;
    if (c->state == TS_ESTAB) tcp_out(c, TCP_ACK, c->snd_nxt, 0, 0, 0);   /* window update */
    return (long)n;
}
static void tcp_close(struct tconn *c) {
    if (c->state == TS_ESTAB) {
        uint32_t fseq = c->snd_nxt;
        c->snd_nxt++;
        tcp_out(c, TCP_FIN | TCP_ACK, fseq, 0, 0, 0);
        uint64_t until = ticks + MS(1000);
        while (seq_lt(c->snd_una, c->snd_nxt) && ticks < until) { net_poll(); if (seq_lt(c->snd_una, c->snd_nxt)) net_pause(); }
    }
    c->state = TS_FREE;
}

/* ---- IPv4 receive dispatch ---- */
static void ip_input(const uint8_t *p, size_t n, const uint8_t *smac) {
    if (n < 20 || (p[0] >> 4) != 4) return;
    size_t ihl = (size_t)(p[0] & 15) * 4;
    size_t tot = rd16(p + 2);
    if (ihl < 20 || tot < ihl || tot > n) return;
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    int mine = dst == BCAST || (my_ip && (dst == my_ip || (my_mask && dst == (my_ip | ~my_mask))));
    if (!mine || (rd16(p + 6) & 0x3FFF)) return;                   /* not ours, or a fragment */
    const uint8_t *pl = p + ihl;
    size_t pn = tot - ihl;
    switch (p[9]) {
    case IP_ICMP: icmp_input(src, pl, pn, smac); break;
    case IP_UDP:  udp_input(src, pl, pn); break;
    case IP_TCP:  tcp_input(src, pl, pn); break;
    }
}

void net_input(const uint8_t *f, size_t n) {
    if (n < 14) return;
    uint16_t type = rd16(f + 12);
    if (type == ETH_ARP) arp_input(f + 14, n - 14);
    else if (type == ETH_IP) ip_input(f + 14, n - 14, f + 6);
}

/* ---- ping ---- */
static long net_ping(uint32_t ip, int timeout_ms) {
    if (!dev) return -ENODEV;
    net_configure();
    net_lock();
    uint8_t m[6];
    int r = route(ip, m);
    if (r) { net_unlock(); return r; }
    uint8_t req[40];
    ping_seq++;
    req[0] = 8; req[1] = 0; wr16(req + 2, 0); wr16(req + 4, ping_id); wr16(req + 6, ping_seq);
    memset(req + 8, 0x5a, 32);
    wr16(req + 2, csum(req, sizeof req));
    ping_got = 0;
    uint64_t t0 = now_us(), until = ticks + MS(timeout_ms);
    ip_out_to(m, ip, IP_ICMP, req, sizeof req);
    while (!ping_got && ticks < until) { net_poll(); if (!ping_got) net_pause(); }
    long res = ping_got ? (long)(now_us() - t0) : -ETIMEDOUT;
    net_unlock();
    return res;
}

/* ---- system calls ---- */
static int dotted_quad(const char *s, uint32_t *out) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t x = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (uint32_t)(*s - '0'); s++; digits++; }
        if (!digits || x > 255) return 0;
        v = v << 8 | x;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out = v;
    return 1;
}
static char namebuf[128];

/* The NIC is probed on the first network call, not at boot: probing and DHCP both wait, and waiting
 * needs a running process (boot has none). */
static int probed;
long net_syscall(long nr, long a0, long a1, long a2, long a3) {
    if (!probed) { probed = 1; board_net_probe(); }
    if (!dev) return -ENODEV;
    switch (nr) {
    case CSYS_net_info: {
        if (!uok(a0, sizeof(struct chi_netinfo))) return -EFAULT;
        net_configure();
        struct chi_netinfo *u = (struct chi_netinfo *)a0;
        u->ip = my_ip; u->mask = my_mask; u->gateway = my_gw; u->dns = my_dns;
        memcpy(u->mac, mac, 6); u->configured = (uint8_t)configured; u->_pad = 0;
        return 0;
    }
    case CSYS_net_ping:
        net_configure();
        return net_ping((uint32_t)a0, (int)a1);      /* round trip in microseconds */
    case CSYS_net_resolve: {
        int cr = ustrcpy(namebuf, a0, sizeof namebuf);     /* 0 on success, -errno otherwise */
        if (cr) return cr;
        if (!uok(a1, 4)) return -EFAULT;
        net_configure();
        uint32_t ip;
        if (dotted_quad(namebuf, &ip)) { *(uint32_t *)a1 = ip; return 0; }
        net_lock();
        int r = dns_query(namebuf, &ip);
        net_unlock();
        if (r) return r;
        *(uint32_t *)a1 = ip;
        return 0;
    }
    case CSYS_net_connect:
        net_configure();
        net_lock();
        {
            long r = tcp_connect((uint32_t)a0, (uint16_t)a1);
            net_unlock();
            return r;
        }
    case CSYS_net_send: {
        if (a0 < 0 || a0 >= TCP_CONNS || tc[a0].state == TS_FREE) return -EBADF;
        if (!uok(a1, a2)) return -EFAULT;
        net_lock();
        long r = tcp_send(&tc[a0], (const uint8_t *)a1, (size_t)a2);
        net_unlock();
        return r;
    }
    case CSYS_net_recv: {
        if (a0 < 0 || a0 >= TCP_CONNS || tc[a0].state == TS_FREE) return -EBADF;
        if (!uok(a1, a2)) return -EFAULT;
        net_lock();
        long r = tcp_recv(&tc[a0], (uint8_t *)a1, (size_t)a2, (int)a3);
        net_unlock();
        return r;
    }
    case CSYS_net_close:
        if (a0 < 0 || a0 >= TCP_CONNS || tc[a0].state == TS_FREE) return -EBADF;
        net_lock();
        tcp_close(&tc[a0]);
        net_unlock();
        return 0;
    }
    return -ENOSYS;
}
