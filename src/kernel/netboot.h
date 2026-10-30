/* netboot (kernel) — the root filesystem over the network, before storage
 * attaches: the MKNB client (core/net/netboot.h) on the live interface,
 * the image in pages taken for good, handed to the disk layer as its RAM
 * disk. The server is `src/go/tools/mknbserver`.
 */
#ifndef MINIKERN_KERNEL_NETBOOT_H
#define MINIKERN_KERNEL_NETBOOT_H

#include <stddef.h>
#include <stdint.h>

#include "core/net/net.h"
#include "core/types.h"

/* Runs a session (pinned, when not 0, is the only address probed) and
 * leaves the image at *at, *len bytes, in physical pages kept for good.
 * `poll` drains the network device's receive ring, which the boot path
 * must do itself. KERR_OK or a negative errno; it logs its own progress
 * and the reason for a failure. */
int knetboot_fetch(ip4_t pinned, void (*poll)(void), phys_t *at, uint32_t *len);

/* Accepts only an image with this digest / offers signed by this key,
 * each 64 hex characters (`minikern.rootfs.sha256=`, `minikern.netboot.key=`).
 * KERR_INVAL for anything else: a pin that silently did not parse would
 * leave the boot looking protected without being so. */
int knetboot_expect(const char *hex, size_t len);
int knetboot_require_key(const char *hex, size_t len);

#endif
