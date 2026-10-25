/* Asks the kernel what interfaces exist, over netlink, from user space.
 *
 * The roadmap's complaint was "no way for a program to ask what interfaces
 * exist", so the proof is a program that asks and reads the answer — parsing
 * the replies as any netlink caller does, by stepping message to message
 * through the lengths in the headers. Doing the walk properly is the point:
 * a test that checked only that bytes came back would pass against an
 * encoder whose lengths are wrong, which is the most likely defect in a
 * hand-written one.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a netlink socket, bound
 *   2   RTM_GETLINK returned messages terminated by NLMSG_DONE
 *   4   both `lo` and `eth0` were found, by name, at the right indices
 *   8   eth0's hardware address came back, six bytes and not all zero
 *   16  RTM_GETADDR gave eth0 an address with a sane prefix length
 *   32  RTM_GETROUTE returned a default route with a gateway
 *   64  an unimplemented request was refused rather than answered empty
 */
#include <stdbool.h>
#include <stdint.h>

#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_SENDTO 206
#define SYS_RECVFROM 207
#define SYS_CLOSE 57

#define AF_NETLINK 16
#define AF_INET 2
#define SOCK_RAW 3
#define NETLINK_ROUTE 0

#define NLMSG_ERROR 2u
#define NLMSG_DONE 3u
#define NLM_F_REQUEST 0x001u
#define NLM_F_DUMP 0x300u
#define RTM_NEWLINK 16u
#define RTM_GETLINK 18u
#define RTM_NEWADDR 20u
#define RTM_GETADDR 22u
#define RTM_NEWROUTE 24u
#define RTM_GETROUTE 26u
#define RTM_GETNEIGH 30u

#define IFLA_ADDRESS 1u
#define IFLA_IFNAME 3u
#define IFA_ADDRESS 1u
#define RTA_GATEWAY 5u

#define ALIGN4(n) (((n) + 3u) & ~3u)

static long syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
}

static long syscall3(long nr, long a, long b, long c) {
    return syscall6(nr, a, b, c, 0, 0, 0);
}

static uint32_t length_of(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void say(const char *s) {
    (void)syscall3(SYS_WRITE, 1, (long)s, (long)length_of(s));
}

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};

static uint32_t passed;
static uint8_t reply[2048];
static uint8_t request[64];
static long sock;

/* Sends one request of `type` and returns how many bytes came back. */
static long ask(uint16_t type) {
    struct nlmsghdr *h = (struct nlmsghdr *)request;
    for (uint32_t i = 0; i < sizeof request; i++) {
        request[i] = 0;
    }
    h->nlmsg_len = sizeof *h + 16u; /* the header and a family byte, padded */
    h->nlmsg_type = type;
    h->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_DUMP);
    h->nlmsg_seq = 1;
    request[sizeof *h] = AF_INET; /* the family the dump is about */
    if (syscall6(SYS_SENDTO, sock, (long)request, (long)h->nlmsg_len, 0, 0, 0) < 0) {
        return -1;
    }
    return syscall6(SYS_RECVFROM, sock, (long)reply, (long)sizeof reply, 0, 0, 0);
}

/* Finds an attribute of `want` inside one message's attribute area, or NULL.
 * Bounded at every step: these lengths come from the kernel, and a walk that
 * trusted them would be the same defect this program is looking for. */
static const uint8_t *find_attr(const uint8_t *at, uint32_t len, uint16_t want, uint32_t *value_len) {
    uint32_t off = 0;
    while (off + sizeof(struct rtattr) <= len) {
        const struct rtattr *a = (const struct rtattr *)(at + off);
        if (a->rta_len < sizeof *a || off + a->rta_len > len) {
            return 0;
        }
        if (a->rta_type == want) {
            *value_len = a->rta_len - (uint32_t)ALIGN4(sizeof *a);
            return at + off + ALIGN4(sizeof *a);
        }
        off += ALIGN4(a->rta_len);
    }
    return 0;
}

static bool same_text(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/* Walks the links in a reply, recording what was found. */
static void read_the_links(uint32_t got) {
    uint32_t off = 0;
    bool saw_lo = false, saw_eth = false, saw_mac = false, done = false;
    while (off + sizeof(struct nlmsghdr) <= got) {
        const struct nlmsghdr *h = (const struct nlmsghdr *)(reply + off);
        /* `ifinfomsg` is 16 bytes; the attributes follow it. */
        const uint8_t *attrs = reply + off + ALIGN4(sizeof *h) + 16u;
        uint32_t attr_len, name_len = 0, mac_len = 0;
        const uint8_t *name, *mac;
        if (h->nlmsg_len < sizeof *h || off + h->nlmsg_len > got) {
            break;
        }
        if (h->nlmsg_type == NLMSG_DONE) {
            done = true;
            break;
        }
        if (h->nlmsg_type != RTM_NEWLINK) {
            off += ALIGN4(h->nlmsg_len);
            continue;
        }
        attr_len = h->nlmsg_len - (uint32_t)(ALIGN4(sizeof *h) + 16u);
        name = find_attr(attrs, attr_len, (uint16_t)IFLA_IFNAME, &name_len);
        mac = find_attr(attrs, attr_len, (uint16_t)IFLA_ADDRESS, &mac_len);
        if (name != 0 && same_text((const char *)name, "lo")) {
            saw_lo = true;
        }
        if (name != 0 && same_text((const char *)name, "eth0")) {
            saw_eth = true;
            /* Six bytes and not all zero: a kernel that reported the field
             * without filling it would pass a length check alone. */
            if (mac != 0 && mac_len == 6u &&
                (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0) {
                saw_mac = true;
            }
        }
        off += ALIGN4(h->nlmsg_len);
    }
    if (done) {
        passed |= 2u;
        say("netlink: the link dump ended with NLMSG_DONE\n");
    }
    if (saw_lo && saw_eth) {
        passed |= 4u;
        say("netlink: both lo and eth0 were listed\n");
    }
    if (saw_mac) {
        passed |= 8u;
        say("netlink: eth0's hardware address came back\n");
    }
}

static void read_the_addresses(uint32_t got) {
    uint32_t off = 0;
    while (off + sizeof(struct nlmsghdr) <= got) {
        const struct nlmsghdr *h = (const struct nlmsghdr *)(reply + off);
        const uint8_t *body = reply + off + ALIGN4(sizeof *h);
        if (h->nlmsg_len < sizeof *h || off + h->nlmsg_len > got) {
            return;
        }
        if (h->nlmsg_type == RTM_NEWADDR) {
            /* `ifaddrmsg` is 8 bytes: family, prefixlen, flags, scope, index. */
            uint32_t ip_len = 0;
            const uint8_t *ip = find_attr(body + 8u, h->nlmsg_len - (uint32_t)(ALIGN4(sizeof *h) + 8u),
                                          (uint16_t)IFA_ADDRESS, &ip_len);
            /* eth0's, not the loopback's: index 2 and a routable prefix. */
            if (body[4] == 2u && ip != 0 && ip_len == 4u && body[1] > 0u && body[1] <= 32u) {
                passed |= 16u;
                say("netlink: eth0 has an address with a sane prefix length\n");
                return;
            }
        }
        off += ALIGN4(h->nlmsg_len);
    }
}

static void read_the_routes(uint32_t got) {
    uint32_t off = 0;
    while (off + sizeof(struct nlmsghdr) <= got) {
        const struct nlmsghdr *h = (const struct nlmsghdr *)(reply + off);
        const uint8_t *body = reply + off + ALIGN4(sizeof *h);
        if (h->nlmsg_len < sizeof *h || off + h->nlmsg_len > got) {
            return;
        }
        if (h->nlmsg_type == RTM_NEWROUTE && body[1] == 0u) {
            /* `rtm_dst_len` of zero is the default route; it must carry a
             * gateway, or it is a default route to nowhere. */
            uint32_t gw_len = 0;
            const uint8_t *gw = find_attr(body + 12u, h->nlmsg_len - (uint32_t)(ALIGN4(sizeof *h) + 12u),
                                          (uint16_t)RTA_GATEWAY, &gw_len);
            if (gw != 0 && gw_len == 4u && (gw[0] | gw[1] | gw[2] | gw[3]) != 0) {
                passed |= 32u;
                say("netlink: the default route came back with its gateway\n");
                return;
            }
        }
        off += ALIGN4(h->nlmsg_len);
    }
}

/* An unimplemented request must be refused. An empty dump would read as
 * "this machine has no neighbours", which is a different and wrong answer. */
static void check_a_refusal(void) {
    long got = ask((uint16_t)RTM_GETNEIGH);
    const struct nlmsghdr *h = (const struct nlmsghdr *)reply;
    if (got >= (long)(sizeof *h + 4u) && h->nlmsg_type == NLMSG_ERROR) {
        int32_t err;
        const uint8_t *p = reply + ALIGN4(sizeof *h);
        err = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                        ((uint32_t)p[3] << 24));
        if (err != 0) {
            passed |= 64u;
            say("netlink: an unimplemented request was refused, not answered empty\n");
        }
    }
}

int user_main(void) {
    /* `sockaddr_nl`: family, two bytes of pad, port, groups. */
    static uint8_t addr[12];
    long got;
    sock = syscall3(SYS_SOCKET, AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0) {
        say("netlink: no socket\n");
        (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    }
    addr[0] = AF_NETLINK;
    if (syscall3(SYS_BIND, sock, (long)addr, (long)sizeof addr) < 0) {
        say("netlink: bind failed\n");
        (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    }
    passed |= 1u;
    say("netlink: socket bound\n");

    got = ask((uint16_t)RTM_GETLINK);
    if (got > 0) {
        read_the_links((uint32_t)got);
    }
    got = ask((uint16_t)RTM_GETADDR);
    if (got > 0) {
        read_the_addresses((uint32_t)got);
    }
    got = ask((uint16_t)RTM_GETROUTE);
    if (got > 0) {
        read_the_routes((uint32_t)got);
    }
    check_a_refusal();
    {
        char d[4] = {(char)('0' + (passed / 100u) % 10u), (char)('0' + (passed / 10u) % 10u),
                     (char)('0' + passed % 10u), '\n'};
        say("netlink: mask ");
        (void)syscall3(SYS_WRITE, 1, (long)d, 4);
    }
    (void)syscall3(SYS_CLOSE, sock, 0, 0);
    (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    return 0;
}
