/* POSIX capabilities: the privileged things a process may do, one bit each.
 *
 * **Why this exists when uid 0 already works.** A kernel that asks "is the
 * effective uid zero?" has exactly two answers for every privileged
 * operation, so a program that needs to bind a low port must be allowed to
 * reboot the machine as well. Capabilities make the question narrower: the
 * same program holds `CAP_NET_BIND_SERVICE` and nothing else, and the
 * check at each site names what it actually needs.
 *
 * **uid 0 is not special in the check.** Root is given a full effective set
 * when it is created or exec'd, so `uspace_capable` asks the same question
 * for every process. Keeping the uid test as a second path would mean two
 * answers that can disagree, and the one that disagrees would be the one
 * nobody tested.
 *
 * The numbers are Linux's, because the ABI is: `capget` and `capset` carry
 * these bits and a program built for Linux expects `CAP_SYS_ADMIN` to be
 * 21. Inventing a numbering would make the syscalls useless.
 */
#ifndef CORE_CAPS_H
#define CORE_CAPS_H

#include <stdbool.h>
#include <stdint.h>

#define CAP_CHOWN            0u
#define CAP_DAC_OVERRIDE     1u
#define CAP_DAC_READ_SEARCH  2u
#define CAP_FOWNER           3u
#define CAP_FSETID           4u
#define CAP_KILL             5u
#define CAP_SETGID           6u
#define CAP_SETUID           7u
#define CAP_SETPCAP          8u
#define CAP_LINUX_IMMUTABLE  9u
#define CAP_NET_BIND_SERVICE 10u
#define CAP_NET_BROADCAST    11u
#define CAP_NET_ADMIN        12u
#define CAP_NET_RAW          13u
#define CAP_IPC_LOCK         14u
#define CAP_IPC_OWNER        15u
#define CAP_SYS_MODULE       16u
#define CAP_SYS_RAWIO        17u
#define CAP_SYS_CHROOT       18u
#define CAP_SYS_PTRACE       19u
#define CAP_SYS_PACCT        20u
#define CAP_SYS_ADMIN        21u
#define CAP_SYS_BOOT         22u
#define CAP_SYS_NICE         23u
#define CAP_SYS_RESOURCE     24u
#define CAP_SYS_TIME         25u
#define CAP_SYS_TTY_CONFIG   26u
#define CAP_MKNOD            27u
#define CAP_LEASE            28u
#define CAP_AUDIT_WRITE      29u
#define CAP_AUDIT_CONTROL    30u
#define CAP_SETFCAP          31u
#define CAP_MAC_OVERRIDE     32u
#define CAP_MAC_ADMIN        33u
#define CAP_SYSLOG           34u
#define CAP_WAKE_ALARM       35u
#define CAP_BLOCK_SUSPEND    36u
#define CAP_AUDIT_READ       37u
#define CAP_PERFMON          38u
#define CAP_BPF              39u
#define CAP_CHECKPOINT_RESTORE 40u
#define CAP_LAST_CAP         CAP_CHECKPOINT_RESTORE

/* A set of capabilities. 41 of the 64 bits are defined; the rest must stay
 * zero, because a process that held an undefined capability would be
 * holding something no check ever asks about — which reads as privilege
 * nobody can account for. */
typedef uint64_t cap_mask;

/* The four sets a process carries, as POSIX names them.
 *
 * - **permitted** is what it *may* put into its effective set.
 * - **effective** is what the checks actually consult.
 * - **inheritable** is what survives into a child's permitted set across
 *   `execve`, which is how a privilege is handed to a program rather than
 *   to a process.
 * - **bounding** is a ceiling that can only ever be lowered: dropping a
 *   capability from it means no descendant can regain it, which is the one
 *   thing that makes dropping privilege irreversible rather than polite.
 *
 * `ambient` is Linux's later addition and the reason unprivileged
 * inheritance works at all: without it, `inheritable` only takes effect for
 * a file that carries capability bits, and this kernel has no file
 * capabilities — so inheritable alone would be a set that never did
 * anything. */
struct cap_sets {
    cap_mask permitted;
    cap_mask effective;
    cap_mask inheritable;
    cap_mask bounding;
    cap_mask ambient;
};

/* Every capability this kernel defines. */
cap_mask cap_all(void);

/* The bit for one capability, or 0 for a number that names none. */
cap_mask cap_bit(uint32_t cap);

bool cap_valid(uint32_t cap);
bool cap_in(cap_mask set, uint32_t cap);

/* The sets a process starts with. Root gets everything; anybody else gets
 * nothing, and must be given capabilities deliberately. */
void cap_sets_init(struct cap_sets *out, uint32_t uid);

/* What the sets become across `execve`.
 *
 * **There are no file capabilities here**, so the rule reduces to its two
 * honest halves: a process that is root after the exec gets the whole
 * bounding set, and anybody else gets only what the ambient set carries.
 * Linux's full rule mixes in the executable's own stored capabilities,
 * which needs them to be stored somewhere — an extended attribute this
 * kernel reads but does not yet consult for this purpose. Pretending
 * otherwise would be a set computed from a number that is always zero.
 *
 * `no_new_privs` clears the ambient set and forbids gaining anything,
 * which is what makes it safe for an unprivileged process to install a
 * `seccomp` filter: a filter cannot become a way to confuse a setuid
 * program into granting privilege it would not otherwise give. */
void cap_after_exec(struct cap_sets *sets, uint32_t euid_after, bool no_new_privs);

/* Whether `want` may be set given what is permitted, and whether the
 * bounding set may be reduced to `want`.
 *
 * Both are the rules `capset` enforces, and both are "no new privilege":
 * the effective set may hold nothing the permitted set does not, the
 * permitted set may only shrink, and the bounding set may only shrink.
 * Returning the decision rather than applying it keeps the policy here and
 * the process table's mutation where the process table is. */
bool cap_change_allowed(const struct cap_sets *now, const struct cap_sets *want, bool has_setpcap);

#endif /* CORE_CAPS_H */
