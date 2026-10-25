/* Resolves a name, from user space, over the board's own network.
 *
 * This is the proof for the roadmap's DNS item, whose complaint was precise:
 * the resolver's address is learned from DHCP and stored, a resolver in the
 * kernel would be the wrong place for one, "but nothing in user space can
 * resolve a name either". The missing piece was never the protocol — it was
 * that the address had nowhere to be read from. So this program does what
 * any resolver does, using only what a program can see:
 *
 *   1. read `/etc/resolv.conf` — which the kernel synthesises from the DHCP
 *      lease when the filesystem has none;
 *   2. build a query, send it over UDP to port 53;
 *   3. read the answer and walk the records to an A record.
 *
 * It is deliberately *not* a library: it proves the kernel's side, which is
 * the descriptor, the socket calls, the route and the device under them.
 *
 * Freestanding, static, no libc — the syscalls are the interface under test.
 *
 * The exit status is a bitmask of the steps that **passed**, so a partial
 * failure says where it stopped rather than just "no":
 *   1  resolv.conf opened and a nameserver parsed
 *   2  a UDP socket, bound and connected
 *   4  the query was sent whole
 *   8  an answer came back that matches the query id
 *   16 an A record was found in it
 */
#include <stdint.h>

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_SOCKET 198
#define SYS_CONNECT 203
#define SYS_SENDTO 206
#define SYS_RECVFROM 207

#define AF_INET 2
#define SOCK_DGRAM 2
/* **Non-blocking, and retried with a sleep between tries.** A blocking read
 * here is wrong twice over: a resolver whose server does not answer must
 * give up rather than hang its caller, and on this kernel a thread blocked
 * with no deadline is also the one case nothing can wake if the device it
 * waits on is only polled when something else happens. The first reason is
 * the one that would still apply on Linux. */
#define SOCK_NONBLOCK 0x800
#define SYS_NANOSLEEP 101
#define TRIES 200
#define SLEEP_NS 10000000L /* 10 ms a try, so 2 s in all */

#define AT_FDCWD (-100)

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

/* `struct sockaddr_in`, written out because there is no libc. The port and
 * the address are big-endian on the wire and in this structure. */
struct sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t pad[8];
};

static uint16_t to_be16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* The nameserver from resolv.conf's first `nameserver` line, as a big-endian
 * address. Zero if there is none — which is a real answer on a machine with
 * no lease, and distinct from a malformed line. */
static uint32_t parse_nameserver(const char *text, uint32_t len) {
    const char *want = "nameserver ";
    uint32_t at = 0;
    while (at < len) {
        uint32_t i = 0;
        while (want[i] != '\0' && at + i < len && text[at + i] == want[i]) {
            i++;
        }
        if (want[i] == '\0') {
            uint32_t octet = 0, got = 0, addr = 0;
            at += i;
            while (at < len && text[at] != '\n') {
                if (text[at] >= '0' && text[at] <= '9') {
                    octet = octet * 10u + (uint32_t)(text[at] - '0');
                } else if (text[at] == '.') {
                    addr = (addr << 8) | (octet & 0xffu);
                    octet = 0;
                    got++;
                }
                at++;
            }
            addr = (addr << 8) | (octet & 0xffu);
            return got == 3u ? addr : 0u;
        }
        while (at < len && text[at] != '\n') {
            at++;
        }
        at++;
    }
    return 0;
}

/* A name in DNS wire form: each label prefixed by its length, a zero at the
 * end. Returns how many bytes were written. */
static uint32_t encode_name(const char *name, uint8_t *out) {
    uint32_t at = 0, label = 0, i = 0;
    for (;;) {
        if (name[i] == '.' || name[i] == '\0') {
            out[at] = (uint8_t)label;
            at += label + 1u;
            label = 0;
            if (name[i] == '\0') {
                break;
            }
        } else {
            out[at + 1u + label] = (uint8_t)name[i];
            label++;
        }
        i++;
    }
    out[at] = 0;
    return at + 1u;
}

/* A standard A query for `name`, with the id the caller chose. */
static uint32_t build_query(const char *name, uint16_t id, uint8_t *out) {
    uint32_t at = 12;
    out[0] = (uint8_t)(id >> 8);
    out[1] = (uint8_t)id;
    out[2] = 0x01; /* recursion desired */
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x01; /* one question */
    for (uint32_t i = 6; i < 12u; i++) {
        out[i] = 0;
    }
    at += encode_name(name, out + at);
    out[at++] = 0x00;
    out[at++] = 0x01; /* A */
    out[at++] = 0x00;
    out[at++] = 0x01; /* IN */
    return at;
}

/* Steps a name in the answer section, which may be a compression pointer —
 * two bytes whose top bits are set, pointing back into the message. Not
 * followed: nothing here needs the name, only its length, and following an
 * attacker-supplied pointer is how a resolver loops for ever. */
static uint32_t skip_name(const uint8_t *msg, uint32_t len, uint32_t at) {
    while (at < len) {
        if ((msg[at] & 0xc0u) == 0xc0u) {
            return at + 2u;
        }
        if (msg[at] == 0) {
            return at + 1u;
        }
        at += (uint32_t)msg[at] + 1u;
    }
    return len;
}

/* The first A record's address, or zero. Bounded at every step: this parses
 * a reply from the network, so a truncated or lying length must end the walk
 * rather than read past the buffer. */
static uint32_t first_a_record(const uint8_t *msg, uint32_t len) {
    uint32_t answers, at = 12;
    if (len < 12u) {
        return 0;
    }
    answers = ((uint32_t)msg[6] << 8) | msg[7];
    at = skip_name(msg, len, at) + 4u; /* the question's type and class */
    for (uint32_t i = 0; i < answers && at + 10u <= len; i++) {
        uint32_t type, rdlen;
        at = skip_name(msg, len, at);
        if (at + 10u > len) {
            return 0;
        }
        type = ((uint32_t)msg[at] << 8) | msg[at + 1u];
        rdlen = ((uint32_t)msg[at + 8u] << 8) | msg[at + 9u];
        at += 10u;
        if (at + rdlen > len) {
            return 0;
        }
        if (type == 1u && rdlen == 4u) {
            return ((uint32_t)msg[at] << 24) | ((uint32_t)msg[at + 1u] << 16) |
                   ((uint32_t)msg[at + 2u] << 8) | msg[at + 3u];
        }
        at += rdlen;
    }
    return 0;
}

static uint32_t read_resolv_conf(char *buf, uint32_t room) {
    long fd = syscall6(SYS_OPENAT, AT_FDCWD, (long)"/etc/resolv.conf", 0, 0, 0, 0);
    long n;
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)buf, (long)room);
    (void)syscall3(SYS_CLOSE, fd, 0, 0);
    return n > 0 ? (uint32_t)n : 0u;
}

/* The buffers are static, not automatic: 128 + 256 + 512 bytes on one frame
 * is over the 1 KiB this tree holds every frame to, and the limit applies to
 * a user program for the same reason it applies to the kernel — the stack is
 * a fixed mapping and nothing warns when it is gone. */
static char conf[128];
static uint8_t query[256];
static uint8_t reply[512];

int user_main(void) {
    uint32_t passed = 0;
    uint32_t server = 0;
    uint32_t qlen;
    long sock, n;
    struct sockaddr_in to = {0};
    const uint16_t id = 0x4d4b; /* "MK", so a stray reply cannot match */

    server = parse_nameserver(conf, read_resolv_conf(conf, (uint32_t)sizeof conf));
    if (server == 0) {
        say("resolve: no nameserver in /etc/resolv.conf\n");
        (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    }
    passed |= 1u;
    say("resolve: nameserver read from /etc/resolv.conf\n");

    sock = syscall3(SYS_SOCKET, AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        say("resolve: no socket\n");
        (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    }
    to.family = AF_INET;
    to.port = to_be16(53);
    to.addr = __builtin_bswap32(server);
    if (syscall3(SYS_CONNECT, sock, (long)&to, (long)sizeof to) < 0) {
        say("resolve: cannot connect to the nameserver\n");
        (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    }
    passed |= 2u;

    qlen = build_query("one.one.one.one", id, query);
    n = syscall6(SYS_SENDTO, sock, (long)query, (long)qlen, 0, 0, 0);
    if (n != (long)qlen) {
        say("resolve: the query did not go out whole\n");
        (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    }
    passed |= 4u;
    say("resolve: query sent\n");

    {
        /* `struct timespec`, written out: no libc. */
        struct { long sec; long nsec; } nap = {0, SLEEP_NS};
        n = -1;
        for (uint32_t i = 0; i < TRIES; i++) {
            n = syscall6(SYS_RECVFROM, sock, (long)reply, (long)sizeof reply, 0, 0, 0);
            if (n > 0) {
                break;
            }
            (void)syscall3(SYS_NANOSLEEP, (long)&nap, 0, 0);
            /* **Re-sent, not just waited for.** The first datagram to a next
             * hop whose hardware address is not yet known can be dropped
             * while ARP resolves it — and a resolver that sends once then
             * only listens would report "no answer" for what is really "the
             * first packet paid for the ARP". Every real resolver retries;
             * this one retries on a schedule slow enough that a lost reply
             * and a slow server look the same from outside, which is all a
             * caller can act on anyway. */
            if (i % 25u == 24u) {
                (void)syscall6(SYS_SENDTO, sock, (long)query, (long)qlen, 0, 0, 0);
            }
        }
    }
    if (n < 12 || reply[0] != (uint8_t)(id >> 8) || reply[1] != (uint8_t)id) {
        say("resolve: no answer, or one for a different query\n");
        (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    }
    passed |= 8u;
    say("resolve: an answer came back\n");

    if (first_a_record(reply, (uint32_t)n) != 0) {
        passed |= 16u;
        say("resolve: an A record was found; a name resolved from user space\n");
    } else {
        say("resolve: the answer carried no A record\n");
    }
    (void)syscall3(SYS_EXIT, (long)passed, 0, 0);
    return 0;
}
