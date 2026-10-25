/* SPI on the JH7110: an ARM PrimeCell PL022, not a DesignWare block.
 *
 * **The tree said so, and that is the whole reason this is a PL022 driver.**
 * The SoC's documentation describes a Synopsys SSI, and writing to that
 * register map would have been the SD controller's mistake a second time —
 * a complete driver for the wrong hardware, which an emulator would have
 * agreed with. `minikern.buses` on the board prints `"arm,pl022"` for all
 * seven controllers.
 */
#ifndef DRIVERS_SPI_PL022_H
#define DRIVERS_SPI_PL022_H

#include "core/fdt.h"
#include "core/types.h"

struct spi_pl022 {
    volatile uint32_t *regs;
    uint32_t irq;
    bool up;
};

/* Binds the controller at `path`: clocks it, maps it and leaves it disabled.
 * KERR_NOENT when the node is not there, KERR_NOMEM when it cannot be
 * mapped. */
int spi_pl022_init(struct spi_pl022 *bus, const struct fdt *fdt, const char *path);

/* The PrimeCell identification registers, which are the proof that the
 * register map is the one this driver believes: a PL022 reports part number
 * 0x022 and designer 0x41 ("ARM"). A block that answered zeroes, or
 * something else, is either unclocked or not a PL022 — and either way this
 * driver must not drive it. */
uint32_t spi_pl022_part(const struct spi_pl022 *bus);

/* Clocks the bus for `hz` and sets 8-bit frames, master mode. `loopback`
 * ties transmit to receive **inside the controller**, which is what lets the
 * data path be proved on a board with nothing wired to the header. */
int spi_pl022_configure(struct spi_pl022 *bus, uint32_t hz, bool loopback);

/* Sends one byte and returns what arrived, or -errno. In loopback that is
 * the byte sent, which is the assertion. */
int spi_pl022_transfer(struct spi_pl022 *bus, uint8_t out, uint8_t *in);

#endif
