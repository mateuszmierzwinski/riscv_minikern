#include "kernel/spidev.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/uspace.h"
#include "lib/kfmt.h"

#define SPIDEVS_MAX 4u
#define TRANSFERS_MAX 4u
#define TRANSFER_BYTES_MAX 256u
#define SPEED_DEFAULT_HZ 1000000u
#define SPEED_MAX_HZ 25000000u

/* `_IOW('k', 0, char[32 * n])`: the type and number are fixed, the size
 * field says how many transfers follow. */
#define SPI_IOC_MESSAGE_BASE 0x40006b00u
#define SPI_IOC_SIZE_MASK 0x3fff0000u
#define SPI_IOC_RD_MODE 0x80016b01u
#define SPI_IOC_WR_MODE 0x40016b01u
#define SPI_IOC_RD_BITS_PER_WORD 0x80016b03u
#define SPI_IOC_WR_BITS_PER_WORD 0x40016b03u
#define SPI_IOC_RD_MAX_SPEED_HZ 0x80046b04u
#define SPI_IOC_WR_MAX_SPEED_HZ 0x40046b04u

/* `struct spi_ioc_transfer`, by offset: two buffer pointers, a length and
 * a speed, then fields this bus does not vary (and refuses when set). */
#define XFER_BYTES 32u
#define XFER_TX 0u
#define XFER_RX 8u
#define XFER_LEN 16u
#define XFER_SPEED 20u
#define XFER_BITS 26u

static struct dev {
    struct spi_port port;
    uint32_t max_hz;
    char path[24];
    bool up;
} devs[SPIDEVS_MAX];

static uint32_t u32_at(const uint8_t *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint64_t u64_at(const uint8_t *b) {
    return (uint64_t)u32_at(b) | (uint64_t)u32_at(b + 4) << 32;
}

/* One transfer: `len` bytes out of `tx` (zeros when it is 0) while the
 * same number come in to `rx` (dropped when it is 0). */
static int32_t one_transfer(struct dev *d, const uint8_t *raw) {
    uint8_t out[TRANSFER_BYTES_MAX] = {0}, in[TRANSFER_BYTES_MAX];
    virt_t tx = (virt_t)u64_at(raw + XFER_TX), rx = (virt_t)u64_at(raw + XFER_RX);
    uint32_t len = u32_at(raw + XFER_LEN), hz = u32_at(raw + XFER_SPEED);
    int rc;
    if (len == 0 || len > TRANSFER_BYTES_MAX || (raw[XFER_BITS] != 0 && raw[XFER_BITS] != 8u)) {
        return KERR_INVAL;
    }
    if (tx != 0 && uspace_copy_in(out, tx, len) != KERR_OK) {
        return KERR_FAULT;
    }
    rc = d->port.configure(d->port.self, hz != 0 && hz < d->max_hz ? hz : d->max_hz);
    for (uint32_t i = 0; rc == KERR_OK && i < len; i++) {
        rc = d->port.exchange(d->port.self, out[i], &in[i]);
    }
    if (rc == KERR_OK && rx != 0 && uspace_copy_out(rx, in, len) != KERR_OK) {
        return KERR_FAULT;
    }
    return rc != KERR_OK ? rc : (int32_t)len;
}

/* `SPI_IOC_MESSAGE(n)`: the transfers in order; the total bytes moved. */
static int32_t message(struct dev *d, uint32_t request, virt_t arg) {
    uint32_t size = (request & SPI_IOC_SIZE_MASK) >> 16, n = size / XFER_BYTES, total = 0;
    uint8_t raw[XFER_BYTES];
    if (n == 0 || n > TRANSFERS_MAX || size % XFER_BYTES != 0) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (uspace_copy_in(raw, arg + (virt_t)i * XFER_BYTES, XFER_BYTES) != KERR_OK) {
            return KERR_FAULT;
        }
        int32_t moved = one_transfer(d, raw);
        if (moved < 0) {
            return moved;
        }
        total += (uint32_t)moved;
    }
    return (int32_t)total;
}

/* The settings: mode 0 and 8-bit words, which are all this bus does, and
 * the maximum speed, which is the caller's to lower. */
static int32_t setting(struct dev *d, uint32_t request, virt_t arg) {
    uint8_t byte = 0;
    uint32_t hz = 0;
    switch (request) {
    case SPI_IOC_RD_MODE:
        return uspace_copy_out(arg, &byte, 1) == KERR_OK ? 0 : KERR_FAULT;
    case SPI_IOC_RD_BITS_PER_WORD:
        byte = 8u;
        return uspace_copy_out(arg, &byte, 1) == KERR_OK ? 0 : KERR_FAULT;
    case SPI_IOC_WR_MODE:
        if (uspace_copy_in(&byte, arg, 1) != KERR_OK) {
            return KERR_FAULT;
        }
        return byte == 0 ? 0 : KERR_INVAL; /* mode 0 only */
    case SPI_IOC_WR_BITS_PER_WORD:
        if (uspace_copy_in(&byte, arg, 1) != KERR_OK) {
            return KERR_FAULT;
        }
        return byte == 0 || byte == 8u ? 0 : KERR_INVAL; /* 0 means 8, as on Linux */
    case SPI_IOC_RD_MAX_SPEED_HZ:
        return uspace_copy_out(arg, &d->max_hz, sizeof d->max_hz) == KERR_OK ? 0 : KERR_FAULT;
    default:
        if (uspace_copy_in(&hz, arg, sizeof hz) != KERR_OK) {
            return KERR_FAULT;
        }
        d->max_hz = hz != 0 && hz <= SPEED_MAX_HZ ? hz : d->max_hz;
        return hz != 0 && hz <= SPEED_MAX_HZ ? 0 : KERR_INVAL;
    }
}

static int32_t spi_ioctl(struct file *f, uint32_t request, virt_t arg) {
    struct dev *d = f->priv;
    if ((request & ~SPI_IOC_SIZE_MASK) == SPI_IOC_MESSAGE_BASE) {
        return message(d, request, arg);
    }
    switch (request) {
    case SPI_IOC_RD_MODE:
    case SPI_IOC_WR_MODE:
    case SPI_IOC_RD_BITS_PER_WORD:
    case SPI_IOC_WR_BITS_PER_WORD:
    case SPI_IOC_RD_MAX_SPEED_HZ:
    case SPI_IOC_WR_MAX_SPEED_HZ:
        return setting(d, request, arg);
    default:
        return KERR_NOTTY;
    }
}

static const struct file_ops spi_ops = {
    .ioctl = spi_ioctl,
};

int spidev_install(uint32_t bus, uint32_t cs, const struct spi_port *port) {
    static const struct device_owner root_only = {0600u, 0u, 0u};
    struct dev *d = NULL;
    for (uint32_t i = 0; i < SPIDEVS_MAX && d == NULL; i++) {
        d = devs[i].up ? NULL : &devs[i];
    }
    if (d == NULL || bus > 9u || cs > 9u) {
        return KERR_NOSPC;
    }
    *d = (struct dev){.port = *port, .max_hz = SPEED_DEFAULT_HZ};
    (void)kfmt_snprintf(d->path, sizeof d->path, "/dev/spidev%u.%u", bus, cs);
    int rc = fdobjects_add_device(d->path, &spi_ops, d, &root_only);
    if (rc != KERR_OK) {
        return rc;
    }
    (void)synthfs_add_device(d->path, &root_only);
    d->up = true;
    return KERR_OK;
}
