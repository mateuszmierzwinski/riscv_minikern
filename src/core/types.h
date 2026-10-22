/* types — the few widths that are a property of the machine.
 *
 * A physical address is **64 bits**, and that is not future-proofing: the
 * JH7110's DRAM starts at 0x4000_0000 and a VisionFive 2 carries up to
 * 8 GiB, so the top of its own memory is past 0x2_0000_0000. A 32-bit
 * physical address would overflow in the middle of the board's RAM.
 *
 * This is the first thing the ARM kernel this grew from had to give up: it
 * used uint32_t for a physical address throughout, which was correct on a
 * 512 MiB Cortex-A7 and is wrong here.
 */
#ifndef MINIKERN_CORE_TYPES_H
#define MINIKERN_CORE_TYPES_H

#include <stdint.h>

typedef uint64_t phys_t;  /* a physical address */
typedef uint64_t virt_t;  /* a virtual address, Sv39: 39 bits of it */

#endif
