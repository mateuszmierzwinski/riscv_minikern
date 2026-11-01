/* /dev/spidev0.0 from a program, through the operator's jumper: run with
 * `minikern.spi.jumper`, which puts SPI0's transmit line on pad 63 and its
 * receive line on pad 60, so every byte sent comes back.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   /dev/spidev0.0 opens (as root)
 *   2   SPI_IOC_RD_MODE says 0 and SPI_IOC_RD_BITS_PER_WORD 8
 *   4   SPI_IOC_WR_MODE 3 is refused: EINVAL (mode 0 only)
 *   8   SPI_IOC_MESSAGE(1) of 16 bytes moves 16, and they come back
 *   16  SPI_IOC_MESSAGE(2), 16 and 8 bytes, moves 24, both come back
 *   32  a transfer from a bad address is refused: EFAULT
 * 63 is all six; without the jumper's routing, 8 and 16 fail (39).
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_EXIT_GROUP 94
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define EINVAL 22
#define EFAULT 14
#define SPI_IOC_MESSAGE(n) (0x40006b00L | ((long)(32 * (n)) << 16))
#define SPI_IOC_RD_MODE 0x80016b01L
#define SPI_IOC_WR_MODE 0x40016b01L
#define SPI_IOC_RD_BITS_PER_WORD 0x80016b03L
#define SPI_IOC_WR_MAX_SPEED_HZ 0x40046b04L

struct spi_ioc_transfer {
    uint64_t tx_buf, rx_buf;
    uint32_t len, speed_hz;
    uint16_t delay_usecs;
    uint8_t bits_per_word, cs_change, tx_nbits, rx_nbits, word_delay_usecs, pad;
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

static const uint8_t pattern[16] = {'m', 'i', 'n', 'i', 'k', 'e', 'r', 'n',
                                    0x00, 0xff, 0x55, 0xaa, 0x0f, 0xf0, 0x81, 0x7e};

static int same(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Bits 8 and 16: one transfer, then two. */
static uint32_t messages(long fd) {
    static uint8_t in1[16], in2[8];
    static struct spi_ioc_transfer t[2];
    uint32_t passed = 0;
    t[0] = (struct spi_ioc_transfer){.tx_buf = (uint64_t)(uintptr_t)pattern, .rx_buf = (uint64_t)(uintptr_t)in1, .len = 16};
    passed |= syscall4(SYS_IOCTL, fd, SPI_IOC_MESSAGE(1), (long)t, 0) == 16 && same(pattern, in1, 16) ? 8u : 0u;
    t[0].rx_buf = (uint64_t)(uintptr_t)in1;
    t[1] = (struct spi_ioc_transfer){.tx_buf = (uint64_t)(uintptr_t)(pattern + 8), .rx_buf = (uint64_t)(uintptr_t)in2, .len = 8};
    for (int i = 0; i < 16; i++) {
        in1[i] = 0;
    }
    passed |= syscall4(SYS_IOCTL, fd, SPI_IOC_MESSAGE(2), (long)t, 0) == 24 && same(pattern, in1, 16) &&
                      same(pattern + 8, in2, 8)
                  ? 16u
                  : 0u;
    return passed;
}

void user_main(void) {
    static struct spi_ioc_transfer bad;
    uint8_t mode = 9, bits = 0, three = 3;
    uint32_t hz = 1000000;
    long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/spidev0.0", O_RDWR, 0);
    uint32_t passed = fd >= 0 ? 1u : 0u;
    if (fd >= 0) {
        (void)syscall4(SYS_IOCTL, fd, SPI_IOC_WR_MAX_SPEED_HZ, (long)&hz, 0);
        passed |= syscall4(SYS_IOCTL, fd, SPI_IOC_RD_MODE, (long)&mode, 0) == 0 && mode == 0 &&
                          syscall4(SYS_IOCTL, fd, SPI_IOC_RD_BITS_PER_WORD, (long)&bits, 0) == 0 && bits == 8
                      ? 2u
                      : 0u;
        passed |= syscall4(SYS_IOCTL, fd, SPI_IOC_WR_MODE, (long)&three, 0) == -EINVAL ? 4u : 0u;
        passed |= messages(fd);
        bad = (struct spi_ioc_transfer){.tx_buf = 8u, .len = 4u};
        passed |= syscall4(SYS_IOCTL, fd, SPI_IOC_MESSAGE(1), (long)&bad, 0) == -EFAULT ? 32u : 0u;
    }
    (void)syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
