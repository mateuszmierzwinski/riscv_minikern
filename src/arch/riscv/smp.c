#include "arch/riscv/smp.h"

#include "arch/riscv/sbi.h"
#include "core/console.h"
#include "core/errno.h"

static struct smp_hart harts[SMP_MAX_HARTS];
static uint32_t count;
static uint32_t running;

/* The hart id of whichever hart is executing this.
 *
 * There is **no CSR that answers it** from S-mode: `mhartid` is an M-mode
 * register, and reading it here is an illegal instruction — the same trap
 * that killed this project's first spike when it read `misa`. The firmware
 * passes the id in `a0` at entry, so each hart remembers its own, in a
 * variable indexed by nothing: `tp` is the obvious home for it, but `tp`
 * belongs to a user thread, so this uses a per-hart slot keyed by the value
 * itself being set once at entry. */
static uint64_t this_hart[SMP_MAX_HARTS];
static uint32_t slots_used;

void smp_set_this_hart(uint64_t hart) {
    /* Written into `sscratch`'s companion: the `tp` register, which is free
     * while the kernel is on its own stack and is restored from the trap
     * frame before any user code runs. One instruction to read back, which
     * matters because every lock acquisition will want it. */
    __asm__ volatile("mv tp, %0" : : "r"(hart));
    if (slots_used < SMP_MAX_HARTS) {
        this_hart[slots_used++] = hart;
    }
}

uint64_t smp_this_hart(void) {
    uint64_t hart;
    __asm__ volatile("mv %0, tp" : "=r"(hart));
    return hart;
}

/* Whether a hart's `mmu-type` is one this kernel can use. Sv48 and Sv57
 * harts can run Sv39 tables too — the mode is chosen by `satp`, not by the
 * hart — so anything at least as capable is accepted, and a hart with no
 * MMU at all is not. */
static bool mmu_is_usable(const char *type, uint32_t len) {
    const char *known[] = {"riscv,sv39", "riscv,sv48", "riscv,sv57"};
    for (uint32_t i = 0; i < 3u; i++) {
        uint32_t n = 0;
        while (known[i][n] != '\0') {
            n++;
        }
        if (len >= n + 1u) {
            bool same = true;
            for (uint32_t j = 0; j <= n; j++) {
                same = same && type[j] == known[i][j];
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

/* Records one `cpu@N` node. The hart id is its `reg`, which is **not** its
 * position in the node list: that is the whole reason this reads the tree
 * instead of counting. */
static void add_hart(const struct fdt *fdt, const char *path, uint64_t boot_hart) {
    struct smp_hart *h;
    struct fdt_prop reg, mmu;
    uint64_t id = 0;
    if (count == SMP_MAX_HARTS || fdt_get_prop(fdt, path, "reg", &reg) != KERR_OK ||
        fdt_prop_cells(&reg, 0, 1, &id) != KERR_OK) {
        return;
    }
    h = &harts[count++];
    *h = (struct smp_hart){.id = id};
    for (uint32_t i = 0; i < sizeof h->path - 1u && path[i] != '\0'; i++) {
        h->path[i] = path[i];
    }
    h->has_sv39 = fdt_get_prop(fdt, path, "mmu-type", &mmu) == KERR_OK &&
                  mmu_is_usable((const char *)mmu.value, mmu.len);
    h->startable = id != boot_hart && h->has_sv39;
    h->running = id == boot_hart;
    /* Before any other hart is started, so a plain increment is correct
     * here and only here. */
    running += h->running ? 1u : 0u;
}

int smp_enumerate(const struct fdt *fdt, uint64_t boot_hart) {
    char path[64];
    count = 0;
    running = 0;
    /* Found by `compatible`, not by walking `/cpus`'s children: every hart
     * node carries "riscv" as the last entry of its compatible list — after
     * its specific model, which on the target board is "sifive,u74-mc" —
     * and matching the generic name finds all of them on both the emulator
     * and the silicon. */
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        if (fdt_find_compatible_nth(fdt, "riscv", i, path, sizeof path) != KERR_OK) {
            break;
        }
        add_hart(fdt, path, boot_hart);
    }
    return count == 0 ? KERR_NOENT : KERR_OK;
}

/* How many times a hart failed to find itself. **Not nothing**: every
 * per-hart structure in the kernel is indexed by this, so a hart that is not
 * found and is handed index 0 shares another hart's scheduler state —
 * including which thread it is running. Two harts running one thread is
 * indistinguishable, from user space, from locks that do not work.
 *
 * Counted rather than assumed away, because the fallback used to be silent
 * and a silent wrong answer here is the most expensive kind. */
static uint32_t index_misses;

uint32_t smp_hart_misses(void) {
    return index_misses;
}

uint32_t smp_hart_index(void) {
    uint64_t me = smp_this_hart();
    for (uint32_t i = 0; i < count; i++) {
        if (harts[i].id == me) {
            return i;
        }
    }
    /* Before enumeration there is one hart and index 0 is right. After it,
     * this is a hart whose id is not in the table — which must not quietly
     * become hart 0. */
    if (count != 0) {
        __atomic_add_fetch(&index_misses, 1u, __ATOMIC_RELAXED);
    }
    return 0;
}

uint32_t smp_hart_count(void) {
    return count;
}

const struct smp_hart *smp_hart(uint32_t index) {
    return index < count ? &harts[index] : NULL;
}

uint32_t smp_running_count(void) {
    /* Read atomically because harts are still arriving while the boot hart
     * reports the total. */
    return __atomic_load_n(&running, __ATOMIC_SEQ_CST);
}

/* A hart reporting that it reached the kernel.
 *
 * **Every hart calls this at once, so none of it may be a plain
 * read-modify-write.** It used to claim the slot and then write `running++`,
 * and on four real U74s starting together increments were simply lost: the
 * atomic counter beside it in the composition root said three harts had
 * arrived while this one said two, and an earlier boot of the same binary
 * said three. A deficit that varies run to run is the signature.
 *
 * It is not a cosmetic counter. `sched_nprocs` reports it to user space,
 * which is where a Go runtime gets `NumCPU` — so the board ran Go on two of
 * its four cores and nothing looked wrong. QEMU never showed it, because
 * its harts arrive far enough apart.
 *
 * The slot is claimed with an exchange, which also makes a second call for
 * the same hart harmless. */
void smp_secondary_arrived(uint64_t hart) {
    for (uint32_t i = 0; i < count; i++) {
        if (harts[i].id != hart) {
            continue;
        }
        if (!__atomic_exchange_n(&harts[i].running, true, __ATOMIC_SEQ_CST)) {
            __atomic_add_fetch(&running, 1u, __ATOMIC_SEQ_CST);
        }
        return;
    }
}

uint32_t smp_start_others(phys_t entry, uint64_t opaque) {
    uint32_t started = 0;
    for (uint32_t i = 0; i < count; i++) {
        long rc;
        if (!harts[i].startable) {
            continue;
        }
        rc = sbi_hart_start(harts[i].id, entry, opaque);
        if (rc != 0) {
            /* Logged and skipped. A machine with fewer cores still works,
             * and a hart the firmware will not start is a fact about the
             * firmware rather than a reason to stop. */
            kprintf("smp: hart %llu refused to start (sbi %ld)\n",
                    (unsigned long long)harts[i].id, rc);
            continue;
        }
        started++;
    }
    return started;
}
