#include "drivers/ccache_jh7110.h"

#include <stddef.h>

#include "arch/riscv/cache.h"
#include "arch/riscv/mmu.h"
#include "core/errno.h"

#define CCACHE_COMPATIBLE "sifive,ccache0"
#define FLUSH64 0x200u

static volatile uint64_t *flush64;

int ccache_init(const struct fdt *fdt) {
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    uint8_t *at;
    if (fdt_find_compatible(fdt, CCACHE_COMPATIBLE, path, sizeof path) != KERR_OK ||
        fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK || size <= FLUSH64) {
        return KERR_NOENT;
    }
    at = mmu_map_device(base, size);
    if (at == NULL) {
        return KERR_NOMEM;
    }
    flush64 = (volatile uint64_t *)(at + FLUSH64);
    return KERR_OK;
}

/* All of this hart's stores reach the cache before the flushes are issued,
 * and the flushes complete before anything that follows. */
static inline void full_fence(void) {
    __asm__ volatile("fence rw, rw" ::: "memory");
}

int ccache_flush_range(phys_t base, uint64_t bytes) {
    phys_t line = base & ~(phys_t)(CACHE_LINE_BYTES - 1u);
    if (flush64 == NULL) {
        return KERR_NODEV;
    }
    full_fence();
    for (; line < base + bytes; line += CACHE_LINE_BYTES) {
        *flush64 = line;
    }
    full_fence();
    return KERR_OK;
}
