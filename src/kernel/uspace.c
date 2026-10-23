#include "kernel/uspace.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "core/elf.h"
#include "core/access.h"
#include "core/errno.h"
#include "core/ustack.h"
#include "kernel/mem.h"
#include "kernel/random.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/klock.h"
#include "kernel/sched.h"
#include "arch/riscv/sigframe.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"

/* Mappings the address-space record could not hold. Any value but zero means
 * placement may hand out an address that is already in use. */
static uint32_t unrecorded_regions;

/* Pages backed on first touch rather than at `mmap`. The measure of whether
 * demand paging is doing anything: zero means every mapping is still being
 * backed up front. */
static uint64_t demand_pages;

/* Defined further down, next to the rest of the mapping machinery: the
 * fault handler is above it because the trap path is read first. */
static int back_one_page(struct uproc *p, virt_t va, uint32_t prot);
static int back_for_region(struct uproc *p, const struct vma *r, virt_t va);
static int drop_pages(struct uproc *p, virt_t at, uint64_t bytes);
static void record_stack(struct uproc *p);
static void drop_every_file(struct uproc *p);
static void published_addition(void);

/* The generic Linux syscall numbers, which is the set RISC-V uses — and the
 * modern one: there is no `mmap2`, no `_llseek`, no `stat64`, no
 * `*_time64` duplicate of anything. The architecture this migrated from
 * implemented all of those as separate entry points. */
#define SYS_WRITE 64u
#define SYS_EXIT 93u
#define SYS_EXIT_GROUP 94u

/* sstatus bits for the way into user mode. */
#define SSTATUS_SPP (1ull << 8)   /* 0 = return to user */
#define SSTATUS_SPIE (1ull << 5)  /* interrupts enabled after sret */
#define SSTATUS_SUM (1ull << 18)  /* the kernel may read user memory */

static uint64_t prot_bits(uint32_t prot);

/* The process table. Static because a process is too big for a stack — the
 * frame-size guard said so the first time one was put there — and because a
 * process must outlive whatever created it. */
static struct uproc procs[PROC_MAX];
static uint32_t next_pid = 1;

static struct uproc *running;
static struct uproc *building;
static int64_t exit_status;
static bool exited;
static uint64_t kernel_satp_saved;
static uint64_t calls_at_start;
static uint64_t stale_faults;

/* The pair in arch/riscv/uspace_entry.S. `uspace_leave` does not return to
 * its caller: control reappears wherever `uspace_enter` was called from. */
void uspace_enter(virt_t entry, virt_t user_sp, uint64_t *context);
void uspace_leave(uint64_t *context);

/* Table memory for a process, from the frame allocator. The kernel's own
 * map used a bump arena because it never goes away; a process's does. */
static int proc_alloc(void *self, void **virt, phys_t *phys) {
    phys_t page = 0;
    int rc;
    (void)self;
    rc = mem_alloc_pages(1, 1, &page);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)page)[i] = 0;
    }
    *virt = (void *)(uintptr_t)page;
    *phys = page;
    return KERR_OK;
}

static void proc_free(void *self, void *virt) {
    (void)self;
    (void)mem_free_pages((phys_t)(uintptr_t)virt, 1);
}

static void *proc_to_virt(void *self, phys_t phys) {
    (void)self;
    return (void *)(uintptr_t)phys;
}

/* Shares the kernel's mappings into a process's table.
 *
 * Every one of them, because a trap does not change translation: when a
 * process enters the kernel the *process's* `satp` is still loaded, so the
 * handler's code, its stack and **the console's registers** all have to be
 * addressable from there. A process's own pages carry `PTE_U` and the
 * kernel's do not, so sharing them costs no isolation — user mode cannot
 * reach a page that is not marked user-reachable.
 *
 * Copying the top-level entries alone is *not* enough, and finding that out
 * was instructive. An Sv39 root entry covers 1 GiB; the kernel's device
 * registers sit low — the console's at `0x10000000` — and so does a
 * process's text at `0x10000`. They collide in root entry 0, so copying
 * only where the process's entry was empty silently dropped the console
 * from the process's address space. The first `write` syscall then faulted
 * inside the kernel while printing, and the fault handler faulted while
 * reporting that, until the stack ran off the end — a hang with no output
 * at all.
 *
 * So a collision is resolved one level down: the process keeps its own
 * level-1 table and the kernel's entries are copied into the gaps. A
 * collision deeper than that cannot be resolved this way and is refused
 * rather than papered over.
 *
 * **This whole function disappears when the kernel moves to the top half**
 * (docs/DEVELOPMENT_PROGRESS.md P0.4): with the kernel above the sign-extension
 * hole and user space below it, the two cannot share a root entry at all,
 * and sharing becomes copying 256 entries that can never collide. */
static int share_kernel_mappings(struct pgtable *pt) {
    const struct pgtable *kernel = mmu_kernel_table();
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        uint64_t theirs = kernel->root[i];
        if (theirs == 0) {
            continue;
        }
        if (pt->root[i] == 0) {
            pt->root[i] = theirs; /* no collision: share the subtree whole */
            continue;
        }
        /* A collision. Both must be tables for this to be resolvable: a
         * gigapage on either side covers the other's mappings entirely. */
        if ((theirs & (PTE_R | PTE_W | PTE_X)) != 0 ||
            (pt->root[i] & (PTE_R | PTE_W | PTE_X)) != 0) {
            return KERR_EXIST;
        }
        {
            const uint64_t *kernel_l1 = kernel->mem.to_virt(kernel->mem.self, pte_phys_of(theirs));
            uint64_t *ours_l1 = pt->mem.to_virt(pt->mem.self, pte_phys_of(pt->root[i]));
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                if (kernel_l1[j] != 0 && ours_l1[j] == 0) {
                    ours_l1[j] = kernel_l1[j];
                } else if (kernel_l1[j] != 0 && ours_l1[j] != 0) {
                    return KERR_EXIST; /* a 2 MiB collision: not resolvable here */
                }
            }
        }
    }
    return KERR_OK;
}

int uspace_build(struct uproc *p, const void *program, uint32_t bytes) {
    struct pt_mem mem = {.alloc = proc_alloc, .free = proc_free, .to_virt = proc_to_virt};
    phys_t text = 0, stack = 0;
    int rc;
    if (bytes > PAGE_SIZE) {
        return KERR_INVAL; /* one page of program is all this builds */
    }
    rc = pgtable_create(&p->pt, &mem);
    rc = rc != KERR_OK ? rc : mem_alloc_pages(1, 1, &text);
    rc = rc != KERR_OK ? rc : mem_alloc_pages(USER_STACK_PAGES, 1, &stack);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        ((volatile uint8_t *)(uintptr_t)text)[i] = ((const uint8_t *)program)[i];
    }
    /* Read, write and execute for the program's page: this is one page of
     * hand-written instructions with no data section, and splitting it would
     * need an ELF to say where the split is. */
    rc = pgtable_map(&p->pt, USER_TEXT, text, PAGE_SIZE, PT_PROT_RWX | PT_PROT_USER);
    /* **And recorded, not only mapped.**
     *
     * The address-space record has to include everything in the address
     * space — the rule this file already states, and the third thing to be
     * caught breaking it. `give_address_space` walks the record and nothing
     * else, so a page that is mapped and unrecorded is a page a forked
     * child does not get: the child of an embedded program resumed with no
     * text at all and took an instruction page fault at its own entry.
     *
     * The ELF path records its segments (see `add_load`), which is why a
     * real program forks correctly and only these hand-written ones did
     * not. Placement reads the same record, so an unrecorded text page is
     * also an address `mmap` believes is free. */
    if (rc == KERR_OK &&
        vma_insert(&p->vmas, USER_TEXT, USER_TEXT + PAGE_SIZE,
                   PROT_READ | PROT_WRITE | PROT_EXEC, 0) != KERR_OK) {
        unrecorded_regions++;
    }
    for (uint32_t i = 0; rc == KERR_OK && i < USER_STACK_PAGES; i++) {
        rc = pgtable_map(&p->pt, USER_STACK_TOP - (uint64_t)(i + 1u) * PAGE_SIZE,
                         stack + (uint64_t)i * PAGE_SIZE, PAGE_SIZE, PT_PROT_RW | PT_PROT_USER);
    }
    /* **And the stack recorded too** — the same rule as the text page above,
     * missed on the same path, and this one was live. Nothing recorded it
     * here, so the 64 pages the program was running on were addresses
     * `mmap` believed were free: the first mapping an embedded program asked
     * for was placed on **the top eight pages of its own stack**, and
     * `uspace_map` drops the pages of the range before it records it,
     * so the stack under the running program was handed back to the
     * allocator. It was found by this section's `mremap` test, which is the
     * first embedded program to both use its stack and call `mmap`; before
     * it, every program did one or the other. The ELF path calls
     * `record_fixed_regions`, which is why real programs were safe.
     *
     * Fourth instance of the rule, and the pattern is now unmistakable: it
     * breaks once per *path* that builds an address space, because each one
     * maps something the others do not. */
    record_stack(p);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = share_kernel_mappings(&p->pt);
    if (rc != KERR_OK) {
        kprintf("uspace: the kernel's map and the process's collide; cannot share\n");
        return rc;
    }
    p->entry = USER_TEXT;
    p->stack_top = USER_STACK_TOP;
    p->built = true;
    return KERR_OK;
}

/* Undoes exactly what `share_kernel_mappings` did, at both levels.
 *
 * This is the mirror image of the sharing, and it has to be, because
 * destroying a process walks its tables and frees every page it finds — so
 * any entry the kernel owns must be gone from the process's tables before
 * the walk starts, or **the kernel's own page tables are handed back to the
 * allocator**.
 *
 * The root level is easy: an entry equal to the kernel's is the kernel's.
 * The level below is the one that was missed. A collision at the root gives
 * the process its *own* level-1 table with the kernel's level-1 entries
 * copied into it — and those entries point at the kernel's level-2 tables.
 * The root entry is not equal to the kernel's (it is the process's own
 * table), so clearing by comparison at the root alone left them in place,
 * and the walk descended through them and freed the kernel's second level.
 *
 * What that looked like: nothing at all, for one process. The freed pages
 * were still mapped and still intact, so the machine carried on. The next
 * process's kernel stack was then allocated out of them and written over —
 * and the kernel's own mappings changed underneath it, so loading that
 * process's `satp` faulted at an address with no console behind it and the
 * machine stopped with no output. One process never showed it; the SMP work
 * added the allocation that landed on it.
 *
 * It disappears when the kernel moves to the top half, where the two halves
 * cannot collide and there is nothing to share.
 */
static void unshare_kernel_mappings(struct pgtable *pt) {
    const struct pgtable *kernel = mmu_kernel_table();
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        uint64_t *ours_l1;
        const uint64_t *kernel_l1;
        if (pt->root[i] == 0 || kernel->root[i] == 0) {
            continue;
        }
        if (pt->root[i] == kernel->root[i]) {
            pt->root[i] = 0; /* shared whole: not ours to free */
            continue;
        }
        /* A resolved collision: our table, carrying copies of the kernel's
         * entries. Clear each copy, leaving only what this process owns. */
        if ((kernel->root[i] & (PTE_R | PTE_W | PTE_X)) != 0) {
            continue; /* a kernel gigapage: never shared into a collision */
        }
        kernel_l1 = kernel->mem.to_virt(kernel->mem.self, pte_phys_of(kernel->root[i]));
        ours_l1 = pt->mem.to_virt(pt->mem.self, pte_phys_of(pt->root[i]));
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (kernel_l1[j] != 0 && ours_l1[j] == kernel_l1[j]) {
                ours_l1[j] = 0;
            }
        }
    }
}

void uspace_destroy(struct uproc *p) {
    if (!p->built) {
        return;
    }
    drop_every_file(p);
    unshare_kernel_mappings(&p->pt);
    pgtable_destroy(&p->pt);
    p->built = false;
}

/* On the way back to user mode: take the preemption the tick asked for.
 *
 * `sched_yield_now` may not come back for a while — another thread runs on
 * its own kernel stack — but when it does, this frame is intact and the
 * trap return carries on, which is the whole point of a kernel stack per
 * thread. */
/* Frames that could not possibly belong to user mode, and how many. */
static uint32_t impossible_frames;

uint32_t uspace_impossible_frames(void) {
    return impossible_frames;
}

/* **A frame that cannot be a user frame**, caught on the way out instead of
 * as a fault afterwards.
 *
 * A user thread's `sepc` and stack pointer are both below `USER_TOP`, always
 * — there is nothing a program can do to make either one a kernel address.
 * So a frame about to be returned through that holds one is not a program's
 * mistake, it is this kernel having restored the wrong registers, and the
 * fault it causes happens later and somewhere else.
 *
 * Measured, once in roughly twenty runs on the board: `store access fault
 * at sepc 0x4020f3fe, stval 0x40000000` with `sp 0x407b7e30` — a PC inside
 * the kernel's own text and a stack pointer in the kernel's identity map,
 * reported as a fault "in user code". By then everything that could say
 * where those values came from is gone. This says it at the moment of
 * restoration, with the thread and the hart that did it, which is the whole
 * difference between a signature and a cause.
 *
 * Reported once and then counted: the frame is left alone, because refusing
 * to return would strand the thread and the program's own fault handling is
 * still the most informative thing that can happen next. */
static void check_the_frame_is_user(const struct trapframe *tf) {
    bool bad_pc = tf->sepc == 0 || tf->sepc >= USER_TOP;
    bool bad_sp = tf->x[2] == 0 || tf->x[2] >= USER_TOP;
    if (!bad_pc && !bad_sp) {
        return;
    }
    if (impossible_frames++ == 0) {
        kprintf("\nuspace: **this frame cannot be a user frame** (pid %u tid %llu):"
                " sepc %016llx sp %016llx sstatus %016llx\n",
                uspace_current() != NULL ? uspace_current()->pid : 0u,
                (unsigned long long)sched_current_tid(), (unsigned long long)tf->sepc,
                (unsigned long long)tf->x[2], (unsigned long long)tf->sstatus);
    }
}

static void on_user_return(struct trapframe *tf) {
    uint32_t fatal = 0;
    check_the_frame_is_user(tf);
    if (sched_take_preempt_request()) {
        sched_yield_now();
    }
    /* A signal raised by something other than a fault — `tgkill`, which is
     * how a runtime preempts a goroutine — is delivered **here**, on the way
     * out, rather than where it was raised. A signal raised inside a syscall
     * must not rewrite the frame that syscall is still using, and the frame
     * is only finished with at this point.
     *
     * A fault's signal is the exception and is delivered where it happens,
     * because the handler must start *instead of* the faulting instruction
     * rather than after it. */
    if (signal_is_fatal(&fatal)) {
        /* A signal nothing handled. It ends the process it was raised
         * against, which may be a child — so it goes through the ordinary
         * exit path rather than stopping the machine. */
        kprintf("\nuspace: pid %u killed by signal %u\n",
                uspace_current() != NULL ? uspace_current()->pid : 0u, fatal);
        uspace_exit(-(int64_t)fatal);
        return;
    }
    (void)signal_deliver(tf);
    /* **On the way back to user mode, say when to be interrupted next.**
     * Every trap can change the answer — a syscall that blocked, a signal
     * that woke something, a thread that ended — and this is the one place
     * every one of them passes through. `timer_arm_at` skips the firmware
     * call when the answer has not changed, which it usually has not. */
    sched_arm_timer();
}

/* A fault on an address that **is** mapped, which means this hart's TLB had
 * not caught up.
 *
 * It is the other half of only flushing locally when a mapping is added: a
 * hart that was not told fences itself here and retries the instruction, and
 * the cost is one spurious fault instead of a machine-wide fence on every
 * `mmap`. The fault is genuinely spurious — the tables say the access is
 * allowed — so retrying is not a guess.
 *
 * `sepc` is deliberately left alone: the faulting instruction runs again.
 */
/* Resolves a store to a page two address spaces share.
 *
 * The page is read-only with `PTE_COW` set, so the store faulted. What
 * happens next depends on how many holders the frame still has:
 *
 *   - **One.** Everyone else has gone, so there is nothing to copy: give the
 *     write bit back and let the store proceed. A process that forks in a
 *     loop would otherwise copy the same page every time.
 *   - **More than one.** A private copy is made, the old frame loses this
 *     holder, and the new one is mapped writable.
 *
 * The mapping is only ever made *more* permissive for this hart, so a local
 * fence is enough: another hart that still has the read-only entry is
 * correct to have it — the page really is read-only for the other process.
 */
static uint32_t cow_copies;
static uint32_t cow_takeovers;

uint32_t uspace_cow_copies(void) {
    return cow_copies;
}

uint32_t uspace_cow_takeovers(void) {
    return cow_takeovers;
}

static bool resolve_copy_on_write(struct trapframe *tf) {
    virt_t va = tf->stval & ~(virt_t)(PAGE_SIZE - 1u);
    /* **The faulting thread's process, not "the" process.**
     *
     * This used the single global that `uspace_run` sets to the first
     * process and never changes, which was right while there was only ever
     * one. With a real `fork` there are two, and the child's first store to
     * a copy-on-write page was resolved against the *parent's* page tables:
     * the parent's entry was made writable, the child's was not, and the
     * child faulted on the same instruction for ever — silently, because a
     * handled fault prints nothing. Isolation starts with knowing whose
     * address space the fault is in. */
    struct uproc *me = uspace_current();
    phys_t phys = 0, fresh = 0;
    uint64_t prot = 0, size = 0;
    if ((tf->scause & ~SCAUSE_INTERRUPT) != EXC_STORE_PAGE_FAULT || me == NULL) {
        return false;
    }
    if (!pgtable_lookup(&me->pt, va, &phys, &prot, &size) || (prot & PTE_COW) == 0) {
        if (cow_copies + cow_takeovers < 6u && syscall_tracing_errors()) {
            kprintf("cow: pid %u fault at %016llx: mapped %u prot %llx\n", me->pid,
                    (unsigned long long)va,
                    pgtable_lookup(&me->pt, va, &phys, &prot, &size) ? 1u : 0u,
                    (unsigned long long)prot);
        }
        return false;
    }
    if (cow_copies + cow_takeovers < 6u && syscall_tracing_errors()) {
        kprintf("cow: pid %u copying %016llx (%u holders)\n", me->pid, (unsigned long long)va,
                mem_page_shares(phys & ~(phys_t)(PAGE_SIZE - 1u)));
    }
    phys &= ~(phys_t)(PAGE_SIZE - 1u);
    if (mem_page_shares(phys) <= 1u) {
        (void)pgtable_unmap(&me->pt, va);
        if (pgtable_map(&me->pt, va, phys, PAGE_SIZE, (prot & ~PTE_COW) | PTE_W) !=
            KERR_OK) {
            return false;
        }
        cow_takeovers++;
        mmu_flush_tlb_local();
        return true;
    }
    if (mem_alloc_pages(1, 1, &fresh) != KERR_OK) {
        return false;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)fresh)[i] = ((const volatile uint8_t *)(uintptr_t)phys)[i];
    }
    (void)pgtable_unmap(&me->pt, va);
    if (pgtable_map(&me->pt, va, fresh, PAGE_SIZE, (prot & ~PTE_COW) | PTE_W) != KERR_OK) {
        (void)mem_free_pages(fresh, 1);
        return false;
    }
    (void)mem_free_pages(phys, 1); /* one holder fewer, not necessarily freed */
    cow_copies++;
    mmu_flush_tlb_local();
    return true;
}

/* **Demand paging**: a fault inside a region the process was given, on a
 * page that has no frame yet, is backed here rather than at `mmap`.
 *
 * Why it needs the region record and not the page tables: an unbacked page
 * and an address nobody ever asked for are indistinguishable to the page
 * tables, and backing the second would turn a wild pointer into a silently
 * working one — the opposite of what a fault is for. `vma_find` answers
 * the question the tables cannot.
 *
 * A `PROT_NONE` region stays unbacked on purpose: it is a reservation, and
 * touching it is a fault the program has earned. So is a write to a region
 * that is only readable — the rights come from the region, which is the
 * whole reason they are recorded with it.
 *
 * What this buys is the thing the roadmap asked for: a mapping now costs
 * its pages when they are *touched*, so a runtime that maps a large arena
 * and uses a corner of it pays for the corner. What it costs is a fault per
 * first touch, which is why `MAP_POPULATE` exists for callers that would
 * rather pay up front.
 */
static bool back_on_demand(struct trapframe *tf) {
    uint64_t code = tf->scause & ~SCAUSE_INTERRUPT;
    struct uproc *me = uspace_current();
    const struct vma *region;
    virt_t page;
    phys_t phys = 0;
    uint64_t flags = 0, size = 0;
    uint32_t want;
    if (code != EXC_INSN_PAGE_FAULT && code != EXC_LOAD_PAGE_FAULT &&
        code != EXC_STORE_PAGE_FAULT) {
        return false;
    }
    if (me == NULL) {
        return false;
    }
    page = tf->stval & ~(virt_t)(PAGE_SIZE - 1u);
    /* Already mapped: this is somebody else's fault to handle — a
     * copy-on-write store, or a stale translation. */
    if (pgtable_lookup(&me->pt, page, &phys, &flags, &size)) {
        return false;
    }
    region = vma_find(&me->vmas, tf->stval);
    if (region == NULL || region->prot == 0) {
        return false; /* not ours, or a reservation that must keep faulting */
    }
    want = code == EXC_INSN_PAGE_FAULT ? PROT_EXEC
                                       : (code == EXC_STORE_PAGE_FAULT ? PROT_WRITE : PROT_READ);
    if ((region->prot & want) == 0) {
        return false; /* the region does not allow what was attempted */
    }
    if (back_for_region(me, region, page) != KERR_OK) {
        return false; /* out of memory: a fault the process must be told about */
    }
    demand_pages++;
    published_addition();
    return true;
}

static bool retry_after_stale_translation(struct trapframe *tf) {
    uint64_t code = tf->scause & ~SCAUSE_INTERRUPT;
    uint64_t want;
    phys_t phys = 0;
    uint64_t flags = 0, size = 0;
    if (code == EXC_INSN_PAGE_FAULT) {
        want = PTE_X;
    } else if (code == EXC_LOAD_PAGE_FAULT) {
        want = PTE_R;
    } else if (code == EXC_STORE_PAGE_FAULT) {
        want = PTE_W;
    } else {
        return false;
    }
    /* The faulting thread's process, for the same reason the copy-on-write
     * handler needs it: a stale translation belongs to one address space. */
    {
        struct uproc *me = uspace_current();
        if (me == NULL || !pgtable_lookup(&me->pt, tf->stval, &phys, &flags, &size)) {
            return false;
        }
    }
    /* Mapped, for user mode, with the right to do what was attempted — so
     * the tables and the hardware disagree, and the hardware is the one
     * holding something out of date. */
    if ((flags & want) == 0 || (flags & PTE_U) == 0) {
        return false;
    }
    stale_faults++;
    mmu_flush_tlb_local();
    return true;
}

/* A fault the program has asked to be told about.
 *
 * This is what makes a nil pointer dereference a **recoverable panic** in Go
 * rather than a dead process: the runtime installs a SIGSEGV handler, reads
 * the faulting address out of the frame this builds, and raises a Go panic.
 * Without it the same program dies, and the difference shows up in ordinary
 * code — any `recover` around a nil dereference.
 *
 * `SEGV_MAPERR` means "nothing is mapped there", which is exactly what a
 * nil dereference is, and a runtime treats that code differently from an
 * access violation. */
static bool fault_becomes_a_signal(struct trapframe *tf) {
    uint64_t code = tf->scause & ~SCAUSE_INTERRUPT;
    uint32_t signo;
    int32_t si_code;
    switch (code) {
    case EXC_INSN_PAGE_FAULT:
    case EXC_LOAD_PAGE_FAULT:
    case EXC_STORE_PAGE_FAULT:
        signo = SIGSEGV;
        si_code = SEGV_MAPERR;
        break;
    case EXC_LOAD_MISALIGNED:
    case EXC_STORE_MISALIGNED:
        signo = SIGBUS;
        si_code = BUS_ADRALN;
        break;
    case EXC_ILLEGAL_INSN:
        signo = SIGILL;
        si_code = ILL_ILLOPC;
        break;
    default:
        return false;
    }
    signal_raise_fault(signo, tf->stval, si_code);
    /* Delivered here rather than on the way out, because the frame this
     * rewrites is the faulting one: the handler must start instead of the
     * instruction that faulted, not after it. */
    return signal_deliver(tf);
}

/* Faults the kernel resolved and returned from.
 *
 * **A handled fault prints nothing**, which is right when there are a few of
 * them and hides a disaster when there are millions: a handler that resolves
 * a fault without actually changing anything leaves the instruction to fault
 * again, for ever, in complete silence. Counting them is how that looks like
 * something. */
static uint32_t handled_faults;

uint32_t uspace_handled_faults(void) {
    return handled_faults;
}

static void trace_handled_fault(const struct trapframe *tf, const char *how) {
    /* The first few only: a loop announces itself in ten lines, and ten
     * thousand would be the same disaster with the evidence scrolled off. */
    if (handled_faults > 12u || !syscall_tracing_errors()) {
        return;
    }
    kprintf("fault: %s at sepc %016llx, stval %016llx (%s)\n", trap_cause_name(tf->scause),
            (unsigned long long)tf->sepc, (unsigned long long)tf->stval, how);
}

static bool on_user_trap(struct trapframe *tf) {
    if ((tf->scause & SCAUSE_INTERRUPT) != 0) {
        return false; /* an interrupt: the trap layer deals with those */
    }
    if ((tf->scause & ~SCAUSE_INTERRUPT) == EXC_ECALL_FROM_U) {
        bool step = syscall_steps_over_ecall(tf);
        (void)syscall_dispatch(tf);
        /* Past the `ecall`, or the process makes the same call for ever.
         * `ecall` is always 4 bytes, unlike most of this ISA.
         *
         * Asked **before** the call, because `rt_sigreturn` replaces the
         * whole register set — including the syscall number this would be
         * read from afterwards. */
        tf->sepc += step ? 4u : 0u;
        return true;
    }
    /* **Before the stale-translation retry**, because a copy-on-write page
     * is mapped and readable, so that check would see a mapping with the
     * rights the access wanted and retry for ever. */
    if (resolve_copy_on_write(tf)) {
        handled_faults++;
        trace_handled_fault(tf, "copy-on-write");
        return true;
    }
    if (back_on_demand(tf)) {
        handled_faults++;
        trace_handled_fault(tf, "demand paging");
        return true;
    }
    if (retry_after_stale_translation(tf)) {
        handled_faults++;
        trace_handled_fault(tf, "stale translation");
        return true;
    }
    if (fault_becomes_a_signal(tf)) {
        return true;
    }
    /* A fault in user code is the process's problem, not the kernel's: it
     * stops, and the kernel carries on. Reporting it this way — rather than
     * panicking — is the whole difference between a user fault and a kernel
     * one. */
    /* **Which thread**, not just which process. With one thread the pid
     * said everything; with four harts running four of them, "a fault
     * happened somewhere in this program" is not something anyone can act
     * on — and the thread's own stack is usually the first thing to compare
     * the faulting address against. */
    kprintf("\nuspace: fault in user code (pid %u tid %llu, sp %016llx): %s at sepc %016llx, "
            "stval %016llx\n",
            uspace_current() != NULL ? uspace_current()->pid : 0u,
            (unsigned long long)sched_current_tid(), (unsigned long long)tf->x[2],
            trap_cause_name(tf->scause), (unsigned long long)tf->sepc,
            (unsigned long long)tf->stval);
    /* The registers, when asked for. A faulting address alone says where the
     * program stopped but not what it was holding, and the difference
     * between "a pointer was null" and "a pointer was wrong" is the whole
     * diagnosis. */
    if (syscall_tracing_errors()) {
        trap_dump(tf);
    }
    uspace_exit(KERR_FAULT);
    return true;
}

/* Enters user mode, and resumes here when the process stops.
 *
 * `uspace_enter` does not return — `sret` goes to user mode, and the kernel
 * is next entered through the trap vector with no memory of this function.
 * What comes back here is `uspace_leave`, called from the exit syscall,
 * which restores this function's own registers and stack: control reappears
 * on the line after the call, as if the process had returned. That is the
 * honest shape of it — the process did not return, the kernel resumed.
 *
 * Runs to 26 lines: one ordered handover (install the address space, go,
 * unwind), and splitting it would put the `satp` switch and the switch back
 * in different functions (Rule 11, Locking/lifetime). */
/* The kernel lock's two counters, when tracing.
 *
 * They are the one thing worth printing between processes: a ticket lock
 * whose `next` and `serving` have drifted apart will stop the machine the
 * next time a hart asks for it, and the gap says which side lost count. It
 * found the bug it exists for — one acquisition against two releases,
 * because the lock had been wired into the trap path after a program had
 * already run, so every syscall took the "no lock configured" path while
 * the scheduler still released one. */
static void report_lock_state(void) {
    uint32_t next = 0, serving = 0;
    uint64_t owner = 0;
    if (!sched_tracing()) {
        return;
    }
    klock_state(&next, &serving, &owner);
    kprintf("klock: next %u serving %u owner %llu%s\n", next, serving,
            (unsigned long long)owner, next == serving ? "" : " — THESE MUST MATCH");
}

int64_t uspace_run(struct uproc *p) {
    if (!p->built) {
        return KERR_INVAL;
    }
    struct trapframe first = {0};
    int rc;
    running = p;
    exited = false;
    exit_status = 0;
    trap_set_expected(on_user_trap);
    trap_set_user_return_hook(on_user_return);
    calls_at_start = syscall_count();
    __asm__ volatile("csrr %0, satp" : "=r"(kernel_satp_saved));
    /* The first thread's register set. `sstatus` carries SUM, because every
     * frame restored from here onwards inherits it — including a cloned
     * thread's, which is copied from its parent's. */
    first.sepc = p->entry;
    first.sstatus = SSTATUS_SUM;
    building = p;
    signal_reset();
    fd_reset();
    /* Descriptors 0, 1 and 2 before the program starts: a runtime checks
     * all three before it does anything else and aborts if one is missing. */
    if (fdobjects_open_console() != KERR_OK) {
        running = NULL;
        return KERR_NOMEM;
    }
    rc = sched_init(p, &first, p->stack_top, &p->main_tid);
    if (rc != KERR_OK) {
        running = NULL;
        return rc;
    }
    report_lock_state();
    (void)timer_add_tick_hook(sched_tick);
    /* Secondary harts may now join. They were waiting for exactly this,
     * because a hart with no process has nothing to claim. */
    sched_set_accepting(true);
    uspace_adopt_address_space(p);
    sched_run();
    /* **Stop accepting before waiting**, not after. A secondary hart that
     * sees `accepting` still set re-enters the scheduling loop, marks itself
     * joined, and leaves again the moment it reads `process_over` — so a
     * boot hart waiting for the count to reach zero can watch it bounce off
     * zero for ever. That is a livelock, and it is what a process ending
     * actually did until this line moved. */
    sched_set_accepting(false);
    /* Now wait for them to leave, before anything they might still be
     * standing on is freed — a kernel stack, or the page tables their own
     * `satp` still points at. */
    {
        /* Unbounded, because there is nothing better to do than wait — but
         * **not silent**. A hart that never leaves used to stop the machine
         * with no output of any kind, which is indistinguishable from a
         * board that has died; the report names it instead. The threshold is
         * in loop iterations rather than time because the clock is one of
         * the things that may have stopped. */
        uint64_t spins = 0;
        bool said = false;
        while (sched_joined_harts() > 0) {
            if (++spins > 200000000ull && !said) {
                said = true;
                kprintf("\nuspace: **harts have not left the scheduler**\n");
                sched_report_joined();
            }
        }
    }
    uspace_release_address_space();
    trap_set_expected(NULL);
    trap_set_user_return_hook(NULL);
    timer_remove_tick_hook(sched_tick);
    sched_shutdown();
    building = p;
    fd_reset();
    building = NULL;
    running = NULL;
    return exit_status;
}

/* The permissions a segment's flags ask for, as page-table bits. A segment
 * is always readable from user mode — a loadable segment the process cannot
 * read is not a thing a linker emits — and write and execute follow the
 * flags. */
static uint64_t prot_bits(uint32_t prot) {
    return PTE_R | PTE_U | ((prot & PROT_WRITE) != 0 ? PTE_W : 0u) |
           ((prot & PROT_EXEC) != 0 ? PTE_X : 0u);
}

/* Maps and fills one segment, a page at a time.
 *
 * Page at a time because a segment's file bytes and its memory size differ:
 * the tail beyond `filesz` must read as zero, which is where a `.bss` comes
 * from. Allocating per page and zeroing it first makes that automatic
 * rather than a separate step that can be forgotten.
 *
 * Segments are **page-aligned down** on the way in: a linker is entitled to
 * start one part-way into a page, and two segments may share a page at
 * their boundary — so a page that is already mapped is filled rather than
 * mapped again. */
static int load_segment(struct uproc *p, const struct elf_load *l, const uint8_t *image) {
    uint64_t start = l->vaddr & ~(PAGE_SIZE - 1u);
    uint64_t end = (l->vaddr + l->memsz + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    /* The program's own segments belong in the address space's record like
     * everything else. They are below the window `mmap` places into, so
     * nothing would be put on top of them — but a record that is only
     * *mostly* complete cannot be walked, and `fork` has to walk it to copy
     * an address space. Overlap at a page shared by two segments is
     * expected and is not an error here. */
    (void)vma_insert(&p->vmas, start, end, l->prot, 0);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        phys_t page = 0;
        uint64_t have = 0, want = 0, size = 0;
        int rc;
        if (pgtable_lookup(&p->pt, va, &page, &have, &size)) {
            /* A shared page at a segment boundary: widen its permissions to
             * the union of both segments', because one page cannot be both
             * read-only and writable and the linker put them together. */
            (void)pgtable_unmap(&p->pt, va);
            rc = pgtable_map(&p->pt, va, page, PAGE_SIZE, have | prot_bits(l->prot));
        } else {
            rc = mem_alloc_pages(1, 1, &page);
            if (rc == KERR_OK) {
                for (uint32_t i = 0; i < PAGE_SIZE; i++) {
                    ((volatile uint8_t *)(uintptr_t)page)[i] = 0;
                }
                rc = pgtable_map(&p->pt, va, page, PAGE_SIZE, prot_bits(l->prot));
            }
        }
        if (rc != KERR_OK) {
            return rc;
        }
        /* Copy whatever of the file belongs on this page. */
        for (uint64_t i = 0; i < PAGE_SIZE; i++) {
            uint64_t at = va + i;
            if (at >= l->vaddr && at < l->vaddr + l->filesz) {
                ((volatile uint8_t *)(uintptr_t)page)[i] = image[l->offset + (at - l->vaddr)];
            }
        }
        (void)want;
    }
    return KERR_OK;
}

/* The auxiliary vector a Go runtime reads. AT_PAGESZ and AT_RANDOM are the
 * two it cannot do without; the rest describe the image it was loaded from.
 *
 * **No AT_HWCAP bit for thread-local storage**, unlike the architecture this
 * migrated from, where a C runtime checked `HWCAP_TLS` to decide whether to
 * read the thread pointer from a register or from a helper page the kernel
 * had to map. Here `tp` is in the ABI, so there is nothing to advertise and
 * no helper page to provide. */
static uint32_t build_auxv(const struct elf_image *img, struct auxv_pair *out) {
    uint32_t n = 0;
    out[n++] = (struct auxv_pair){AT_PAGESZ, PAGE_SIZE};
    out[n++] = (struct auxv_pair){AT_PHDR, img->phdr_vaddr};
    out[n++] = (struct auxv_pair){AT_PHENT, img->phent};
    out[n++] = (struct auxv_pair){AT_PHNUM, img->phnum};
    out[n++] = (struct auxv_pair){AT_ENTRY, img->entry};
    out[n++] = (struct auxv_pair){AT_UID, 0};
    out[n++] = (struct auxv_pair){AT_EUID, 0};
    out[n++] = (struct auxv_pair){AT_GID, 0};
    out[n++] = (struct auxv_pair){AT_EGID, 0};
    out[n++] = (struct auxv_pair){AT_CLKTCK, 100};
    out[n++] = (struct auxv_pair){AT_HWCAP, 0};
    out[n++] = (struct auxv_pair){AT_HWCAP2, 0};
    return n;
}

/* Maps the kernel's signal-return trampoline into the process.
 *
 * Two instructions, `li a7, 139; ecall`, at a fixed address, read-execute
 * and reachable from user mode. It exists because a handler is an ordinary
 * function that returns, and on this architecture there is no address in the
 * request to return through: a runtime sets the `SA_RESTORER` flag but
 * leaves `sa_restorer` zero. Linux maps the same thing in its VDSO.
 *
 * Right below the stack, so it is nowhere near anything a program places
 * itself. */
static int map_signal_trampoline(struct uproc *p) {
    uint32_t bytes = 0;
    const uint8_t *code = signal_trampoline_code(&bytes);
    phys_t page = 0;
    int rc = mem_alloc_pages(1, 1, &page);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)page)[i] = i < bytes ? code[i] : 0u;
    }
    return pgtable_map(&p->pt, SIGNAL_TRAMPOLINE, page, PAGE_SIZE, PTE_R | PTE_X | PTE_U);
}

static void record_fixed_regions(struct uproc *p);


uint64_t uspace_demand_pages(void) {
    return demand_pages;
}

uint32_t uspace_unrecorded_regions(void) {
    return unrecorded_regions;
}

/* The extra environment variable, copied because the command line it comes
 * from is not kept. */
static char extra_env[128];

void uspace_set_extra_env(const char *text, uint32_t len) {
    uint32_t i = 0;
    for (; text != NULL && i < len && i + 1u < sizeof extra_env; i++) {
        extra_env[i] = text[i];
    }
    extra_env[i] = '\0';
}

/* Builds the stack: the block is assembled in the page that will be the top
 * of the stack, then mapped. Assembling it in place would need the process's
 * address space to be loaded, which it is not yet. */
static int build_start_state(struct uproc *p, const struct elf_image *img, const char *path,
                             const char *const *argv, uint32_t argc) {
    struct auxv_pair auxv[USTACK_MAX_AUXV];
    const char *envp[3] = {"HOME=/", "PATH=/", extra_env};
    struct ustack_args args = {.argv = argv,
                               .argc = argc,
                               .envp = envp,
                               .envc = extra_env[0] != '\0' ? 3u : 2u,
                               .auxv = auxv,
                               .execfn = path};
    phys_t stack = 0;
    virt_t sp = 0;
    uint32_t used = 0;
    int rc;
    args.nauxv = build_auxv(img, auxv);
    random_fill(args.random, sizeof args.random);
    rc = mem_alloc_pages(USER_STACK_PAGES, 1, &stack);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < USER_STACK_PAGES * PAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)stack)[i] = 0;
    }
    /* The block lands at the very top of the stack, so it is built into the
     * last page's worth of the allocation. */
    record_fixed_regions(p);
    rc = ustack_build((uint8_t *)(uintptr_t)stack, USER_STACK_PAGES * PAGE_SIZE, USER_STACK_TOP,
                      &args, &sp, &used);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; rc == KERR_OK && i < USER_STACK_PAGES; i++) {
        rc = pgtable_map(&p->pt, USER_STACK_TOP - (uint64_t)(i + 1u) * PAGE_SIZE,
                         stack + (uint64_t)(USER_STACK_PAGES - 1u - i) * PAGE_SIZE, PAGE_SIZE,
                         PT_PROT_RW | PT_PROT_USER);
    }
    p->stack_top = sp;
    return rc;
}

int uspace_build_elf(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path) {
    const char *argv[1] = {path};
    return uspace_build_elf_args(p, image, bytes, path, argv, 1u);
}

int uspace_build_elf_args(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path,
                          const char *const *argv, uint32_t argc) {
    struct pt_mem mem = {.alloc = proc_alloc, .free = proc_free, .to_virt = proc_to_virt};
    struct elf_image img;
    int rc;
    /* Only what **this function owns** is cleared: the address space and the
     * things derived from it. Zeroing the whole process here wiped its
     * identity as well — `used` and `pid` — so the next `uspace_alloc`
     * handed the same slot out again and the parent's descriptor table
     * disappeared the moment it forked. The symptom was a child whose every
     * descriptor operation failed, which looked like the child's table never
     * having been copied rather than the parent's having been deleted. */
    p->pt = (struct pgtable){0};
    p->entry = 0;
    p->stack_top = 0;
    p->brk = 0;
    p->brk_floor = 0;
    p->mmap_next = 0;
    p->main_tid = 0;
    p->built = false;
    p->shares_pt = false;
    /* A fresh address space has nothing in it. A process slot is reused, so
     * the previous occupant's regions would otherwise still be recorded and
     * placement would refuse addresses that are free — and a region that
     * was reading from a file is a reference this process stops holding. */
    drop_every_file(p);
    p->vmas.n = 0;
    rc = elf_parse(image, bytes, USER_TOP, &img);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = pgtable_create(&p->pt, &mem);
    for (uint32_t i = 0; rc == KERR_OK && i < img.nloads; i++) {
        rc = load_segment(p, &img.loads[i], image);
    }
    rc = rc != KERR_OK ? rc : build_start_state(p, &img, path, argv, argc);
    rc = rc != KERR_OK ? rc : map_signal_trampoline(p);
    rc = rc != KERR_OK ? rc : share_kernel_mappings(&p->pt);
    if (rc != KERR_OK) {
        return rc;
    }
    p->entry = img.entry;
    p->built = true;
    /* The break starts above the last segment, page-aligned, and anonymous
     * mappings start far above it so that neither can grow into the other.
     * A program that uses `brk` and `mmap` both, as a Go runtime does, is
     * entitled to assume they are not the same region. */
    for (uint32_t i = 0; i < img.nloads; i++) {
        virt_t end = img.loads[i].vaddr + img.loads[i].memsz;
        p->brk = end > p->brk ? end : p->brk;
    }
    p->brk = (p->brk + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    p->brk_floor = p->brk;
    p->mmap_next = USER_MMAP_BASE;
    return KERR_OK;
}

void kfree_image(void *image) {
    kfree(image);
}

int uspace_fork_shared(struct uproc *child, struct uproc *parent) {
    uint32_t pid = child->pid;
    /* Everything of the parent's except its identity and its threads. The
     * page tables are **shared, not copied**: that is what `vfork` is, and
     * it is why the parent must then be suspended. */
    child->pt = parent->pt;
    child->shares_pt = true;
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->brk = parent->brk;
    child->brk_floor = parent->brk_floor;
    child->mmap_next = parent->mmap_next;
    child->built = true;
    child->pid = pid;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->rlim_nofile_soft = parent->rlim_nofile_soft;
    child->rlim_nofile_hard = parent->rlim_nofile_hard;
    child->rlim_nproc_soft = parent->rlim_nproc_soft;
    child->rlim_nproc_hard = parent->rlim_nproc_hard;
    for (uint32_t i = 0; i < SIG_COUNT; i++) {
        child->actions[i] = parent->actions[i];
    }
    return fd_copy_table(child, parent);
}

uint32_t uspace_pid_of(const struct uproc *p) {
    return p == NULL ? 0u : p->pid;
}

uint32_t uspace_pgid_of(const struct uproc *p) {
    return p == NULL ? 0u : p->pgid;
}

uint64_t uspace_rlimit_nofile(void) {
    const struct uproc *p = uspace_current();
    return p == NULL || p->rlim_nofile_soft == 0 ? ~0ull : p->rlim_nofile_soft;
}

uint64_t uspace_rlimit_nproc(void) {
    const struct uproc *p = uspace_current();
    return p == NULL || p->rlim_nproc_soft == 0 ? ~0ull : p->rlim_nproc_soft;
}

uint32_t uspace_count_by_uid(uint32_t uid) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        n += procs[i].used && !procs[i].zombie && procs[i].uid == uid ? 1u : 0u;
    }
    return n;
}

uint32_t uspace_euid(void) {
    const struct uproc *p = uspace_current();
    return p == NULL ? 0u : p->euid;
}

uint32_t uspace_egid(void) {
    const struct uproc *p = uspace_current();
    return p == NULL ? 0u : p->egid;
}

bool uspace_may_access(uint32_t file_uid, uint32_t file_gid, uint32_t mode, uint32_t want) {
    /* The rule itself is pure and host-tested in `core/access.c`; what this
     * layer knows is who is asking. */
    return access_permitted(uspace_euid(), uspace_egid(), file_uid, file_gid, mode, want);
}

/* Whether this process has claimed this signal for itself. */
static bool handler_installed(const struct uproc *p, uint32_t signo) {
    uint64_t h = p->actions[signo].sa_handler;
    return h != SIG_DFL && h != SIG_IGN;
}

/* Whether a signal **stops** a process when nothing has claimed it. */
static bool stops_by_default(uint32_t signo) {
    return signo == SIGTSTP || signo == SIGTTIN || signo == SIGTTOU;
}

void uspace_deliver_signal(struct uproc *p, uint32_t signo) {
    if (signo == SIGCONT) {
        (void)sched_continue_process(p->pid);
        return;
    }
    if (signo == SIGSTOP || (stops_by_default(signo) && !handler_installed(p, signo))) {
        (void)sched_stop_process(p->pid);
        return;
    }
    (void)sched_raise_on_process(p->pid, signo);
}

static void raise_one(struct uproc *p, uint32_t signo) {
    uspace_deliver_signal(p, signo);
}

uint32_t uspace_signal_group(uint32_t pgid, uint32_t signo) {
    return uspace_for_each_in_group(pgid, raise_one, signo);
}

uint32_t uspace_for_each_in_group(uint32_t pgid, void (*fn)(struct uproc *p, uint32_t arg),
                                  uint32_t arg) {
    uint32_t n = 0;
    if (fn == NULL || pgid == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].used && !procs[i].zombie && procs[i].pgid == pgid) {
            fn(&procs[i], arg);
            n++;
        }
    }
    return n;
}

uint32_t uspace_count_children(uint32_t parent_pid) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        n += procs[i].used && procs[i].parent_pid == parent_pid ? 1u : 0u;
    }
    return n;
}

void uspace_wait_for_vfork(struct uproc *child) {
    /* Waits on the child's own address, which whatever ends the borrowing
     * wakes: `uspace_exec` when it gives the child its own tables, and the
     * exit path when it does not get that far. A deadline so that a child
     * which somehow does neither costs a stall rather than the machine. */
    while (child->used && child->shares_pt && !child->zombie) {
        (void)sched_wait_on(child, timer_ns() + 1000000000ull);
    }
}

void uspace_wait_for_child(struct uproc *parent) {
    (void)sched_wait_on(parent, timer_ns() + 1000000000ull);
}

/* Where an `execve` assembles the new address space before adopting it.
 *
 * Static, not a local: a `struct uproc` is two and a half kilobytes and the
 * frame-size guard refuses that on a stack — twice now, which is the guard
 * earning its place. One is enough because an `execve` holds the kernel lock
 * from beginning to end and never sleeps, so two cannot overlap. */
static struct uproc exec_scratch;

/* Frees a set of page tables that belong to nobody, which is what the old
 * address space becomes once a new one has been adopted.
 *
 * The holder is static for the same reason the scratch is: a `struct uproc`
 * on a stack is two and a half kilobytes, which the frame guard refuses, and
 * an `execve` is serialised by the kernel lock. */
static struct uproc free_holder;

static void free_tables(struct pgtable *pt) {
    free_holder = (struct uproc){.built = true};
    free_holder.pt = *pt;
    uspace_destroy(&free_holder);
}

int uspace_exec(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path,
                const char *const *argv, uint32_t argc) {
    struct pgtable old = p->pt;
    bool had_own = !p->shares_pt;
    struct uproc *fresh = &exec_scratch;
    int rc;
    /* The new address space is built **beside** the old one, so a failure
     * here leaves the process exactly as it was. A program that handles a
     * failing `execve` is entitled to carry on. */
    /* `execve` replaces the image and keeps the identity — pid, parent,
     * group and session all survive it, which is what lets a shell put a
     * job in a group *before* exec'ing into it. */
    *fresh = (struct uproc){.pid = p->pid,
                            .parent_pid = p->parent_pid,
                            .pgid = p->pgid,
                            .sid = p->sid,
                            .uid = p->uid,
                            .gid = p->gid,
                            .euid = p->euid,
                            .egid = p->egid,
                            .rlim_nofile_soft = p->rlim_nofile_soft,
                            .rlim_nofile_hard = p->rlim_nofile_hard,
                            .rlim_nproc_soft = p->rlim_nproc_soft,
                            .rlim_nproc_hard = p->rlim_nproc_hard,
                            .used = true};
    rc = uspace_build_elf_args(fresh, image, bytes, path, argv, argc);
    if (rc != KERR_OK) {
        return rc;
    }
    /* Now the swap. The descriptors stay, minus close-on-exec; the signal
     * handlers go, because they pointed into code that no longer exists;
     * the thread's own signal mask stays, which is what the ABI says. */
    p->pt = fresh->pt;
    p->entry = fresh->entry;
    p->stack_top = fresh->stack_top;
    p->brk = fresh->brk;
    p->brk_floor = fresh->brk_floor;
    p->mmap_next = fresh->mmap_next;
    p->built = true;
    p->shares_pt = false;
    fd_close_on_exec();
    signal_reset();
    /* The old tables are freed only if they were this process's. A `vfork`
     * child's were its parent's, and freeing them would take the parent's
     * program with it — which is the whole reason `shares_pt` exists. */
    if (had_own) {
        free_tables(&old);
    }
    /* Whoever was waiting for this child to stop borrowing can go. */
    (void)sched_wake_on(p, SCHED_MAX_THREADS);
    /* This hart's loaded address space is now stale. */
    uspace_adopt_address_space(p);
    return KERR_OK;
}

void uspace_restart_thread(struct trapframe *tf, const struct uproc *p) {
    /* A fresh register set: an execed program must not inherit anything
     * from what it replaced. Only `sstatus` is carried over, because it
     * holds the mode and the user-access bits rather than any of the old
     * program's state. */
    uint64_t sstatus = tf->sstatus;
    for (uint32_t i = 0; i < 32u; i++) {
        tf->x[i] = 0;
    }
    tf->sstatus = sstatus;
    tf->sepc = p->entry;
    tf->x[2] = p->stack_top;
}

uint64_t uspace_stale_faults(void) {
    return stale_faults;
}

uint64_t uspace_syscalls(void) {
    /* **This process's** calls, not the kernel's total. The table counts
     * cumulatively, which is what a kernel wants; a process's own count is
     * the difference since it started, and conflating the two makes the
     * second process on a machine report the first one's work as well. */
    return syscall_count() - calls_at_start;
}

/* Whether every page of a range carries the given rights in the running
 * process's table. A zero-length range is vacuously fine, which matters
 * because a caller may legitimately pass one. */
/* Makes a page present if the process is entitled to it.
 *
 * **The interaction demand paging creates, and the reason it is here.** The
 * kernel validates a user pointer by walking the process's page tables,
 * which is the only honest test of "may this be touched". Once mappings are
 * backed lazily, a buffer the program has not written to yet has no entry —
 * so a perfectly valid pointer reads as `EFAULT`, and the first thing that
 * happened was a socket read failing with "bad address" into a freshly
 * allocated buffer.
 *
 * So the validator does what the fault handler would: if the address is
 * inside a region with the rights being asked for, back it now. That makes
 * "may the process touch this" and "will a touch succeed" the same
 * question, which is what every caller already assumed.
 *
 * It backs rather than merely permitting, because the kernel is about to
 * write through the pointer itself and a kernel fault is a panic. A program
 * touching the page would have taken an ordinary fault; the kernel cannot.
 */
static bool make_present(struct uproc *p, virt_t va, uint64_t want) {
    const struct vma *region = vma_find(&p->vmas, va);
    uint32_t need;
    if (region == NULL || region->prot == 0) {
        return false;
    }
    /* The page-table bit asked for, as the region's rights spell it. */
    need = (want & PTE_W) != 0 ? PROT_WRITE : ((want & PTE_X) != 0 ? PROT_EXEC : PROT_READ);
    if ((region->prot & need) == 0) {
        return false;
    }
    if (back_for_region(p, region, va) != KERR_OK) {
        return false;
    }
    demand_pages++;
    published_addition();
    return true;
}

static bool range_has(virt_t at, uint32_t n, uint64_t want) {
    /* **The calling process's** tables, not the root program's. They were
     * the same thing while there was one process; with a child they are not,
     * and checking the wrong ones would either refuse a valid pointer or —
     * worse — accept one that is only valid in another address space. */
    const struct uproc *p = uspace_current();
    if (n == 0) {
        return true;
    }
    if (p == NULL || at == 0 || at >= USER_TOP || n > USER_TOP - at) {
        return false;
    }
    for (virt_t va = at & ~(PAGE_SIZE - 1u); va < at + n; va += PAGE_SIZE) {
        phys_t phys = 0;
        uint64_t flags = 0, size = 0;
        if (!pgtable_lookup(&p->pt, va, &phys, &flags, &size)) {
            /* Not mapped: either a page this process is entitled to and has
             * not touched, or an address it has no claim on. The region
             * record is what tells them apart. */
            if (!make_present((struct uproc *)p, va, want)) {
                return false;
            }
            if (!pgtable_lookup(&p->pt, va, &phys, &flags, &size)) {
                return false;
            }
        }
        if ((flags & want) != want || (flags & PTE_U) == 0) {
            return false;
        }
    }
    return true;
}

bool uspace_can_read(virt_t at, uint32_t n) {
    return range_has(at, n, PTE_R);
}

bool uspace_can_write(virt_t at, uint32_t n) {
    return range_has(at, n, PTE_W);
}

int uspace_copy_in(void *to, virt_t from, uint32_t n) {
    if (!uspace_can_read(from, n)) {
        return KERR_FAULT;
    }
    for (uint32_t i = 0; i < n; i++) {
        ((uint8_t *)to)[i] = ((const volatile uint8_t *)(uintptr_t)from)[i];
    }
    return KERR_OK;
}

int uspace_copy_out(virt_t to, const void *from, uint32_t n) {
    if (!uspace_can_write(to, n)) {
        return KERR_FAULT;
    }
    for (uint32_t i = 0; i < n; i++) {
        ((volatile uint8_t *)(uintptr_t)to)[i] = ((const uint8_t *)from)[i];
    }
    return KERR_OK;
}

struct uproc *uspace_alloc(void) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (!procs[i].used) {
            procs[i] = (struct uproc){.used = true, .pid = next_pid};
            /* **Its own group and session leader**, which is what makes the
             * first process one without a special case: a group of one is
             * still a group, and `setpgid`/`setsid` then move it the same
             * way they move anything else. A fork inherits both. */
            procs[i].pgid = next_pid;
            procs[i].sid = next_pid;
            next_pid++;
            return &procs[i];
        }
    }
    return NULL;
}

void uspace_release(struct uproc *p) {
    if (p != NULL) {
        *p = (struct uproc){0};
    }
}

struct uproc *uspace_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].used && procs[i].pid == pid) {
            return &procs[i];
        }
    }
    return NULL;
}

struct uproc *uspace_reapable_child(uint32_t parent_pid) {
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (procs[i].used && procs[i].zombie && procs[i].parent_pid == parent_pid) {
            return &procs[i];
        }
    }
    return NULL;
}

struct uproc *uspace_current(void) {
    /* The running thread's process, which is the honest answer once there is
     * more than one. `building` covers the window in which a process is
     * being set up and has no thread yet — its descriptor table is opened
     * and its signals reset before its first thread exists. */
    struct uproc *p = sched_current_proc();
    return p != NULL ? p : building;
}

void uspace_exit(int64_t status) {
    struct uproc *me = uspace_current();
    bool is_root = me == NULL || me == running;
    uint32_t left = 0;
    if (is_root) {
        /* The program the kernel started. Its status is what `uspace_run`
         * returns, and its ending stops the harts' loops. */
        exit_status = status;
        exited = true;
    } else {
        /* A child. It becomes a **zombie**: the slot and the status stay
         * until its parent reaps them, because a parent is entitled to ask
         * what happened to a child that has already finished. Its address
         * space goes now, unless it was borrowing its parent's. */
        me->status = status;
        me->zombie = true;
        /* **Its other threads first, and off their harts**, before the
         * address space goes. `exit_group` ends the process rather than the
         * caller, so unlike the last-thread path there may be siblings
         * still executing — and one of those on another hart would be
         * running against page tables being freed underneath it.
         *
         * If one will not leave, the address space is **leaked rather than
         * freed**: a process's worth of pages lost is a bounded cost, and
         * freeing memory another hart is still writing through is not. */
        (void)sched_end_siblings(me);
        left = sched_await_siblings(me);
        if (left != 0) {
            kprintf("uspace: **%u thread(s) still on a hart; keeping the address space**\n", left);
        }
        if (!me->shares_pt && left == 0) {
            uspace_destroy(me);
        }
        me->shares_pt = false;
        /* Whoever is waiting — a `vfork` parent, or one in `wait4`. */
        (void)sched_wake_on(me, SCHED_MAX_THREADS);
        {
            struct uproc *parent = uspace_by_pid(me->parent_pid);
            if (parent != NULL) {
                (void)sched_wake_on(parent, SCHED_MAX_THREADS);
            }
        }
    }
    sched_exit_proc(me, is_root);
}

/* Makes this hart able to run the process's threads.
 *
 * `satp` and `sstatus.SUM` are **per-hart** registers, so every hart that
 * runs a thread of this process has to set both for itself — which is the
 * least obvious consequence of going from one hart to four, and the kind of
 * thing that works on the hart that set them and faults on every other.
 *
 * SUM lets the kernel read the process's memory for a syscall's arguments.
 * Without it a supervisor load from a user page faults, which is a
 * deliberate guard against dereferencing a user pointer by accident — so it
 * is on for exactly the window where the kernel means to. */
void uspace_adopt_address_space(struct uproc *p) {
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SUM));
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero"
                     :
                     : "r"(pgtable_satp(&p->pt, 1))
                     : "memory");
}

void uspace_release_address_space(void) {
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero"
                     :
                     : "r"(kernel_satp_saved)
                     : "memory");
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SUM));
}

/* A secondary hart joining a running process. Returns when the process is
 * over. It is the whole of what a secondary does with a process: there is
 * nothing per-hart to set up beyond its own translation, because the
 * scheduler's state is shared and the kernel lock is what makes that
 * safe. */
void uspace_join(struct uproc *p) {
    uspace_adopt_address_space(p);
    sched_run();
    uspace_release_address_space();
}

/* Rounds a byte count up to whole pages, refusing a request so large that
 * rounding it would wrap. */
static bool page_count(uint64_t bytes, uint64_t *pages) {
    if (bytes == 0 || bytes > USER_TOP) {
        return false;
    }
    *pages = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    return true;
}

/* Backs one page of a mapping, or records a reservation by doing nothing.
 *
 * A reservation (`PROT_NONE`) deliberately leaves the range unmapped rather
 * than mapping it without permissions: an Sv39 leaf with no R, W or X is a
 * pointer to a next-level table, not a page, so "mapped but inaccessible"
 * is not something the format can express at a leaf. Leaving it absent
 * gives the same behaviour — a touch faults — without the page tables. */
static int back_one_page(struct uproc *p, virt_t va, uint32_t prot) {
    phys_t page = 0;
    int rc;
    if (prot == 0) {
        return KERR_OK;
    }
    rc = mem_alloc_pages(1, 1, &page);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)page)[i] = 0;
    }
    rc = pgtable_map(&p->pt, va, page, PAGE_SIZE, prot_bits(prot));
    if (rc != KERR_OK) {
        mem_free_pages(page, 1);
    }
    return rc;
}

/* How many 4 KiB frames a 2 MiB page is made of. */
#define MEGAPAGE_PAGES ((uint32_t)(MEGAPAGE_SIZE / PAGE_SIZE))

/* Megapages handed to user space, and the frames they cost. */
static uint64_t huge_pages;

uint64_t uspace_huge_pages(void) {
    return huge_pages;
}

/* Backs one 2 MiB block as a **single** leaf entry.
 *
 * The whole point is the one entry: 512 frames reached through one
 * translation, so a program walking that memory takes one TLB entry where
 * it would have taken 512. Sv39 says nothing special about it — a leaf at
 * level 1 *is* a 2 MiB page — but it does demand that both the virtual and
 * the physical address be 2 MiB aligned, which is why the frames are asked
 * for with that alignment and the placement had to learn about it too.
 *
 * It is a contiguous physical allocation, which is a thing that gets harder
 * as memory fragments: the caller falls back to 4 KiB pages rather than
 * failing, because a mapping that works slowly is worth more than one that
 * does not exist. */
static int back_one_megapage(struct uproc *p, virt_t base, uint32_t prot) {
    phys_t block = 0;
    int rc = mem_alloc_pages(MEGAPAGE_PAGES, MEGAPAGE_PAGES, &block);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint64_t i = 0; i < MEGAPAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)block)[i] = 0;
    }
    rc = pgtable_map(&p->pt, base, block, MEGAPAGE_SIZE, prot_bits(prot));
    if (rc != KERR_OK) {
        mem_free_pages(block, MEGAPAGE_PAGES);
        return rc;
    }
    huge_pages++;
    return KERR_OK;
}

/* How many pages have been filled from a file at their first touch. */
static uint64_t file_pages;

uint64_t uspace_file_pages(void) {
    return file_pages;
}

/* Fills a freshly backed page from the file behind its region.
 *
 * The page is already zero, which is exactly what the bytes past the end of
 * the file must read as: a mapping's last page is partly file and partly
 * nothing, and Linux's rule for that nothing is zero rather than a fault.
 * So this copies what there is and leaves the rest alone.
 *
 * It reads through the kernel's own copy of the file rather than the disk,
 * because that is what `openat` produced — see `fdobjects.c` for why, and
 * for what it costs. */
static void fill_from_file(struct uproc *p, const struct vma *r, virt_t page) {
    const uint8_t *bytes = NULL;
    uint32_t size = 0;
    uint64_t at = r->offset + (page - r->start);
    phys_t phys = 0;
    uint64_t flags = 0, span = 0;
    if (fdobjects_file_of_object(r->file, &bytes, &size) != KERR_OK ||
        !pgtable_lookup(&p->pt, page, &phys, &flags, &span) || at >= size) {
        return;
    }
    {
        uint64_t take = size - at < PAGE_SIZE ? size - at : PAGE_SIZE;
        for (uint64_t i = 0; i < take; i++) {
            ((volatile uint8_t *)(uintptr_t)(phys & ~(phys_t)(PAGE_SIZE - 1u)))[i] = bytes[at + i];
        }
    }
    file_pages++;
}

/* Backs whatever the region says belongs at `va`: a 2 MiB block for a
 * region that asked for huge pages and has room for a whole one, a 4 KiB
 * page otherwise.
 *
 * Both conditions matter. A region's *edges* are not necessarily 2 MiB
 * aligned even when its interior is, and a megapage mapped past the end of
 * its region would give the program memory it never asked for — reachable,
 * and in the way of the next mapping. */
static int back_for_region(struct uproc *p, const struct vma *r, virt_t va) {
    virt_t base = va & ~(virt_t)(MEGAPAGE_SIZE - 1u);
    virt_t page = va & ~(virt_t)(PAGE_SIZE - 1u);
    int rc;
    if (r->file == NULL && (r->flags & VMA_HUGE) != 0 && base >= r->start &&
        r->end - base >= MEGAPAGE_SIZE && back_one_megapage(p, base, r->prot) == KERR_OK) {
        return KERR_OK;
    }
    rc = back_one_page(p, page, r->prot);
    if (rc == KERR_OK && r->file != NULL) {
        fill_from_file(p, r, page);
    }
    return rc;
}

/* Live pages destroyed by a mapping nobody insisted on. Always a bug. */
static uint32_t replaced_pages;

uint32_t uspace_replaced_pages(void) {
    return replaced_pages;
}

/* Records the regions a process is given before it runs a single
 * instruction: its stack and the page its signal handlers return through.
 *
 * **Neither is made by `mmap`, and both are in the window `mmap` places
 * into.** The address-space record only knew about mappings the program
 * asked for, so a gap search handed out the stack's own addresses — the
 * first allocation a Go runtime made landed on the stack it was running on,
 * and the program jumped to nothing. A record of the address space has to
 * include everything in it, not everything the program requested. */
/* The stack. **Every** build path makes one, at the same address, and no
 * path asks `mmap` for it — which is exactly why it has to be recorded by
 * each of them rather than by the one that happened to be written first. */
static void record_stack(struct uproc *p) {
    virt_t stack_low = USER_STACK_TOP - (virt_t)USER_STACK_PAGES * PAGE_SIZE;
    (void)vma_insert(&p->vmas, stack_low, USER_STACK_TOP, PROT_READ | PROT_WRITE, VMA_GROWSDOWN);
}

/* The page signal handlers return through, which only the ELF path maps —
 * so only that path records it. Recording it for a program that has none
 * would be a region with nothing behind it, and demand paging answers a
 * touch of such a region with a blank page instead of the fault it
 * deserves. A record must describe the address space, in both directions. */
static void record_trampoline(struct uproc *p) {
    (void)vma_insert(&p->vmas, SIGNAL_TRAMPOLINE, SIGNAL_TRAMPOLINE + PAGE_SIZE,
                     PROT_READ | PROT_EXEC, 0);
}

static void record_fixed_regions(struct uproc *p) {
    record_stack(p);
    record_trampoline(p);
}

/* ── fork ──────────────────────────────────────────────────────────────── */

static void published_removal(void);

/* One page of a parent's address space, given to a child **copy-on-write**.
 *
 * A writable page becomes read-only in *both*, with `PTE_COW` set so the
 * fault handler knows the next store is a copy to make rather than a
 * program's mistake. A read-only page is simply shared: neither can write
 * it, so there is nothing to copy. Either way the frame gains a holder, so
 * whichever process lets go first does not take it from the other.
 *
 * The parent's entry changes, which is a *removal* of rights, so it has to
 * reach every hart before either process runs again — the caller does that
 * once for the whole address space rather than once per page. */
static int give_page_cow(struct uproc *child, struct uproc *parent, virt_t va) {
    phys_t phys = 0;
    uint64_t prot = 0, size = 0;
    int rc;
    if (!pgtable_lookup(&parent->pt, va, &phys, &prot, &size)) {
        return KERR_OK; /* a hole: nothing to give */
    }
    if ((prot & PTE_U) == 0) {
        return KERR_OK; /* the kernel's own mapping, shared another way */
    }
    if ((prot & PTE_W) != 0) {
        uint64_t shared = (prot & ~PTE_W) | PTE_COW;
        (void)pgtable_unmap(&parent->pt, va);
        rc = pgtable_map(&parent->pt, va, phys, PAGE_SIZE, shared);
        rc = rc != KERR_OK ? rc : pgtable_map(&child->pt, va, phys, PAGE_SIZE, shared);
    } else {
        rc = pgtable_map(&child->pt, va, phys, PAGE_SIZE, prot);
    }
    return rc != KERR_OK ? rc : mem_share_page(phys);
}

/* Gives a child every region the parent has, page by page. */
/* One of a parent's 2 MiB pages, given to a child by **copying it**.
 *
 * Copy-on-write would be better and is not what this does. The mark lives
 * in the page-table entry, and for a megapage there is one entry over 512
 * frames — so a copy-on-write fault would have to copy all 2 MiB anyway,
 * or split the page into 4 KiB entries and stop being what the program
 * asked for. Copying at `fork` is 2 MiB of memcpy per huge page the parent
 * has touched, paid once and predictably, instead of the same work paid at
 * an unpredictable moment plus the machinery to decide.
 *
 * An untouched block is not copied: with demand paging a huge mapping is
 * mostly nothing, and the child faults its own blocks in from its own
 * record. */
static int give_megapage(struct uproc *child, struct uproc *parent, virt_t at, uint32_t prot) {
    phys_t from = 0, to = 0;
    uint64_t flags = 0, size = 0;
    int rc;
    if (!pgtable_lookup(&parent->pt, at, &from, &flags, &size) || size != MEGAPAGE_SIZE) {
        return KERR_OK;
    }
    from &= ~(phys_t)(MEGAPAGE_SIZE - 1u);
    rc = mem_alloc_pages(MEGAPAGE_PAGES, MEGAPAGE_PAGES, &to);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint64_t i = 0; i < MEGAPAGE_SIZE; i++) {
        ((volatile uint8_t *)(uintptr_t)to)[i] = ((const volatile uint8_t *)(uintptr_t)from)[i];
    }
    rc = pgtable_map(&child->pt, at, to, MEGAPAGE_SIZE, prot_bits(prot));
    if (rc != KERR_OK) {
        mem_free_pages(to, MEGAPAGE_PAGES);
        return rc;
    }
    huge_pages++;
    return KERR_OK;
}

static int give_address_space(struct uproc *child, struct uproc *parent) {
    for (uint32_t i = 0; i < parent->vmas.n; i++) {
        virt_t at = parent->vmas.v[i].start;
        virt_t end = parent->vmas.v[i].end;
        bool huge = (parent->vmas.v[i].flags & VMA_HUGE) != 0;
        uint32_t prot = parent->vmas.v[i].prot;
        /* **The child's own reference.** Its region set is a copy of the
         * parent's, pointers and all, so without this the two processes
         * would share one reference and whichever let go first would take
         * the file's bytes out from under the other. */
        if (parent->vmas.v[i].file != NULL) {
            (void)file_get((struct file *)parent->vmas.v[i].file);
        }
        for (; at < end;) {
            int rc = huge ? give_megapage(child, parent, at, prot)
                          : give_page_cow(child, parent, at);
            if (rc != KERR_OK) {
                return rc;
            }
            at += huge ? MEGAPAGE_SIZE : PAGE_SIZE;
        }
    }
    /* The break is not an `mmap` region and is not recorded as one, so it
     * is walked explicitly. */
    for (virt_t at = parent->brk_floor; at < parent->brk; at += PAGE_SIZE) {
        int rc = give_page_cow(child, parent, at);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* A real `fork`: the child gets an address space **of its own**.
 *
 * Of its own, and initially made of the parent's pages: every writable page
 * is marked read-only and copy-on-write in both, so the first store by
 * either produces a private copy and nothing else is copied at all. A
 * process that forks and immediately execs copies nothing but page tables.
 *
 * This is what `uspace_fork_shared` is not. That one hands the child the
 * parent's tables and requires the parent to be suspended until the child
 * execs — which is `vfork`, and which cannot support a parent that keeps
 * running. Isolation is the point: after this, neither process can reach
 * the other's memory, whatever either of them does.
 *
 * The parent's own entries lose their write bit here, so the change is a
 * removal and must reach every hart before the parent runs again
 * (`published_removal`). Doing it once for the whole address space rather
 * than per page is the difference between one firmware call and thousands. */
int uspace_fork(struct uproc *child, struct uproc *parent) {
    struct pt_mem mem = {.alloc = proc_alloc, .free = proc_free, .to_virt = proc_to_virt};
    uint32_t pid = child->pid;
    int rc;
    child->pt = (struct pgtable){0};
    rc = pgtable_create(&child->pt, &mem);
    if (rc != KERR_OK) {
        return rc;
    }
    child->shares_pt = false;
    child->vmas = parent->vmas;
    child->entry = parent->entry;
    child->stack_top = parent->stack_top;
    child->brk = parent->brk;
    child->brk_floor = parent->brk_floor;
    child->mmap_next = parent->mmap_next;
    child->pid = pid;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->rlim_nofile_soft = parent->rlim_nofile_soft;
    child->rlim_nofile_hard = parent->rlim_nofile_hard;
    child->rlim_nproc_soft = parent->rlim_nproc_soft;
    child->rlim_nproc_hard = parent->rlim_nproc_hard;
    for (uint32_t i = 0; i < SIG_COUNT; i++) {
        child->actions[i] = parent->actions[i];
    }
    rc = give_address_space(child, parent);
    rc = rc != KERR_OK ? rc : share_kernel_mappings(&child->pt);
    if (rc != KERR_OK) {
        pgtable_destroy(&child->pt);
        return rc;
    }
    child->built = true;
    published_removal();
    return fd_copy_table(child, parent);
}

/* Where to put a mapping the caller did not insist on.
 *
 * **The address space answers this, not the page tables.** A hint is
 * honoured when the whole range is free and inside user space, and
 * otherwise a gap is found — and "free" has to include ranges that have no
 * pages, which is what a `PROT_NONE` reservation is. Asking the page tables
 * could not see one, so a mapping could be placed inside a runtime's
 * reserved arena and then mapped over when the runtime committed it.
 *
 * `vma_find_free` takes the hint and the window and does both cases, which
 * is why the bump cursor is gone: a cursor can only ever move forwards, so
 * it could not step over a reservation without eventually walking off the
 * end of user space. */
static virt_t place_mapping(struct uproc *p, virt_t at, uint64_t pages, virt_t align) {
    virt_t bytes = pages * PAGE_SIZE;
    virt_t chosen = 0;
    if (vma_find_free(&p->vmas, bytes, at, USER_MMAP_BASE, USER_STACK_TOP, align, &chosen) !=
        KERR_OK) {
        return 0;
    }
    return chosen;
}

/* A page-table change must reach the harts that could be using it.
 *
 * This is the one piece of SMP that is invisible until it is wrong. A stale
 * translation does not fault: the hart reads the **wrong page**, so the
 * program sees a value that was never written there and faults later, at an
 * address that means nothing. It cost one such fault to find — a wild
 * pointer `0x2148f0180003` in the middle of the heap, reproducible only
 * under load, on a machine where the hart that changed the mapping was the
 * only one that could see the change.
 *
 * **Two functions, because the two directions are not symmetrical.**
 * Removing or narrowing a mapping must reach every hart at once: a stale
 * entry there grants access that is supposed to be gone, and the page may
 * already have been handed to something else. Adding one need only reach
 * this hart: the worst a stale entry can say is "absent" for something that
 * is now present, and the hart that trips over that fences locally and
 * retries (`retry_after_stale_translation`).
 *
 * The difference is not a micro-optimisation. A garbage collector maps
 * constantly and unmaps rarely, and broadcasting on every map turned a 1.6x
 * parallel speedup into 0.6x — slower than running on one hart. */
static void published_removal(void) {
    mmu_flush_tlb_everywhere();
}

/* Whether an addition is broadcast too. Off by default — see above — and a
 * knob rather than a constant because "is the cheap asymmetry sound?" is a
 * question only a four-hart machine can answer, and answering it needs the
 * expensive version to compare against. */
static bool broadcast_additions;

void uspace_broadcast_additions(bool on) {
    broadcast_additions = on;
}

static void published_addition(void) {
    if (broadcast_additions) {
        mmu_flush_tlb_everywhere();
        return;
    }
    mmu_flush_tlb_local();
}

/* Whether a mapping may have the addresses it insists on.
 *
 * `MAP_FIXED` replaces whatever is there, which is how a runtime commits
 * part of a reservation it already holds. `MAP_FIXED_NOREPLACE` is the same
 * insistence with the opposite answer to a collision — `EEXIST` rather than
 * destruction — and that is the whole reason it exists: without it a
 * program's only way to find out whether an address was free was to take
 * it, and a wrong guess cost whatever was there.
 *
 * The bound is written as a subtraction because the addition **wraps**. It
 * used to read `at + pages * PAGE_SIZE > USER_STACK_TOP`, and an address a
 * few pages below 2^64 wrapped past zero and compared as being inside user
 * space. Nothing reached the page tables — they refuse a non-canonical
 * address — so the check was wrong without being a hole, which is the kind
 * that survives. */
static int fixed_is_allowed(struct uproc *p, const struct uspace_mapping *m, uint64_t pages) {
    uint64_t bytes = pages * PAGE_SIZE;
    if ((m->at & (PAGE_SIZE - 1u)) != 0 || m->at == 0 || m->at >= USER_STACK_TOP ||
        bytes > USER_STACK_TOP - m->at) {
        return KERR_INVAL;
    }
    if (m->noreplace && !vma_range_free(&p->vmas, m->at, m->at + bytes)) {
        return KERR_EXIST;
    }
    return KERR_OK;
}

/* How many pages were backed at `mmap` because the caller asked for them to
 * be, rather than on first touch. */
static uint64_t populated_pages;

uint64_t uspace_populated_pages(void) {
    return populated_pages;
}

/* Backs a whole mapping now, for a caller that would rather pay up front
 * than take a fault per page (`MAP_POPULATE`, and `MAP_LOCKED`, which on
 * Linux populates as part of locking).
 *
 * **Advisory, here as there**: running out of memory part-way does not fail
 * the mapping, because the pages that did not get one will be faulted in
 * exactly as they would have been without the flag. A reservation has
 * nothing to populate — `PROT_NONE` means there is deliberately nothing
 * there — so it is left alone rather than quietly given pages. */
static void populate(struct uproc *p, virt_t base, uint64_t pages, uint32_t prot) {
    if (prot == 0) {
        return;
    }
    for (uint64_t i = 0; i < pages; i++) {
        if (back_one_page(p, base + i * PAGE_SIZE, prot) != KERR_OK) {
            return;
        }
        populated_pages++;
    }
}

/* Takes the addresses a new mapping is about to occupy.
 *
 * A mapping the caller did *not* insist on must never replace anything — it
 * was placed somewhere free — so when it does, that is a placement bug
 * destroying live memory, and it is counted rather than left to be inferred
 * from the program misbehaving later.
 *
 * **Unmapped, and not re-backed.** This used to back each page here, which
 * is what made a large mapping cost all of its pages the moment it was
 * asked for; the first touch of each page faults into `back_on_demand`
 * instead. The pages still have to go: a fixed mapping replaces what was
 * there, and leaving the old frame mapped under a new region's rights would
 * be the previous contents showing through a fresh mapping, which a caller
 * is entitled to assume is zero.
 *
 * `drop_pages` and not `uspace_unmap`, because the record is the caller's
 * to rewrite — it removes and re-inserts the whole range. Going through
 * `munmap`'s half would remove the record a page at a time, splitting a
 * region once per page on the way. */
static void clear_the_range(struct uproc *p, bool fixed, virt_t base, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        virt_t va = base + i * PAGE_SIZE;
        phys_t was = 0;
        uint64_t f = 0, sz = 0;
        if (!fixed && pgtable_lookup(&p->pt, va, &was, &f, &sz)) {
            replaced_pages++;
        }
        (void)drop_pages(p, va, PAGE_SIZE);
    }
}

/* Writes the new region into the address-space record, whatever its rights
 * are: a reservation is a range that is spoken for, and the whole point of
 * the set is that it says so. A fixed mapping replaces what was there, so
 * the old regions go first.
 *
 * **A mapping that cannot be recorded is refused**, and the pages it had
 * been given are handed back. An unrecorded region is worse than a failed
 * `mmap`: the next gap search cannot see it, so it hands the same addresses
 * out again and two allocations share pages. A program that is told
 * `ENOMEM` can cope; one that is given memory somebody else is using
 * cannot. The failure is silent and total — measured, a kernel that kept
 * going here stopped producing any output at all. */
/* Lets go of the file a region was reading from.
 *
 * Passed to `vma_remove` everywhere, because **every** way a region
 * disappears is a reference the kernel stops holding — and the one place
 * that forgets is the one that frees a file's bytes while a mapping is
 * still faulting pages out of them. The core calls it with the region
 * rather than its range for exactly this reason. */
static void drop_the_file(void *ctx, const struct vma *v) {
    (void)ctx;
    if (v->file != NULL) {
        file_put((struct file *)v->file);
    }
}

/* The same for every region a process still holds, which is what ending or
 * replacing an address space has to do. */
static void drop_every_file(struct uproc *p) {
    for (uint32_t i = 0; i < p->vmas.n; i++) {
        if (p->vmas.v[i].file != NULL) {
            file_put((struct file *)p->vmas.v[i].file);
            p->vmas.v[i].file = NULL;
        }
    }
}

static int record_the_mapping(struct uproc *p, virt_t base, uint64_t pages, uint32_t prot,
                              uint32_t flags, struct file *file, uint64_t offset) {
    if (vma_remove(&p->vmas, base, base + pages * PAGE_SIZE, drop_the_file, NULL) == KERR_OK &&
        vma_insert_backed(&p->vmas, base, base + pages * PAGE_SIZE, prot, flags, file, offset) ==
            KERR_OK) {
        return KERR_OK;
    }
    unrecorded_regions++;
    (void)drop_pages(p, base, pages * PAGE_SIZE);
    return KERR_NOMEM;
}

int uspace_map(struct uproc *p, const struct uspace_mapping *m, virt_t *out) {
    uint64_t pages = 0;
    virt_t base;
    int rc;
    if (p == NULL || m == NULL || out == NULL || !page_count(m->bytes, &pages)) {
        return KERR_INVAL;
    }
    if (m->fixed) {
        rc = fixed_is_allowed(p, m, pages);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    base = m->fixed ? m->at : place_mapping(p, m->at, pages, m->huge ? MEGAPAGE_SIZE : 0u);
    if (base == 0) {
        return KERR_NOMEM;
    }
    clear_the_range(p, m->fixed, base, pages);
    rc = record_the_mapping(p, base, pages, m->prot, m->huge ? VMA_HUGE : 0u, m->file, m->offset);
    if (rc != KERR_OK) {
        return rc;
    }
    if (m->populate) {
        populate(p, base, pages, m->prot);
    }
    *out = base;
    published_addition();
    return KERR_OK;
}

/* Drops the pages backing a range and leaves the address-space record
 * alone.
 *
 * The two halves of unmapping are separate because its callers want
 * different ones. A fixed `mmap` drops pages and then records a *new*
 * region over the same addresses; `mprotect` to `PROT_NONE` drops the pages
 * of a region that is still there as a reservation; a shrinking `brk` drops
 * pages below the break, which the record never described. Only `munmap`
 * wants both, and that is `uspace_unmap`. */
static int drop_pages(struct uproc *p, virt_t at, uint64_t bytes) {
    uint64_t pages = 0;
    if ((at & (PAGE_SIZE - 1u)) != 0 || !page_count(bytes, &pages)) {
        return KERR_INVAL;
    }
    for (virt_t va = at; va < at + pages * PAGE_SIZE;) {
        phys_t phys = 0;
        uint64_t flags = 0, size = 0;
        uint64_t span;
        if (!pgtable_lookup(&p->pt, va, &phys, &flags, &size)) {
            va += PAGE_SIZE;
            continue;
        }
        /* **However big the mapping is.** A 2 MiB page is one entry over
         * 512 frames, and `pgtable_lookup` reports the physical address
         * *inside* it — so freeing the value it returns would hand back one
         * frame from the middle of a block and leak the rest, while
         * unmapping took all 512 out of the address space. The base and the
         * count both come from the span. */
        span = size < PAGE_SIZE ? PAGE_SIZE : size;
        phys &= ~(phys_t)(span - 1u);
        (void)pgtable_unmap(&p->pt, va);
        /* The mapping is gone from the tables, but another hart may still
         * have it cached — so the page cannot be handed back to the
         * allocator until every hart has been told. Freeing first would let
         * a hart write through a stale translation into memory that now
         * belongs to something else. That is also why the flush is inside
         * the loop and there is no second one after it: every removal is
         * already published before its frame is reusable. */
        published_removal();
        mem_free_pages(phys, (uint32_t)(span / PAGE_SIZE));
        va = (va & ~(virt_t)(span - 1u)) + span;
    }
    return KERR_OK;
}

/* `munmap`: the pages **and** the record.
 *
 * **Nothing used to remove the record**, and the cost of that grew. While
 * mappings were backed eagerly it was an address-space leak: the range kept
 * its entry for ever, so the gap search never offered those addresses again
 * and a program that mapped and unmapped in a loop walked down the window
 * until `mmap` returned `ENOMEM` with the address space nearly empty.
 *
 * Demand paging made it worse than a leak. A fault inside a recorded region
 * is backed on the spot, so a read of memory the program had *returned*
 * found a region that still claimed it and was handed a fresh zero page —
 * a use-after-free that silently works, which is the opposite of what
 * unmapping is for. The record is the authority on what a process may
 * touch, so forgetting a range is not bookkeeping: it is the unmapping.
 *
 * The record goes first. It is the only step that can fail having changed
 * nothing (a partial unmap splits a region, and the split needs a slot), so
 * doing it first means a refusal leaves the mapping intact rather than
 * leaving pages freed under a record that still promises them. */
/* Whether a range would cut in half a region that has to stay whole.
 *
 * Two kinds, for two reasons that happen to have the same answer. A **huge**
 * region is one table entry over 2 MiB: there is no such thing as unmapping
 * a quarter of it, and the alternatives are to split it into 4 KiB pages —
 * the opposite of what the caller asked for — or to refuse. A
 * **file-backed** region holds one reference to the file it reads from, and
 * splitting it would make two regions sharing one reference: the first to
 * be unmapped would drop it and the other would go on faulting pages out of
 * freed memory. Splitting one properly means taking another reference at
 * the split, which the pure record cannot do.
 *
 * So both are unmapped and moved whole, and a program that asks for less is
 * told `EINVAL` rather than given a quiet half-measure. */
static bool must_be_whole(struct uproc *p, virt_t at, uint64_t bytes) {
    const struct vma *r = vma_find(&p->vmas, at);
    if (r == NULL || ((r->flags & VMA_HUGE) == 0 && r->file == NULL)) {
        return false;
    }
    return at != r->start || bytes != r->end - r->start;
}

int uspace_unmap(struct uproc *p, virt_t at, uint64_t bytes) {
    uint64_t pages = 0;
    if ((at & (PAGE_SIZE - 1u)) != 0 || !page_count(bytes, &pages)) {
        return KERR_INVAL;
    }
    if (must_be_whole(p, at, pages * PAGE_SIZE)) {
        return KERR_INVAL;
    }
    if (vma_remove(&p->vmas, at, at + pages * PAGE_SIZE, drop_the_file, NULL) != KERR_OK) {
        return KERR_NOMEM;
    }
    return drop_pages(p, at, bytes);
}

/* ── mremap ────────────────────────────────────────────────────────────── */

/* How many pages `mremap` has re-pointed rather than copied. */
static uint64_t moved_pages;

uint64_t uspace_moved_pages(void) {
    return moved_pages;
}

/* Times a page was found mapped at an address the region record called
 * free. Always a bug, in whatever put it there. */
static uint32_t stale_records;

uint32_t uspace_stale_records(void) {
    return stale_records;
}

/* The one region the old range lies inside, or NULL.
 *
 * Linux's rule, and it is not a formality: a range spanning two regions has
 * two sets of rights, and moving it would have to invent one for the
 * result. The caller is told `EFAULT`, which is also the answer for a range
 * that is not mapped at all — including one a program has already unmapped,
 * now that `munmap` forgets it. */
static const struct vma *whole_region(struct uproc *p, virt_t at, uint64_t bytes) {
    const struct vma *r = vma_find(&p->vmas, at);
    if (r == NULL || bytes > r->end - at) {
        return NULL;
    }
    return r;
}

/* Re-points one page's frame at a new address, or does nothing when there is
 * no frame there.
 *
 * **The flags come from the old entry, not from the region's rights.** A
 * page can carry less than its region allows — a copy-on-write page is left
 * read-only with `PTE_COW` set, so a child's and a parent's next store each
 * fault — and recomputing the flags from `prot` would hand it back writable,
 * which is two processes sharing a page each believes it owns. That is the
 * kind of corruption this kernel has already paid for once.
 *
 * An absent page stays absent, and that is not a shortcut: with demand
 * paging a mapping is mostly holes, and faulting them in to move them would
 * make a move cost what the mapping would have cost eagerly. */
static int move_one_frame(struct uproc *p, virt_t from, virt_t to) {
    phys_t phys = 0;
    uint64_t flags = 0, size = 0;
    int rc;
    if (!pgtable_lookup(&p->pt, from, &phys, &flags, &size)) {
        return KERR_OK;
    }
    rc = pgtable_map(&p->pt, to, phys, PAGE_SIZE, flags);
    if (rc != KERR_OK) {
        return rc;
    }
    (void)pgtable_unmap(&p->pt, from);
    moved_pages++;
    return KERR_OK;
}

/* Moves a whole range's frames, all or nothing.
 *
 * Nothing is copied, which is the entire reason `mremap` exists: growing a
 * 64 MiB arena costs the page-table entries for the pages that have been
 * touched and not one byte of memcpy.
 *
 * Only the table levels can run out, and part-way through is the one state
 * a caller cannot be told about — so what moved moves back, and the refusal
 * leaves the mapping exactly where it was. Putting it back cannot fail in
 * turn: unmapping leaves the intermediate tables behind, so the addresses
 * being restored still have theirs. */
static int move_frames(struct uproc *p, virt_t from, virt_t to, uint64_t pages) {
    uint64_t before = moved_pages;
    int rc = KERR_OK;
    for (uint64_t i = 0; i < pages; i++) {
        rc = move_one_frame(p, from + i * PAGE_SIZE, to + i * PAGE_SIZE);
        if (rc == KERR_OK) {
            continue;
        }
        while (i-- > 0) {
            (void)move_one_frame(p, to + i * PAGE_SIZE, from + i * PAGE_SIZE);
        }
        moved_pages = before; /* a move and its undoing are not two moves */
        return rc;
    }
    return rc;
}

/* Grows a mapping where it stands, by recording the pages past its end.
 *
 * Inserting the tail **is** the test of whether the space is free:
 * `vma_insert` refuses a range that overlaps a region, and one that is
 * adjacent with the same rights is merged into the region it extends. So
 * the whole operation is a record change — demand paging means the new
 * pages cost nothing until they are touched, which is what makes growing in
 * place the cheap case rather than merely the convenient one. */
static int grow_in_place(struct uproc *p, virt_t at, uint64_t old_bytes, uint64_t new_bytes,
                         uint32_t prot) {
    if (new_bytes > USER_STACK_TOP - at) {
        return KERR_NOMEM;
    }
    return vma_insert(&p->vmas, at + old_bytes, at + new_bytes, prot, 0);
}

/* Moves a mapping somewhere it fits.
 *
 * The destination is recorded **first**, because that is the only step that
 * can fail without having changed anything — and recording it is also what
 * makes the destination unavailable to anything else that looks for a gap.
 * A caller-chosen destination that overlaps the source is refused by the
 * insert itself, which is `EINVAL` and the right answer. */
static int move_mapping(struct uproc *p, virt_t at, uint64_t old_bytes, uint64_t new_bytes,
                        uint32_t prot, virt_t to, virt_t *out) {
    virt_t base = to != 0 ? to : place_mapping(p, 0, new_bytes / PAGE_SIZE, 0u);
    int rc;
    if (base == 0) {
        return KERR_NOMEM; /* no gap in the window is large enough */
    }
    /* A caller-chosen destination has to be inside the window `mmap` places
     * into. Below it lie the program's text and its break, and the break is
     * **not** in the region record — so an overlap there is one `vma_insert`
     * cannot see, and the move would map over live memory. */
    if (base < USER_MMAP_BASE || base >= USER_STACK_TOP || new_bytes > USER_STACK_TOP - base) {
        return KERR_INVAL;
    }
    rc = vma_insert(&p->vmas, base, base + new_bytes, prot, 0);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = move_frames(p, at, base, old_bytes / PAGE_SIZE);
    if (rc != KERR_OK) {
        (void)vma_remove(&p->vmas, base, base + new_bytes, drop_the_file, NULL);
        /* **The destination was already mapped**, at an address the record
         * said was free. That is not a reason to refuse a move; it is the
         * record and the page tables disagreeing, and the move is the only
         * thing in a position to notice. Counted rather than flattened into
         * the caller's `ENOMEM`, because it is the symptom of a mapping that
         * nothing recorded — which is how this kernel's worst class of bug
         * presents, and it was this very path that found the last one. */
        if (rc == KERR_EXIST) {
            stale_records++;
            rc = KERR_NOMEM; /* what `mremap` is allowed to say */
        }
        return rc;
    }
    (void)vma_remove(&p->vmas, at, at + old_bytes, drop_the_file, NULL);
    published_removal(); /* the old addresses no longer translate anywhere */
    *out = base;
    return KERR_OK;
}

int uspace_remap(struct uproc *p, virt_t at, uint64_t old_bytes, uint64_t new_bytes, bool may_move,
                 virt_t to, virt_t *out) {
    uint64_t old_pages = 0, new_pages = 0;
    const struct vma *region;
    uint32_t prot;
    if (p == NULL || out == NULL || (at & (PAGE_SIZE - 1u)) != 0 || (to & (PAGE_SIZE - 1u)) != 0) {
        return KERR_INVAL;
    }
    if (!page_count(old_bytes, &old_pages) || !page_count(new_bytes, &new_pages)) {
        return KERR_INVAL;
    }
    old_bytes = old_pages * PAGE_SIZE;
    new_bytes = new_pages * PAGE_SIZE;
    region = whole_region(p, at, old_bytes);
    if (region == NULL) {
        return KERR_FAULT;
    }
    if ((region->flags & VMA_HUGE) != 0 || region->file != NULL) {
        /* Resizing a huge region would have to split or extend a 2 MiB
         * page and moving it would have to re-point one; a file-backed
         * region cannot be resized without deciding what the new part
         * reads from, and `mremap` is not told. Refused rather than half
         * done. */
        return KERR_INVAL;
    }
    prot = region->prot; /* read now: the next insert or remove invalidates it */
    *out = at;
    if (new_bytes < old_bytes) {
        return uspace_unmap(p, at + new_bytes, old_bytes - new_bytes);
    }
    if (to == 0 && new_bytes == old_bytes) {
        return KERR_OK;
    }
    if (to == 0 && grow_in_place(p, at, old_bytes, new_bytes, prot) == KERR_OK) {
        published_addition();
        return KERR_OK;
    }
    if (!may_move) {
        return KERR_NOMEM;
    }
    return move_mapping(p, at, old_bytes, new_bytes, prot, to, out);
}

int uspace_protect(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot) {
    uint64_t pages = 0;
    /* The record follows the rights, not only the ranges: a region whose
     * permissions changed is still the same region, and a reader of the set
     * that saw stale rights would answer a fault wrongly. */
    (void)vma_protect(&p->vmas, at, at + bytes, prot);
    if ((at & (PAGE_SIZE - 1u)) != 0 || !page_count(bytes, &pages)) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < pages; i++) {
        virt_t va = at + i * PAGE_SIZE;
        phys_t phys = 0;
        uint64_t flags = 0, size = 0;
        int rc;
        if (!pgtable_lookup(&p->pt, va, &phys, &flags, &size)) {
            /* Making a reservation usable: there is nothing there yet, so
             * this is the moment it gets a page. */
            rc = back_one_page(p, va, prot);
        } else if (prot == 0) {
            rc = drop_pages(p, va, PAGE_SIZE);
        } else {
            (void)pgtable_unmap(&p->pt, va);
            rc = pgtable_map(&p->pt, va, phys, PAGE_SIZE, prot_bits(prot));
        }
        if (rc != KERR_OK) {
            return rc;
        }
    }
    /* `mprotect` both adds and takes away, and which one it did is not
     * tracked here — so it takes the safe side. It is rare compared with
     * `mmap`, so there is nothing to win by being clever. */
    published_removal();
    return KERR_OK;
}

virt_t uspace_brk(struct uproc *p, virt_t want) {
    virt_t from = (p->brk + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    virt_t to = (want + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (want == 0 || want < p->brk_floor || want >= USER_MMAP_BASE) {
        return p->brk;
    }
    for (virt_t va = from; va < to; va += PAGE_SIZE) {
        if (back_one_page(p, va, PROT_READ | PROT_WRITE) != KERR_OK) {
            return p->brk; /* refused, and the break has not moved */
        }
    }
    if (to < from) {
        (void)drop_pages(p, to, from - to);
    }
    p->brk = want;
    /* The break only ever grows here (a shrink unmaps, and that path
     * broadcasts for itself), so this hart is enough. */
    published_addition();
    return p->brk;
}
