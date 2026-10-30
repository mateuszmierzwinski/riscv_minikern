/* overlayfs — two directories mounted as one (`core/overlay.h` says how).
 *
 * `mount("overlay", target, "overlay", 0, "lowerdir=…,upperdir=…")`. Both
 * layers are ordinary paths, so either can be on the root XFS, a FAT volume
 * or a netfs export — the usual use is a read-only netfs root under a
 * writable directory on the board's own disk.
 *
 * Copy-up is in place, file by file (no workdir), and makes missing upper
 * directories 0755. Removing a directory is refused, as it is on every
 * filesystem this kernel writes; renaming one that exists in the lower
 * layer is `EXDEV`, as Linux answers without `redirect_dir`.
 */
#ifndef MINIKERN_KERNEL_OVERLAYFS_H
#define MINIKERN_KERNEL_OVERLAYFS_H

#define OVERLAY_MOUNTS 4u

/* Mounts an overlay at `target`. KERR_INVAL for bad options, a layer that
 * is not a directory, or a layer at, above or below the target (a lookup
 * that would find itself); KERR_BUSY for a layer that is itself an overlay;
 * KERR_NOMEM when every overlay slot is mounted. */
int overlayfs_mount(const char *data, const char *target);

#endif
