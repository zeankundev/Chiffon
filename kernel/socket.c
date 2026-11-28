/* BSD sockets & Netlink (kernel/socket.c).
 * Supports AF_NETLINK (NETLINK_ROUTE), AF_INET (TCP, UDP, raw ICMP), and AF_UNIX. */
#include "kernel.h"
#include "net.h"
#include "socket.h"

#ifdef CHI_TINY
#define MAX_SOCKS    16
#define SOCK_PKT_MAX 8
#define SOCK_RXBUF   2048
#else
#define MAX_SOCKS    64
#define SOCK_PKT_MAX 16
#define SOCK_RXBUF   8192
#endif
#define SOCK_BACKLOG 8
#define SOCK_IO_MAX ((size_t)((unsigned long)-1 >> 1))
#define TCP_MSS      1400
#define MS_TO_TICKS(ms) (((ms) + 9) / 10)

enum { SS_UNCONNECTED = 0, SS_BOUND, SS_LISTENING, SS_CONNECTING, SS_CONNECTED, SS_CLOSED };
enum { TCPS_CLOSED = 0, TCPS_LISTEN, TCPS_SYN_SENT, TCPS_SYN_RCVD, TCPS_ESTAB, TCPS_FIN_WAIT1, TCPS_FIN_WAIT2, TCPS_CLOSE_WAIT, TCPS_LAST_ACK };

struct sock_pkt {
    uint8_t *data;
    size_t   len;
    struct sockaddr_storage src;
    int      srclen;
};

struct socket {
    int used, refs;
    int domain, type, protocol;
    int state, err;
    int shut_rd, shut_wr, nonblock;
    int so_reuseaddr, so_broadcast, so_keepalive, tcp_nodelay;
    int rcv_tmo_ms, snd_tmo_ms;

    struct sockaddr_storage local, peer;
    int local_len, peer_len;

    /* Netlink */
    uint32_t nl_pid, nl_groups;

    /* Packet queue (Netlink, UDP, raw ICMP, AF_UNIX DGRAM) */
    struct sock_pkt rxq[SOCK_PKT_MAX];
    int rxq_head, rxq_tail, rxq_count;

    /* Stream ring buffer (TCP, AF_UNIX STREAM) */
    uint8_t *rxbuf;
    size_t rxcap, rxrd, rxwr, rxlen;
    int eof;

    /* TCP state */
    uint8_t  tcp_state;
    uint16_t lport, rport;
    uint32_t lip, rip;          /* host byte order */
    uint32_t snd_una, snd_nxt, rcv_nxt;
    struct socket *parent;

    /* Accept queue (TCP & AF_UNIX STREAM) */
    struct socket *acceptq[SOCK_BACKLOG];
    int aq_head, aq_tail, aq_count;

    /* AF_UNIX peer & abstract/path key */
    struct socket *un_peer;
    char un_path[108];
    int  un_pathlen;
};

static struct socket socks[MAX_SOCKS];
static uint16_t next_ephemeral = 49152;

static uint16_t htons_u(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint16_t ntohs_u(uint16_t v) { return htons_u(v); }
static uint32_t htonl_u(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static uint32_t ntohl_u(uint32_t v) { return htonl_u(v); }

static uint16_t rd16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16be(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static struct socket *sock_alloc(int domain, int type, int protocol) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (!socks[i].used) {
            struct socket *s = &socks[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->refs = 1;
            s->domain = domain;
            s->type = type & SOCK_TYPE_MASK;
            s->protocol = protocol;
            s->nonblock = (type & SOCK_NONBLOCK) ? 1 : 0;
            return s;
        }
    }
    return 0;
}

static void sock_free_queues(struct socket *s) {
    while (s->rxq_count > 0) {
        kfree(s->rxq[s->rxq_head].data);
        s->rxq[s->rxq_head].data = 0;
        s->rxq_head = (s->rxq_head + 1) % SOCK_PKT_MAX;
        s->rxq_count--;
    }
    if (s->rxbuf) { kfree(s->rxbuf); s->rxbuf = 0; }
    while (s->aq_count > 0) {
        struct socket *c = s->acceptq[s->aq_head];
        s->aq_head = (s->aq_head + 1) % SOCK_BACKLOG;
        s->aq_count--;
        if (c) sock_close(c);
    }
}

static int sock_ensure_rxbuf(struct socket *s) {
    if (s->rxbuf) return 0;
    s->rxbuf = kmalloc(SOCK_RXBUF);
    if (!s->rxbuf) return -ENOMEM;
    s->rxcap = SOCK_RXBUF;
    s->rxrd = s->rxwr = s->rxlen = 0;
    return 0;
}

static int sock_rxq_push(struct socket *s, const void *data, size_t len, const void *src, int srclen) {
    if (s->shut_rd) return 0;
    if (s->rxq_count >= SOCK_PKT_MAX) return -ENOBUFS;
    uint8_t *copy = kmalloc(len ? len : 1);
    if (!copy) return -ENOMEM;
    if (len) memcpy(copy, data, len);
    struct sock_pkt *p = &s->rxq[s->rxq_tail];
    p->data = copy;
    p->len = len;
    memset(&p->src, 0, sizeof p->src);
    if (src && srclen > 0) {
        int cp = srclen > (int)sizeof p->src ? (int)sizeof p->src : srclen;
        memcpy(&p->src, src, (size_t)cp);
        p->srclen = srclen;
    } else {
        p->srclen = 0;
    }
    s->rxq_tail = (s->rxq_tail + 1) % SOCK_PKT_MAX;
    s->rxq_count++;
    wakeup(s);
    return 0;
}

static size_t sock_rxbuf_push(struct socket *s, const uint8_t *data, size_t len) {
    if (sock_ensure_rxbuf(s)) return 0;
    size_t room = s->rxcap - s->rxlen;
    size_t take = len < room ? len : room;
    for (size_t i = 0; i < take; i++) {
        s->rxbuf[s->rxwr] = data[i];
        s->rxwr = (s->rxwr + 1) % s->rxcap;
    }
    s->rxlen += take;
    if (take > 0) wakeup(s);
    return take;
}

static size_t sock_rxbuf_pop(struct socket *s, uint8_t *dst, size_t len, int peek) {
    size_t take = len < s->rxlen ? len : s->rxlen;
    size_t rd = s->rxrd;
    for (size_t i = 0; i < take; i++) {
        dst[i] = s->rxbuf[rd];
        rd = (rd + 1) % s->rxcap;
    }
    if (!peek) {
        s->rxrd = rd;
        s->rxlen -= take;
        wakeup(s);
    }
    return take;
}

static struct file *sock_wrap_file(struct socket *s, int nonblock) {
    struct file *f = kmalloc(sizeof *f);
    if (!f) return 0;
    memset(f, 0, sizeof *f);
    f->type = F_SOCKET;
    f->refs = 1;
    f->flags = O_RDWR | (nonblock ? O_NONBLOCK : 0);
    f->sock = s;
    return f;
}

static int is_nonblock(struct file *f, int flags) {
    return (f->flags & O_NONBLOCK) || f->sock->nonblock || (flags & MSG_DONTWAIT);
}

/* ---- TCP helpers for AF_INET SOCK_STREAM ---- */
static uint16_t alloc_port(int type) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_ephemeral++;
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        int clash = 0;
        for (int i = 0; i < MAX_SOCKS; i++) {
            if (socks[i].used && socks[i].domain == AF_INET && socks[i].type == type && socks[i].lport == p) {
                clash = 1; break;
            }
        }
        if (!clash) return p;
    }
    return 50000;
}

static int tcp_send_seg(struct socket *s, uint8_t flags, uint32_t seq, const uint8_t *pl, size_t n, int with_mss) {
    size_t hl = with_mss ? 24 : 20;
    uint8_t *seg = kmalloc(hl + n);
    if (!seg) return -ENOMEM;
    memset(seg, 0, hl);
    wr16be(seg, s->lport);
    wr16be(seg + 2, s->rport);
    wr32be(seg + 4, seq);
    wr32be(seg + 8, (flags & 0x10) ? s->rcv_nxt : 0);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    size_t win = s->rxbuf ? (s->rxcap - s->rxlen) : SOCK_RXBUF;
    if (win > 65535) win = 65535;
    wr16be(seg + 14, (uint16_t)win);
    if (with_mss) { seg[20] = 2; seg[21] = 4; wr16be(seg + 22, TCP_MSS); }
    if (n) memcpy(seg + hl, pl, n);
    uint32_t sip = s->lip ? s->lip : net_source_ip(s->rip);
    if (!sip) {
        net_ensure();
        sip = net_source_ip(s->rip);
    }
    wr16be(seg + 16, net_tcp_udp_csum(sip, s->rip, IPPROTO_TCP, seg, hl + n));
    int r = net_ip_send(sip, s->rip, IPPROTO_TCP, seg, hl + n);
    kfree(seg);
    return r;
}

static struct socket *tcp_find_sock(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport) {
    struct socket *listener = 0;
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->domain != AF_INET || s->type != SOCK_STREAM) continue;
        if (s->tcp_state == TCPS_LISTEN && s->lport == dport && (!s->lip || s->lip == dst)) {
            listener = s;
        } else if (s->tcp_state != TCPS_CLOSED && s->tcp_state != TCPS_LISTEN &&
                   s->lport == dport && s->rport == sport && s->rip == src) {
            return s;
        }
    }
    return listener;
}

void sock_tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n) {
    if (n < 20) return;
    size_t hl = (size_t)(p[12] >> 4) * 4;
    if (hl < 20 || hl > n) return;
    uint16_t sport = rd16be(p), dport = rd16be(p + 2);
    uint32_t seq = rd32be(p + 4), ack = rd32be(p + 8);
    uint8_t fl = p[13];
    const uint8_t *pl = p + hl;
    size_t pn = n - hl;

    struct socket *s = tcp_find_sock(src, dst, sport, dport);
    if (!s) return;

    if (s->tcp_state == TCPS_LISTEN) {
        if ((fl & 0x02) && !(fl & 0x10) && s->aq_count < SOCK_BACKLOG) {
            struct socket *c = sock_alloc(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (!c) return;
            if (sock_ensure_rxbuf(c)) { sock_close(c); return; }
            c->lip = dst; c->rip = src;
            c->lport = dport; c->rport = sport;
            c->rcv_nxt = seq + 1;
            uint32_t iss = (uint32_t)ticks * 64000u + 2000u + (uint32_t)(c - socks);
            c->snd_una = iss; c->snd_nxt = iss + 1;
            c->tcp_state = TCPS_SYN_RCVD;
            c->state = SS_CONNECTING;
            c->parent = s;
            struct sockaddr_in *la = (struct sockaddr_in *)&c->local;
            la->sin_family = AF_INET; la->sin_port = htons_u(dport); la->sin_addr = htonl_u(dst);
            c->local_len = sizeof *la;
            struct sockaddr_in *pa = (struct sockaddr_in *)&c->peer;
            pa->sin_family = AF_INET; pa->sin_port = htons_u(sport); pa->sin_addr = htonl_u(src);
            c->peer_len = sizeof *pa;
            tcp_send_seg(c, 0x12 /* SYN|ACK */, iss, 0, 0, 1);
        }
        return;
    }

    if (fl & 0x04 /* RST */) {
        s->err = ECONNRESET;
        s->eof = 1;
        s->tcp_state = TCPS_CLOSED;
        s->state = SS_CLOSED;
        wakeup(s);
        return;
    }

    if (s->tcp_state == TCPS_SYN_SENT) {
        if ((fl & 0x12) == 0x12 && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->tcp_state = TCPS_ESTAB;
            s->state = SS_CONNECTED;
            if (!s->lip) s->lip = dst;
            tcp_send_seg(s, 0x10 /* ACK */, s->snd_nxt, 0, 0, 0);
            wakeup(s);
        }
        return;
    }

    if (s->tcp_state == TCPS_SYN_RCVD) {
        if ((fl & 0x10) && ack == s->snd_nxt) {
            s->snd_una = ack;
            s->tcp_state = TCPS_ESTAB;
            s->state = SS_CONNECTED;
            if (s->parent && s->parent->used && s->parent->aq_count < SOCK_BACKLOG) {
                s->parent->acceptq[s->parent->aq_tail] = s;
                s->parent->aq_tail = (s->parent->aq_tail + 1) % SOCK_BACKLOG;
                s->parent->aq_count++;
                wakeup(s->parent);
            } else {
                sock_close(s);
            }
        }
        return;
    }

    if ((fl & 0x10) && seq_lt(s->snd_una, ack) && !seq_lt(s->snd_nxt, ack)) {
        s->snd_una = ack;
        if (s->tcp_state == TCPS_FIN_WAIT1 && ack == s->snd_nxt) s->tcp_state = TCPS_FIN_WAIT2;
        else if (s->tcp_state == TCPS_LAST_ACK && ack == s->snd_nxt) {
            s->tcp_state = TCPS_CLOSED; s->state = SS_CLOSED;
        }
        wakeup(s);
    }

    int need_ack = 0;
    if (pn && (s->tcp_state == TCPS_ESTAB || s->tcp_state == TCPS_FIN_WAIT1 || s->tcp_state == TCPS_FIN_WAIT2)) {
        if (seq == s->rcv_nxt) {
            size_t took = sock_rxbuf_push(s, pl, pn);
            s->rcv_nxt += (uint32_t)took;
        }
        need_ack = 1;
    }
    if ((fl & 0x01 /* FIN */) && seq + (uint32_t)pn == s->rcv_nxt) {
        s->rcv_nxt++;
        s->eof = 1;
        if (s->tcp_state == TCPS_ESTAB) s->tcp_state = TCPS_CLOSE_WAIT;
        else if (s->tcp_state == TCPS_FIN_WAIT1 || s->tcp_state == TCPS_FIN_WAIT2) {
            s->tcp_state = TCPS_CLOSED; s->state = SS_CLOSED;
        }
        need_ack = 1;
        wakeup(s);
    }
    if (need_ack) tcp_send_seg(s, 0x10 /* ACK */, s->snd_nxt, 0, 0, 0);
}

void sock_udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n) {
    if (n < 8) return;
    uint16_t sport = rd16be(p), dport = rd16be(p + 2);
    size_t ulen = rd16be(p + 4);
    if (ulen < 8 || ulen > n) return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons_u(sport);
    sa.sin_addr = htonl_u(src);

    for (int i = 0; i < MAX_SOCKS; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->domain != AF_INET || s->type != SOCK_DGRAM) continue;
        if (s->protocol == IPPROTO_ICMP) continue;
        if (s->lport != dport) continue;
        if (s->lip && s->lip != dst && dst != 0xFFFFFFFFu) continue;
        if (s->state == SS_CONNECTED && (s->rip != src || s->rport != sport)) continue;
        sock_rxq_push(s, p + 8, ulen - 8, &sa, sizeof sa);
    }
}

void sock_icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr = htonl_u(src);

    for (int i = 0; i < MAX_SOCKS; i++) {
        struct socket *s = &socks[i];
        if (!s->used || s->domain != AF_INET || s->protocol != IPPROTO_ICMP) continue;
        if (s->state == SS_CONNECTED && s->rip != src) continue;
        if (s->type == SOCK_RAW) {
            uint8_t *pkt = kmalloc(20 + n);
            if (!pkt) continue;
            pkt[0] = 0x45; pkt[1] = 0; wr16be(pkt + 2, (uint16_t)(20 + n));
            wr16be(pkt + 4, 0); wr16be(pkt + 6, 0);
            pkt[8] = 64; pkt[9] = IPPROTO_ICMP; wr16be(pkt + 10, 0);
            wr32be(pkt + 12, src); wr32be(pkt + 16, dst);
            wr16be(pkt + 10, net_csum(pkt, 20));
            memcpy(pkt + 20, p, n);
            sock_rxq_push(s, pkt, 20 + n, &sa, sizeof sa);
            kfree(pkt);
        } else if (s->type == SOCK_DGRAM) {
            sock_rxq_push(s, p, n, &sa, sizeof sa);
        }
    }
}

/* ---- Netlink (NETLINK_ROUTE) ---- */
struct nl_buf {
    uint8_t *buf;
    size_t   len, cap;
};

static int nl_grow(struct nl_buf *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t nc = b->cap ? b->cap * 2 : 1024;
    while (nc < b->len + need) nc *= 2;
    uint8_t *nb = krealloc(b->buf, nc);
    if (!nb) return -ENOMEM;
    b->buf = nb;
    b->cap = nc;
    return 0;
}

static struct nlmsghdr *nl_begin(struct nl_buf *b, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid, size_t hdrlen) {
    size_t tot = NLMSG_ALIGN(sizeof(struct nlmsghdr)) + NLMSG_ALIGN(hdrlen);
    if (nl_grow(b, tot)) return 0;
    struct nlmsghdr *h = (struct nlmsghdr *)(b->buf + b->len);
    memset(h, 0, tot);
    h->nlmsg_len = (uint32_t)(NLMSG_ALIGN(sizeof(struct nlmsghdr)) + hdrlen);
    h->nlmsg_type = type;
    h->nlmsg_flags = flags;
    h->nlmsg_seq = seq;
    h->nlmsg_pid = pid;
    b->len += tot;
    return h;
}

static int nl_add_attr(struct nl_buf *b, size_t msg_off, uint16_t type, const void *data, size_t len) {
    size_t alen = RTA_ALIGN(sizeof(struct rtattr)) + len;
    size_t pad_alen = RTA_ALIGN(alen);
    if (nl_grow(b, pad_alen)) return -ENOMEM;
    struct rtattr *rta = (struct rtattr *)(b->buf + b->len);
    memset(rta, 0, pad_alen);
    rta->rta_len = (uint16_t)alen;
    rta->rta_type = type;
    if (len && data) memcpy((uint8_t *)rta + RTA_ALIGN(sizeof(struct rtattr)), data, len);
    b->len += pad_alen;
    struct nlmsghdr *h = (struct nlmsghdr *)(b->buf + msg_off);
    h->nlmsg_len = (uint32_t)(b->len - msg_off);
    return 0;
}

static uint8_t mask_to_prefix(uint32_t mask) {
    uint8_t p = 0;
    while (mask & 0x80000000u) { p++; mask <<= 1; }
    return p;
}

static uint32_t prefix_to_mask(uint8_t p) {
    return p == 0 ? 0 : (p >= 32 ? 0xFFFFFFFFu : (~0u << (32 - p)));
}

static void nl_queue_done(struct socket *s, uint32_t seq, uint32_t pid) {
    struct nl_buf b = {0};
    struct nlmsghdr *h = nl_begin(&b, NLMSG_DONE, NLM_F_MULTI, seq, pid, sizeof(int));
    if (h) {
        struct sockaddr_nl src = { AF_NETLINK, 0, 0, 0 };
        sock_rxq_push(s, b.buf, b.len, &src, sizeof src);
    }
    kfree(b.buf);
}

static void nl_queue_ack(struct socket *s, uint32_t seq, uint32_t pid, const struct nlmsghdr *req, int err) {
    struct nl_buf b = {0};
    struct nlmsghdr *h = nl_begin(&b, NLMSG_ERROR, 0, seq, pid, sizeof(struct nlmsgerr));
    if (h) {
        struct nlmsgerr *e = (struct nlmsgerr *)((uint8_t *)h + NLMSG_HDRLEN);
        e->error = err;
        memcpy(&e->msg, req, sizeof *req);
        struct sockaddr_nl src = { AF_NETLINK, 0, 0, 0 };
        sock_rxq_push(s, b.buf, b.len, &src, sizeof src);
    }
    kfree(b.buf);
}

static void nl_handle_msg(struct socket *s, const struct nlmsghdr *req) {
    uint32_t seq = req->nlmsg_seq;
    uint32_t pid = s->nl_pid;
    struct sockaddr_nl src = { AF_NETLINK, 0, 0, 0 };

    if (req->nlmsg_type == RTM_GETLINK) {
        net_ensure();
        int cnt = net_if_count();
        for (int i = 0; i < cnt; i++) {
            struct net_ifinfo info;
            if (net_if_info(i, &info)) continue;
            struct nl_buf b = {0};
            size_t off = 0;
            struct nlmsghdr *h = nl_begin(&b, RTM_NEWLINK, NLM_F_MULTI, seq, pid, sizeof(struct ifinfomsg));
            if (!h) { kfree(b.buf); continue; }
            struct ifinfomsg *ifi = (struct ifinfomsg *)(b.buf + off + NLMSG_HDRLEN);
            ifi->ifi_family = AF_UNSPEC;
            ifi->ifi_type = (i == 0) ? 772 : 1; /* ARPHRD_LOOPBACK : ARPHRD_ETHER */
            ifi->ifi_index = info.index;
            ifi->ifi_flags = info.flags;
            uint32_t mtu = info.mtu, txq = 1000;
            uint8_t oper = (info.flags & IFF_RUNNING) ? 6 : 2;
            uint8_t bcast[6];
            memset(bcast, i == 0 ? 0 : 0xFF, 6);
            nl_add_attr(&b, off, IFLA_IFNAME, info.name, strlen(info.name) + 1);
            nl_add_attr(&b, off, IFLA_TXQLEN, &txq, 4);
            nl_add_attr(&b, off, IFLA_OPERSTATE, &oper, 1);
            nl_add_attr(&b, off, IFLA_MTU, &mtu, 4);
            nl_add_attr(&b, off, IFLA_QDISC, "noqueue", 8);
            nl_add_attr(&b, off, IFLA_ADDRESS, info.mac, 6);
            nl_add_attr(&b, off, IFLA_BROADCAST, bcast, 6);
            sock_rxq_push(s, b.buf, b.len, &src, sizeof src);
            kfree(b.buf);
        }
        nl_queue_done(s, seq, pid);
        return;
    }

    if (req->nlmsg_type == RTM_GETADDR) {
        net_ensure();
        int cnt = net_if_count();
        for (int i = 0; i < cnt; i++) {
            struct net_ifinfo info;
            if (net_if_info(i, &info) || !info.ip) continue;
            struct nl_buf b = {0};
            size_t off = 0;
            struct nlmsghdr *h = nl_begin(&b, RTM_NEWADDR, NLM_F_MULTI, seq, pid, sizeof(struct ifaddrmsg));
            if (!h) { kfree(b.buf); continue; }
            struct ifaddrmsg *ifa = (struct ifaddrmsg *)(b.buf + off + NLMSG_HDRLEN);
            ifa->ifa_family = AF_INET;
            ifa->ifa_prefixlen = mask_to_prefix(info.mask);
            ifa->ifa_flags = 0x80; /* IFA_F_PERMANENT */
            ifa->ifa_scope = (i == 0) ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE;
            ifa->ifa_index = (uint32_t)info.index;
            uint32_t ip_be = htonl_u(info.ip);
            nl_add_attr(&b, off, IFA_ADDRESS, &ip_be, 4);
            nl_add_attr(&b, off, IFA_LOCAL, &ip_be, 4);
            if (i > 0 && info.mask) {
                uint32_t brd_be = htonl_u(info.ip | ~info.mask);
                nl_add_attr(&b, off, IFA_BROADCAST, &brd_be, 4);
            }
            nl_add_attr(&b, off, IFA_LABEL, info.name, strlen(info.name) + 1);
            sock_rxq_push(s, b.buf, b.len, &src, sizeof src);
            kfree(b.buf);
        }
        nl_queue_done(s, seq, pid);
        return;
    }

    if (req->nlmsg_type == RTM_GETROUTE) {
        net_ensure();
        int cnt = net_if_count();
        for (int i = 0; i < cnt; i++) {
            struct net_ifinfo info;
            if (net_if_info(i, &info) || !info.ip) continue;
            struct nl_buf b = {0};
            struct nlmsghdr *h = nl_begin(&b, RTM_NEWROUTE, NLM_F_MULTI, seq, pid, sizeof(struct rtmsg));
            if (h) {
                struct rtmsg *rt = (struct rtmsg *)(b.buf + NLMSG_HDRLEN);
                rt->rtm_family = AF_INET;
                rt->rtm_dst_len = mask_to_prefix(info.mask);
                rt->rtm_table = RT_TABLE_MAIN;
                rt->rtm_protocol = RTPROT_KERNEL;
                rt->rtm_scope = (i == 0) ? RT_SCOPE_HOST : RT_SCOPE_LINK;
                rt->rtm_type = RTN_UNICAST;
                uint32_t tbl = RT_TABLE_MAIN, oif = (uint32_t)info.index;
                uint32_t dst_be = htonl_u(info.ip & info.mask), src_be = htonl_u(info.ip);
                nl_add_attr(&b, 0, RTA_TABLE, &tbl, 4);
                nl_add_attr(&b, 0, RTA_DST, &dst_be, 4);
                nl_add_attr(&b, 0, RTA_PREFSRC, &src_be, 4);
                nl_add_attr(&b, 0, RTA_OIF, &oif, 4);
                sock_rxq_push(s, b.buf, b.len, &src, sizeof src);
            }
            kfree(b.buf);
            if (info.gw) {
                struct nl_buf b2 = {0};
                struct nlmsghdr *h2 = nl_begin(&b2, RTM_NEWROUTE, NLM_F_MULTI, seq, pid, sizeof(struct rtmsg));
                if (h2) {
                    struct rtmsg *rt = (struct rtmsg *)(b2.buf + NLMSG_HDRLEN);
                    rt->rtm_family = AF_INET;
                    rt->rtm_table = RT_TABLE_MAIN;
                    rt->rtm_protocol = RTPROT_BOOT;
                    rt->rtm_scope = RT_SCOPE_UNIVERSE;
                    rt->rtm_type = RTN_UNICAST;
                    uint32_t tbl = RT_TABLE_MAIN, oif = (uint32_t)info.index;
                    uint32_t gw_be = htonl_u(info.gw), src_be = htonl_u(info.ip);
                    nl_add_attr(&b2, 0, RTA_TABLE, &tbl, 4);
                    nl_add_attr(&b2, 0, RTA_GATEWAY, &gw_be, 4);
                    nl_add_attr(&b2, 0, RTA_PREFSRC, &src_be, 4);
                    nl_add_attr(&b2, 0, RTA_OIF, &oif, 4);
                    sock_rxq_push(s, b2.buf, b2.len, &src, sizeof src);
                }
                kfree(b2.buf);
            }
        }
        nl_queue_done(s, seq, pid);
        return;
    }

    if (req->nlmsg_type == RTM_NEWADDR && req->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifaddrmsg))) {
        const struct ifaddrmsg *ifa = (const struct ifaddrmsg *)((const uint8_t *)req + NLMSG_HDRLEN);
        uint32_t ip = 0;
        size_t off = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
        while (off + sizeof(struct rtattr) <= req->nlmsg_len) {
            const struct rtattr *rta = (const struct rtattr *)((const uint8_t *)req + off);
            if (rta->rta_len < sizeof(struct rtattr) || off + rta->rta_len > req->nlmsg_len) break;
            if ((rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) &&
                rta->rta_len >= RTA_LENGTH(4)) {
                ip = ntohl_u(*(const uint32_t *)((const uint8_t *)rta + RTA_ALIGN(sizeof(struct rtattr))));
            }
            off += RTA_ALIGN(rta->rta_len);
        }
        int err = net_if_set_addr((int)ifa->ifa_index - 1, ip, prefix_to_mask(ifa->ifa_prefixlen));
        if (req->nlmsg_flags & NLM_F_ACK) nl_queue_ack(s, seq, pid, req, err);
        return;
    }

    if (req->nlmsg_type == RTM_NEWLINK && req->nlmsg_len >= NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
        const struct ifinfomsg *ifi = (const struct ifinfomsg *)((const uint8_t *)req + NLMSG_HDRLEN);
        struct net_ifinfo info;
        int err = net_if_info(ifi->ifi_index - 1, &info);
        if (!err && ifi->ifi_change) {
            uint32_t nf = (info.flags & ~ifi->ifi_change) | (ifi->ifi_flags & ifi->ifi_change);
            err = net_if_set_flags(ifi->ifi_index - 1, nf);
        }
        if (req->nlmsg_flags & NLM_F_ACK) nl_queue_ack(s, seq, pid, req, err);
        return;
    }

    if (req->nlmsg_flags & NLM_F_ACK) nl_queue_ack(s, seq, pid, req, -EOPNOTSUPP);
}

static ssize_t nl_send(struct socket *s, const uint8_t *buf, size_t len) {
    if (!s->nl_pid) s->nl_pid = (uint32_t)cur->pid;
    size_t off = 0;
    while (off + sizeof(struct nlmsghdr) <= len) {
        const struct nlmsghdr *h = (const struct nlmsghdr *)(buf + off);
        if (h->nlmsg_len < sizeof(struct nlmsghdr) || off + h->nlmsg_len > len) break;
        nl_handle_msg(s, h);
        off += NLMSG_ALIGN(h->nlmsg_len);
    }
    return (ssize_t)len;
}

/* ---- Socket lifecycle & VFS file ops ---- */
long sock_create(int domain, int type, int protocol, struct file **out) {
    int stype = type & SOCK_TYPE_MASK;
    if (domain != AF_INET && domain != AF_NETLINK && domain != AF_UNIX) return -EAFNOSUPPORT;
    if (stype != SOCK_STREAM && stype != SOCK_DGRAM && stype != SOCK_RAW) return -ESOCKTNOSUPPORT;
    if (domain == AF_NETLINK && (stype != SOCK_RAW && stype != SOCK_DGRAM)) return -ESOCKTNOSUPPORT;
    if (domain == AF_NETLINK && protocol != NETLINK_ROUTE) return -EPROTONOSUPPORT;
    if (domain == AF_INET) {
        if (stype == SOCK_STREAM && protocol == 0) protocol = IPPROTO_TCP;
        if (stype == SOCK_DGRAM && protocol == 0) protocol = IPPROTO_UDP;
        if (stype == SOCK_STREAM && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
        if (stype == SOCK_DGRAM && protocol != IPPROTO_UDP && protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        if (stype == SOCK_RAW && protocol != IPPROTO_ICMP && protocol != IPPROTO_RAW) return -EPROTONOSUPPORT;
    }
    struct socket *s = sock_alloc(domain, type, protocol);
    if (!s) return -ENOMEM;
    struct file *f = sock_wrap_file(s, (type & SOCK_NONBLOCK) != 0);
    if (!f) { s->used = 0; return -ENOMEM; }
    *out = f;
    return 0;
}

long sock_socketpair(int domain, int type, int protocol, struct file **out0, struct file **out1) {
    int stype = type & SOCK_TYPE_MASK;
    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if (stype != SOCK_STREAM && stype != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
    struct socket *s0 = sock_alloc(domain, type, protocol);
    struct socket *s1 = sock_alloc(domain, type, protocol);
    if (!s0 || !s1) {
        if (s0) s0->used = 0;
        if (s1) s1->used = 0;
        return -ENOMEM;
    }
    if (stype == SOCK_STREAM && (sock_ensure_rxbuf(s0) || sock_ensure_rxbuf(s1))) {
        sock_free_queues(s0); s0->used = 0;
        sock_free_queues(s1); s1->used = 0;
        return -ENOMEM;
    }
    s0->state = s1->state = SS_CONNECTED;
    s0->un_peer = s1; s1->un_peer = s0;
    struct file *f0 = sock_wrap_file(s0, (type & SOCK_NONBLOCK) != 0);
    struct file *f1 = sock_wrap_file(s1, (type & SOCK_NONBLOCK) != 0);
    if (!f0 || !f1) {
        if (f0) kfree(f0);
        if (f1) kfree(f1);
        sock_free_queues(s0); s0->used = 0;
        sock_free_queues(s1); s1->used = 0;
        return -ENOMEM;
    }
    *out0 = f0; *out1 = f1;
    return 0;
}

void sock_close(struct socket *s) {
    if (!s || !s->used) return;
    if (--s->refs > 0) return;
    if (s->domain == AF_INET && s->type == SOCK_STREAM && s->tcp_state == TCPS_ESTAB) {
        uint32_t fseq = s->snd_nxt++;
        s->tcp_state = TCPS_FIN_WAIT1;
        tcp_send_seg(s, 0x11 /* FIN|ACK */, fseq, 0, 0, 0);
        uint64_t until = ticks + MS_TO_TICKS(500);
        while (s->tcp_state != TCPS_CLOSED && seq_lt(s->snd_una, s->snd_nxt) && ticks < until) {
            net_poll_once();
            if (seq_lt(s->snd_una, s->snd_nxt)) net_pause_once();
        }
    }
    if (s->domain == AF_UNIX && s->un_peer) {
        struct socket *p = s->un_peer;
        s->un_peer = 0;
        if (p->used && p->un_peer == s) {
            p->eof = 1;
            p->un_peer = 0;
            wakeup(p);
        }
    }
    sock_free_queues(s);
    s->used = 0;
}

/* ---- Core send/recv ---- */
static long sock_wait_rx(struct socket *s, int flags, int nonblock) {
    uint64_t dl = s->rcv_tmo_ms > 0 ? ticks + MS_TO_TICKS(s->rcv_tmo_ms) : 0;
    for (;;) {
        if (s->domain == AF_INET) net_poll_once();
        if (s->domain == AF_INET && s->type == SOCK_STREAM) {
            if (s->rxlen > 0 || s->eof || s->shut_rd || s->err) return 0;
        } else if (s->domain == AF_UNIX && s->type == SOCK_STREAM) {
            if (s->rxlen > 0 || s->eof || s->shut_rd) return 0;
        } else {
            if (s->rxq_count > 0 || s->shut_rd) return 0;
        }
        if (nonblock) return -EAGAIN;
        if (s->rcv_tmo_ms > 0 && ticks >= dl) return -EAGAIN;
        if (cur->killed) return -EINTR;
        if (s->domain == AF_INET) net_pause_once();
        else sleep_on(s);
    }
}

static long sock_recv_core(struct file *f, uint8_t *kbuf, size_t len, int flags, struct sockaddr_storage *src, int *srclen) {
    struct socket *s = f->sock;
    if (s->shut_rd) return 0;
    long r = sock_wait_rx(s, flags, is_nonblock(f, flags));
    if (r) return r;

    if ((s->domain == AF_INET || s->domain == AF_UNIX) && s->type == SOCK_STREAM) {
        if (s->rxlen == 0) return s->err ? -s->err : 0;
        size_t got = sock_rxbuf_pop(s, kbuf, len, flags & MSG_PEEK);
        if (s->domain == AF_INET && !(flags & MSG_PEEK) && s->tcp_state == TCPS_ESTAB)
            tcp_send_seg(s, 0x10, s->snd_nxt, 0, 0, 0);
        if (src && srclen && s->peer_len) {
            memcpy(src, &s->peer, (size_t)s->peer_len);
            *srclen = s->peer_len;
        }
        return (long)got;
    }

    if (s->rxq_count == 0) return 0;
    struct sock_pkt *p = &s->rxq[s->rxq_head];
    size_t cp = p->len < len ? p->len : len;
    if (cp) memcpy(kbuf, p->data, cp);
    if (src && srclen) {
        if (p->srclen > 0) { memcpy(src, &p->src, (size_t)p->srclen); *srclen = p->srclen; }
        else *srclen = 0;
    }
    if (!(flags & MSG_PEEK)) {
        kfree(p->data);
        p->data = 0;
        s->rxq_head = (s->rxq_head + 1) % SOCK_PKT_MAX;
        s->rxq_count--;
    }
    return (long)cp;
}

static long sock_send_core(struct file *f, const uint8_t *kbuf, size_t len, int flags, const struct sockaddr_storage *dst, int dstlen) {
    struct socket *s = f->sock;
    if (s->shut_wr) return -EPIPE;

    if (s->domain == AF_NETLINK) return nl_send(s, kbuf, len);

    if (s->domain == AF_UNIX) {
        struct socket *peer = s->un_peer;
        if (!peer || !peer->used || peer->shut_rd) return -EPIPE;
        if (s->type == SOCK_DGRAM) {
            int r = sock_rxq_push(peer, kbuf, len, &s->local, s->local_len);
            return r ? r : (long)len;
        }
        size_t done = 0;
        while (done < len) {
            if (!peer->used || peer->shut_rd) return done ? (long)done : -EPIPE;
            size_t took = sock_rxbuf_push(peer, kbuf + done, len - done);
            done += took;
            if (done == len) break;
            if (is_nonblock(f, flags)) return done ? (long)done : -EAGAIN;
            if (cur->killed) return done ? (long)done : -EINTR;
            sleep_on(peer);
        }
        return (long)done;
    }

    if (s->type == SOCK_STREAM) {
        if (s->tcp_state != TCPS_ESTAB) return -ENOTCONN;
        size_t done = 0;
        while (done < len) {
            size_t chunk = len - done < TCP_MSS ? len - done : TCP_MSS;
            uint32_t seq = s->snd_nxt, target = seq + (uint32_t)chunk;
            s->snd_nxt = target;
            int tries = 0;
            uint64_t until = 0;
            for (;;) {
                if (tries == 0 || ticks >= until) {
                    if (tries++ == 5) { s->tcp_state = TCPS_CLOSED; return -ETIMEDOUT; }
                    int r = tcp_send_seg(s, 0x18 /* PSH|ACK */, seq, kbuf + done, chunk, 0);
                    if (r) return done ? (long)done : r;
                    until = ticks + MS_TO_TICKS(1000);
                }
                net_poll_once();
                if (!seq_lt(s->snd_una, target)) break;
                if (s->tcp_state != TCPS_ESTAB) return done ? (long)done : -EPIPE;
                if (cur->killed) return done ? (long)done : -EINTR;
                net_pause_once();
            }
            done += chunk;
        }
        return (long)done;
    }

    /* AF_INET DGRAM / RAW */
    uint32_t rip = s->rip;
    uint16_t rport = s->rport;
    if (dst && dstlen >= (int)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)dst;
        if (in->sin_family != AF_INET) return -EAFNOSUPPORT;
        rip = ntohl_u(in->sin_addr);
        rport = ntohs_u(in->sin_port);
    } else if (s->state != SS_CONNECTED) {
        return -EDESTADDRREQ;
    }

    uint32_t sip = s->lip ? s->lip : net_source_ip(rip);
    if (!sip && (rip >> 24) != 127) {
        int r = net_ensure();
        if (r) return r;
        sip = net_source_ip(rip);
    }

    if (s->protocol == IPPROTO_ICMP) {
        if (len < 8 || len > 1472) return -EINVAL;
        uint8_t *pkt = kmalloc(len);
        if (!pkt) return -ENOMEM;
        memcpy(pkt, kbuf, len);
        wr16be(pkt + 2, 0);
        wr16be(pkt + 2, net_csum(pkt, len));
        int r = net_ip_send(sip, rip, IPPROTO_ICMP, pkt, len);
        kfree(pkt);
        return r ? r : (long)len;
    }

    if (len > 1472) return -EMSGSIZE;
    if (!s->lport) s->lport = alloc_port(SOCK_DGRAM);
    uint8_t *udp = kmalloc(8 + len);
    if (!udp) return -ENOMEM;
    wr16be(udp, s->lport);
    wr16be(udp + 2, rport);
    wr16be(udp + 4, (uint16_t)(8 + len));
    wr16be(udp + 6, 0);
    if (len) memcpy(udp + 8, kbuf, len);
    wr16be(udp + 6, net_tcp_udp_csum(sip, rip, IPPROTO_UDP, udp, 8 + len));
    int r = net_ip_send(sip, rip, IPPROTO_UDP, udp, 8 + len);
    kfree(udp);
    return r ? r : (long)len;
}

ssize_t sock_read(struct file *f, void *buf, size_t n) {
    return sock_recv_core(f, (uint8_t *)buf, n, 0, 0, 0);
}

ssize_t sock_write(struct file *f, const void *buf, size_t n) {
    return sock_send_core(f, (const uint8_t *)buf, n, 0, 0, 0);
}

int sock_readable(struct file *f) {
    struct socket *s = f->sock;
    if (s->domain == AF_INET) net_poll_once();
    if (s->state == SS_LISTENING) return s->aq_count > 0;
    if ((s->domain == AF_INET || s->domain == AF_UNIX) && s->type == SOCK_STREAM)
        return s->rxlen > 0 || s->eof || s->shut_rd || s->err;
    return s->rxq_count > 0 || s->shut_rd;
}

int sock_writable(struct file *f) {
    struct socket *s = f->sock;
    if (s->shut_wr) return 1;
    if (s->domain == AF_UNIX && s->type == SOCK_STREAM && s->un_peer)
        return s->un_peer->rxlen < s->un_peer->rxcap;
    return 1;
}

int sock_hangup(struct file *f) {
    struct socket *s = f->sock;
    if (s->domain == AF_INET && s->type == SOCK_STREAM)
        return s->eof || s->err || s->tcp_state == TCPS_CLOSED;
    if (s->domain == AF_UNIX && s->type == SOCK_STREAM)
        return s->eof;
    return 0;
}

/* ---- Syscall handlers ---- */
long sock_bind(struct file *f, uintptr_t uaddr, int addrlen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (addrlen < 2 || addrlen > (int)sizeof(struct sockaddr_storage)) return -EINVAL;
    if (!uok(uaddr, (size_t)addrlen)) return -EFAULT;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    memcpy(&ss, (const void *)uaddr, (size_t)addrlen);

    if (s->domain == AF_NETLINK) {
        if (addrlen < (int)sizeof(struct sockaddr_nl) || ss.ss_family != AF_NETLINK) return -EINVAL;
        struct sockaddr_nl *nl = (struct sockaddr_nl *)&ss;
        s->nl_pid = nl->nl_pid ? nl->nl_pid : (uint32_t)cur->pid;
        s->nl_groups = nl->nl_groups;
        nl->nl_pid = s->nl_pid;
        memcpy(&s->local, nl, sizeof *nl);
        s->local_len = sizeof *nl;
        s->state = SS_BOUND;
        return 0;
    }

    if (s->domain == AF_INET) {
        if (addrlen < (int)sizeof(struct sockaddr_in) || ss.ss_family != AF_INET) return -EINVAL;
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        uint16_t port = ntohs_u(in->sin_port);
        uint32_t ip = ntohl_u(in->sin_addr);
        if (port == 0 && s->type != SOCK_RAW) port = alloc_port(s->type);
        if (port != 0 && !s->so_reuseaddr) {
            for (int i = 0; i < MAX_SOCKS; i++) {
                if (&socks[i] != s && socks[i].used && socks[i].domain == AF_INET &&
                    socks[i].type == s->type && socks[i].lport == port)
                    return -EADDRINUSE;
            }
        }
        s->lip = ip;
        s->lport = port;
        in->sin_port = htons_u(port);
        memcpy(&s->local, in, sizeof *in);
        s->local_len = sizeof *in;
        s->state = SS_BOUND;
        return 0;
    }

    if (s->domain == AF_UNIX) {
        if (addrlen < 3 || ss.ss_family != AF_UNIX) return -EINVAL;
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        int plen = addrlen - 2;
        if (plen > (int)sizeof s->un_path) plen = sizeof s->un_path;
        memcpy(s->un_path, un->sun_path, (size_t)plen);
        s->un_pathlen = plen;
        memcpy(&s->local, un, (size_t)addrlen);
        s->local_len = addrlen;
        s->state = SS_BOUND;
        return 0;
    }
    return -EAFNOSUPPORT;
}

long sock_listen(struct file *f, int backlog) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (s->domain == AF_INET) {
        if (!s->lport) s->lport = alloc_port(SOCK_STREAM);
        s->tcp_state = TCPS_LISTEN;
    }
    s->state = SS_LISTENING;
    return 0;
}

long sock_accept(struct file *f, uintptr_t uaddr, uintptr_t ulen, int flags, struct file **out) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    if (uaddr && !ulen) return -EFAULT;
    struct socket *s = f->sock;
    if (s->state != SS_LISTENING) return -EINVAL;

    for (;;) {
        if (s->domain == AF_INET) net_poll_once();
        if (s->aq_count > 0) break;
        if (is_nonblock(f, flags)) return -EAGAIN;
        if (cur->killed) return -EINTR;
        if (s->domain == AF_INET) net_pause_once();
        else sleep_on(s);
    }

    struct socket *c = s->acceptq[s->aq_head];
    s->aq_head = (s->aq_head + 1) % SOCK_BACKLOG;
    s->aq_count--;
    c->parent = 0;
    c->nonblock = (flags & SOCK_NONBLOCK) ? 1 : 0;

    if (uaddr && ulen) {
        if (!uok(ulen, 4) || !uwok(ulen, 4)) { sock_close(c); return -EFAULT; }
        int cap = *(int *)ulen;
        if (cap < 0) { sock_close(c); return -EINVAL; }
        int cp = c->peer_len < cap ? c->peer_len : cap;
        if (cp > 0) {
            if (!uwok(uaddr, (size_t)cp)) { sock_close(c); return -EFAULT; }
            memcpy((void *)uaddr, &c->peer, (size_t)cp);
        }
        *(int *)ulen = c->peer_len;
    }

    struct file *cf = sock_wrap_file(c, (flags & SOCK_NONBLOCK) != 0);
    if (!cf) { sock_close(c); return -ENOMEM; }
    *out = cf;
    return 0;
}

long sock_connect(struct file *f, uintptr_t uaddr, int addrlen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (addrlen < 2 || addrlen > (int)sizeof(struct sockaddr_storage)) return -EINVAL;
    if (!uok(uaddr, (size_t)addrlen)) return -EFAULT;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    memcpy(&ss, (const void *)uaddr, (size_t)addrlen);

    if (s->domain == AF_UNIX) {
        if (addrlen < 3 || ss.ss_family != AF_UNIX) return -EINVAL;
        struct sockaddr_un *un = (struct sockaddr_un *)&ss;
        int plen = addrlen - 2;
        if (plen > (int)sizeof s->un_path) plen = sizeof s->un_path;
        struct socket *srv = 0;
        for (int i = 0; i < MAX_SOCKS; i++) {
            if (socks[i].used && socks[i].domain == AF_UNIX && socks[i].state == SS_LISTENING &&
                socks[i].un_pathlen == plen && !memcmp(socks[i].un_path, un->sun_path, (size_t)plen)) {
                srv = &socks[i]; break;
            }
        }
        if (!srv) return -ECONNREFUSED;
        if (srv->aq_count >= SOCK_BACKLOG) return -EAGAIN;
        struct socket *peer = sock_alloc(AF_UNIX, s->type, s->protocol);
        if (!peer) return -ENOMEM;
        if (sock_ensure_rxbuf(s) || sock_ensure_rxbuf(peer)) {
            sock_close(peer); return -ENOMEM;
        }
        s->un_peer = peer; peer->un_peer = s;
        s->state = peer->state = SS_CONNECTED;
        memcpy(&peer->peer, &s->local, (size_t)s->local_len);
        peer->peer_len = s->local_len;
        srv->acceptq[srv->aq_tail] = peer;
        srv->aq_tail = (srv->aq_tail + 1) % SOCK_BACKLOG;
        srv->aq_count++;
        wakeup(srv);
        return 0;
    }

    if (s->domain != AF_INET || addrlen < (int)sizeof(struct sockaddr_in) || ss.ss_family != AF_INET)
        return -EAFNOSUPPORT;

    struct sockaddr_in *in = (struct sockaddr_in *)&ss;
    uint32_t rip = ntohl_u(in->sin_addr);
    uint16_t rport = ntohs_u(in->sin_port);
    if ((rip >> 24) != 127) {
        int r = net_ensure();
        if (r) return r;
    }
    s->rip = rip;
    s->rport = rport;
    memcpy(&s->peer, in, sizeof *in);
    s->peer_len = sizeof *in;
    if (!s->lport && s->type != SOCK_RAW) s->lport = alloc_port(s->type);
    if (!s->lip) s->lip = net_source_ip(rip);

    struct sockaddr_in *la = (struct sockaddr_in *)&s->local;
    la->sin_family = AF_INET; la->sin_port = htons_u(s->lport); la->sin_addr = htonl_u(s->lip);
    s->local_len = sizeof *la;

    if (s->type != SOCK_STREAM) {
        s->state = SS_CONNECTED;
        return 0;
    }

    if (sock_ensure_rxbuf(s)) return -ENOMEM;
    uint32_t iss = (uint32_t)ticks * 64000u + 1000u + (uint32_t)(s - socks);
    s->snd_una = iss; s->snd_nxt = iss + 1;
    s->tcp_state = TCPS_SYN_SENT;
    s->state = SS_CONNECTING;

    for (int t = 0; t < 3 && s->tcp_state == TCPS_SYN_SENT; t++) {
        int r = tcp_send_seg(s, 0x02 /* SYN */, iss, 0, 0, 1);
        if (r) { s->tcp_state = TCPS_CLOSED; s->state = SS_UNCONNECTED; return r; }
        if (is_nonblock(f, 0)) return -EINPROGRESS;
        uint64_t until = ticks + MS_TO_TICKS(1000);
        while (s->tcp_state == TCPS_SYN_SENT && ticks < until) {
            net_poll_once();
            if (cur->killed) return -EINTR;
            if (s->tcp_state == TCPS_SYN_SENT) net_pause_once();
        }
    }
    if (s->tcp_state != TCPS_ESTAB) {
        int err = s->err ? -s->err : -ETIMEDOUT;
        s->tcp_state = TCPS_CLOSED;
        s->state = SS_UNCONNECTED;
        return err;
    }
    return 0;
}

static long sock_copyout_addr(const struct sockaddr_storage *addr, int len, uintptr_t uaddr, uintptr_t ulen) {
    if (!uok(ulen, 4) || !uwok(ulen, 4)) return -EFAULT;
    int cap = *(int *)ulen;
    if (cap < 0) return -EINVAL;
    int cp = len < cap ? len : cap;
    if (cp > 0) {
        if (!uwok(uaddr, (size_t)cp)) return -EFAULT;
        memcpy((void *)uaddr, addr, (size_t)cp);
    }
    *(int *)ulen = len;
    return 0;
}

long sock_getsockname(struct file *f, uintptr_t uaddr, uintptr_t ulen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (!s->local_len) {
        memset(&s->local, 0, sizeof s->local);
        s->local.ss_family = (uint16_t)s->domain;
        s->local_len = (s->domain == AF_NETLINK) ? (int)sizeof(struct sockaddr_nl) :
                       (s->domain == AF_INET)    ? (int)sizeof(struct sockaddr_in) : 2;
    }
    return sock_copyout_addr(&s->local, s->local_len, uaddr, ulen);
}

long sock_getpeername(struct file *f, uintptr_t uaddr, uintptr_t ulen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (s->state != SS_CONNECTED) return -ENOTCONN;
    return sock_copyout_addr(&s->peer, s->peer_len, uaddr, ulen);
}

long sock_sendto(struct file *f, uintptr_t ubuf, size_t len, int flags, uintptr_t uaddr, int addrlen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    if (len > SOCK_IO_MAX) return -EMSGSIZE;
    if (len && !uok(ubuf, len)) return -EFAULT;
    struct sockaddr_storage dst;
    memset(&dst, 0, sizeof dst);
    if (uaddr && addrlen > 0) {
        if (addrlen > (int)sizeof dst) return -EINVAL;
        if (!uok(uaddr, (size_t)addrlen)) return -EFAULT;
        memcpy(&dst, (const void *)uaddr, (size_t)addrlen);
    }
    uint8_t *kbuf = len ? kmalloc(len) : 0;
    if (len && !kbuf) return -ENOMEM;
    if (len) memcpy(kbuf, (const void *)ubuf, len);
    long r = sock_send_core(f, kbuf, len, flags, uaddr ? &dst : 0, addrlen);
    kfree(kbuf);
    return r;
}

long sock_recvfrom(struct file *f, uintptr_t ubuf, size_t len, int flags, uintptr_t uaddr, uintptr_t ulen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    if (uaddr && !ulen) return -EFAULT;
    if (len > SOCK_IO_MAX) return -EMSGSIZE;
    if (len && !uwok(ubuf, len)) return -EFAULT;
    uint8_t *kbuf = len ? kmalloc(len) : 0;
    if (len && !kbuf) return -ENOMEM;
    struct sockaddr_storage src;
    int srclen = 0;
    long r = sock_recv_core(f, kbuf, len, flags, &src, &srclen);
    if (r > 0 && !uwok(ubuf, (size_t)r)) r = -EFAULT;
    else if (r > 0) memcpy((void *)ubuf, kbuf, (size_t)r);
    kfree(kbuf);
    if (r >= 0 && uaddr && ulen && srclen > 0) {
        long ar = sock_copyout_addr(&src, srclen, uaddr, ulen);
        if (ar) return ar;
    }
    return r;
}

long sock_sendmsg(struct file *f, uintptr_t umsg, int flags) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    if (!uok(umsg, sizeof(struct msghdr))) return -EFAULT;
    struct msghdr msg;
    memcpy(&msg, (const void *)umsg, sizeof msg);
    if (msg.msg_iovlen > 1024) return -EINVAL;

    struct sockaddr_storage dst;
    memset(&dst, 0, sizeof dst);
    if (msg.msg_name && msg.msg_namelen > 0) {
        if (msg.msg_namelen > sizeof dst) return -EINVAL;
        if (!uok((uintptr_t)msg.msg_name, msg.msg_namelen)) return -EFAULT;
        memcpy(&dst, msg.msg_name, msg.msg_namelen);
    }

    size_t tot = 0;
    struct sock_iovec *iov = 0;
    if (msg.msg_iovlen > 0) {
        if (!uok((uintptr_t)msg.msg_iov, msg.msg_iovlen * sizeof *iov)) return -EFAULT;
        iov = kmalloc(msg.msg_iovlen * sizeof *iov);
        if (!iov) return -ENOMEM;
        memcpy(iov, msg.msg_iov, msg.msg_iovlen * sizeof *iov);
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len && !uok((uintptr_t)iov[i].iov_base, iov[i].iov_len)) {
                kfree(iov); return -EFAULT;
            }
            if (iov[i].iov_len > SOCK_IO_MAX - tot) { kfree(iov); return -EMSGSIZE; }
            tot += iov[i].iov_len;
        }
    }

    uint8_t *kbuf = tot ? kmalloc(tot) : 0;
    if (tot && !kbuf) { kfree(iov); return -ENOMEM; }
    size_t off = 0;
    for (size_t i = 0; i < msg.msg_iovlen; i++) {
        if (iov[i].iov_len) {
            memcpy(kbuf + off, iov[i].iov_base, iov[i].iov_len);
            off += iov[i].iov_len;
        }
    }
    kfree(iov);

    long r = sock_send_core(f, kbuf, tot, flags, msg.msg_name ? &dst : 0, (int)msg.msg_namelen);
    kfree(kbuf);
    return r;
}

long sock_recvmsg(struct file *f, uintptr_t umsg, int flags) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    if (!uok(umsg, sizeof(struct msghdr)) || !uwok(umsg, sizeof(struct msghdr))) return -EFAULT;
    struct msghdr msg;
    memcpy(&msg, (const void *)umsg, sizeof msg);
    if (msg.msg_iovlen > 1024) return -EINVAL;

    size_t tot = 0;
    struct sock_iovec *iov = 0;
    if (msg.msg_iovlen > 0) {
        if (!uok((uintptr_t)msg.msg_iov, msg.msg_iovlen * sizeof *iov)) return -EFAULT;
        iov = kmalloc(msg.msg_iovlen * sizeof *iov);
        if (!iov) return -ENOMEM;
        memcpy(iov, msg.msg_iov, msg.msg_iovlen * sizeof *iov);
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (iov[i].iov_len && !uwok((uintptr_t)iov[i].iov_base, iov[i].iov_len)) {
                kfree(iov); return -EFAULT;
            }
            if (iov[i].iov_len > SOCK_IO_MAX - tot) { kfree(iov); return -EMSGSIZE; }
            tot += iov[i].iov_len;
        }
    }

    uint8_t *kbuf = tot ? kmalloc(tot) : 0;
    if (tot && !kbuf) { kfree(iov); return -ENOMEM; }
    struct sockaddr_storage src;
    int srclen = 0;
    long r = sock_recv_core(f, kbuf, tot, flags, &src, &srclen);
    if (r > 0) {
        size_t rem = (size_t)r, off = 0;
        for (size_t i = 0; i < msg.msg_iovlen && rem > 0; i++) {
            size_t cp = iov[i].iov_len < rem ? iov[i].iov_len : rem;
            if (cp && !uwok((uintptr_t)iov[i].iov_base, cp)) { r = -EFAULT; break; }
            if (cp) memcpy(iov[i].iov_base, kbuf + off, cp);
            off += cp; rem -= cp;
        }
    }
    kfree(kbuf);
    kfree(iov);

    if (r >= 0) {
        if (msg.msg_name && srclen > 0) {
            size_t cp = (size_t)srclen < msg.msg_namelen ? (size_t)srclen : msg.msg_namelen;
            if (cp && !uwok((uintptr_t)msg.msg_name, cp)) return -EFAULT;
            if (cp) memcpy(msg.msg_name, &src, cp);
            msg.msg_namelen = (uint32_t)srclen;
        } else if (msg.msg_name) {
            msg.msg_namelen = 0;
        }
        msg.msg_controllen = 0;
        msg.msg_flags = 0;
        if (!uwok(umsg, sizeof msg)) return -EFAULT;
        memcpy((void *)umsg, &msg, sizeof msg);
    }
    return r;
}

long sock_shutdown(struct file *f, int how) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (how == SHUT_RD || how == SHUT_RDWR) s->shut_rd = 1;
    if (how == SHUT_WR || how == SHUT_RDWR) {
        s->shut_wr = 1;
        if (s->domain == AF_INET && s->type == SOCK_STREAM && s->tcp_state == TCPS_ESTAB) {
            uint32_t fseq = s->snd_nxt++;
            s->tcp_state = TCPS_FIN_WAIT1;
            tcp_send_seg(s, 0x11 /* FIN|ACK */, fseq, 0, 0, 0);
        }
        if (s->domain == AF_UNIX && s->un_peer && s->un_peer->used) {
            s->un_peer->eof = 1;
            wakeup(s->un_peer);
        }
    }
    wakeup(s);
    return 0;
}

long sock_setsockopt(struct file *f, int level, int optname, uintptr_t optval, int optlen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (optlen < 0 || !uok(optval, (size_t)optlen)) return -EFAULT;
    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR && optlen >= 4) { s->so_reuseaddr = *(const int *)optval != 0; return 0; }
        if (optname == SO_BROADCAST && optlen >= 4) { s->so_broadcast = *(const int *)optval != 0; return 0; }
        if (optname == SO_KEEPALIVE && optlen >= 4) { s->so_keepalive = *(const int *)optval != 0; return 0; }
        if ((optname == SO_RCVTIMEO || optname == SO_RCVTIMEO_NEW) && optlen >= 16) {
            const int64_t *tv = (const int64_t *)optval;
            s->rcv_tmo_ms = (int)(tv[0] * 1000 + tv[1] / 1000);
            return 0;
        }
        if ((optname == SO_SNDTIMEO || optname == SO_SNDTIMEO_NEW) && optlen >= 16) {
            const int64_t *tv = (const int64_t *)optval;
            s->snd_tmo_ms = (int)(tv[0] * 1000 + tv[1] / 1000);
            return 0;
        }
        if (optname == SO_RCVBUF || optname == SO_SNDBUF) return 0;
    }
    if (level == IPPROTO_TCP && optname == TCP_NODELAY) { s->tcp_nodelay = 1; return 0; }
    if (level == IPPROTO_IP && optname == IP_TTL) return 0;
    return 0;
}

long sock_getsockopt(struct file *f, int level, int optname, uintptr_t optval, uintptr_t optlen) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (!uok(optlen, 4) || !uwok(optlen, 4)) return -EFAULT;
    int cap = *(int *)optlen;
    if (cap < 4) return -EINVAL;
    if (!uwok(optval, 4)) return -EFAULT;
    int v = 0;
    if (level == SOL_SOCKET) {
        if (optname == SO_TYPE) v = s->type;
        else if (optname == SO_ERROR) { v = s->err; s->err = 0; }
        else if (optname == SO_REUSEADDR) v = s->so_reuseaddr;
        else if (optname == SO_BROADCAST) v = s->so_broadcast;
        else if (optname == SO_KEEPALIVE) v = s->so_keepalive;
        else if (optname == SO_RCVBUF || optname == SO_SNDBUF) v = SOCK_RXBUF;
    } else if (level == IPPROTO_TCP && optname == TCP_NODELAY) {
        v = s->tcp_nodelay;
    }
    *(int *)optval = v;
    *(int *)optlen = 4;
    return 0;
}

long sock_ioctl(struct file *f, unsigned long cmd, uintptr_t arg) {
    if (f->type != F_SOCKET) return -ENOTSOCK;
    struct socket *s = f->sock;
    if (cmd == FIONBIO) {
        if (!uok(arg, 4)) return -EFAULT;
        int nb = *(const int *)arg != 0;
        s->nonblock = nb;
        f->flags = nb ? (f->flags | O_NONBLOCK) : (f->flags & ~O_NONBLOCK);
        return 0;
    }
    if (cmd == FIONREAD) {
        if (!uwok(arg, 4)) return -EFAULT;
        int avail = 0;
        if (s->type == SOCK_STREAM) avail = (int)s->rxlen;
        else if (s->rxq_count > 0) avail = (int)s->rxq[s->rxq_head].len;
        *(int *)arg = avail;
        return 0;
    }
    if (cmd == SIOCGIFCONF) {
        net_ensure();
        if (!uok(arg, sizeof(struct ifconf)) || !uwok(arg, sizeof(struct ifconf))) return -EFAULT;
        struct ifconf *ifc = (struct ifconf *)arg;
        int cnt = net_if_count();
        int max_n = ifc->ifc_len / (int)sizeof(struct ifreq);
        int out_n = 0;
        for (int i = 0; i < cnt && out_n < max_n; i++) {
            struct net_ifinfo info;
            if (net_if_info(i, &info)) continue;
            uintptr_t base = (uintptr_t)ifc->ifc_req;
            size_t off = (size_t)out_n * sizeof(struct ifreq);
            if (base > (uintptr_t)-1 - off || !uwok(base + off, sizeof(struct ifreq))) return -EFAULT;
            struct ifreq *ifr = (struct ifreq *)(base + off);
            memset(ifr, 0, sizeof *ifr);
            strlcpy(ifr->ifr_name, info.name, sizeof ifr->ifr_name);
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_addr;
            in->sin_family = AF_INET;
            in->sin_addr = htonl_u(info.ip);
            out_n++;
        }
        ifc->ifc_len = out_n * (int)sizeof(struct ifreq);
        return 0;
    }
    if (cmd >= SIOCGIFNAME && cmd <= SIOCGIFINDEX) {
        net_ensure();
        if (!uok(arg, sizeof(struct ifreq)) || !uwok(arg, sizeof(struct ifreq))) return -EFAULT;
        struct ifreq *ifr = (struct ifreq *)arg;
        if (cmd == SIOCGIFNAME) {
            struct net_ifinfo info;
            if (net_if_info(ifr->ifru_ivalue - 1, &info)) return -ENODEV;
            strlcpy(ifr->ifr_name, info.name, sizeof ifr->ifr_name);
            return 0;
        }
        int idx = net_if_find(ifr->ifr_name);
        if (idx < 0) return -ENODEV;
        struct net_ifinfo info;
        if (net_if_info(idx, &info)) return -ENODEV;
        switch (cmd) {
        case SIOCGIFFLAGS:   ifr->ifru_flags = (short)info.flags; return 0;
        case SIOCSIFFLAGS:   return net_if_set_flags(idx, (uint32_t)(uint16_t)ifr->ifru_flags);
        case SIOCGIFADDR: {
            if (!info.ip) return -EADDRNOTAVAIL;
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_addr;
            memset(in, 0, sizeof *in);
            in->sin_family = AF_INET; in->sin_addr = htonl_u(info.ip);
            return 0;
        }
        case SIOCSIFADDR: {
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_addr;
            return net_if_set_addr(idx, ntohl_u(in->sin_addr), 0);
        }
        case SIOCGIFNETMASK: {
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_netmask;
            memset(in, 0, sizeof *in);
            in->sin_family = AF_INET; in->sin_addr = htonl_u(info.mask);
            return 0;
        }
        case SIOCSIFNETMASK: {
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_netmask;
            return net_if_set_addr(idx, info.ip, ntohl_u(in->sin_addr));
        }
        case SIOCGIFBRDADDR: {
            struct sockaddr_in *in = (struct sockaddr_in *)&ifr->ifru_broadaddr;
            memset(in, 0, sizeof *in);
            in->sin_family = AF_INET;
            in->sin_addr = htonl_u(info.mask ? (info.ip | ~info.mask) : 0);
            return 0;
        }
        case SIOCGIFHWADDR:
            memset(&ifr->ifru_hwaddr, 0, sizeof ifr->ifru_hwaddr);
            ifr->ifru_hwaddr.sa_family = (idx == 0) ? 772 : 1;
            memcpy(ifr->ifru_hwaddr.sa_data, info.mac, 6);
            return 0;
        case SIOCGIFMTU:     ifr->ifru_mtu = info.mtu; return 0;
        case SIOCGIFINDEX:   ifr->ifru_ivalue = info.index; return 0;
        }
    }
    return -ENOTTY;
}
