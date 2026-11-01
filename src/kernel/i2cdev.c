#include "kernel/i2cdev.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/uspace.h"
#include "lib/kfmt.h"

#define I2C_BUSES_MAX 8u
#define I2C_FUNCS 0x0705u
#define I2C_RDWR 0x0707u
#define I2C_FUNC_I2C 0x1u
#define I2C_M_RD 0x0001u
#define MESSAGE_BYTES_MAX 64u /* the bus driver's own limit each way */

/* The ABI's structures, by offset (LP64): `i2c_rdwr_ioctl_data` is a
 * pointer and a count; `i2c_msg` is addr, flags, len (16 bits each), then
 * the buffer pointer at 8. */
#define RDWR_MSGS 0u
#define RDWR_NMSGS 8u
#define RDWR_BYTES 16u
#define MSG_ADDR 0u
#define MSG_FLAGS 2u
#define MSG_LEN 4u
#define MSG_BUF 8u
#define MSG_BYTES 16u

struct i2c_msg_in {
    uint16_t addr, flags, len;
    virt_t buf;
};

static struct {
    struct i2c_port port;
    char path[16];
    bool up;
} buses[I2C_BUSES_MAX];

static uint16_t u16_at(const uint8_t *b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint64_t u64_at(const uint8_t *b) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8u; i++) {
        v |= (uint64_t)b[i] << (8u * i);
    }
    return v;
}

/* Reads message `i` of the user's array. */
static int read_message(virt_t msgs, uint32_t i, struct i2c_msg_in *m) {
    uint8_t raw[MSG_BYTES];
    if (uspace_copy_in(raw, msgs + (virt_t)i * MSG_BYTES, MSG_BYTES) != KERR_OK) {
        return KERR_FAULT;
    }
    *m = (struct i2c_msg_in){u16_at(raw + MSG_ADDR), u16_at(raw + MSG_FLAGS), u16_at(raw + MSG_LEN),
                             (virt_t)u64_at(raw + MSG_BUF)};
    return m->len <= MESSAGE_BYTES_MAX && m->addr < 0x80u ? KERR_OK : KERR_INVAL;
}

/* The shapes the bus carries: a write, a read, or a write then a read to
 * one address. Anything else (three messages, a read then a write) is
 * refused rather than split, since splitting changes what a device sees. */
static bool supported(uint32_t n, const struct i2c_msg_in *m) {
    if (n == 1u) {
        return m[0].len != 0;
    }
    return n == 2u && (m[0].flags & I2C_M_RD) == 0 && (m[1].flags & I2C_M_RD) != 0 &&
           m[0].addr == m[1].addr && m[0].len != 0 && m[1].len != 0;
}

/* Runs the messages: the write's bytes in, the transfer, the read's out. */
static int32_t run(const struct i2c_port *p, uint32_t n, const struct i2c_msg_in *m) {
    uint8_t out[MESSAGE_BYTES_MAX], in[MESSAGE_BYTES_MAX];
    const struct i2c_msg_in *w = (m[0].flags & I2C_M_RD) == 0 ? &m[0] : NULL;
    const struct i2c_msg_in *r = (m[n - 1u].flags & I2C_M_RD) != 0 ? &m[n - 1u] : NULL;
    int rc;
    if (w != NULL && uspace_copy_in(out, w->buf, w->len) != KERR_OK) {
        return KERR_FAULT;
    }
    rc = p->transfer(p->self, (uint8_t)m[0].addr, out, w != NULL ? w->len : 0u, in,
                     r != NULL ? r->len : 0u);
    if (rc != KERR_OK) {
        return rc;
    }
    if (r != NULL && uspace_copy_out(r->buf, in, r->len) != KERR_OK) {
        return KERR_FAULT;
    }
    return (int32_t)n; /* Linux answers with the number of messages done */
}

static int32_t rdwr(const struct i2c_port *p, virt_t arg) {
    uint8_t raw[RDWR_BYTES];
    struct i2c_msg_in m[2];
    uint32_t n;
    if (uspace_copy_in(raw, arg, RDWR_BYTES) != KERR_OK) {
        return KERR_FAULT;
    }
    n = (uint32_t)raw[RDWR_NMSGS] | (uint32_t)raw[RDWR_NMSGS + 1u] << 8 |
        (uint32_t)raw[RDWR_NMSGS + 2u] << 16 | (uint32_t)raw[RDWR_NMSGS + 3u] << 24;
    if (n == 0 || n > 2u) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < n; i++) {
        int rc = read_message((virt_t)u64_at(raw + RDWR_MSGS), i, &m[i]);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return supported(n, m) ? run(p, n, m) : KERR_INVAL;
}

static int32_t i2c_ioctl(struct file *f, uint32_t request, virt_t arg) {
    const struct i2c_port *p = f->priv;
    uint64_t funcs = I2C_FUNC_I2C;
    switch (request) {
    case I2C_FUNCS:
        return uspace_copy_out(arg, &funcs, sizeof funcs) == KERR_OK ? 0 : KERR_FAULT;
    case I2C_RDWR:
        return rdwr(p, arg);
    default:
        return KERR_NOTTY;
    }
}

static const struct file_ops i2c_ops = {
    .ioctl = i2c_ioctl,
};

int i2cdev_install(uint32_t number, const struct i2c_port *port) {
    static const struct device_owner root_only = {0600u, 0u, 0u};
    int rc;
    if (number >= I2C_BUSES_MAX || buses[number].up) {
        return KERR_INVAL;
    }
    buses[number].port = *port;
    buses[number].path[0] = '\0';
    (void)kfmt_snprintf(buses[number].path, sizeof buses[number].path, "/dev/i2c-%u", number);
    rc = fdobjects_add_device(buses[number].path, &i2c_ops, &buses[number].port, &root_only);
    if (rc != KERR_OK) {
        return rc;
    }
    (void)synthfs_add_device(buses[number].path, &root_only);
    buses[number].up = true;
    return KERR_OK;
}
