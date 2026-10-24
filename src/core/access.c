#include "core/access.h"

bool access_permitted(uint32_t euid, uint32_t egid, uint32_t file_uid, uint32_t file_gid,
                      uint32_t mode, uint32_t want) {
    /* The root exemption, and then the ordinary rule. Kept as a facade over
     * the strict form rather than as a second copy of the mode arithmetic,
     * so the two can never come to disagree about what 0640 means. */
    return euid == ACCESS_ROOT ||
           access_permitted_strict(euid, egid, file_uid, file_gid, mode, want);
}

bool access_permitted_strict(uint32_t euid, uint32_t egid, uint32_t file_uid, uint32_t file_gid,
                             uint32_t mode, uint32_t want) {
    uint32_t bits;
    if (want == 0) {
        return true; /* asking nothing is permitted; it is how existence is probed */
    }
    /* **The class is chosen, then its bits are the whole answer.** Falling
     * through from a denied owner to the group bits would make 0077 mean
     * the opposite of what it says. */
    if (euid == file_uid) {
        bits = (mode >> 6) & 7u;
    } else if (egid == file_gid) {
        bits = (mode >> 3) & 7u;
    } else {
        bits = mode & 7u;
    }
    /* Every requested bit, not any: a request to read and write is denied
     * by a mode that permits only one of them. */
    return (bits & want) == want;
}
