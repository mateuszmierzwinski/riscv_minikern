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

/* True when the firmware implements an extension. Asked before use, because
 * a kernel that assumes DBCN and finds legacy-only prints nothing at all
 * and looks dead. */
bool sbi_probe_extension(long eid);

/* Powers the machine off, or resets it. Does not return on success — and
 * reports the error if the firmware declines, rather than idling in silence. */
void sbi_shutdown(void);

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

#endif
