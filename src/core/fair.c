#include "core/fair.h"

/* Nice -20 to 19, nice 0 at index 20. Each step is about 1.25x, which is
 * what makes one nice level worth roughly ten percent of the processor
 * between two competing threads — the property programs actually rely on.
 *
 * These are the values Linux uses, taken as an interface fact: a program
 * that asks for nice -5 is asking for a share, and a kernel that invented
 * its own table would answer a different question. Nothing is copied but
 * the numbers, which are the contract. */
static const uint32_t weights[FAIR_NICE_MAX - FAIR_NICE_MIN + 1] = {
    88761, 71755, 56483, 46273, 36291, /* -20 .. -16 */
    29154, 23254, 18705, 14949, 11916, /* -15 .. -11 */
    9548,  7620,  6100,  4904,  3906,  /* -10 ..  -6 */
    3121,  2501,  1991,  1586,  1277,  /*  -5 ..  -1 */
    1024,  820,   655,   526,   423,   /*   0 ..   4 */
    335,   272,   215,   172,   137,   /*   5 ..   9 */
    110,   87,    70,    56,    45,    /*  10 ..  14 */
    36,    29,    23,    18,    15,    /*  15 ..  19 */
};

int32_t fair_clamp_nice(int32_t nice) {
    if (nice < FAIR_NICE_MIN) {
        return FAIR_NICE_MIN;
    }
    return nice > FAIR_NICE_MAX ? FAIR_NICE_MAX : nice;
}

uint32_t fair_weight_of_nice(int32_t nice) {
    return weights[fair_clamp_nice(nice) - FAIR_NICE_MIN];
}

uint64_t fair_vruntime_delta(uint64_t ran_ns, uint32_t weight) {
    uint64_t w = weight == 0 ? FAIR_NICE0_WEIGHT : weight;
    if (w == FAIR_NICE0_WEIGHT) {
        return ran_ns; /* the common case, exactly, with no rounding */
    }
    /* **Split across the division**, because `ran_ns * 88761` overflows 64
     * bits after about two hours of accumulated runtime at the heaviest
     * weight, and a virtual clock that wraps makes the heaviest thread the
     * lightest. The remainder term keeps the result exact to the
     * nanosecond. */
    return (ran_ns / w) * FAIR_NICE0_WEIGHT + (ran_ns % w) * FAIR_NICE0_WEIGHT / w;
}

uint64_t fair_slice_ns(uint32_t weight, uint32_t total_weight, uint64_t target_ns,
                       uint64_t min_ns) {
    uint64_t share;
    if (weight == 0 || total_weight == 0 || weight >= total_weight) {
        /* Alone, or the only weight there is: the whole period is this
         * thread's, and the caller decides whether a timer is wanted at all
         * (with no competition, none is). */
        return target_ns < min_ns ? min_ns : target_ns;
    }
    share = target_ns / total_weight * weight + target_ns % total_weight * weight / total_weight;
    return share < min_ns ? min_ns : share;
}

uint64_t fair_wake_vruntime(uint64_t own, uint64_t minimum, uint64_t credit_ns) {
    uint64_t floor = minimum > credit_ns ? minimum - credit_ns : 0u;
    /* Forward only. A thread that has been asleep is behind, and taking the
     * floor is what stops it monopolising the processor to catch up; a
     * thread that is *ahead* keeps its own time, because the floor would be
     * a gift it has not earned. */
    return own > floor ? own : floor;
}
