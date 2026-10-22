#include "core/bootinfo.h"

#include "core/errno.h"
#include "core/fdt.h"

/* /memory reg: the first (base, size) pair, in the root's cell sizes. */
static int fdt_read_memory(const struct fdt *fdt, struct bootinfo *out) {
    uint32_t acells = fdt_get_u32(fdt, "/", "#address-cells", 2);
    uint32_t scells = fdt_get_u32(fdt, "/", "#size-cells", 1);
    struct fdt_prop reg;
    int rc = fdt_get_prop(fdt, "/memory", "reg", &reg);
    if (rc == KERR_NOENT) {
        return KERR_OK;
    }
    if (rc != KERR_OK || fdt_prop_cells(&reg, 0, acells, &out->mem_base) != KERR_OK ||
        fdt_prop_cells(&reg, acells, scells, &out->mem_size) != KERR_OK) {
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* /chosen bootargs, accepted only when NUL-terminated inside its value. */
static void fdt_read_bootargs(const struct fdt *fdt, struct bootinfo *out) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, "/chosen", "bootargs", &p) == KERR_OK && p.len > 0 && p.value[p.len - 1] == '\0') {
        out->cmdline = (const char *)p.value;
    }
}

/* Reads a 1- or 2-cell /chosen number; the cell count is the value's size. */
static int fdt_read_chosen_number(const struct fdt *fdt, const char *prop, uint64_t *v) {
    struct fdt_prop p;
    if (fdt_get_prop(fdt, "/chosen", prop, &p) != KERR_OK) {
        return KERR_NOENT;
    }
    return fdt_prop_cells(&p, 0, p.len / 4u, v);
}

/* The initrd counts only when both ends are present, valid and ordered. */
static void fdt_read_initrd(const struct fdt *fdt, struct bootinfo *out) {
    uint64_t start, end;
    if (fdt_read_chosen_number(fdt, "linux,initrd-start", &start) == KERR_OK &&
        fdt_read_chosen_number(fdt, "linux,initrd-end", &end) == KERR_OK && end > start) {
        out->initrd_start = start;
        out->initrd_end = end;
    }
}

int bootinfo_from_fdt(const void *blob, size_t max_size, struct bootinfo *out) {
    struct fdt fdt;
    struct bootinfo info = {.source = BOOTINFO_FDT, .cmdline = ""};
    if (fdt_open(&fdt, blob, max_size) != KERR_OK || fdt_read_memory(&fdt, &info) != KERR_OK) {
        return KERR_INVAL;
    }
    info.block_size = fdt.total_size;
    fdt_read_bootargs(&fdt, &info);
    fdt_read_initrd(&fdt, &info);
    *out = info;
    return KERR_OK;
}

void bootinfo_clamp_memory(struct bootinfo *info, phys_t board_max) {
    if (info->mem_size == 0 || info->mem_size > board_max) {
        info->mem_size = board_max;
    }
}
