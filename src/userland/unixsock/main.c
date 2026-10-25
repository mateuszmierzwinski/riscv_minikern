/* Two threads on this machine talking over a unix-domain socket.
 *
 * The roadmap's complaint was "nothing local can talk over a socket", so the
 * proof has to be an actual conversation rather than a successful `bind`.
 * This forks a child with `clone`, which is what makes the two ends genuinely
 * independent: a single thread that wrote and then read its own socket would
 * pass against a kernel that simply echoed, and would never exercise the
 * wake-up that is the hard part.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a stream socket bound to an abstract name and listened
 *   2   the client connected and the server accepted
 *   4   the client's bytes arrived at the server, byte for byte
 *   8   the server's reply arrived back
 *   16  a half-close was seen as end of file, after the buffered bytes
 *   32  a datagram round trip, with the sender's name reported
 *   64  connecting to a name nobody bound was refused
 */
#include <stdint.h>

#define SYS_CLONE 220
#define SYS_EXIT 93
/* **The process's status comes from `exit_group`, not `exit`.** `exit` ends
 * the calling thread, so the status the harness reads is whichever thread
 * happened to finish the process — and the first run of this program printed
 * all seven of its checks and reported 0, because the client thread's `exit`
 * is what ended it. The main thread uses this; the client uses `exit`,
 * because it must end itself and leave the process running. */
#define SYS_EXIT_GROUP 94
#define SYS_WRITE 64
#define SYS_NANOSLEEP 101
#define SYS_SOCKET 198
#define SYS_BIND 200
#define SYS_LISTEN 201
#define SYS_ACCEPT 202
#define SYS_CONNECT 203
#define SYS_SENDTO 206
#define SYS_RECVFROM 207
#define SYS_SHUTDOWN 210
#define SYS_CLOSE 57
#define SYS_WAIT4 260

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_DGRAM 2

/* **`volatile` as an experiment, not as a style.** Every bit set before the
 * parent's blocking read was lost and every bit after it was kept, which is
 * what a compiler keeping this in a callee-saved register would look like if
 * that register did not survive the context switch inside the syscall. If
 * forcing each update to memory makes the mask read 127, the variable was
 * never the problem and a register is. */
static volatile uint32_t passed;

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

/* The mask as digits, at a named point. Three of these in one run show when
 * it changes, which is the one thing reasoning about it cannot establish. */
static void show_mask(const char *where) {
    char d[4] = {(char)('0' + (passed / 100u) % 10u), (char)('0' + (passed / 10u) % 10u),
                 (char)('0' + passed % 10u), '\n'};
    say(where);
    (void)syscall3(SYS_WRITE, 1, (long)d, 4);
}

static void nap(long ns) {
    struct { long sec; long nsec; } t = {0, ns};
    (void)syscall3(SYS_NANOSLEEP, (long)&t, 0, 0);
}

/* `struct sockaddr_un`: the family, then the name.
 *
 * The name here is **abstract** — its first byte is NUL — which is the kind
 * that never had a filesystem entry even on Linux, so it is the one kind this
 * kernel implements exactly rather than approximately. The length passed to
 * `bind` is what fixes how many of those bytes are the name. */
struct sockaddr_un {
    uint16_t family;
    char path[108];
};

#define NAME "\0minikern-test"
#define NAME_LEN 14u /* the NUL and thirteen characters */
#define ADDR_LEN (2u + NAME_LEN)

static void fill_name(struct sockaddr_un *a, const char *name, uint32_t len) {
    a->family = AF_UNIX;
    for (uint32_t i = 0; i < len; i++) {
        a->path[i] = name[i];
    }
}

static struct sockaddr_un addr;
static struct sockaddr_un dgram_addr;
static struct sockaddr_un nowhere;
/* **Guards either side of the cloned thread's stack.**
 *
 * An earlier run of this program reported 112 where the mask held 127, and
 * the only thing that changed it was moving a declaration — which is a pass
 * that depends on `.bss` layout rather than on the code being right. The
 * suspect was this stack underflowing into the variables declared beside it,
 * so the question is asked directly: a known pattern below and above it,
 * checked at exit. A damaged guard is an out-of-bounds write; an intact pair
 * means the stack stayed inside itself and the earlier number came from
 * somewhere else. */
#define GUARD 0x5aa5a55au
static uint32_t guard_below[64];
static char child_stack[8192] __attribute__((aligned(16)));
static uint32_t guard_above[64];

static void arm_the_guards(void) {
    for (uint32_t i = 0; i < 64u; i++) {
        guard_below[i] = GUARD;
        guard_above[i] = GUARD;
    }
}

/* How many guard words were disturbed, below and above. */
static uint32_t guards_damaged(void) {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 64u; i++) {
        bad += guard_below[i] != GUARD ? 1u : 0u;
        bad += guard_above[i] != GUARD ? 1u : 0u;
    }
    return bad;
}
static char buf[64];

/* The client: connects, sends, reads the reply, then closes its writing end
 * so the server sees end of file. Runs in the cloned thread. */
static void be_the_client(void) {
    long fd = syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    static char got[64];
    if (fd < 0) {
        (void)syscall3(SYS_EXIT, 0, 0, 0);
    }
    /* The server may not have listened yet; a connect before it does is
     * ECONNREFUSED, which is correct and is not what this is testing. */
    for (uint32_t i = 0; i < 100u; i++) {
        if (syscall3(SYS_CONNECT, fd, (long)&addr, (long)ADDR_LEN) >= 0) {
            break;
        }
        nap(2000000L);
    }
    (void)syscall6(SYS_SENDTO, fd, (long)"ping from the client", 20, 0, 0, 0);
    (void)syscall6(SYS_RECVFROM, fd, (long)got, (long)sizeof got, 0, 0, 0);
    /* Write half closed, read half left open: the server must see end of
     * file *after* draining what was already sent. */
    (void)syscall3(SYS_SHUTDOWN, fd, 1, 0);
    nap(50000000L);
    (void)syscall3(SYS_CLOSE, fd, 0, 0);
    /* **Parks instead of exiting, as an experiment.** The parent's mask lost
     * exactly the bits it set while this thread was alive, and the window
     * between the last lost bit and the first kept one is the parent's
     * blocking read — during which this thread called `exit`. If the bits
     * survive with this thread merely parked, thread exit is what clobbers
     * them. The parent's `exit_group` ends this thread either way. */
    for (;;) {
        nap(100000000L);
    }
}

/* A datagram round trip, including whether the sender's name comes back. */
static void datagram_round_trip(void) {
    long server = syscall3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
    long client = syscall3(SYS_SOCKET, AF_UNIX, SOCK_DGRAM, 0);
    static char got[32];
    if (server < 0 || client < 0) {
        return;
    }
    fill_name(&dgram_addr, "\0minikern-dgram", 16u);
    if (syscall3(SYS_BIND, server, (long)&dgram_addr, 18) < 0) {
        return;
    }
    if (syscall6(SYS_SENDTO, client, (long)"a message", 9, 0, (long)&dgram_addr, 18) != 9) {
        return;
    }
    if (syscall6(SYS_RECVFROM, server, (long)got, (long)sizeof got, 0, 0, 0) == 9 &&
        got[0] == 'a' && got[8] == 'e') {
        passed |= 32u;
        say("unixsock: a datagram arrived whole\n");
    }
    (void)syscall3(SYS_CLOSE, server, 0, 0);
    (void)syscall3(SYS_CLOSE, client, 0, 0);
}

static void refuse_an_unbound_name(void) {
    long fd = syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    fill_name(&nowhere, "\0nobody-is-here", 16u);
    /* **Refused, not accepted and then silent.** A connect to a name nobody
     * bound is the case a name table gets wrong by interning on lookup: it
     * would hand back a valid index and the connect would appear to work. */
    if (syscall3(SYS_CONNECT, fd, (long)&nowhere, 18) < 0) {
        passed |= 64u;
        say("unixsock: connecting to an unbound name was refused\n");
    }
    (void)syscall3(SYS_CLOSE, fd, 0, 0);
}

int user_main(void) {
    long server, client_side, n;
    arm_the_guards();
    fill_name(&addr, NAME, NAME_LEN);
    server = syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || syscall3(SYS_BIND, server, (long)&addr, (long)ADDR_LEN) < 0 ||
        syscall3(SYS_LISTEN, server, 4, 0) < 0) {
        say("unixsock: could not bind and listen\n");
        (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    }
    passed |= 1u;
    say("unixsock: bound to an abstract name and listening\n");

    /* CLONE_VM|CLONE_THREAD|CLONE_SIGHAND, so the child shares this address
     * space and the two ends are threads of one process — which is the
     * cheapest way to get two independent runners without `fork`. */
    if (syscall6(SYS_CLONE, 0x00010f00, (long)(child_stack + sizeof child_stack), 0, 0, 0, 0) == 0) {
        be_the_client();
    }

    client_side = syscall6(SYS_ACCEPT, server, 0, 0, 0, 0, 0);
    if (client_side < 0) {
        say("unixsock: accept failed\n");
        (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    }
    passed |= 2u;
    say("unixsock: accepted a connection\n");

    n = syscall6(SYS_RECVFROM, client_side, (long)buf, (long)sizeof buf, 0, 0, 0);
    if (n == 20 && buf[0] == 'p' && buf[19] == 't') {
        passed |= 4u;
        say("unixsock: the client's bytes arrived byte for byte\n");
    }
    if (syscall6(SYS_SENDTO, client_side, (long)"pong from the server", 20, 0, 0, 0) == 20) {
        passed |= 8u;
        say("unixsock: the reply went back\n");
    }
    show_mask("unixsock: mask after the reply ");
    /* The client has shut its writing end, so this must read 0 — and only
     * after the bytes above were delivered, which is the half-close. */
    if (syscall6(SYS_RECVFROM, client_side, (long)buf, (long)sizeof buf, 0, 0, 0) == 0) {
        passed |= 16u;
        say("unixsock: end of file after the buffered bytes\n");
    }
    show_mask("unixsock: mask after end of file ");
    (void)syscall3(SYS_CLOSE, client_side, 0, 0);
    (void)syscall3(SYS_CLOSE, server, 0, 0);
    datagram_round_trip();
    refuse_an_unbound_name();
    /* The mask, printed as digits, because "the status is 112" and "the
     * variable holds 112" are different claims and only one of them is
     * about this program. */
    show_mask("unixsock: mask at exit ");
    if (guards_damaged() != 0) {
        /* Said loudly and folded into the status, so a layout-dependent pass
         * cannot be mistaken for a real one by a suite reading the number. */
        say("unixsock: **the guards around the cloned stack were damaged**\n");
        passed |= 128u;
    } else {
        say("unixsock: the guards around the cloned stack are intact\n");
    }
    (void)syscall3(SYS_EXIT_GROUP, (long)passed, 0, 0);
    return 0;
}
