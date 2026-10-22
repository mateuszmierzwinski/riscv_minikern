/* ed25519 — verifying a signature, and only verifying one.
 *
 * There is no signing here and there never should be: signing needs a
 * secret, and a secret in a kernel image that is handed out is not a
 * secret. The private key lives on the machine that signs the netboot
 * offers; this side only ever checks them (NETBOOT.md).
 *
 * Written from scratch like everything else in this tree — no third-party
 * code, not even for cryptography (CLAUDE.md §2) — in the plain way rather
 * than the quick one, and checked against signatures Go's `crypto/ed25519`
 * produced. It does not need to be constant-time: a signature, a public
 * key and a message are all public, and there is no secret here to leak.
 */
#ifndef MINIKERN_LIB_ED25519_H
#define MINIKERN_LIB_ED25519_H

#include <stdbool.h>
#include <stdint.h>

#define ED25519_PUBLIC_KEY 32u
#define ED25519_SIGNATURE  64u

/* True when sig is a signature of msg under key. False for every kind of
 * wrong: a key that is not a point, an R that is not a point, an S at or
 * above the group order, or an honest signature of something else. */
bool ed25519_verify(const uint8_t key[ED25519_PUBLIC_KEY], const uint8_t sig[ED25519_SIGNATURE],
                    const void *msg, uint32_t len);

#endif
