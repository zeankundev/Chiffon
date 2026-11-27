/* BSD socket & Netlink kernel interface (kernel/socket.c).
 * Implements Linux-compatible AF_INET, AF_NETLINK, and AF_UNIX sockets over
 * Chiffon's file descriptor table and kernel/net.c IPv4 stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    1
#define AF_INET     2
#define AF_NETLINK  16

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

#define NETLINK_ROUTE 0

#define MSG_OOB       0x1
#define MSG_PEEK      0x2
#define MSG_DONTROUTE 0x4
#define MSG_TRUNC     0x20
#define MSG_DONTWAIT  0x40
#define MSG_WAITALL   0x100
#define MSG_NOSIGNAL  0x4000

#define SOL_SOCKET    1
#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_DONTROUTE  5
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21
#define SO_RCVTIMEO_NEW 66
#define SO_SNDTIMEO_NEW 67

#define TCP_NODELAY   1
#define IP_TTL        2

#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

/* Interface flags (IFF_*) */
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_NOARP     0x80
#define IFF_MULTICAST 0x1000

/* Socket ioctls */
#define FIONREAD       0x541B
#define FIONBIO        0x5421
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;      /* network byte order */
    uint32_t sin_addr;      /* network byte order */
    uint8_t  sin_zero[8];
};

struct sockaddr_un {
    uint16_t sun_family;
    char     sun_path[108];
};

struct sockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

struct sockaddr_storage {
    uint16_t ss_family;
    char     __data[126];
};

struct sock_iovec {
    void  *iov_base;
    size_t iov_len;
};

struct msghdr {
    void              *msg_name;
    uint32_t           msg_namelen;
    uint32_t           __pad0;
    struct sock_iovec *msg_iov;
    size_t             msg_iovlen;
    void              *msg_control;
    size_t             msg_controllen;
    int                msg_flags;
    int                __pad1;
};

struct ifreq {
    char ifr_name[16];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short           ifru_flags;
        int             ifru_ivalue;
        int             ifru_mtu;
        char            ifru_slave[16];
        char            ifru_newname[16];
        void           *ifru_data;
    };
};

struct ifconf {
    int ifc_len;
    int __pad;
    union {
        char         *ifc_buf;
        struct ifreq *ifc_req;
    };
};

/* Netlink definitions */
#define NLMSG_ALIGNTO   4U
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN    ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)

#define NLMSG_NOOP      1
#define NLMSG_ERROR     2
#define NLMSG_DONE      3

#define NLM_F_REQUEST   0x01
#define NLM_F_MULTI     0x02
#define NLM_F_ACK       0x04
#define NLM_F_ROOT      0x100
#define NLM_F_MATCH     0x200

#define RTM_NEWLINK     16
#define RTM_DELLINK     17
#define RTM_GETLINK     18
#define RTM_NEWADDR     20
#define RTM_DELADDR     21
#define RTM_GETADDR     22
#define RTM_NEWROUTE    24
#define RTM_DELROUTE    25
#define RTM_GETROUTE    26

#define RTA_ALIGNTO     4U
#define RTA_ALIGN(len)  (((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_LENGTH(len) (RTA_ALIGN(sizeof(struct rtattr)) + (len))

#define IFLA_ADDRESS    1
#define IFLA_BROADCAST  2
#define IFLA_IFNAME     3
#define IFLA_MTU        4
#define IFLA_LINK       5
#define IFLA_QDISC      6
#define IFLA_STATS      7
#define IFLA_TXQLEN     13
#define IFLA_OPERSTATE  16

#define IFA_ADDRESS     1
#define IFA_LOCAL       2
#define IFA_LABEL       3
#define IFA_BROADCAST   4
#define IFA_FLAGS       8

#define RTA_DST         1
#define RTA_SRC         2
#define RTA_OIF         4
#define RTA_GATEWAY     5
#define RTA_PRIORITY    6
#define RTA_PREFSRC     7
#define RTA_TABLE       15

#define RT_TABLE_MAIN   254
#define RTN_UNICAST     1
#define RTPROT_KERNEL   2
#define RTPROT_BOOT     3
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK   253
#define RT_SCOPE_HOST   254

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct nlmsgerr {
    int             error;
    struct nlmsghdr msg;
};

struct rtgenmsg {
    uint8_t rtgen_family;
};

struct ifinfomsg {
    uint8_t  ifi_family;
    uint8_t  __ifi_pad;
    uint16_t ifi_type;
    int32_t  ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

struct ifaddrmsg {
    uint8_t  ifa_family;
    uint8_t  ifa_prefixlen;
    uint8_t  ifa_flags;
    uint8_t  ifa_scope;
    uint32_t ifa_index;
};

struct rtmsg {
    uint8_t  rtm_family;
    uint8_t  rtm_dst_len;
    uint8_t  rtm_src_len;
    uint8_t  rtm_tos;
    uint8_t  rtm_table;
    uint8_t  rtm_protocol;
    uint8_t  rtm_scope;
    uint8_t  rtm_type;
    uint32_t rtm_flags;
};

struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};

struct file;
struct socket;

long sock_create(int domain, int type, int protocol, struct file **out);
long sock_socketpair(int domain, int type, int protocol, struct file **out0, struct file **out1);
void sock_close(struct socket *s);
ssize_t sock_read(struct file *f, void *buf, size_t n);
ssize_t sock_write(struct file *f, const void *buf, size_t n);
int  sock_readable(struct file *f);
int  sock_writable(struct file *f);
int  sock_hangup(struct file *f);
long sock_ioctl(struct file *f, unsigned long cmd, uintptr_t arg);

long sock_bind(struct file *f, uintptr_t uaddr, int addrlen);
long sock_listen(struct file *f, int backlog);
long sock_accept(struct file *f, uintptr_t uaddr, uintptr_t ulen, int flags, struct file **out);
long sock_connect(struct file *f, uintptr_t uaddr, int addrlen);
long sock_getsockname(struct file *f, uintptr_t uaddr, uintptr_t ulen);
long sock_getpeername(struct file *f, uintptr_t uaddr, uintptr_t ulen);
long sock_sendto(struct file *f, uintptr_t ubuf, size_t len, int flags, uintptr_t uaddr, int addrlen);
long sock_recvfrom(struct file *f, uintptr_t ubuf, size_t len, int flags, uintptr_t uaddr, uintptr_t ulen);
long sock_sendmsg(struct file *f, uintptr_t umsg, int flags);
long sock_recvmsg(struct file *f, uintptr_t umsg, int flags);
long sock_shutdown(struct file *f, int how);
long sock_setsockopt(struct file *f, int level, int optname, uintptr_t optval, int optlen);
long sock_getsockopt(struct file *f, int level, int optname, uintptr_t optval, uintptr_t optlen);
