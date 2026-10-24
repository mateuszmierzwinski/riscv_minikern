/* swap — pages kept in a file, the way Windows keeps a page file.
 *
 * **A file, not a partition.** The operator asked for it that way and it is
 * the better shape for this kernel: a partition means a second thing to
 * create and a table to agree about, and a file means `dd` on a filesystem
 * that already exists. It costs nothing in the kernel, because a swapfile's
 * blocks are allocated once by whoever made it and are then only ever
 * **overwritten** — which changes no metadata at all. So swapping needs no
 * filesystem write path: the blocks are resolved once at enable time
 * (`xfsmount_map_file`) and read and written as blocks after that. Linux's
 * `swapon` works the same way and for the same reason.
 *
 * **Off by default**, because a kernel that starts swapping without being
 * asked is a kernel whose memory behaviour nobody chose. It is turned on by
 * the command line or by `swapon`, and only by root.
 *
 * What a slot is: one page's worth of the file, numbered from its start. A
 * page that is swapped out leaves its slot number in the page table entry,
 * with the valid bit clear — so the fault that touches it again knows where
 * to find it, and an address that was never mapped at all still looks
 * absent. That is the whole of the mechanism; the policy (when to swap, and
 * how hard) is `swappiness`, which lives beside it.
 */
#ifndef MINIKERN_KERNEL_SWAP_H
#define MINIKERN_KERNEL_SWAP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/types.h"

/* How many pages of swap this build can address.
 *
 * One bit of a bitmap and one 64-bit offset per slot, so 16,384 slots is
 * 64 MiB of swap for 128 KiB of kernel memory. A bigger file is accepted
 * and used up to this many pages rather than refused, because "your swap
 * file is larger than I can use" is not a reason to run with none. */
#define SWAP_MAX_SLOTS 16384u

/* No slot. Zero is a real slot, so the sentinel cannot be zero. */
#define SWAP_NO_SLOT 0xFFFFFFFFu

/* Turns swap on, using `path` as the page file.
 *
 * The file must already exist and be fully allocated: a hole or an
 * unwritten extent has nowhere to put a page, so a sparse file is refused
 * rather than half used. `swappiness` is 0 to 100 and says how willing the
 * kernel is to swap — see `swap_should_reclaim`.
 *
 * KERR_NODEV without a filesystem, KERR_INVAL for a file that cannot be
 * used, KERR_EXIST when swap is already on. */
int swap_enable(const char *path, uint32_t swappiness);

/* Turns swap off. Refused with KERR_BUSY while any page is still out there:
 * the honest alternative is to read every one of them back in, which needs
 * the address spaces that own them and is not this layer's to do. */
int swap_disable(void);

bool swap_enabled(void);

/* How willing the kernel is to swap, 0 to 100. Root's to change. */
uint32_t swap_swappiness(void);
void swap_set_swappiness(uint32_t swappiness);

/* **The free-page count below which the kernel pages out without being
 * asked**, scaled by swappiness and zero when swappiness is zero — so that
 * the knob genuinely includes "off".
 *
 * `swap_set_watermark` overrides it absolutely, which is how the path is
 * tested: no program this kernel can run will put 8 GiB under pressure, so
 * the pressure is brought to the program instead. */
/* How many pages one unasked reclaim takes. A batch rather than one page,
 * because the sweep's first pass may only clear accessed bits and the fence
 * that makes it honest is paid per sweep: asking for one page at a time
 * would pay that cost for every page. */
#define SWAP_RECLAIM_BATCH 32u

uint32_t swap_watermark(void);
void swap_set_watermark(uint32_t pages);

/* Writes a frame out, reporting the slot it went to. KERR_NOSPC when the
 * file is full, KERR_NODEV when swap is off. */
int swap_out(phys_t page, uint32_t *slot);

/* Reads a slot back into a frame **and frees the slot**, because a page
 * that has been brought back is not out any more and the alternative is a
 * file that fills up with pages nobody is looking for. */
int swap_in(uint32_t slot, phys_t page);

/* Gives up a slot whose page nobody wants again — an address space being
 * destroyed, or a mapping being unmapped while it was out. */
void swap_release(uint32_t slot);

/* The numbers: slots in the file, slots in use, pages written out, pages
 * read back. `used` is what a program's memory pressure looks like from
 * here, and `written` against `read` says whether anything ever came back. */
void swap_report(uint32_t *slots, uint32_t *used, uint64_t *written, uint64_t *read_back);

#endif
