/* bootinfo — what the boot loader told us, which here is a device tree.
 *
 * On RISC-V the firmware (OpenSBI, or U-Boot's SBI payload) enters the
 * kernel with the hart id in a0 and the **physical address of a flattened
 * device tree in a1**. There is no second format: the ATAG list the ARM
 * kernel this grew from also had to understand — and the code that told the
 * two apart — is gone, which is the first piece of 32-bit ARM gymnastics
 * this port drops outright.
 *
 * Pure: it reads only the memory it is pointed at, bounded by the caller.
 * Every address it reports is 64-bit (core/types.h). */
#ifndef MINIKERN_CORE_BOOTINFO_H
#define MINIKERN_CORE_BOOTINFO_H

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

enum bootinfo_source {
    BOOTINFO_NONE = 0,  /* a1 was 0, or did not point at a device tree */
    BOOTINFO_FDT,
};

struct bootinfo {
    enum bootinfo_source source;
    phys_t mem_base;
    phys_t mem_size;      /* 0 when the loader did not say */
    const char *cmdline;  /* "" when absent; points into the blob itself */
    phys_t initrd_start;    /* physical; both 0 when the loader passed none */
    phys_t initrd_end;      /* exclusive */
    uint32_t block_size;    /* the blob's own size: it is kept while cmdline points into it */
};



/* Reads a device tree blob of at most max_size bytes: /memory reg (honouring
 * the root's #address-cells and #size-cells), /chosen bootargs and
 * linux,initrd-start/-end. Returns KERR_OK, or KERR_INVAL for a blob the
 * reader rejects; a missing property is not an error. */
int bootinfo_from_fdt(const void *blob, size_t max_size, struct bootinfo *out);

/* Caps the memory size at what the board really has, for an emulator that
 * offers more than the board does. */
void bootinfo_clamp_memory(struct bootinfo *info, phys_t board_max);

#endif
