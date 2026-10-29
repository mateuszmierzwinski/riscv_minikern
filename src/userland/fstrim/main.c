/* `fstrim`, as a program asks for it: `FITRIM` on the mount point.
 *
 * A megabyte is written and removed, so the filesystem has free space it
 * once used, and then the root is trimmed through the ioctl Linux's
 * `fstrim` sends. The kernel's own self-test checks the bytes against what
 * the device saw; this checks the system-call contract around it — the
 * struct copied in and back, the minimum honoured, an unknown request
 * refused, and an unprivileged caller turned away.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   the root opened as a directory
 *   2   a megabyte written
 *   4   and removed
 *   8   FITRIM succeeded and reported bytes trimmed
 *   16  a minimum longer than any free extent trimmed nothing
 *   32  an unknown ioctl on a directory was ENOTTY
 *   64  FITRIM from a non-root caller was EPERM
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_UNLINKAT 35
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_SETRESUID 147

#define AT_FDCWD (-100)
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_DIRECTORY 0200000
#define FITRIM 0xc0185879u /* _IOWR('X', 121, struct fstrim_range) */
#define ENOTTY 25
#define EPERM 1

struct fstrim_range {
    uint64_t start;
    uint64_t len;
    uint64_t minlen;
};

static uint8_t chunk[65536];

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static uint32_t length_of(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void say(const char *s) {
    (void)syscall4(SYS_WRITE, 1, (long)s, (long)length_of(s), 0);
}

/* A number in decimal, then a line's end. */
static void say_number(uint64_t v) {
    char d[24];
    uint32_t at = sizeof d - 1u;
    d[at] = '\0';
    do {
        d[--at] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && at > 0);
    say(&d[at]);
    say("\n");
}

static long trim(long dir, uint64_t minlen, struct fstrim_range *r) {
    *r = (struct fstrim_range){.start = 0, .len = ~(uint64_t)0, .minlen = minlen};
    return syscall4(SYS_IOCTL, dir, (long)FITRIM, (long)r, 0);
}

/* A megabyte at `path`; 2 when it all went. */
static uint32_t write_megabyte(const char *path) {
    long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    long put = 0;
    for (uint32_t i = 0; i < sizeof chunk; i++) {
        chunk[i] = (uint8_t)(i * 7u + 1u);
    }
    for (uint32_t i = 0; fd >= 0 && i < 16u; i++) {
        put += syscall4(SYS_WRITE, fd, (long)chunk, sizeof chunk, 0);
    }
    if (fd >= 0) {
        (void)syscall4(SYS_CLOSE, fd, 0, 0, 0);
    }
    return put == 16 * (long)sizeof chunk ? 2u : 0u;
}

/* The checks a root caller can make: bits 8, 16 and 32. */
static uint32_t root_checks(long dir) {
    struct fstrim_range r;
    uint32_t passed = 0;
    if (trim(dir, 0, &r) == 0 && r.len > 0) {
        passed |= 8u;
        say("fstrim: / trimmed, bytes: ");
        say_number(r.len);
    }
    if (trim(dir, (uint64_t)1 << 40, &r) == 0 && r.len == 0) {
        passed |= 16u;
        say("fstrim: a long minimum trimmed nothing\n");
    }
    if (syscall4(SYS_IOCTL, dir, 0x5401, (long)&r, 0) == -ENOTTY) {
        passed |= 32u;
        say("fstrim: an unknown ioctl on a directory is ENOTTY\n");
    }
    return passed;
}

void user_main(void) {
    struct fstrim_range r;
    uint32_t passed = 0;
    long dir = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/", O_DIRECTORY, 0);
    passed |= dir >= 0 ? 1u : 0u;
    passed |= write_megabyte("/trimme");
    passed |= syscall4(SYS_UNLINKAT, AT_FDCWD, (long)"/trimme", 0, 0) == 0 ? 4u : 0u;
    passed |= dir >= 0 ? root_checks(dir) : 0u;
    if (syscall4(SYS_SETRESUID, 1000, 1000, 1000, 0) == 0 && trim(dir, 0, &r) == -EPERM) {
        passed |= 64u;
        say("fstrim: an unprivileged caller is refused\n");
    }
    (void)syscall4(SYS_EXIT_GROUP, passed, 0, 0, 0);
}
