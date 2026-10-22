#include "arch/riscv/sbi.h"

#include "core/console.h"

#define SBI_SUCCESS 0L
#define SBI_BASE_PROBE_EXTENSION 3L
#define SBI_SRST_SYSTEM_RESET 0L
#define SBI_SRST_SHUTDOWN 0L
#define SBI_SRST_REASON_NONE 0L
#define SBI_TIME_SET_TIMER 0L
#define SBI_HSM_HART_START 0L
#define SBI_HSM_HART_STOP 1L
#define SBI_HSM_HART_STATUS 2L

struct sbi_ret sbi_call(long eid, long fid, long a0, long a1, long a2, long a3) {
    register long r_a0 __asm__("a0") = a0;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a3 __asm__("a3") = a3;
    register long r_a6 __asm__("a6") = fid;
    register long r_a7 __asm__("a7") = eid;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a3), "r"(r_a6), "r"(r_a7)
                     : "memory");
    return (struct sbi_ret){.error = r_a0, .value = r_a1};
}

void sbi_console_putchar(char c) {
    (void)sbi_call(SBI_EXT_LEGACY_PUTCHAR, 0, (unsigned char)c, 0, 0, 0);
}

bool sbi_probe_extension(long eid) {
    struct sbi_ret r = sbi_call(SBI_EXT_BASE, SBI_BASE_PROBE_EXTENSION, eid, 0, 0, 0);
    return r.error == SBI_SUCCESS && r.value != 0;
}

void sbi_shutdown(void) {
    struct sbi_ret r = sbi_call(SBI_EXT_SRST, SBI_SRST_SYSTEM_RESET, SBI_SRST_SHUTDOWN,
                                SBI_SRST_REASON_NONE, 0, 0);
    /* Only reached when the firmware declines. Said out loud, because the
     * ARM kernel's equivalent silently idled and looked like a hang. */
    kprintf("sbi: the firmware refused to power off (error %ld)\n", r.error);
}

void sbi_set_timer(uint64_t when) {
    (void)sbi_call(SBI_EXT_TIME, SBI_TIME_SET_TIMER, (long)when, 0, 0, 0);
}

long sbi_hart_start(uint64_t hartid, uint64_t start_addr, uint64_t opaque) {
    return sbi_call(SBI_EXT_HSM, SBI_HSM_HART_START, (long)hartid, (long)start_addr, (long)opaque, 0)
        .error;
}

long sbi_hart_stop(void) {
    return sbi_call(SBI_EXT_HSM, SBI_HSM_HART_STOP, 0, 0, 0, 0).error;
}

long sbi_hart_status(uint64_t hartid) {
    struct sbi_ret r = sbi_call(SBI_EXT_HSM, SBI_HSM_HART_STATUS, (long)hartid, 0, 0, 0);
    return r.error == SBI_SUCCESS ? r.value : r.error;
}

/* RFENCE: "RFNC" as four ASCII bytes, which is how the specification names
 * its extensions. */
#define SBI_EXT_RFENCE 0x52464E43u
#define SBI_RFENCE_SFENCE_VMA 1u

long sbi_remote_sfence_vma(uint64_t hart_mask, uint64_t hart_mask_base, uint64_t start,
                           uint64_t size) {
    struct sbi_ret r = sbi_call(SBI_EXT_RFENCE, SBI_RFENCE_SFENCE_VMA, (long)hart_mask,
                                (long)hart_mask_base, (long)start, (long)size);
    return r.error;
}

bool sbi_has_rfence(void) {
    return sbi_probe_extension(SBI_EXT_RFENCE) > 0;
}
