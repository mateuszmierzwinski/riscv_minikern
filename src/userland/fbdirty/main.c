/* Dirty rectangles on /dev/fb0: draw a band, say which rows changed, and
 * let the kernel flush only those (`MINIKERN_FBIO_DIRTY`, ROADMAP §10).
 *
 * The flush itself is the kernel's to report ("display: dirty rectangles in
 * use; the first flushed …"); this program checks the ioctl's contract.
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a band of 32 rows written with pwrite, and marked: 0
 *   2   a rectangle off the screen is accepted and flushes nothing: 0
 *   4   a rectangle at a bad pointer is refused: EFAULT
 *   8   a second mark, after the beat has flushed the first: 0
 * 15 is all four.
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_PWRITE64 68
#define SYS_EXIT_GROUP 94
#define SYS_NANOSLEEP 101
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define EFAULT 14
#define FBIOGET_FSCREENINFO 0x4602L
#define MINIKERN_FBIO_DIRTY 0x40104640L
#define FIX_LINE_LENGTH 48u
#define FIX_BYTES 80u
#define BAND_TOP 100u
#define BAND_ROWS 32u

struct fb_dirty {
    uint32_t x, y, width, height;
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

static uint32_t line_length(long fd) {
    static uint8_t fix[FIX_BYTES];
    if (syscall4(SYS_IOCTL, fd, FBIOGET_FSCREENINFO, (long)fix, 0) != 0) {
        return 0;
    }
    return (uint32_t)fix[FIX_LINE_LENGTH] | (uint32_t)fix[FIX_LINE_LENGTH + 1] << 8 |
           (uint32_t)fix[FIX_LINE_LENGTH + 2] << 16 | (uint32_t)fix[FIX_LINE_LENGTH + 3] << 24;
}

/* Writes one white line, `stride` bytes, at row `y`. */
static long white_line(long fd, uint32_t stride, uint32_t y) {
    static uint32_t line[2048];
    for (uint32_t i = 0; i < stride / 4u && i < 2048u; i++) {
        line[i] = 0x00ffffffu;
    }
    return syscall4(SYS_PWRITE64, fd, (long)line, stride, (long)y * stride);
}

static long mark(long fd, const struct fb_dirty *d) {
    return syscall4(SYS_IOCTL, fd, MINIKERN_FBIO_DIRTY, (long)d, 0);
}

void user_main(void) {
    long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/fb0", O_RDWR, 0);
    uint32_t stride = fd >= 0 ? line_length(fd) : 0u, passed = 0, written = 0;
    struct fb_dirty band = {0, BAND_TOP, stride / 4u, BAND_ROWS}, off = {0, 100000u, 10u, 10u};
    long ts[2] = {0, 300000000L};
    for (uint32_t y = 0; stride != 0 && y < BAND_ROWS; y++) {
        written += white_line(fd, stride, BAND_TOP + y) == (long)stride ? 1u : 0u;
    }
    passed |= written == BAND_ROWS && mark(fd, &band) == 0 ? 1u : 0u;
    passed |= mark(fd, &off) == 0 ? 2u : 0u;
    passed |= mark(fd, (const struct fb_dirty *)8L) == -EFAULT ? 4u : 0u;
    (void)syscall4(SYS_NANOSLEEP, (long)ts, 0, 0, 0);
    passed |= mark(fd, &band) == 0 ? 8u : 0u;
    (void)syscall4(SYS_NANOSLEEP, (long)ts, 0, 0, 0);
    (void)syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
