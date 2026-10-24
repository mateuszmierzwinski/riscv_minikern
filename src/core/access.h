/* access — whether an identity may touch a file, as pure arithmetic.
 *
 * Three bits, three classes, one identity. It looks too small to put in its
 * own file until you notice that the rule is **exclusive**: the class is
 * chosen first and only that class's bits are consulted. A check that fell
 * through from a denied owner to the group bits would make a mode of 0077 —
 * "everybody except me" — mean its opposite, and that mode exists precisely
 * to be used that way.
 *
 * It is pure because it is a decision, and because the interesting cases are
 * the ones nobody types by hand: an owner denied by their own bits, a group
 * member whose group bits are narrower than other's, root against a mode of
 * 0000. None of those can be produced on a board without first building a
 * filesystem to hold them.
 */
#ifndef MINIKERN_CORE_ACCESS_H
#define MINIKERN_CORE_ACCESS_H

#include <stdbool.h>
#include <stdint.h>

/* What a caller wants to do, in the same bit positions the mode uses — so
 * that a want can be tested against a class's bits directly and no
 * translation table exists to get wrong. */
#define ACCESS_READ 4u
#define ACCESS_WRITE 2u
#define ACCESS_EXEC 1u

/* The uid that owns a fresh filesystem and whose processes start with a
 * full capability set. **It is no longer the thing the checks exempt**:
 * that is now `CAP_DAC_OVERRIDE`, which root holds by default and may drop.
 * The name is kept because "uid 0" appears in on-disk inodes and in the
 * `uproc` table, and calling it something else there would be worse. */
#define ACCESS_ROOT 0u

/* Whether an identity may do `want` to a file with these owners and mode.
 *
 * `want` may combine bits, and then **all** of them must be permitted: a
 * request to read and write is denied by a mode that allows only reading,
 * rather than being half-granted. */
bool access_permitted(uint32_t euid, uint32_t egid, uint32_t file_uid, uint32_t file_gid,
                      uint32_t mode, uint32_t want);

/* The same rule **without the root exemption**.
 *
 * This is the one the kernel uses, because the exemption belongs to a
 * capability rather than to a uid: a root process that has dropped
 * `CAP_DAC_OVERRIDE` must be refused by the mode bits like anybody else,
 * and a non-root process that holds it must pass. A function that exempted
 * uid 0 on its own could not express either.
 *
 * `access_permitted` keeps the exemption and stays the function to reach
 * for outside a process context — a check made on behalf of the kernel
 * itself, where there is no capability set to consult. */
bool access_permitted_strict(uint32_t euid, uint32_t egid, uint32_t file_uid, uint32_t file_gid,
                             uint32_t mode, uint32_t want);

#endif
