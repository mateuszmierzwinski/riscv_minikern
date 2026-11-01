/* /dev/watchdog from a process, as a Linux watchdog daemon uses it.
 *
 * Run with `minikern.watchdog=<seconds>`. While the device is open the
 * process feeds the watchdog and the kernel does not; the magic close —
 * writing 'V' before closing — hands the feeding back. The board staying up
 * for the rest of the run is the proof that it did; the control
 * is a boot that starves the kernel instead (`minikern.wdtstarve`).
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   open succeeds (as root)
 *   2   WDIOC_GETTIMEOUT gives the boot's timeout
 *   4   WDIOC_SETTIMEOUT to 5 is set and written back
 *   8   a second open is refused with EBUSY
 *   16  three WDIOC_KEEPALIVEs a second apart and a write all succeed
 *   32  the close after 'V' succeeds
 * 63 is all six.
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_NANOSLEEP 101
#define AT_FDCWD (-100L)
#define O_WRONLY 1L
#define EBUSY 16
#define WDIOC_KEEPALIVE 0x80045705L
#define WDIOC_SETTIMEOUT 0xc0045706L
#define WDIOC_GETTIMEOUT 0x80045707L


static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static long open_watchdog(void) {
    return syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/watchdog", O_WRONLY, 0);
}

static void sleep_one_second(void) {
    long ts[2] = {1, 0};
    (void)syscall4(SYS_NANOSLEEP, (long)ts, 0, 0, 0);
}

/* Bits 2 and 4: the timeout, read and set. */
static uint32_t timeouts(long fd) {
    int32_t t = 0;
    uint32_t passed = 0;
    if (syscall4(SYS_IOCTL, fd, WDIOC_GETTIMEOUT, (long)&t, 0) == 0 && t >= 2) {
        passed |= 2u;
    }
    t = 5;
    if (syscall4(SYS_IOCTL, fd, WDIOC_SETTIMEOUT, (long)&t, 0) == 0 && t == 5) {
        passed |= 4u;
    }
    return passed;
}

/* Bit 16: keep-alives a second apart, then a plain write. */
static uint32_t keep_alive(long fd) {
    for (int i = 0; i < 3; i++) {
        if (syscall4(SYS_IOCTL, fd, WDIOC_KEEPALIVE, 0, 0) != 0) {
            return 0;
        }
        sleep_one_second();
    }
    return syscall4(SYS_WRITE, fd, (long)"x", 1, 0) == 1 ? 16u : 0u;
}

void user_main(void) {
    long fd = open_watchdog(), second;
    uint32_t passed = fd >= 0 ? 1u : 0u;
    if (fd >= 0) {
        passed |= timeouts(fd);
        second = open_watchdog();
        passed |= second == -EBUSY ? 8u : 0u;
        passed |= keep_alive(fd);
        (void)syscall4(SYS_WRITE, fd, (long)"V", 1, 0);
        passed |= syscall4(SYS_CLOSE, fd, 0, 0, 0) == 0 ? 32u : 0u;
    }
    (void)syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
