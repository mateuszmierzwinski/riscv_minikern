/* The U74 complex's shared L2 cache controller (`sifive,ccache0`), for the
 * one thing a device that does not snoop the cache needs: dirty lines
 * written back to memory before it reads.
 *
 * The display controller is such a reader as far as anyone has shown — the
 * device tree marks nothing non-coherent, but StarFive's own display driver
 * flushes the L2 on every update (docs/DISPLAY.md §5) — so the framebuffer
 * is flushed before the controller is pointed at it, and as it changes.
 *
 * `Flush64` takes a physical address and writes back and invalidates the
 * 64-byte line holding it. A full 1080p frame is 129,600 lines, so the
 * cost is measured and reported rather than assumed.
 */
#ifndef MINIKERN_DRIVERS_CCACHE_JH7110_H
#define MINIKERN_DRIVERS_CCACHE_JH7110_H

#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* Finds and maps the controller (`sifive,ccache0`). KERR_NOENT without one. */
int ccache_init(const struct fdt *fdt);

/* Writes back and invalidates every line of [base, base + bytes).
 * KERR_NODEV before `ccache_init`. */
int ccache_flush_range(phys_t base, uint64_t bytes);

#endif
