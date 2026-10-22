#include "arch/riscv/timer.h"

#include "arch/riscv/sbi.h"
#include "core/errno.h"

/* The supervisor interrupt-enable bit for the timer. */
#define SIE_STIMER (1ull << 5)

static struct {
    uint64_t frequency;  /* counter ticks per second, from the device tree */
    uint64_t interval;   /* counter ticks between ticks of ours */
    uint64_t next;       /* the deadline currently armed */
    uint64_t ticks;
    uint32_t hz;
    bool up;
} timer;

uint64_t timer_now(void) {
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

int timer_init(const struct fdt *fdt, uint32_t hz) {
    uint64_t freq = fdt_get_u32(fdt, "/cpus", "timebase-frequency", 0);
    if (freq == 0 || hz == 0) {
        return KERR_NOENT;
    }
    timer.frequency = freq;
    timer.hz = hz;
    timer.interval = freq / hz;
    timer.interval = timer.interval == 0 ? 1 : timer.interval;
    timer.next = timer_now() + timer.interval;
    timer.up = true;
    sbi_set_timer(timer.next);
    /* Enabling the source is separate from enabling interrupts at all: the
     * caller decides when the kernel is ready to be interrupted. */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER));
    return KERR_OK;
}

void timer_rearm(void) {
    /* From the deadline that passed, not from now: a handler that took a
     * while must not push the next tick out by however long it took, or the
     * clock drifts by exactly the kernel's own cost. */
    timer.next += timer.interval;
    if (timer.next < timer_now()) {
        /* Unless it fell so far behind that catching up would mean a storm
         * of back-to-back ticks. Then the clock has lost time, which is
         * honest, rather than spending the next second making it up. */
        timer.next = timer_now() + timer.interval;
    }
    sbi_set_timer(timer.next);
}

void timer_on_tick(void) {
    timer.ticks++;
    timer_rearm();
}

uint64_t timer_ns(void) {
    uint64_t now = timer_now();
    return timer.frequency == 0 ? 0 : (now / timer.frequency) * 1000000000ull +
                                          (now % timer.frequency) * 1000000000ull / timer.frequency;
}

uint64_t timer_ticks(void) {
    return timer.ticks;
}

uint32_t timer_hz(void) {
    return timer.hz;
}

uint64_t timer_frequency(void) {
    return timer.frequency;
}

bool timer_ready(void) {
    return timer.up;
}
