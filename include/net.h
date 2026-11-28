/* Network stack interface (kernel/net.c).  A NIC driver fills in a struct netdev, calls net_attach()
 * once, and feeds received frames to net_input() from its poll() hook. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "uapi/chiffon_abi.h"

#ifndef ENETDOWN
#define ENETDOWN 100
#endif
#define NETDEV_MAX 4                                 /* NICs the stack can hold at once */

struct netdev {
    const char *name;
    uint8_t mac[6];
    /* The stack reuses its TX buffer after send returns. Copy the frame or
     * finish consuming it before returning. The driver owns its DMA rings. */
    int (*send)(const uint8_t *frame, size_t len);   /* transmit one Ethernet frame: 0 or -errno */
    int (*poll)(void);                               /* deliver pending frames via net_input(): count */
    int (*link)(void);                               /* optional: 1 = carrier, 0 = none; NULL = always up.
                                                      * Must be cheap and must not print or sleep. */
};

int  net_attach(const struct netdev *dev);           /* a driver found a NIC: 0 on success.  `dev` must stay valid;
                                                      * up to NETDEV_MAX NICs may attach, the stack picks the
                                                      * first one that has carrier and answers DHCP. */
size_t net_dump(char *buf, size_t n);                /* /proc/net/chiffon: interfaces, link state, IPv4 */
size_t net_proc_dev(char *buf, size_t n);            /* /proc/net/dev: Linux-formatted interface stats */
/* Consume one borrowed RX frame synchronously, before the driver recycles it. */
void net_input(const uint8_t *frame, size_t len);
int  vnet_probe(uintptr_t mmio_pa);                 /* drivers/aarch64/virtio_net.c */
int  board_net_probe(void);                          /* board: attach the NIC(s); weak default: none */
long net_syscall(long nr, long a0, long a1, long a2, long a3);   /* CSYS_net_* numbers */

/* Socket-layer hooks into kernel/net.c */
struct net_ifinfo {
    char     name[16];
    int      index;          /* 1 = lo, 2+ = eth0.. */
    uint8_t  mac[6];
    uint16_t mtu;
    uint32_t flags;          /* IFF_UP | IFF_RUNNING | ... */
    uint32_t ip;             /* host byte order */
    uint32_t mask;           /* host byte order */
    uint32_t gw;             /* host byte order */
    uint32_t dns;            /* host byte order */
    uint64_t rx_packets, rx_bytes;
    uint64_t tx_packets, tx_bytes;
};

void net_probe_only(void);
int  net_ensure(void);
void net_poll_once(void);
void net_pause_once(void);
int  net_if_count(void);
int  net_if_info(int idx, struct net_ifinfo *out);
int  net_if_find(const char *name);
int  net_if_set_addr(int idx, uint32_t ip, uint32_t mask);
int  net_if_set_flags(int idx, uint32_t flags);
int  net_is_local_ip(uint32_t ip);
uint32_t net_source_ip(uint32_t dst);
int  net_ip_send(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *pl, size_t n);
uint16_t net_csum(const void *p, size_t n);
uint16_t net_tcp_udp_csum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *pl, size_t n);

/* Weak/registered callbacks implemented in kernel/socket.c */
void sock_icmp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n);
void sock_udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n);
void sock_tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n);
