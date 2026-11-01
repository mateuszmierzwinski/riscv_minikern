/* i2cdev.h — an I²C bus as `/dev/i2c-N`, the way Linux programs reach one.
 *
 * `I2C_RDWR` carries one message (a write or a read) or two to the same
 * address (a write, then a read with a repeated start) — the register read
 * every sensor and PMIC wants. `I2C_FUNCS` says plain I²C. Root only, 0600:
 * the bus with the PMIC on it is one where a write can destroy the board.
 */
#ifndef MINIKERN_KERNEL_I2CDEV_H
#define MINIKERN_KERNEL_I2CDEV_H

#include <stdint.h>

/* The bus, through the composition root: write `outlen` bytes then read
 * `inlen` to 7-bit `addr`, either count 0 but not both. */
struct i2c_port {
    int (*transfer)(void *self, uint8_t addr, const uint8_t *out, uint32_t outlen, uint8_t *in,
                    uint32_t inlen);
    void *self;
};

/* Installs `/dev/i2c-<number>` over `port`; at most a handful of buses. */
int i2cdev_install(uint32_t number, const struct i2c_port *port);

#endif
