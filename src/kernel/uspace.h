/* uspace — user mode: an address space of its own, and the way into it.
 *
 * This is the step everything else in the kernel exists for. A process has
 * its own Sv39 table with the kernel's half shared and its own half private,
 * and entering it is `sret` with `sstatus.SPP` cleared — one instruction,
 * where the architecture this migrated from needed a mode switch, a banked
 * register set and a stack per mode.
 *
 * **The kernel's half must be mapped in every address space.** When a trap
 * takes a user process into the kernel, translation does not change — the
 * same `satp` is still loaded — so the handler's code and stack have to be
 * addressable from where the process was. That is why the kernel is mapped
 * `PTE_G` (global, not flushed per address space) and why a process table
 * is built by copying the kernel's top-level entries rather than starting
 * empty.
 *
 * Nothing here loads an ELF yet: the first user program is a page of
 * instructions the kernel places, which is enough to prove the mode switch,
 * the trap path out of user mode and the syscall convention. An ELF loader
 * on top of a path that has never been entered would be two untested things
 * at once.
 */
#ifndef MINIKERN_KERNEL_USPACE_H
#define MINIKERN_KERNEL_USPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/pgtable.h"
#include "core/types.h"

/* Where a user program and its stack live. Low addresses, well inside the
 * bottom half, and nowhere near the kernel's. */
#define USER_TEXT 0x10000ull
#define USER_STACK_PAGES 64u

/* The top of user space, and the stack just under it.
 *
 * 256 GiB is **Sv39's whole user half**: bit 38 must be clear for an
 * address to be a user address at all, so `0x40_0000_0000` is the limit the
 * hardware imposes rather than a number chosen here. It matters because a
 * Go runtime places its heap by hint, high up, and a kernel that offers
 * only the bottom gigabyte makes every one of those hints fail.
 *
 * The stack sits just below the top, and `mmap` grows up from the middle,
 * so the two cannot meet by accident. */
#define USER_TOP 0x4000000000ull
#define USER_STACK_TOP 0x3FFFFFF000ull
#define USER_MMAP_BASE 0x2000000000ull

/* The kernel registers that must survive a trip through user mode: the
 * return address, the stack pointer, and s0..s11 — exactly what a C caller
 * expects a call to preserve. */
#define USPACE_CONTEXT_WORDS 14u

/* One process: its address space, where it starts, and the kernel context
 * to come back to when it stops. */
struct uproc {
    uint64_t main_tid; /* the first thread's id */
    virt_t brk;        /* the program break, past the last segment */
    virt_t brk_floor;  /* where it started, which it may not go below */
    virt_t mmap_next;  /* the cursor anonymous mappings are placed at */
    struct pgtable pt;
    virt_t entry;
    virt_t stack_top;
    uint64_t context[USPACE_CONTEXT_WORDS];
    bool built;
};

/* Builds an address space with the kernel's mappings shared, `bytes` of
 * program copied in at USER_TEXT, and a stack. KERR_NOMEM when pages run
 * out. For the hand-written test programs: no headers, no start state. */
int uspace_build(struct uproc *p, const void *program, uint32_t bytes);

/* Makes this hart able to run the process's threads: its own `satp` and its
 * own `sstatus.SUM`, both per-hart registers. */
void uspace_adopt_address_space(struct uproc *p);
void uspace_release_address_space(void);

/* A secondary hart joining a running process; returns when it is over. */
void uspace_join(struct uproc *p);

/* ── reading and writing a process's memory ────────────────────────────────
 *
 * **The kernel should not dereference a user pointer.** It can — `sstatus.SUM`
 * is set while a syscall runs and the process's own table is loaded — and
 * that is exactly the problem: a pointer that is wrong faults *in the
 * kernel*, where a fault is a panic. A program that passes a bad pointer to
 * a syscall would then stop the machine, which is a denial of service any
 * user can perform with one line.
 *
 * These check the range against the process's page table, a page at a time,
 * for the rights the direction needs, and copy through it. A bad pointer is
 * `KERR_FAULT` returned to the caller, which is what the ABI promises.
 *
 * The architecture this migrated from had this property from early on and
 * named it: "the kernel never dereferences a user pointer". This kernel grew
 * the other way round, so the older syscalls still dereference directly and
 * are being moved over; the sockets, which take the most pointers, use these
 * from the start.
 */
int uspace_copy_in(void *to, virt_t from, uint32_t n);
int uspace_copy_out(virt_t to, const void *from, uint32_t n);

/* Whether a range is readable, or writable, by the process — for a caller
 * that wants to check before doing something it cannot undo. */
bool uspace_can_read(virt_t at, uint32_t n);
bool uspace_can_write(virt_t at, uint32_t n);

/* The process a syscall is being made by, or NULL outside one. There is one
 * at a time until there is a scheduler. */
struct uproc *uspace_current(void);

/* Ends the running process with a status, from inside a syscall. Does not
 * return: it unwinds to whoever called `uspace_run`. */
void uspace_exit(int64_t status);

/* Maps anonymous zeroed pages into the running process.
 *
 * `at` of 0 lets the kernel choose, and a non-zero `at` is **a request, not
 * a demand** unless `fixed` — which is what a runtime placing its heap by
 * hint expects: an address it cannot have comes back as a different one, not
 * as a failure.
 *
 * `prot` is PROT_* from `core/elf.h`. PROT_NONE is a **reservation**: an
 * address range is promised and nothing is backed, because a Go runtime
 * reserves its arenas before it uses any of them, and backing hundreds of
 * gigabytes of address space with page tables would exhaust memory for
 * nothing. The pages appear when the range is mapped again with real
 * permissions.
 *
 * Reports the address it chose. KERR_NOMEM when pages or address space run
 * out, KERR_INVAL for a length of zero or an unaligned fixed address. */
int uspace_map_anon(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot, bool fixed,
                    virt_t *out);

/* Unmaps a range and frees whatever was backing it. A range that was only
 * reserved unmaps successfully, having nothing to free — which is what a
 * runtime releasing an arena it never touched expects. */
int uspace_unmap(struct uproc *p, virt_t at, uint64_t bytes);

/* Changes the permissions on a range that is already mapped, and backs a
 * reservation that is being made usable. */
int uspace_protect(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot);

/* The program break. `want` of 0 asks where it is; anything else moves it
 * and reports where it ended up — which is the old value when the move was
 * refused, because that is how `brk` reports failure. */
virt_t uspace_brk(struct uproc *p, virt_t want);

/* Builds an address space from an **ELF image**: every loadable segment at
 * the address its header names, with the permissions its flags ask for, a
 * stack, and the start state a C runtime expects — `argc`, `argv`, the
 * environment and an auxiliary vector.
 *
 * `argv[0]` and the path are what the program will see; the image itself is
 * not kept, so the caller may free it the moment this returns.
 *
 * KERR_NOEXEC for something that is not a static RISC-V 64 executable,
 * KERR_INVAL for headers that contradict the file, KERR_NOMEM when pages
 * run out. */
int uspace_build_elf(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path);

/* Releases the process's pages and its table. */
void uspace_destroy(struct uproc *p);

/* Enters the process. Returns when it exits — with the status it asked to
 * exit with, or a negative error when it faulted. */
int64_t uspace_run(struct uproc *p);

/* How many syscalls the running process has made, for a test to assert
 * against rather than trusting a single line of output. */
uint64_t uspace_syscalls(void);

/* How many faults were spurious — an access the page tables allowed, on a
 * hart whose cached translations had not caught up. It is the price of not
 * broadcasting a fence on every mapping, and worth being able to see: if it
 * ever grew large, the trade would be the wrong way round. */
uint64_t uspace_stale_faults(void);

#endif
