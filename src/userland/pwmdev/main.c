/* /dev/pwmchip0 from a program: Linux 6.13's PWM character device —
 * request, round, set, read back, free. The waveform on a wire is the
 * kernel's own measurement (`minikern.pwm.jumper`); this checks the ABI.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   /dev/pwmchip0 opens and channel 0 is requested
 *   2   a second descriptor's request of channel 0 is refused: EBUSY
 *   4   ROUNDWF of 1 ms / 250 µs rounds to within a tick of it
 *   8   SETEXACTWF of a period the counter cannot hold exactly: EDOM
 *   16  SETROUNDEDWF of 1 ms / 250 µs, and GETWF reads back what ROUNDWF said
 *   32  a duty offset (phase) is refused: EINVAL
 *   64  channel 0 freed, period 0 set first (off)
 * 127 is all seven.
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_EXIT_GROUP 94
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define EBUSY 16
#define EINVAL 22
#define EDOM 33
#define PWM_IOCTL_REQUEST 0x7501L
#define PWM_IOCTL_FREE 0x7502L
#define PWM_IOCTL_ROUNDWF 0xc0207503L
#define PWM_IOCTL_GETWF 0xc0207504L
#define PWM_IOCTL_SETROUNDEDWF 0x40207505L
#define PWM_IOCTL_SETEXACTWF 0x40207506L
#define TICK_NS 20u /* about one tick at 51.2 MHz */

struct waveform {
    uint32_t hwpwm, pad;
    uint64_t period_ns, duty_ns, offset_ns;
};

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static long open_chip(void) {
    return syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/pwmchip0", O_RDWR, 0);
}

static int within_a_tick(uint64_t got, uint64_t want) {
    return got <= want && want - got < TICK_NS;
}

/* Bits 4, 8, 16 and 32: rounding, exactness, setting and reading back. */
static uint32_t waveforms(long fd) {
    static struct waveform w, rounded, back, odd, phase;
    uint32_t passed = 0;
    w = (struct waveform){0, 0, 1000000u, 250000u, 0};
    rounded = w;
    if (syscall4(SYS_IOCTL, fd, PWM_IOCTL_ROUNDWF, (long)&rounded, 0) == 0 &&
        within_a_tick(rounded.period_ns, 1000000u) && within_a_tick(rounded.duty_ns, 250000u)) {
        passed |= 4u;
    }
    odd = (struct waveform){0, 0, 1000003u, 0, 0};
    passed |= syscall4(SYS_IOCTL, fd, PWM_IOCTL_SETEXACTWF, (long)&odd, 0) == -EDOM ? 8u : 0u;
    back = (struct waveform){0, 0, 0, 0, 0};
    if (syscall4(SYS_IOCTL, fd, PWM_IOCTL_SETROUNDEDWF, (long)&w, 0) == 0 &&
        syscall4(SYS_IOCTL, fd, PWM_IOCTL_GETWF, (long)&back, 0) == 0 &&
        back.period_ns == rounded.period_ns && back.duty_ns == rounded.duty_ns) {
        passed |= 16u;
    }
    phase = (struct waveform){0, 0, 1000000u, 250000u, 1000u};
    passed |= syscall4(SYS_IOCTL, fd, PWM_IOCTL_SETROUNDEDWF, (long)&phase, 0) == -EINVAL ? 32u : 0u;
    return passed;
}

void user_main(void) {
    static struct waveform off;
    long fd = open_chip(), other = open_chip();
    uint32_t passed = 0;
    if (fd >= 0 && syscall4(SYS_IOCTL, fd, PWM_IOCTL_REQUEST, 0, 0) == 0) {
        passed |= 1u;
        passed |= other >= 0 && syscall4(SYS_IOCTL, other, PWM_IOCTL_REQUEST, 0, 0) == -EBUSY ? 2u : 0u;
        passed |= waveforms(fd);
        off = (struct waveform){0, 0, 0, 0, 0};
        if (syscall4(SYS_IOCTL, fd, PWM_IOCTL_SETROUNDEDWF, (long)&off, 0) == 0 &&
            syscall4(SYS_IOCTL, fd, PWM_IOCTL_FREE, 0, 0) == 0) {
            passed |= 64u;
        }
    }
    (void)syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
