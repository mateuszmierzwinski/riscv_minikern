/* netfs_tcp — the netfs client's transport over the kernel's own TCP, and
 * the mount it serves (`minikern.netfs=<ip>:<port>`, at `/net`, or at `/`
 * with `minikern.netroot`).
 */
#ifndef MINIKERN_KERNEL_NETFS_TCP_H
#define MINIKERN_KERNEL_NETFS_TCP_H

#include <stddef.h>

/* Parses "a.b.c.d:port" (len bytes), connects, and mounts the export at
 * `at`. `poll` drains the network device's receive ring, which a wait
 * outside any thread must do itself. Says what happened either way.
 * KERR_OK, KERR_INVAL for a bad spec, or the connection's error. */
int netfs_tcp_mount(const char *spec, size_t len, const char *at, void (*poll)(void));

/* The `mount` syscall's hook: fstype "netfs", source "a.b.c.d:port". A
 * second mount of the same server shares its connection; a different
 * server is KERR_BUSY. Anything else is KERR_NODEV. */
int netfs_tcp_mount_hook(const char *fstype, const char *source, const char *target);

#endif
