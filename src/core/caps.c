#include "core/caps.h"

cap_mask cap_all(void) {
    /* A shift of 64 is undefined, so the top capability is set separately
     * rather than by `(1 << (LAST + 1)) - 1` — which would be correct for
     * every value except the one that matters. */
    return (((cap_mask)1 << CAP_LAST_CAP) - 1u) | ((cap_mask)1 << CAP_LAST_CAP);
}

bool cap_valid(uint32_t cap) {
    return cap <= CAP_LAST_CAP;
}

cap_mask cap_bit(uint32_t cap) {
    return cap_valid(cap) ? (cap_mask)1 << cap : 0u;
}

bool cap_in(cap_mask set, uint32_t cap) {
    cap_mask bit = cap_bit(cap);
    return bit != 0 && (set & bit) != 0;
}

void cap_sets_init(struct cap_sets *out, uint32_t uid) {
    cap_mask everything = cap_all();
    /* **The bounding set is full for everybody**, root or not. It is a
     * ceiling on what a descendant may ever hold, not a grant: starting a
     * user's processes with an empty bounding set would mean no program
     * they ever run could be given a capability, including by root. */
    *out = (struct cap_sets){
        .permitted = uid == 0u ? everything : 0u,
        .effective = uid == 0u ? everything : 0u,
        .inheritable = 0u,
        .bounding = everything,
        .ambient = 0u,
    };
}

void cap_after_exec(struct cap_sets *sets, uint32_t euid_after, bool no_new_privs) {
    cap_mask keep = sets->bounding;
    if (no_new_privs) {
        /* **Nothing may be gained, so the ambient set goes too.** Keeping
         * it would let a process arrange to hold a capability across an
         * exec it had asked the kernel to make unprivileged, which is the
         * one thing this flag exists to prevent. */
        sets->ambient = 0u;
        sets->permitted &= keep;
        sets->effective = sets->permitted;
        return;
    }
    if (euid_after == 0u) {
        sets->permitted = keep;
        sets->effective = keep;
        return;
    }
    /* No file capabilities: what survives is what the ambient set carries,
     * bounded. An inheritable set with no ambient bits yields nothing, and
     * that is the correct answer rather than an oversight — it is why the
     * ambient set was added to Linux in the first place. */
    sets->ambient &= keep;
    sets->permitted = sets->ambient;
    sets->effective = sets->ambient;
}

bool cap_change_allowed(const struct cap_sets *now, const struct cap_sets *want, bool has_setpcap) {
    cap_mask defined = cap_all();
    /* An undefined bit is refused rather than ignored: a process that
     * believed it held capability 57 would be wrong in a way nothing here
     * could ever tell it. */
    if (((want->permitted | want->effective | want->inheritable | want->bounding |
          want->ambient) &
         ~defined) != 0u) {
        return false;
    }
    /* The effective set may hold nothing the permitted set does not. */
    if ((want->effective & ~want->permitted) != 0u) {
        return false;
    }
    /* Permitted may only shrink, which is what makes dropping privilege
     * mean something. */
    if ((want->permitted & ~now->permitted) != 0u) {
        return false;
    }
    /* The bounding set may only shrink, and only with `CAP_SETPCAP`. */
    if ((want->bounding & ~now->bounding) != 0u) {
        return false;
    }
    if (want->bounding != now->bounding && !has_setpcap) {
        return false;
    }
    /* Inheritable may gain only what is permitted now or already
     * inheritable — otherwise a process could hand a child a capability it
     * does not itself hold. */
    if ((want->inheritable & ~(now->inheritable | now->permitted)) != 0u) {
        return false;
    }
    /* Ambient may hold only what is both permitted and inheritable, which
     * is Linux's rule and the reason an ambient bit cannot outlive the
     * right to it. */
    if ((want->ambient & ~(want->permitted & want->inheritable)) != 0u) {
        return false;
    }
    return true;
}
