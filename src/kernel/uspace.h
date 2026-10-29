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
#include "core/caps.h"
#include "core/types.h"
#include "core/vma.h"
#include "kernel/file.h"
#include "kernel/seccomp.h"
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
/* **The un-randomised layout**, which is what a process gets when address
 * randomisation is off. With it on, each process's own `stack_top`,
 * `trampoline` and `mmap_floor` are these shifted by an amount chosen when
 * the address space is built — so these are the ceiling of the ranges
 * rather than the addresses anything actually uses.
 *
 * `SIGNAL_TRAMPOLINE` is kept as the un-randomised value for the same
 * reason: it is derived from the stack, so it has to move with it, and the
 * per-process field is `p->trampoline`. A literal address a page under the
 * stack *top* was once inside the stack — 64 pages of it — and the mapping
 * was refused as a collision, which presented as "this program's headers
 * were refused". Deriving it means it cannot drift into the stack when
 * either constant changes; making it per-process means it cannot drift when
 * the stack itself moves. */
#define SIGNAL_TRAMPOLINE (USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES + 1u) * 4096ull)

/* How far the three regions may be shifted, in pages.
 *
 * **Modest on purpose.** 16 MiB of stack slide and 64 MiB of heap and
 * mapping slide is a few thousand possibilities each, which is what makes
 * an address guess fail; going much wider starts refusing the placement
 * *hints* a Go runtime gives for its heap, and a kernel that makes every
 * hint fail is a kernel Go runs slowly on. The number that matters for an
 * attacker is the number of bits, and these give about twelve each — the
 * same order as a 32-bit Linux, on a kernel whose user half is 256 GiB. */
#define ASLR_STACK_PAGES 4096u /* 16 MiB */
#define ASLR_BRK_PAGES   16384u /* 64 MiB */
#define ASLR_MMAP_PAGES  16384u /* 64 MiB */


/* The kernel registers that must survive a trip through user mode: the
 * return address, the stack pointer, and s0..s11 — exactly what a C caller
 * expects a call to preserve. */
#define USPACE_CONTEXT_WORDS 14u

/* One process: its address space, where it starts, and the kernel context
 * to come back to when it stops. */
/* How many processes may exist.
 *
 * **Four was sized for a shell, and an init needs more than a shell.** The
 * old number allowed a parent, a vforked child and whatever it execs — which
 * is the shape of one `os/exec` call. QuokkaOS's init *is* PID 1 with a
 * service per entry in a runlevel, and it hit the wall at its fourth
 * service: `fork/exec: resource temporarily unavailable`, on the board, with
 * no indication that the limit was the kernel's rather than the memory's.
 *
 * Sixteen, because a `struct uproc` is about two and a half kilobytes, so
 * the table is forty kilobytes against eight gigabytes of DRAM — and because
 * a runlevel of a dozen services plus init plus the `os/exec` transient is
 * the shape this now has to fit. It is still a fixed table: a limit that is
 * reached and says so beats an allocator that fails somewhere less
 * obvious. */
#define PROC_MAX 16u

/* How many supplementary groups a process may hold.
 *
 * Sixteen: enough for any plausible service account, and a fixed array
 * rather than an allocation because the one thing a credential must never do
 * is fail to be set for want of memory. A request for more is refused with
 * EINVAL, which `setgroups(2)` already defines for exactly this. */
#define PROC_MAX_GROUPS 16u

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
    uint32_t suid; /* saved: what an unprivileged setresuid may return to */
    /* **The supplementary groups, so that dropping privilege actually
     * drops it.** `setuid` and `setgid` move the primary identity and leave
     * these behind, which is the classic incomplete privilege drop: a
     * process that was root keeps root's groups and the access they carry.
     * QuokkaOS's init had to log that its drop was weaker than it looked
     * because the kernel had nowhere to put them. */
    uint32_t groups[PROC_MAX_GROUPS];
    uint32_t ngroups;
    /* The bits a create must not grant. Inherited by a fork and **kept
     * across an execve**, which is what the ABI says: a shell sets it once
     * and every program it runs is meant to honour it. */
    uint32_t umask;
    /* The working directory, as an absolute path with no trailing slash
     * except at the root. Inherited by a fork and **kept across an
     * `execve`**, which is what makes `cd` in a shell mean anything. */
    char cwd[128];
    /* **Resource limits, soft and hard.**
     *
     * Only the ones this kernel can actually enforce are kept: descriptors,
     * processes and the stack. A limit that is stored and never consulted is
     * worse than an absent one, because a program reads it and plans
     * around a number nothing will hold it to.
     *
     * Inherited at `fork` and across `execve`, which is what makes a shell
     * able to limit a job by setting its own limits before starting it. */
    uint64_t rlim_nofile_soft;
    uint64_t rlim_nofile_hard;
    uint64_t rlim_nproc_soft;
    uint64_t rlim_nproc_hard;
    /* **What this process may do, one bit per privileged operation.** The
     * uid above says who it is; this says what that lets it do, and the
     * checks consult only this — so root is a process whose set happens to
     * be full rather than a special case in every caller. */
    struct cap_sets caps;
    /* The syscall filter it has installed on itself, if any. One-way: see
     * `kernel/seccomp.h`. */
    struct seccomp_state seccomp;
    bool used;
    bool zombie;        /* finished, and keeping its status for its parent */
    int64_t status;     /* what `wait4` will report */

    /* ── what a process owns ───────────────────────────────────────────── */
    /* The descriptor table and the signal dispositions are **the
     * process's**, which is what made them singletons until there were two
     * processes. A thread's own signal state stays in the thread. */
    struct file *fds[FD_MAX];
    struct user_sigaction actions[SIG_COUNT];
    /* Signals sent to the process (`kill`) rather than to a thread: any
     * thread with one unblocked takes it. Kept across `execve`, cleared for
     * a fork's child — as on Linux. */
    uint64_t shared_pending;

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
    /* **Where this process's signal trampoline and mapping floor are.**
     * Both used to be constants, and both have to move when the stack and
     * the heap do — a trampoline left at the un-randomised address would
     * be mapped where the stack now is, and a floor left fixed would let a
     * mapping be placed below the randomised heap. */
    virt_t trampoline;
    virt_t mmap_floor;
    uint64_t context[USPACE_CONTEXT_WORDS];
    bool built;
    /* Where the reclaim clock last stopped: a **virtual address**, not an
     * index into the regions. Regions come and go between sweeps, so an
     * index would point at a different region each time — which is how a
     * rotating policy quietly becomes a fixed one that always takes the
     * first page it sees. */
    virt_t reclaim_cursor;
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

/* Prints the pids the process table holds, capped at a few calls. For a
 * lookup that answered absent for a process the caller had just been
 * handed: the answer and the evidence then come from one instant. */
void uspace_report_pids(const char *why);

/* How many processes hold the frame behind a process's entry point, for the
 * question of whether recording the segments makes `fork` mis-count the
 * text pages it then shares. Capped at a few calls. */
void uspace_report_entry_shares(const char *when, const struct uproc *p);

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

/* **Whether the current process holds a capability.**
 *
 * This is what a privileged syscall asks, in place of `uspace_euid() != 0`.
 * The two are not equivalent and the difference is the point: root's
 * effective set is full, so every check root used to pass it still passes,
 * but a process that has dropped `CAP_SYS_ADMIN` now fails the ones that
 * need it *while still being uid 0*. Keeping the uid test as a second path
 * would give two answers that can disagree, and the disagreeing one would
 * be the one nobody tested.
 *
 * True when there is no current process, because then the caller is the
 * kernel's own start-up and there is nobody to deny. */
bool uspace_capable(uint32_t cap);

/* The current process's signal trampoline, and whether addresses are
 * randomised. `minikern.noaslr` turns randomisation off, which is what
 * makes a crash address comparable between two runs. */
virt_t uspace_trampoline(void);
void uspace_set_aslr(bool on);

/* Whether `fork` hands over pages no region claims — the fix for a child
 * that dies on its own text. On by default; `minikern.noforkleaves` turns it
 * off, as the control. */
void uspace_set_fork_leaves(bool on);
bool uspace_aslr(void);

/* The calling process's soft limit on open descriptors, for the descriptor
 * table to enforce. `RLIM_INFINITY` when there is no process — the kernel's
 * own descriptors are not somebody's quota. */
uint64_t uspace_rlimit_nofile(void);

/* The soft limit on processes, and how many the caller's uid already has.
 * Counting by uid rather than per process is what the limit means: a fork
 * bomb is one program making thousands, and a per-process count would let
 * each child start again from zero. */
uint64_t uspace_rlimit_nproc(void);
uint32_t uspace_count_by_uid(uint32_t uid);
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

/* What a mapping is being asked for.
 *
 * A struct rather than a row of positional arguments: `mmap` has six of its
 * own, the flags that change what this kernel *does* keep being added to
 * them, and the next thing to come — a file to map — belongs here rather
 * than on the end of a signature nobody can read. The caller fills in what
 * it means; everything it leaves alone is the ordinary case.
 *
 * `at` of 0 lets the kernel choose, and a non-zero `at` is **a request, not
 * a demand** unless `fixed` — which is what a runtime placing its heap by
 * hint expects: an address it cannot have comes back as a different one,
 * not as a failure.
 *
 * `prot` is PROT_* from `core/elf.h`. PROT_NONE is a **reservation**: an
 * address range is promised and nothing is backed, because a Go runtime
 * reserves its arenas before it uses any of them, and backing hundreds of
 * gigabytes of address space with page tables would exhaust memory for
 * nothing. The pages appear when the range is mapped again with real
 * permissions. */
struct uspace_mapping {
    virt_t at;
    uint64_t bytes;
    uint32_t prot;
    bool fixed;     /* MAP_FIXED: this address, replacing what is there */
    bool noreplace; /* MAP_FIXED_NOREPLACE: this address, and over nothing */
    bool populate;  /* MAP_POPULATE: back the pages now, not on first touch */
    bool huge;      /* MAP_HUGETLB: 2 MiB pages, one table entry each */
    /* What a file-backed mapping reads from. The mapping takes its own
     * reference, so closing the descriptor afterwards does not take the
     * bytes out from under it — which is what `mmap` of a file promises. */
    struct file *file;
    uint64_t offset;
};

/* Maps pages into the running process: zeroed, or filled from a file at
 * their first touch when the request names one.
 *
 * Reports the address it chose. KERR_NOMEM when pages or address space run
 * out, KERR_INVAL for a length of zero or an unaligned fixed address, and
 * KERR_EXIST for a `noreplace` mapping whose range is not free. */
int uspace_map(struct uproc *p, const struct uspace_mapping *m, virt_t *out);

/* Unmaps a range and frees whatever was backing it. A range that was only
 * reserved unmaps successfully, having nothing to free — which is what a
 * runtime releasing an arena it never touched expects. */
int uspace_unmap(struct uproc *p, virt_t at, uint64_t bytes);

/* Changes the permissions on a range that is already mapped, and backs a
 * reservation that is being made usable. */
int uspace_protect(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot);

/* Resizes a mapping, and moves it when it cannot grow where it stands.
 *
 * `[at, at + old_bytes)` must lie inside **one** region, or the answer is
 * `KERR_FAULT`: a range spanning two has two sets of rights and the result
 * could only have one. Shrinking unmaps the tail. Growing takes the pages
 * past the end when they are free, and otherwise needs `may_move`, without
 * which the answer is `KERR_NOMEM` — a caller that cannot cope with its
 * memory changing address says so by withholding it.
 *
 * `to` is a destination the caller insists on (`MREMAP_FIXED`), or 0 for
 * wherever it fits; it must be inside the window `mmap` places into.
 *
 * Nothing is copied: the frames that exist are re-pointed at the new
 * addresses and the holes stay holes. Reports where the mapping ended up,
 * which is `at` itself in every case but a move. */
int uspace_remap(struct uproc *p, virt_t at, uint64_t old_bytes, uint64_t new_bytes, bool may_move,
                 virt_t to, virt_t *out);

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
/* The running process's umask, for the filesystem's installed reader. A
 * function rather than the field, so that nothing outside this module needs
 * a `struct uproc` to ask. */
/* Turns on the entry-patching measurement: the first two instructions of
 * every exec'd image are replaced with a load from 0x7ad, so that the fault
 * report says what the hart actually fetched. Destroys the program it
 * measures, which is why it is a flag. */
void uspace_patch_entry_for_measurement(bool on);

/* Writes words into a process's address space through its own page tables. */
int uspace_poke_user_words(struct uproc *p, uint64_t at, const uint32_t *words, uint32_t n);

uint32_t uspace_current_umask(void);

/* Sets the running process's umask, for the board self-test — which runs in
 * the kernel and has no syscall layer to go through. */
void uspace_set_umask_for_test(uint32_t mask);

/* The running process's working directory, always absolute. Returns "/" when
 * there is no process, so a caller never has to handle an empty answer. */
const char *uspace_current_cwd(void);

/* Sets it, after the caller has checked the path exists and is a directory.
 * The check is the caller's because this module knows nothing about
 * filesystems — `uspace` is depended on by `xfsmount`, not the reverse. */
int uspace_set_cwd(const char *path);

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

/* Live pages thrown away by a `MAP_FIXED` mapping. A program is entitled to
 * do this; it is evidence when that program thought the range was free. */
uint32_t uspace_fixed_over_live(void);

/* Mappings placed where the address-space record says something already is.
 * The one placement error a page-table check cannot see, because a
 * reservation has no pages. */
uint32_t uspace_misplaced_mappings(void);

/* Throws the pages in a range away, keeping the addresses: what
 * `MADV_DONTNEED` and `MADV_FREE` mean. The next touch faults a fresh zero
 * page, which is the guarantee a runtime relies on when it hands the range
 * out again without zeroing it. */
int uspace_discard_range(struct uproc *p, virt_t at, uint64_t bytes);

/* Pages given up because a program said it would not need them. */
uint64_t uspace_discarded_pages(void);

/* Pages that two harts both set out to back, each with its own frame. Any
 * value but zero means one of those frames was live in a hart's TLB while
 * the page table named the other — two copies of one page. */
uint32_t uspace_backed_twice(void);

/* Mappings the address-space record could not hold, so placement may reuse
 * an address that is live. */
uint32_t uspace_unrecorded_regions(void);

/* How many pages were backed on first touch rather than at `mmap`. Zero
 * means demand paging is not happening, whatever the code says. */
uint64_t uspace_demand_pages(void);

/* How many pages `mremap` re-pointed at a new address rather than copying.
 * It is what makes "nothing is copied" a measurement instead of a claim: a
 * move of a touched 64 MiB mapping reports its pages here and leaves the
 * demand-paging count alone. */
uint64_t uspace_moved_pages(void);

/* Times a page was found already mapped at an address the region record
 * said was free — the record and the page tables disagreeing. Any value but
 * zero means something mapped memory without recording it, which is this
 * kernel's most expensive class of defect. */
uint32_t uspace_stale_records(void);

/* How the reclaim clock is doing: pages left alone because the hardware
 * said they had been touched, and sweeps that found nothing cold. Zero
 * second chances means the clock is choosing exactly as the policy it
 * replaced did, which is the one thing a page count cannot show. */
/* Pages sent out because free memory was below the watermark, rather than
 * because a program asked. Zero on a machine that never came under
 * pressure, which is most of them — so a test of the watermark has to say
 * what this number was. */
uint64_t uspace_pages_out_unasked(void);

/* Narrates the next `n` user page faults and the reclaims they trigger, for
 * a test that must see whether a program is making progress. */
void uspace_trace_faults(uint32_t n);

/* Faults raised purely so software would set `PTE_A`. Non-zero means this
 * core does not update the bit itself, which is the opposite of what this
 * project believed until the reclaim clock cleared one. */
uint64_t uspace_accessed_faults(void);

uint64_t uspace_second_chances(void);
uint64_t uspace_empty_sweeps(void);

/* How many pages were backed at `mmap` because the caller asked for them up
 * front (`MAP_POPULATE`) rather than on first touch. It is how "the flag is
 * honoured" is told from "the flag is accepted": the pages it covers appear
 * here and never in the demand count. */
uint64_t uspace_populated_pages(void);

/* Backs every mapping at `mmap` rather than on first touch — the behaviour
 * before demand paging, kept as a knob because it bisects the fault path
 * out of a hunt in one line. */
void uspace_back_eagerly(bool on);

/* Turns on the checks that are too expensive to leave running: reading back
 * every page the kernel zeroes, and walking the tables for a second mapping
 * of a frame that turns out dirty. For a hunt, not for a boot. */
void uspace_set_paranoid(bool on);

/* Walks a process's whole address space and reports any frame two
 * addresses can reach. The §0 hunt's central question, asked about
 * everything at once instead of in a microsecond-wide window. */
void uspace_audit_aliases(struct uproc *p);

/* What backs one address right now: its entry, the region that claims it,
 * and every other address that reaches the same frame. */
void uspace_audit_address(struct uproc *p, virt_t at);

/* Frames the audit found mapped twice. Any value but zero is one page with
 * two owners, which is what the corruption looks like from outside. */
uint32_t uspace_aliased_frames(void);

/* Sends up to `want` of a process's pages out to swap, reporting how many
 * went. Anonymous pages only: a huge mapping is one entry over 2 MiB, a
 * file-backed page is better dropped and re-read, and the signal
 * trampoline has to be there at the moment a signal is delivered — which
 * is precisely when the kernel cannot wait for a disk. */
uint32_t uspace_reclaim_pages(struct uproc *p, uint32_t want);

/* The same for one range, which is what `madvise(MADV_PAGEOUT)` asks for:
 * the program naming the memory it will not need soon. */
uint32_t uspace_reclaim_range(struct uproc *p, virt_t at, uint64_t pages);

/* Pages brought back from swap, and pages sent out to it. The second
 * without the first is a program that has not missed them yet. */
void uspace_swap_report(uint64_t *in, uint64_t *out);

/* Raises a signal on a process, doing **what that signal means**.
 *
 * Four of them do not reach a handler at all by default: `SIGSTOP` always
 * stops the process, and `SIGTSTP`, `SIGTTIN` and `SIGTTOU` stop it unless
 * it has claimed them — a shell catches `SIGTSTP` so that it can record the
 * job before stopping. `SIGCONT` continues it. Everything else is raised
 * for delivery in the ordinary way.
 *
 * **One place, because it had two and both had half of it.** `kill` and the
 * terminal each carried their own copy handling `SIGSTOP` and `SIGCONT`, so
 * `^Z` — which is `SIGTSTP` — fell through to ordinary delivery, where a
 * signal nothing handles is fatal: the keystroke **killed** the foreground
 * job instead of stopping it. The same hole made a background `read` kill
 * the reader rather than stopping it. A rule about what a signal means
 * belongs with the signal, not with each of its senders. */
void uspace_deliver_signal(struct uproc *p, uint32_t signo);

/* Frames about to be returned through to user mode whose `sepc` or stack
 * pointer was a kernel address. A program cannot cause one: any value but
 * zero means this kernel restored the wrong registers, and the fault that
 * follows happens later and somewhere else. */
uint32_t uspace_impossible_frames(void);

/* How many pages have been filled from a file at their first touch. A
 * file-backed mapping is otherwise indistinguishable from an anonymous one
 * that a program happened to write the same bytes into. */
uint64_t uspace_file_pages(void);

/* Frames that were not zero immediately after being zeroed for a fresh
 * mapping. A fresh anonymous page must read as zero — a Go runtime does not
 * zero a span it believes came from the kernel — so any value but zero here
 * is memory handed to two owners at once. */
uint32_t uspace_dirty_frames(void);

/* Frames handed back to the allocator because a mapping went away. */
uint64_t uspace_freed_frames(void);

/* How many 2 MiB pages have been handed to user space. It is the only way
 * to tell a huge mapping from an ordinary one from outside: the program
 * cannot see the difference, and a kernel that quietly used 4 KiB pages
 * would look identical in every other respect. */
uint64_t uspace_huge_pages(void);

/* A child with an address space of its own, made of the parent's pages
 * copy-on-write. Unlike `uspace_fork_shared` (which is `vfork`), the two
 * processes are isolated from the moment it returns. */
int uspace_fork(struct uproc *child, struct uproc *parent);

/* How many copy-on-write faults produced a private copy, and how many found
 * themselves the last holder and simply took the page. */
/* How many faults the kernel resolved and returned from. A number that
 * climbs without the program progressing is a fault loop. */
uint32_t uspace_handled_faults(void);

/* Gives a process its own copy of a shared copy-on-write page at `va`.
 *
 * Called by the store-fault handler and by the syscall pointer checks: a
 * page left read-only by `fork` is one the process has not written yet, not
 * one it may not write, and a syscall that refuses it reports EFAULT for a
 * perfectly good pointer. False means the page was not copy-on-write, so
 * the caller's refusal stands. */
bool uspace_break_cow(struct uproc *me, virt_t va);

uint32_t uspace_cow_copies(void);
uint32_t uspace_cow_takeovers(void);

#endif
