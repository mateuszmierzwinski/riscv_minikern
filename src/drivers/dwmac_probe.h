/* Reconnaissance for the board's DesignWare GMAC: which controller, which
 * register generation, which PHY, and whether it can be clocked.
 *
 * Reads only (beyond enabling clocks), prints what it finds, and is meant
 * to be deleted or folded into the driver once the driver exists. The GMAC
 * is a large block and this tree has already paid once for writing a
 * storage driver against the wrong register map. */
#ifndef DRIVERS_DWMAC_PROBE_H
#define DRIVERS_DWMAC_PROBE_H

struct fdt;

int dwmac_probe(const struct fdt *fdt);

#endif
