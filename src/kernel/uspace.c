#include "kernel/uspace.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "core/elf.h"
#include "core/errno.h"
#include "core/ustack.h"
#include "kernel/mem.h"
#include "kernel/random.h"
#include "kernel/klock.h"
#include "kernel/sched.h"
#include "arch/riscv/sigframe.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"

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

static struct uproc *running;
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
 * (DEVELOPMENT_PROGRESS.md P0.4): with the kernel above the sign-extension
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
    for (uint32_t i = 0; rc == KERR_OK && i < USER_STACK_PAGES; i++) {
        rc = pgtable_map(&p->pt, USER_STACK_TOP - (uint64_t)(i + 1u) * PAGE_SIZE,
                         stack + (uint64_t)i * PAGE_SIZE, PAGE_SIZE, PT_PROT_RW | PT_PROT_USER);
    }
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
static void on_user_return(struct trapframe *tf) {
    uint32_t fatal = 0;
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
        kprintf("\nuspace: killed by signal %u\n", fatal);
        exit_status = -(int64_t)fatal;
        exited = true;
        sched_exit_process();
        return;
    }
    (void)signal_deliver(tf);
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
    if (running == NULL || !pgtable_lookup(&running->pt, tf->stval, &phys, &flags, &size)) {
        return false;
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
    if (retry_after_stale_translation(tf)) {
        return true;
    }
    if (fault_becomes_a_signal(tf)) {
        return true;
    }
    /* A fault in user code is the process's problem, not the kernel's: it
     * stops, and the kernel carries on. Reporting it this way — rather than
     * panicking — is the whole difference between a user fault and a kernel
     * one. */
    kprintf("\nuspace: fault in user code: %s at sepc %016llx, stval %016llx\n",
            trap_cause_name(tf->scause), (unsigned long long)tf->sepc,
            (unsigned long long)tf->stval);
    exit_status = KERR_FAULT;
    exited = true;
    sched_exit_process();
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
    signal_reset();
    rc = sched_init(&first, p->stack_top, &p->main_tid);
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
    while (sched_joined_harts() > 0) {
    }
    uspace_release_address_space();
    trap_set_expected(NULL);
    trap_set_user_return_hook(NULL);
    timer_remove_tick_hook(sched_tick);
    sched_shutdown();
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

/* Builds the stack: the block is assembled in the page that will be the top
 * of the stack, then mapped. Assembling it in place would need the process's
 * address space to be loaded, which it is not yet. */
static int build_start_state(struct uproc *p, const struct elf_image *img, const char *path) {
    struct auxv_pair auxv[USTACK_MAX_AUXV];
    const char *argv[1];
    const char *envp[2] = {"HOME=/", "PATH=/"};
    struct ustack_args args = {.argv = argv, .argc = 1u, .envp = envp, .envc = 2u, .auxv = auxv,
                               .execfn = path};
    phys_t stack = 0;
    virt_t sp = 0;
    uint32_t used = 0;
    int rc;
    argv[0] = path;
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
    struct pt_mem mem = {.alloc = proc_alloc, .free = proc_free, .to_virt = proc_to_virt};
    struct elf_image img;
    int rc;
    *p = (struct uproc){0};
    rc = elf_parse(image, bytes, USER_TOP, &img);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = pgtable_create(&p->pt, &mem);
    for (uint32_t i = 0; rc == KERR_OK && i < img.nloads; i++) {
        rc = load_segment(p, &img.loads[i], image);
    }
    rc = rc != KERR_OK ? rc : build_start_state(p, &img, path);
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

struct uproc *uspace_current(void) {
    return running;
}

void uspace_exit(int64_t status) {
    exit_status = status;
    exited = true;
    sched_exit_process();
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

/* Where to put a mapping the caller did not insist on. The hint is honoured
 * when it is free and inside user space, because a runtime that places its
 * heap by hint gets a tidier address space when the hint works — and must
 * not fail when it does not. */
static virt_t place_mapping(struct uproc *p, virt_t at, uint64_t pages) {
    phys_t phys = 0;
    uint64_t flags = 0, size = 0;
    virt_t chosen;
    if (at != 0 && (at & (PAGE_SIZE - 1u)) == 0 && at + pages * PAGE_SIZE <= USER_STACK_TOP &&
        !pgtable_lookup(&p->pt, at, &phys, &flags, &size)) {
        return at;
    }
    chosen = p->mmap_next;
    if (chosen + pages * PAGE_SIZE > USER_STACK_TOP) {
        return 0;
    }
    p->mmap_next = chosen + pages * PAGE_SIZE;
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

static void published_addition(void) {
    mmu_flush_tlb_local();
}

int uspace_map_anon(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot, bool fixed,
                    virt_t *out) {
    uint64_t pages = 0;
    virt_t base;
    if (!page_count(bytes, &pages)) {
        return KERR_INVAL;
    }
    if (fixed && ((at & (PAGE_SIZE - 1u)) != 0 || at == 0 ||
                  at + pages * PAGE_SIZE > USER_STACK_TOP)) {
        return KERR_INVAL;
    }
    base = fixed ? at : place_mapping(p, at, pages);
    if (base == 0) {
        return KERR_NOMEM;
    }
    /* A fixed mapping replaces whatever is there, which is how a runtime
     * commits part of a reservation it already holds. */
    for (uint64_t i = 0; i < pages; i++) {
        virt_t va = base + i * PAGE_SIZE;
        int rc;
        (void)uspace_unmap(p, va, PAGE_SIZE);
        rc = back_one_page(p, va, prot);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    *out = base;
    published_addition();
    return KERR_OK;
}

int uspace_unmap(struct uproc *p, virt_t at, uint64_t bytes) {
    uint64_t pages = 0;
    bool changed = false;
    if ((at & (PAGE_SIZE - 1u)) != 0 || !page_count(bytes, &pages)) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < pages; i++) {
        phys_t phys = 0;
        uint64_t flags = 0, size = 0;
        if (pgtable_lookup(&p->pt, at + i * PAGE_SIZE, &phys, &flags, &size)) {
            (void)pgtable_unmap(&p->pt, at + i * PAGE_SIZE);
            /* The mapping is gone from the tables, but another hart may
             * still have it cached — so the page cannot be handed back to
             * the allocator until every hart has been told. Freeing first
             * would let a hart write through a stale translation into memory
             * that now belongs to something else. */
            /* The mapping is gone from the tables, but another hart may
             * still have it cached — so the page cannot be handed back to
             * the allocator until every hart has been told. Freeing first
             * would let a hart write through a stale translation into
             * memory that now belongs to something else. */
            changed = true;
            published_removal();
            mem_free_pages(phys, 1);
        }
    }
    if (changed) {
        published_removal();
    }
    return KERR_OK;
}

int uspace_protect(struct uproc *p, virt_t at, uint64_t bytes, uint32_t prot) {
    uint64_t pages = 0;
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
            rc = uspace_unmap(p, va, PAGE_SIZE);
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
        (void)uspace_unmap(p, to, from - to);
    }
    p->brk = want;
    /* The break only ever grows here (a shrink unmaps, and that path
     * broadcasts for itself), so this hart is enough. */
    published_addition();
    return p->brk;
}
