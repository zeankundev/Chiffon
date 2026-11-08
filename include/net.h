/* Network stack interface (kernel/net.c).  A NIC driver fills in a struct netdev, calls net_attach()
 * once, and feeds received frames to net_input() from its poll() hook. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "uapi/chiffon_abi.h"

struct netdev {
    const char *name;
    uint8_t mac[6];
    int (*send)(const uint8_t *frame, size_t len);   /* transmit one Ethernet frame: 0 or -errno */
    int (*poll)(void);                               /* deliver pending frames via net_input(): count */
};

int  net_attach(const struct netdev *dev);           /* a driver found a NIC: 0 on success */
void net_input(const uint8_t *frame, size_t len);    /* one Ethernet frame from the driver */
int  vnet_probe(uintptr_t mmio_pa);                 /* drivers/aarch64/virtio_net.c */
int  board_net_probe(void);                          /* board: attach the NIC(s); weak default: none */
long net_syscall(long nr, long a0, long a1, long a2, long a3);   /* CSYS_net_* numbers */
