/* The device-tree reader, fuzzed. The blob comes from the firmware, which
 * is trusted to the extent it is right — and it has not always been. The
 * harness opens the blob and then asks it what the kernel asks at boot:
 * properties by path, nodes by compatible string and by phandle, a node's
 * children and its `reg`. */
#include "core/errno.h"
#include "core/fdt.h"
#include "fuzz.h"

#define PATH_CAP 256u

/* The questions bring-up asks of a node, asked of `path`. */
static void ask_about(const struct fdt *f, const char *path) {
    struct fdt_prop p;
    phys_t base;
    uint64_t size, cell;
    char child[PATH_CAP];
    if (fdt_get_prop(f, path, "reg", &p) == KERR_OK) {
        (void)fdt_prop_cells(&p, 0, 2, &cell);
    }
    (void)fdt_get_u32(f, path, "#address-cells", 2);
    (void)fdt_device_reg(f, path, 0, &base, &size);
    for (uint32_t n = 0; n < 8u && fdt_nth_child(f, path, n, child, PATH_CAP) == KERR_OK; n++) {
        (void)fdt_get_prop(f, child, "compatible", &p);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct fdt f;
    char path[PATH_CAP];
    if (fdt_open(&f, data, size) != KERR_OK) {
        return 0;
    }
    ask_about(&f, "/");
    ask_about(&f, "/chosen");
    ask_about(&f, "/soc");
    for (uint32_t skip = 0; skip < 4u; skip++) {
        if (fdt_find_compatible_nth(&f, "ns16550a", skip, path, PATH_CAP) == KERR_OK) {
            ask_about(&f, path);
            (void)fdt_parent_path(path);
        }
    }
    if (fdt_find_phandle(&f, 1, path, PATH_CAP) == KERR_OK) {
        ask_about(&f, path);
    }
    return 0;
}
