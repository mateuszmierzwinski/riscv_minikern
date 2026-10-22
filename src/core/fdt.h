/* fdt — a bounded, read-only reader for a flattened device tree.
 *
 * U-Boot on real sunxi boards (and QEMU with -dtb) hands the kernel a device
 * tree blob in r2. The layout is the Devicetree Specification's (v0.4, §5):
 * a big-endian header, a structure block of tokens and a strings block. The
 * blob is loader-supplied, so nothing in it is trusted: fdt_open checks every
 * offset against the blob's declared size and that size against the caller's
 * bound, and the walker re-checks each token before reading it. */
#ifndef MINIKERN_CORE_FDT_H
#define MINIKERN_CORE_FDT_H

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

/* An opened blob: the checked header fields the walker needs. */
struct fdt {
    const uint8_t *base;
    uint32_t total_size;
    uint32_t struct_off;
    uint32_t struct_size;
    uint32_t strings_off;
    uint32_t strings_size;
};

/* A property's value inside the blob. Big-endian, len bytes, not copied. */
struct fdt_prop {
    const uint8_t *value;
    uint32_t len;
};

/* Validates the header of the blob at base (at most max_size bytes may be
 * read). Returns KERR_OK, or KERR_INVAL for a bad magic, an unsupported
 * version, or any block that runs outside the blob. */
int fdt_open(struct fdt *fdt, const void *base, size_t max_size);

/* Finds property prop of the node at path ("/chosen", "/memory"). A path
 * component without a unit address matches a node name with one, so
 * "/memory" finds "memory@40000000"; the first match in tree order wins.
 * Returns KERR_OK, KERR_NOENT when absent, KERR_INVAL for a malformed
 * structure block. */
int fdt_get_prop(const struct fdt *fdt, const char *path, const char *prop, struct fdt_prop *out);

/* Reads a 1- or 2-cell big-endian number at cell index `at` of a property.
 * A 2-cell value whose high word is non-zero does not fit the 32-bit
 * machine and is rejected. Returns KERR_OK or KERR_INVAL. */
int fdt_prop_cells(const struct fdt_prop *p, uint32_t at, uint32_t cells, uint64_t *out);

/* Reads a single-cell property (#address-cells and the like), or returns
 * fallback when it is absent or malformed. */
uint32_t fdt_get_u32(const struct fdt *fdt, const char *path, const char *prop, uint32_t fallback);

#endif
