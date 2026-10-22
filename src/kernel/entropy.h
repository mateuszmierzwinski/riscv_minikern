/* entropy — a pool fed by the times at which things happen.
 *
 * The generator beside this (kernel/random.h) is a good sequence from a
 * poor seed: the timer and the chip's serial number, which on this chip
 * under emulation are the same on every boot. That is fine for the hash
 * seeds Go asks for and **not fine for a key**, and the difference matters
 * now that there is a signature verifier in the tree.
 *
 * So the kernel harvests what it actually has: the **time at which each
 * interrupt arrives**, measured on the cycle counter. On real hardware
 * those low bits carry genuine jitter — a packet's arrival depends on the
 * other end, the wire and the PHY's clock; a disk reply on a server across
 * a network. Each sample is mixed into the pool with SHA-256, which is
 * already here for the netboot digest, so one predictable sample cannot
 * undo the good ones.
 *
 * **What this is honest about:** under QEMU the jitter is thin, because an
 * emulator's timing follows the host's scheduler rather than a physical
 * clock. What the tests can prove is the mechanism — that samples are
 * mixed, that the output depends on them, and that two boots differ — not
 * the quality, which is a property of the hardware the board runs on.
 */
#ifndef MINIKERN_KERNEL_ENTROPY_H
#define MINIKERN_KERNEL_ENTROPY_H

#include <stdbool.h>
#include <stdint.h>

/* How many samples the pool wants before it calls itself seeded. Each
 * interrupt gives one, and the kernel takes thousands a second, so this is
 * reached in the first moments of a boot — well before anything asks for a
 * key. */
#define ENTROPY_SAMPLES_WANTED 128u

/* Mixes one timing sample in. Called from the interrupt path, so it does
 * the least work that is still a mix: a counter and a word of state, with
 * the hashing left for when the pool is read. */
void entropy_add_timing(uint32_t cycles);

/* Fills buf from the pool. True when the pool had its fill of samples
 * first; false when it did not, and the caller is getting something
 * weaker than it asked for. */
bool entropy_fill(void *buf, uint32_t len);

/* How many samples have been mixed in, for the log and for a test. */
uint32_t entropy_samples(void);

#endif
