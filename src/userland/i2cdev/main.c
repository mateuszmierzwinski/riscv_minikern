/* /dev/i2c-5 from a program, read-only: the PMIC on the bus, at the
 * registers the kernel's own probe has already read (ROADMAP §4).
 *
 * Nothing here writes to the PMIC — the only write on the bus is the
 * register number a read starts with.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   /dev/i2c-5 opens (as root)
 *   2   I2C_FUNCS says plain I2C
 *   4   a write-then-read of register 0x03 gives the id 0x54
 *   8   one transfer of four registers from 0x00 gives 10 00 00 54
 *   16  three messages are refused: EINVAL
 *   32  a message array at a bad address is refused: EFAULT
 *   64  register 0x14, the core rail, reads 0xa8 (900 mV)
 * 127 is all seven.
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_EXIT_GROUP 94
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define EINVAL 22
#define EFAULT 14
#define I2C_FUNCS 0x0705L
#define I2C_RDWR 0x0707L
#define I2C_FUNC_I2C 1u
#define I2C_M_RD 0x0001u
#define PMIC 0x36u

struct i2c_msg {
    uint16_t addr, flags, len;
    uint8_t *buf;
};

struct i2c_rdwr {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
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

/* Reads `n` registers from `reg` in one write-then-read; the ioctl's answer. */
static long read_registers(long fd, uint8_t reg, uint8_t *out, uint16_t n) {
    static struct i2c_msg m[2];
    static struct i2c_rdwr x;
    static uint8_t which;
    which = reg;
    m[0] = (struct i2c_msg){PMIC, 0, 1, &which};
    m[1] = (struct i2c_msg){PMIC, I2C_M_RD, n, out};
    x = (struct i2c_rdwr){m, 2};
    return syscall4(SYS_IOCTL, fd, I2C_RDWR, (long)&x, 0);
}

/* Bits 16 and 32: the shapes and addresses it must refuse. */
static uint32_t refusals(long fd) {
    static struct i2c_msg m[3];
    static struct i2c_rdwr three, bad;
    static uint8_t b[3];
    uint32_t passed = 0;
    for (int i = 0; i < 3; i++) {
        m[i] = (struct i2c_msg){PMIC, i == 0 ? 0u : I2C_M_RD, 1, &b[i]};
    }
    three = (struct i2c_rdwr){m, 3};
    bad = (struct i2c_rdwr){(struct i2c_msg *)8L, 1};
    passed |= syscall4(SYS_IOCTL, fd, I2C_RDWR, (long)&three, 0) == -EINVAL ? 16u : 0u;
    passed |= syscall4(SYS_IOCTL, fd, I2C_RDWR, (long)&bad, 0) == -EFAULT ? 32u : 0u;
    return passed;
}

void user_main(void) {
    static uint8_t one[1], four[4];
    uint64_t funcs = 0;
    long fd = syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/i2c-5", O_RDWR, 0);
    uint32_t passed = fd >= 0 ? 1u : 0u;
    if (fd >= 0) {
        passed |= syscall4(SYS_IOCTL, fd, I2C_FUNCS, (long)&funcs, 0) == 0 && (funcs & I2C_FUNC_I2C) ? 2u : 0u;
        passed |= read_registers(fd, 0x03u, one, 1) == 2 && one[0] == 0x54u ? 4u : 0u;
        passed |= read_registers(fd, 0x00u, four, 4) == 2 && four[0] == 0x10u && four[1] == 0 &&
                          four[2] == 0 && four[3] == 0x54u
                      ? 8u
                      : 0u;
        passed |= refusals(fd);
        passed |= read_registers(fd, 0x14u, one, 1) == 2 && one[0] == 0xa8u ? 64u : 0u;
    }
    (void)syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
