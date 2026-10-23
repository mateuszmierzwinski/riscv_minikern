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
#include "arch/riscv/trap.h"
#include "core/types.h"
#include "core/vma.h"
#include "kernel/file.h"
#include "kernel/signal.h"

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

/* Where the kernel's return trampoline is mapped in the process, and the
 * instructions it contains. The page is mapped read-execute, user, by
 * whoever builds the address space.
 *
 * **One page below the bottom of the stack**, derived rather than written
 * down: a literal address a page under the stack *top* was inside the stack
 * — 64 pages of it — and the mapping was refused as a collision, which
 * presented as "this program's headers were refused". Deriving it from the
 * two constants that define the stack means it cannot drift into it when
 * either changes. */
#define SIGNAL_TRAMPOLINE (USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES + 1u) * 4096ull)


/* The kernel registers that must survive a trip through user mode: the
 * return address, the stack pointer, and s0..s11 — exactly what a C caller
 * expects a call to preserve. */
#define USPACE_CONTEXT_WORDS 14u

/* One process: its address space, where it starts, and the kernel context
 * to come back to when it stops. */
/* How many processes may exist. Two is the interesting number — a shell
 * running a program — and four leaves room for the shape of `os/exec`, where
 * a parent, a vforked child and whatever it execs are briefly all alive. */
#define PROC_MAX 4u

struct uproc {
    /* ── identity ──────────────────────────────────────────────────────── */
    uint32_t pid;
    uint32_t parent_pid;
    /* **Process group and session**, both inherited at `fork` and both
     * defaulting to the process's own pid for the first process — which is
     * what makes it a group leader and a session leader without a special
     * case. A signal sent to a negative pid goes to the group, which is how
     * a shell interrupts a pipeline rather than one of its members. */
    uint32_t pgid;
    uint32_t sid;
    /* **Who this process is.** Real and effective kept apart because the
     * effective one is what a permission check uses and the real one is
     * what a process may return to — the distinction that makes dropping
     * privilege temporarily possible, and the one a kernel that keeps a
     * single uid can never offer.
     *
     * The first process is root, because something has to be: a machine
     * whose first program cannot read its own filesystem does not boot. */
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    bool used;
    bool zombie;        /* finished, and keeping its status for its parent */
    int64_t status;     /* what `wait4` will report */

    /* ── what a process owns ───────────────────────────────────────────── */
    /* The descriptor table and the signal dispositions are **the
     * process's**, which is what made them singletons until there were two
     * processes. A thread's own signal state stays in the thread. */
    struct file *fds[FD_MAX];
    struct user_sigaction actions[SIG_COUNT];

    /* ── the address space ─────────────────────────────────────────────── */
    /* `shares_pt` says the tables belong to another process: a `vfork` child
     * runs in its parent's address space until it execs, and freeing the
     * tables twice would hand the same pages back twice. */
    bool shares_pt;

    uint64_t main_tid; /* the first thread's id */
    virt_t brk;        /* the program break, past the last segment */
    virt_t brk_floor;  /* where it started, which it may not go below */
    virt_t mmap_next;  /* the cursor anonymous mappings are placed at */

    /* **What is mapped where**, including the ranges that have no pages.
     *
     * Placement used to consult the page tables, which cannot answer the
     * question: a `PROT_NONE` reservation is deliberately left unmapped —
     * Sv39 has no way to say "mapped but inaccessible" at a leaf — so a
     * reserved range looks free, and a mapping placed inside one is mapped
     * over the moment the runtime commits its reservation with `MAP_FIXED`.
     * Two unrelated allocations then share pages.
     *
     * This set is the address space's own record, so a reservation is as
     * visible as anything else. It was in the tree all along, carried from
     * the ARM kernel and never built, because its addresses were 32 bits
     * wide and user space here is 256 GiB. */
    struct vma_set vmas;
    struct pgtable pt;
    virt_t entry;
    virt_t stack_top;
    uint64_t context[USPACE_CONTEXT_WORDS];
    bool built;
};

/* ── the process table ─────────────────────────────────────────────────────
 *
 * Processes live in a table rather than wherever a caller puts them. A
 * `struct uproc` carries a 64-entry descriptor table and 64 signal
 * dispositions, which is two and a half kilobytes — the frame-size guard
 * caught the first attempt to keep one on a stack, which is exactly what
 * that guard is for.
 */

/* Takes a free slot and gives it a pid. NULL when the table is full. */
struct uproc *uspace_alloc(void);

/* Gives a slot back. A zombie keeps its slot until its parent reaps it. */
void uspace_release(struct uproc *p);

/* The process with this pid, or NULL. */
struct uproc *uspace_by_pid(uint32_t pid);

/* A zombie child of `parent`, or NULL when it has none. */
struct uproc *uspace_reapable_child(uint32_t parent_pid);

/* ── running a program from a program ──────────────────────────────────────
 */

/* Replaces `p`'s program with the ELF in `image`, keeping its identity, its
 * descriptors (minus close-on-exec) and its thread, and giving it a fresh
 * address space, break, mapping cursor and start state.
 *
 * The old address space is freed **only once the new one is built**, so a
 * failure leaves the process able to carry on — which is what a program that
 * handles a failing `execve` is entitled to. */
int uspace_exec(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path,
                const char *const *argv, uint32_t argc);

/* Points `tf` at the newly execed program, so the calling thread restarts
 * there instead of returning from the syscall. */
void uspace_restart_thread(struct trapframe *tf, const struct uproc *p);

/* Makes `child` a process that **borrows** `parent`'s address space: the
 * `vfork` arrangement, safe only because the parent is then suspended. The
 * child's descriptors are copies of its parent's. */
int uspace_fork_shared(struct uproc *child, struct uproc *parent);

/* Suspends the calling thread until `child` execs or exits, which is what
 * `CLONE_VFORK` promises. */
void uspace_wait_for_vfork(struct uproc *child);

/* Suspends the calling thread until one of its children finishes. */
void uspace_wait_for_child(struct uproc *parent);

/* How many live children a process has, so `wait4` can tell "none finished"
 * from "none exist". */
uint32_t uspace_count_children(uint32_t parent_pid);

/* A process's pid from its handle, for callers that hold one and must not
 * reach into the struct — the scheduler, which knows about threads and
 * their processes but has no business reading a process's fields. */
uint32_t uspace_pid_of(const struct uproc *p);
uint32_t uspace_pgid_of(const struct uproc *p);

/* The caller's effective identity, for a permission check. */
uint32_t uspace_euid(void);
uint32_t uspace_egid(void);

/* Whether the caller may open something with these owners and mode, for
 * `want` of `PERM_READ`/`PERM_WRITE`/`PERM_EXEC`.
 *
 * **Root bypasses**, which is not laziness but the definition: uid 0 is the
 * identity the checks do not apply to, and a kernel that enforced them
 * against it would have no way to administer anything.
 *
 * The three-way owner/group/other test is **exclusive** — if the caller is
 * the owner, only the owner bits are consulted, and a mode of 0077 denies
 * its owner on purpose. A check that fell through to the group bits for an
 * owner who was denied would make that mode mean something else entirely. */
#define PERM_READ 4u
#define PERM_WRITE 2u
#define PERM_EXEC 1u
bool uspace_may_access(uint32_t file_uid, uint32_t file_gid, uint32_t mode, uint32_t want);

/* Raises a signal on every process in a group, the way a terminal must: a
 * pipeline is several processes and a key reaching one of them leaves the
 * others running with their input gone. Returns how many it reached. */
uint32_t uspace_signal_group(uint32_t pgid, uint32_t signo);

/* Every process in a group, for a signal sent to one. Calls `fn` with each,
 * and returns how many it called it for. */
uint32_t uspace_for_each_in_group(uint32_t pgid, void (*fn)(struct uproc *p, uint32_t arg),
                                  uint32_t arg);

/* Frees an image read for an `execve`. A thin name over the heap so
 * `sys_proc.c` need not know which allocator it came from. */
void kfree_image(void *image);

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

/* The same, with the argument list a caller supplies — which is what
 * `execve` needs and what `argv[0]` being the program's own choice means. */
int uspace_build_elf_args(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path,
                          const char *const *argv, uint32_t argc);

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

/* Makes a *new* mapping reach every hart, not just this one.
 *
 * The default is local-only, because a stale "absent" entry is harmless — a
 * hart that trips over one fences and retries — and broadcasting on every
 * map cost more than SMP gained. This exists to test that reasoning against
 * a machine rather than to be believed. */
void uspace_broadcast_additions(bool on);

/* One extra variable in the environment every process starts with, on top of
 * `HOME` and `PATH`.
 *
 * A kernel that can pass nothing to the program it starts cannot ask it to
 * behave differently, and some of the most useful questions are exactly
 * that: a Go runtime's `GODEBUG` can switch its asynchronous preemption off,
 * which separates "the program is wrong" from "signal delivery is wrong"
 * without changing either. Empty or NULL passes nothing. */
void uspace_set_extra_env(const char *text, uint32_t len);

/* How many live pages were destroyed by a mapping the caller did not insist
 * on. Any value but zero means a program's memory was replaced with zeroes
 * underneath it. */
uint32_t uspace_replaced_pages(void);

/* Mappings the address-space record could not hold, so placement may reuse
 * an address that is live. */
uint32_t uspace_unrecorded_regions(void);

/* A child with an address space of its own, made of the parent's pages
 * copy-on-write. Unlike `uspace_fork_shared` (which is `vfork`), the two
 * processes are isolated from the moment it returns. */
int uspace_fork(struct uproc *child, struct uproc *parent);

/* How many copy-on-write faults produced a private copy, and how many found
 * themselves the last holder and simply took the page. */
/* How many faults the kernel resolved and returned from. A number that
 * climbs without the program progressing is a fault loop. */
uint32_t uspace_handled_faults(void);

uint32_t uspace_cow_copies(void);
uint32_t uspace_cow_takeovers(void);

#endif
