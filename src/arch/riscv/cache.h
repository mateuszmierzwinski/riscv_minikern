/* The SoC's cache geometry, and what the kernel does about it.
 *
 * The SiFive U74's L1 data cache has **64-byte lines**, and the JH7110 pairs
 * four of those cores with a shared 2 MiB L2. That one number decides where
 * false sharing happens: two variables in the same line, written by two
 * harts, make every write on one hart invalidate the other's copy, and the
 * cost is paid on every access rather than occasionally.
 *
 * So the kernel's per-hart data is padded to a line, and the two counters of
 * the kernel lock are put in different lines — the arriving half is written
 * by every hart that wants the lock, while the serving half is what every
 * waiter spins on, and sharing a line between those two is the worst
 * possible arrangement.
 *
 * **This cannot be measured here, and the measurement is not pending — it is
 * impossible.** QEMU's TCG models no caches at all, so false sharing costs
 * nothing under the emulator and removing it gains nothing: a run-to-run
 * comparison on this host gave 2.0x, 0.8x, 1.1x and 1.1x for the same
 * binary, which is the scheduler of the host showing through. The padding is
 * here because it is right for the hardware, and it is labelled as unverified
 * on silicon for the same reason every other JH7110 fact in this project is.
 * The ARM project this came from recorded the same limitation about its own
 * cache work, and the one cache bug it did have — handing a device memory the
 * CPU still held dirty lines for — would have been invisible in QEMU too.
 */
#ifndef MINIKERN_ARCH_RISCV_CACHE_H
#define MINIKERN_ARCH_RISCV_CACHE_H

/* The U74's L1 line. From SiFive's documentation for the core; not measured
 * on silicon, like everything else here about this SoC. */
#define CACHE_LINE_BYTES 64u

/* Puts a variable, or a struct's tail padding, on a line of its own. */
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE_BYTES)))

#endif
