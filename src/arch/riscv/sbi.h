/* sbi — the Supervisor Binary Interface: the firmware under the kernel.
 *
 * This one interface replaces three separate things the ARM kernel had to
 * drive itself, and it is the single largest simplification of this port:
 *
 *   * **power** — PSCI's SYSTEM_OFF, which on that board was implemented by
 *     U-Boot and simply *refused* when the kernel asked;
 *   * **bringing other cores up** — PSCI CPU_ON, which started a secondary
 *     in whatever mode the primary was entered in, so a core that skipped a
 *     HYP-to-SVC drop configured the hypervisor's registers instead of its
 *     own and died on its first instruction fetch;
 *   * **the timer** — the generic timer's comparator, per core.
 *
 * Here all three are an `ecall` away, implemented by firmware that is
 * present on every RISC-V machine by specification, and modelled by the
 * emulator. There is no second path and no board-specific variant.
 *
 * Calls return `struct sbi_ret` — an error and a value — because the spec
 * does, and because a kernel that ignores the error is how "the firmware
 * refused" becomes "the machine hung".
 */
#ifndef MINIKERN_ARCH_RISCV_SBI_H
#define MINIKERN_ARCH_RISCV_SBI_H

#include <stdbool.h>
#include <stdint.h>

/* Extension ids. The legacy ones are single-purpose calls kept for the
 * console; the rest are the versioned extensions. */
#define SBI_EXT_LEGACY_PUTCHAR 0x01u
#define SBI_EXT_LEGACY_GETCHAR 0x02u
#define SBI_EXT_BASE           0x10u
#define SBI_EXT_TIME           0x54494D45u /* "TIME" */
#define SBI_EXT_IPI            0x735049u   /* "sPI" */
#define SBI_EXT_RFENCE         0x52464E43u /* "RFNC" */
#define SBI_EXT_HSM            0x48534Du   /* "HSM" */
#define SBI_EXT_SRST           0x53525354u /* "SRST" */
#define SBI_EXT_DBCN           0x4442434Eu /* "DBCN" */

struct sbi_ret {
    long error;
    long value;
};

/* The raw call. Six arguments is what the specification allows. */
struct sbi_ret sbi_call(long eid, long fid, long a0, long a1, long a2, long a3);

/* ── the calls this kernel makes ─────────────────────────────────────────── */

/* Writes one character to the firmware's console. Used only until the
 * kernel owns the UART itself: it is a trap per character, which is fine
 * for a banner and wrong for a terminal. */
void sbi_console_putchar(char c);

/* One character from the firmware's console, or -1 when none is waiting.
 * Polled; the legacy call, which is the only console read that is
 * universally present. */
int sbi_console_getchar(void);

/* True when the firmware implements an extension. Asked before use, because
 * a kernel that assumes DBCN and finds legacy-only prints nothing at all
 * and looks dead. */
bool sbi_probe_extension(long eid);

/* Powers the machine off, or resets it. Does not return on success — and
 * reports the error if the firmware declines, rather than idling in silence. */
void sbi_shutdown(void);

/* Restarts the machine through the firmware's cold-reset request.
 *
 * Separate from `sbi_shutdown` because they are different requests to the
 * firmware and a caller asking to reboot must not be powered off instead —
 * a board that goes dark when it was told to restart needs someone to walk
 * to it, which on a development board is the whole cost. */
void sbi_reboot(void);

/* The timer: one absolute deadline per hart, against the `time` CSR. */
void sbi_set_timer(uint64_t when);

/* Starts a hart at a physical address with an opaque value in a1. This is
 * CPU_ON's replacement, and unlike CPU_ON it cannot start the core in the
 * wrong privilege mode: the specification says S-mode, always. */
long sbi_hart_start(uint64_t hartid, uint64_t start_addr, uint64_t opaque);

/* Stops the calling hart. The firmware owns it afterwards, and
 * `sbi_hart_start` brings it back. */
long sbi_hart_stop(void);

/* A hart's state: started, stopped, or somewhere in between. */
long sbi_hart_status(uint64_t hartid);

/* Makes every other hart discard its cached translations.
 *
 * **There is no broadcast TLB flush instruction on this architecture.**
 * `sfence.vma` affects only the hart that executes it, so a kernel that
 * changes a page table on one hart leaves every other hart reading through
 * whatever it had cached. The firmware's RFENCE extension does the
 * cross-hart part — it sends the other harts an interrupt and they fence
 * themselves — and it is the only portable way to do it: an IPI and a
 * hand-written handler would be the same mechanism, written twice.
 *
 * `hart_mask_base` of -1 means every hart, which is what a kernel wants: a
 * thread can be anywhere, so there is no subset that is safe to skip.
 *
 * This is the one piece of SMP that is invisible until it is wrong. A stale
 * translation does not fault — it reads the **wrong page**, so a program
 * gets a value that was never written and faults later somewhere else, on
 * an address that means nothing. */
long sbi_remote_sfence_vma(uint64_t hart_mask, uint64_t hart_mask_base, uint64_t start,
                           uint64_t size);

/* Whether the firmware offers RFENCE. A machine without it cannot be made
 * safe for more than one hart by this kernel, and says so rather than
 * running on luck. */
bool sbi_has_rfence(void);

/* Raises a software interrupt on the harts a mask names, the same way
 * `sbi_remote_sfence_vma` names them: `hart_mask_base` of -1 means all.
 *
 * It is how one hart tells another that something has changed — a thread it
 * could run, a mapping it must forget. A scheduler that only ever waits for
 * the next tick answers at the tick's resolution; one that can poke a hart
 * answers at this call's. */
long sbi_send_ipi(uint64_t hart_mask, uint64_t hart_mask_base);

#endif
