#include "kernel/uspace.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "core/elf.h"
#include "core/errno.h"
#include "core/ustack.h"
#include "kernel/mem.h"
#include "kernel/random.h"

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

static struct uproc *running;
static int64_t exit_status;
static bool exited;
static uint64_t syscalls;
static uint64_t kernel_satp_saved;

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

void uspace_destroy(struct uproc *p) {
    const struct pgtable *kernel;
    if (!p->built) {
        return;
    }
    kernel = mmu_kernel_table();
    /* Shared entries are cleared before the walk, or destroying a process
     * would free the kernel's own tables — and the kernel would then be
     * running on memory the allocator has handed back. Entries resolved one
     * level down are the process's own tables, so they are freed; what they
     * contain of the kernel's is a copy of a *pointer* to a table the
     * process does not own, and the walk stops at the leaves it finds. */
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (p->pt.root[i] == kernel->root[i]) {
            p->pt.root[i] = 0;
        }
    }
    pgtable_destroy(&p->pt);
    p->built = false;
}

/* What a syscall from user mode does. Only what the first user program
 * needs: say something, and stop. */
static void handle_syscall(struct trapframe *tf) {
    uint64_t nr = tf->x[17];  /* a7 */
    syscalls++;
    switch (nr) {
    case SYS_WRITE: {
        /* write(fd, buf, len). The buffer is a *user* address, and the
         * kernel can read it only because SUM is set in sstatus — without
         * that bit a supervisor load from a user page faults, which is a
         * deliberate guard against dereferencing a user pointer by
         * accident. */
        const char *buf = (const char *)(uintptr_t)tf->x[11]; /* a1 */
        uint64_t len = tf->x[12];                             /* a2 */
        for (uint64_t i = 0; i < len && i < 4096u; i++) {
            kprintf("%c", buf[i]);
        }
        tf->x[10] = len; /* a0: bytes written */
        break;
    }
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        exit_status = (int64_t)tf->x[10];
        exited = true;
        /* Does not return: it unwinds to uspace_run, abandoning this trap
         * frame — which is correct, because the process it belongs to is
         * gone. */
        uspace_leave(running->context);
        break;
    default:
        kprintf("\nuspace: syscall %llu is not implemented\n", (unsigned long long)nr);
        tf->x[10] = (uint64_t)(int64_t)KERR_NOTSUP;
        break;
    }
    /* Past the `ecall`, which is always 4 bytes — returning to it would
     * make the syscall again, for ever. */
    tf->sepc += 4u;
}

/* The expected-fault hook is how a user trap reaches this module without
 * the trap layer knowing what a process is. */
static bool on_user_trap(struct trapframe *tf) {
    if ((tf->scause & SCAUSE_INTERRUPT) != 0) {
        return false; /* an interrupt: the trap layer deals with those */
    }
    if ((tf->scause & ~SCAUSE_INTERRUPT) == EXC_ECALL_FROM_U) {
        handle_syscall(tf);
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
    uspace_leave(running->context);
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
int64_t uspace_run(struct uproc *p) {
    if (!p->built) {
        return KERR_INVAL;
    }
    running = p;
    exited = false;
    exit_status = 0;
    syscalls = 0;
    trap_set_expected(on_user_trap);
    __asm__ volatile("csrr %0, satp" : "=r"(kernel_satp_saved));
    /* SUM so the kernel may read the process's memory for a syscall's
     * arguments. Without it a supervisor load from a user page faults —
     * which is a deliberate guard against dereferencing a user pointer by
     * accident, and is why it is turned on for exactly this window. */
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SUM));
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero"
                     :
                     : "r"(pgtable_satp(&p->pt, 1))
                     : "memory");
    uspace_enter(p->entry, p->stack_top, p->context);
    /* Reached only through uspace_leave. */
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero"
                     :
                     : "r"(kernel_satp_saved)
                     : "memory");
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SUM));
    trap_set_expected(NULL);
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
    int rc = elf_parse(image, bytes, USER_TOP, &img);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = pgtable_create(&p->pt, &mem);
    for (uint32_t i = 0; rc == KERR_OK && i < img.nloads; i++) {
        rc = load_segment(p, &img.loads[i], image);
    }
    rc = rc != KERR_OK ? rc : build_start_state(p, &img, path);
    rc = rc != KERR_OK ? rc : share_kernel_mappings(&p->pt);
    if (rc != KERR_OK) {
        return rc;
    }
    p->entry = img.entry;
    p->built = true;
    return KERR_OK;
}

uint64_t uspace_syscalls(void) {
    return syscalls;
}
