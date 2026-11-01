/* spidev.h — an SPI bus as `/dev/spidevB.C`, the way Linux programs reach
 * one: `SPI_IOC_MESSAGE` (full-duplex transfers), the mode and word-size
 * queries, and the maximum speed. Mode 0 and 8-bit words only, which is what
 * the PL022 here is configured for; anything else is refused, not
 * approximated. Root only, 0600.
 */
#ifndef MINIKERN_KERNEL_SPIDEV_H
#define MINIKERN_KERNEL_SPIDEV_H

#include <stdint.h>

/* The bus, through the composition root: `configure` sets the clock,
 * `exchange` sends one byte and returns the one received with it. */
struct spi_port {
    int (*configure)(void *self, uint32_t hz);
    int (*exchange)(void *self, uint8_t out, uint8_t *in);
    void *self;
};

/* Installs `/dev/spidev<bus>.<cs>` over `port`. */
int spidev_install(uint32_t bus, uint32_t cs, const struct spi_port *port);

#endif
