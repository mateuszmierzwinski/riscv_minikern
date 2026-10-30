/* netfsmount — a netfs export as a vfs filesystem, mounted at a path
 * (`/net`, or `/` to run the whole system off the workstation).
 *
 * The connection is the composition root's (kernel/netfs_tcp.c); this file
 * only adapts the client to the vfs port.
 */
#ifndef MINIKERN_KERNEL_NETFSMOUNT_H
#define MINIKERN_KERNEL_NETFSMOUNT_H

#include "core/netfs.h"

/* Mounts nf at `at`: whatever `vfs_mount` says. */
int netfsmount_attach(struct netfs *nf, const char *at);

#endif
