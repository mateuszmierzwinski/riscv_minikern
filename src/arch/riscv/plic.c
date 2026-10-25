#include "arch/riscv/plic.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

/* The register map. Priorities and pending bits are global; the enable
 * bitmap, the threshold and the claim register are per context. */
#define PLIC_PRIORITY(irq) (4u * (irq))
#define PLIC_ENABLE(ctx) (0x2000u + 0x80u * (ctx))
#define PLIC_THRESHOLD(ctx) (0x200000u + 0x1000u * (ctx))
#define PLIC_CLAIM(ctx) (0x200004u + 0x1000u * (ctx))
#define PLIC_SPAN 0x400000u

/* The privilege levels an `interrupts-extended` entry names. */
#define IRQ_M_EXTERNAL 11u
#define IRQ_S_EXTERNAL_NUM 9u

static struct {
    volatile uint32_t *regs;
    phys_t base;
    uint32_t context;
    bool up;
} plic;

static void write_reg(uint32_t off, uint32_t v) {
    plic.regs[off / 4u] = v;
}

static uint32_t read_reg(uint32_t off) {
    return plic.regs[off / 4u];
}

/* Works out this hart's supervisor context.
 *
 * `interrupts-extended` on the PLIC node is a list of (phandle, cause)
 * pairs, one per context in order: the phandle names a hart's interrupt
 * controller and the cause says which privilege level. So the context index
 * is the position in that list, and finding this hart's means counting
 * entries — **not** assuming it equals the hart id. On a machine where the
 * firmware holds M-mode contexts for a monitor core, those differ, and a
 * kernel that guesses writes another hart's enable bits.
 *
 * Runs to 28 lines: it is one scan of one list, and splitting the counting
 * from the matching would need the list, the index, the hart and the answer
 * passed between them (Rule 11, Parameters). */
static int find_context(const struct fdt *fdt, const char *path, uint64_t hart, uint32_t *ctx) {
    struct fdt_prop p;
    uint32_t entries, found = PLIC_MAX_SOURCES;
    int rc = fdt_get_prop(fdt, path, "interrupts-extended", &p);
    if (rc != KERR_OK) {
        return rc;
    }
    entries = p.len / 8u; /* two cells per context */
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t phandle = 0, cause = 0;
        char hart_path[64];
        uint64_t that_hart = 0;
        if (fdt_prop_cells(&p, i * 2u, 1, &phandle) != KERR_OK ||
            fdt_prop_cells(&p, i * 2u + 1u, 1, &cause) != KERR_OK) {
            return KERR_INVAL;
        }
        if (cause != IRQ_S_EXTERNAL_NUM) {
            continue; /* an M-mode context: the firmware's, not ours */
        }
        /* The phandle names a hart's *interrupt controller*; the hart id is
         * the `reg` of its parent, the `cpu@N` node. */
        if (fdt_find_phandle(fdt, (uint32_t)phandle, hart_path, sizeof hart_path) != KERR_OK ||
            !fdt_parent_path(hart_path)) {
            continue;
        }
        that_hart = fdt_get_u32(fdt, hart_path, "reg", 0xffffffffu);
        if (that_hart == hart) {
            found = i;
            break;
        }
    }
    *ctx = found;
    return found == PLIC_MAX_SOURCES ? KERR_NOENT : KERR_OK;
}

int plic_init(const struct fdt *fdt, uint64_t hart) {
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    uint32_t ctx = 0;
    int rc = fdt_find_compatible(fdt, "riscv,plic0", path, sizeof path);
    if (rc != KERR_OK) {
        rc = fdt_find_compatible(fdt, "sifive,plic-1.0.0", path, sizeof path);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    rc = fdt_device_reg(fdt, path, 0, &base, &size);
    rc = rc != KERR_OK ? rc : find_context(fdt, path, hart, &ctx);
    if (rc != KERR_OK) {
        return rc;
    }
    {
        /* **The mapping says where the registers are**; the physical base is
         * kept only for what has to name it to somebody else. */
        void *at = mmu_map_device(base, size < PLIC_SPAN ? PLIC_SPAN : size);
        if (at == NULL) {
            return KERR_NOMEM;
        }
        plic.regs = (volatile uint32_t *)at;
    }
    plic.base = base;
    plic.context = ctx;
    /* A threshold of zero lets everything through that has a priority above
     * it, which is every source this kernel enables. */
    write_reg(PLIC_THRESHOLD(ctx), 0);
    /* **The hart has to be willing to take an external interrupt**, and
     * nothing here ever said so: `sie` was only ever given the timer and
     * software bits, so bit 9 stayed clear and no external interrupt could
     * be taken at all.
     *
     * Everything downstream of that bit was already written and correct —
     * the claim-and-complete, `irq_dispatch`, the trap's `IRQ_S_EXTERNAL`
     * case, and the one driver that registers a handler. None of it had
     * ever run. The symptom was the quietest possible one: a device whose
     * interrupt was armed, unmasked and never delivered, which looks
     * exactly like hardware that does not raise.
     *
     * Here rather than in `timer.c` because this is the controller that
     * makes an external interrupt mean anything, and a hart that enabled
     * the bit before the controller existed would take an interrupt it
     * could not claim. */
    __asm__ volatile("csrs sie, %0" : : "r"(1ull << 9) : "memory");
    plic.up = true;
    return KERR_OK;
}

void plic_enable(uint32_t irq) {
    if (plic.up && irq < PLIC_MAX_SOURCES) {
        uint32_t off = PLIC_ENABLE(plic.context) + (irq / 32u) * 4u;
        write_reg(PLIC_PRIORITY(irq), 1); /* above the threshold, which is 0 */
        write_reg(off, read_reg(off) | (1u << (irq % 32u)));
    }
}

void plic_disable(uint32_t irq) {
    if (plic.up && irq < PLIC_MAX_SOURCES) {
        uint32_t off = PLIC_ENABLE(plic.context) + (irq / 32u) * 4u;
        write_reg(off, read_reg(off) & ~(1u << (irq % 32u)));
    }
}

uint32_t plic_claim(void) {
    return plic.up ? read_reg(PLIC_CLAIM(plic.context)) : 0;
}

void plic_complete(uint32_t irq) {
    if (plic.up) {
        write_reg(PLIC_CLAIM(plic.context), irq);
    }
}

phys_t plic_base(void) {
    return plic.base;
}

uint32_t plic_context(void) {
    return plic.context;
}

bool plic_ready(void) {
    return plic.up;
}
