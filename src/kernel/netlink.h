/* Netlink's wire structures, as user space declares them.
 *
 * These are **interface facts**, like the syscall numbers and `struct
 * epoll_event`: a program built against Linux headers lays its request out
 * this way and reads the reply this way, so the layouts are copied from the
 * protocol's definition rather than invented. Getting one wrong is the
 * `epoll_event` mistake again — it compiles, it runs, and the reader finds a
 * field half a field late.
 */
#ifndef KERNEL_NETLINK_H
#define KERNEL_NETLINK_H

#include "core/types.h"

#define AF_NETLINK 16
#define NETLINK_ROUTE 0

/* A message header, 16 bytes, in front of every request and every reply. */
struct nlmsghdr {
    uint32_t nlmsg_len; /* this header and its payload */
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

#define NLMSG_NOOP 1u
#define NLMSG_ERROR 2u
#define NLMSG_DONE 3u

#define NLM_F_REQUEST 0x001u
#define NLM_F_MULTI 0x002u
#define NLM_F_ACK 0x004u
#define NLM_F_ROOT 0x100u
#define NLM_F_MATCH 0x200u
/* A dump: everything of this kind. Two bits, and a request carrying only one
 * of them is still a dump as far as this answers — Linux is equally lenient,
 * and refusing would break a caller that set only `NLM_F_ROOT`. */
#define NLM_F_DUMP (NLM_F_ROOT | NLM_F_MATCH)

/* **Everything in netlink is aligned to four bytes**, and the padding is
 * part of the length arithmetic rather than something a reader skips: a
 * message's own length is unaligned (it is the true byte count) while the
 * step to the *next* message is that length rounded up. Confusing the two
 * puts every message after the first at the wrong offset. */
#define NLMSG_ALIGN(n) (((n) + 3u) & ~3u)
#define NLMSG_HDRLEN NLMSG_ALIGN(sizeof(struct nlmsghdr))

/* The route family's message types, the few this answers. */
#define RTM_NEWLINK 16u
#define RTM_GETLINK 18u
#define RTM_NEWADDR 20u
#define RTM_GETADDR 22u
#define RTM_NEWROUTE 24u
#define RTM_GETROUTE 26u

/* A link: what `RTM_GETLINK` is answered with. */
struct ifinfomsg {
    uint8_t ifi_family;
    uint8_t __pad;
    uint16_t ifi_type;
    uint32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

/* An address on a link. `ifa_prefixlen` is the netmask, as a length — which
 * is why the routing table's prefix arithmetic is reused here rather than a
 * mask being reported. */
struct ifaddrmsg {
    uint8_t ifa_family;
    uint8_t ifa_prefixlen;
    uint8_t ifa_flags;
    uint8_t ifa_scope;
    uint32_t ifa_index;
};

/* A route. */
struct rtmsg {
    uint8_t rtm_family;
    uint8_t rtm_dst_len;
    uint8_t rtm_src_len;
    uint8_t rtm_tos;
    uint8_t rtm_table;
    uint8_t rtm_protocol;
    uint8_t rtm_scope;
    uint8_t rtm_type;
    uint32_t rtm_flags;
};

/* An attribute: a length, a kind, then the value, padded to four. The length
 * counts the header, which is the detail a hand-written encoder gets wrong. */
struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};

#define RTA_ALIGN(n) (((n) + 3u) & ~3u)
#define RTA_LENGTH(n) (RTA_ALIGN(sizeof(struct rtattr)) + (n))

#define IFLA_ADDRESS 1u
#define IFLA_IFNAME 3u
#define IFLA_MTU 4u

#define IFA_ADDRESS 1u
#define IFA_LOCAL 2u
#define IFA_LABEL 3u

#define RTA_DST 1u
#define RTA_OIF 4u
#define RTA_GATEWAY 5u

/* Hardware kinds, and the two this machine has. */
#define ARPHRD_ETHER 1u
#define ARPHRD_LOOPBACK 772u

#define IFF_UP 0x1u
#define IFF_LOOPBACK 0x8u
#define IFF_RUNNING 0x40u

/* Where a route came from and how far it reaches. `RT_SCOPE_LINK` is what
 * marks a route needing no gateway, and a tool that reads scope rather than
 * the presence of `RTA_GATEWAY` depends on it being right. */
#define RTPROT_KERNEL 2u
#define RTPROT_BOOT 3u
#define RT_SCOPE_UNIVERSE 0u
#define RT_SCOPE_LINK 253u
#define RT_SCOPE_HOST 254u
#define RT_TABLE_MAIN 254u
#define RTN_UNICAST 1u

#endif
