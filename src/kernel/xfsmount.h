/* xfsmount — mounting the filesystem that is on the disk.
 *
 * The XFS implementation came across whole and host-tested against
 * filesystems `mkfs.xfs` made. What it has never had on this architecture is
 * a **real device** beneath it, and this is what connects the two: the
 * buffer cache over whichever storage adapter the board turned out to have
 * — NVMe, an SD card, or the emulator's virtio disk — and XFS over that.
 *
 * It mounts **read-only by default**, and that is not timidity. A
 * filesystem's log tells you whether it was unmounted cleanly, and this
 * implementation refuses a dirty one rather than replaying it: replaying
 * another implementation's log correctly is a larger piece of work than
 * writing one, and getting it subtly wrong corrupts a filesystem that was
 * merely dirty. Read-only is the state in which that refusal costs nothing.
 */
#ifndef MINIKERN_KERNEL_XFSMOUNT_H
#define MINIKERN_KERNEL_XFSMOUNT_H

#include <stdbool.h>
#include <stdint.h>

/* For `xfs_attr_fn`, the shape of an attribute listing's callback. This
 * header otherwise names no filesystem type, which is why it had no
 * filesystem include until attributes needed one. */
#include "core/xfs/xfs.h"

/* Mounts the filesystem on the attached disk. KERR_NODEV when there is no
 * disk, and whatever the filesystem says when there is one it will not
 * accept — which includes a dirty log, a feature set it does not fully
 * understand, and something that is not XFS at all. Each of those is a
 * different diagnosis and is reported as one. */
/* The mounted filesystem, for reporting counters only it holds. */
const struct xfs *xfsmount_fs(void);

int xfsmount_attach(void);

/* True once a filesystem is mounted. */
bool xfsmount_present(void);

/* Mounts the filesystem again from the disk, dropping the block cache
 * first so that it is read as a fresh boot would read it. */
int xfsmount_remount(void);

/* Leaves the next logged transaction without its unmount record, so that
 * the filesystem is left as a power cut would leave it. */
void xfsmount_leave_log_dirty(bool on);

/* Whether the mounted filesystem's log held uncommitted work, which is why
 * it is read-only. */
bool xfsmount_log_was_dirty(void);

/* Whether that dirty log was replayed, which is what makes the filesystem
 * writable again rather than merely readable. */
bool xfsmount_log_replayed(void);

/* Refuses to replay, so a test can show the read-only fallback still
 * works — what a kernel does when it meets a log item it cannot apply. */
void xfsmount_refuse_replay(bool on);

/* The filesystem's label, which is what decides where it is mounted. */
const char *xfsmount_label(void);

/* Walks the root directory and prints what is there, with sizes: the
 * cheapest proof that a filesystem has really been read rather than merely
 * opened. Returns how many entries it saw, or a negative error. */
int32_t xfsmount_list_root(void);

/* Reads a file whole and reports its first bytes and its length, which
 * proves the extent map and the data path rather than just the metadata. */
int xfsmount_show_file(const char *path);

/* Reads a file into memory the caller must free with `kfree`. Reports the
 * size. KERR_NOENT when it is not there, KERR_NOMEM when it does not fit,
 * KERR_FBIG when it is larger than `limit` — which is a refusal and not a
 * truncation, because half a program is not a program. */
/* A path's owner, group and mode. Reported, not enforced: the rule needs to
 * know who is asking, and this layer does not know about processes. */
int xfsmount_stat(const char *path, uint32_t *uid, uint32_t *gid, uint32_t *mode);

/* A path's mode, size and link count, which is what a `stat` needs and what
 * `xfsmount_stat` leaves out: it answers the ownership question the
 * permission checks ask, and a program wants to know how big a file is. */
int xfsmount_stat_full(const char *path, uint32_t *mode, uint64_t *size, uint32_t *nlink);

/* Lists any directory, not only the root. The callback is the filesystem's
 * own dirent shape; stopping early is a non-zero return, as `xfs_dir_list`
 * defines it. */
int xfsmount_list(const char *path, xfs_dirent_fn fn, void *self);

/* A page's worth, which is what the swap device moves. */
#define XFSMOUNT_PAGE_BYTES 4096u

/* Where each page of a file lives on the device, as byte offsets.
 *
 * It is what makes a swap **file** work on a filesystem this kernel mounts
 * read-only: a swapfile's blocks are allocated once by whoever created it
 * and only ever overwritten, which changes no metadata. A file with a hole
 * or an unwritten extent is refused — there is nowhere to put a page — so a
 * sparse file cannot be swap, which is also `swapon`'s rule on Linux.
 *
 * KERR_NODEV with no filesystem, KERR_INVAL for a file that is too small or
 * not fully allocated. */
int xfsmount_map_file(const char *path, uint64_t *offsets, uint32_t max, uint32_t *pages);

int xfsmount_read_file(const char *path, uint32_t limit, uint8_t **out, uint32_t *size);

/* Up to `len` bytes of a file at `off`, by path, through whichever
 * filesystem the path is on — without the whole file in memory, which is
 * what a read-write descriptor needs: a database is read a page at a time
 * from wherever the page is. `*got` is short only at the end of the file. */
int xfsmount_read_path_at(const char *path, uint64_t off, void *buf, uint32_t len,
                          uint32_t *got);

/* The largest file a range is taken from by reading it whole (a mount with
 * no ranged read). */
#define XFSMOUNT_RANGE_WHOLE_MAX (16u * 1024u * 1024u)

/* Hashes a file **without holding it in memory**, and reports its size.
 *
 * Reading a file whole is the obvious way to checksum one and it stops
 * working at the first file worth checksumming: a five-megabyte video on a
 * real drive is larger than this kernel's heap, so `xfsmount_read_file`
 * answers `KERR_FBIG` and proves nothing. This walks it in chunks, which
 * also means the extent list is followed to its end — the thing a single
 * first-block read never exercises. */
int xfsmount_hash_file(const char *path, uint8_t digest[32], uint64_t *size);

/* How the last mount's lookups were answered: through the directory's hash
 * index, or by walking every entry. Reported rather than asserted — a zero
 * in the first is the difference between an index that works and one that
 * is never reached. */
void xfsmount_lookup_counts(uint64_t *hashed, uint64_t *walked);

/* ── writing ───────────────────────────────────────────────────────────── */

/* Whether this filesystem may be written: it is mounted, the device is not
 * read-only, and it carries no feature that places obligations on a writer
 * this kernel does not meet. */
bool xfsmount_writable(void);

/* The write operations, on names **in the root directory**.
 *
 * Root-relative because that is what the write path can reach: adding and
 * removing entries is implemented for a directory that still fits in its
 * inode, and resolving a parent would promise more than the layer below
 * delivers. Enough to exercise every operation, which is what they had
 * never been — `xfs_create` had no caller at all before this.
 */
int xfsmount_create(const char *name, uint16_t mode);

/* Writes `bytes` into `leaf` inside the directory `dir` names, creating the
 * file if it is not there. **The directory must already exist**: this
 * writer has no `mkdir`, so a missing one is reported rather than created
 * (`KERR_NOENT`). */
int xfsmount_write_into(const char *dir, const char *leaf, const void *bytes, uint32_t len);

/* Writes at an offset into `dir/leaf`, creating the file if absent. This is
 * what a writable descriptor uses, so that it writes through rather than
 * holding a whole file in memory. */
int xfsmount_write_at(const char *dir, const char *leaf, uint64_t off, const void *bytes,
                      uint32_t len);

/* Makes `path` a directory, and every directory above it. An existing
 * component is not an error, which is what makes this safe on every boot. */
int xfsmount_mkdir_p(const char *path);
int xfsmount_write(const char *name, uint64_t off, const void *buf, uint32_t len);

/* Reads a range of a file in the root directory. The whole-file reader
 * needs the file to fit in one allocation, which a test that fills an
 * allocation group deliberately does not. */
int xfsmount_read_at(const char *name, uint64_t off, void *buf, uint32_t len);
/* Extended attributes of a file in the root directory. Names carry Linux's
 * namespace prefix (`user.`, `trusted.`, `security.`), which the filesystem
 * translates into the flag it stores. */
int xfsmount_attr_set(const char *name, const char *attr, const void *value,
                      uint32_t valuelen);
int xfsmount_attr_get(const char *name, const char *attr, void *out, uint32_t room,
                      uint32_t *valuelen);
int xfsmount_attr_remove(const char *name, const char *attr);
int xfsmount_attr_list(const char *name, xfs_attr_fn fn, void *self);

int xfsmount_link(const char *existing, const char *newname);
int xfsmount_rename(const char *from, const char *to);
int xfsmount_unlink(const char *name);

/* Removes a name given a full path, resolving its parent directory. Unlike
 * `xfsmount_unlink` it is not limited to the root. A correct directory is
 * refused (that is `rmdir`); one whose inode is broken is removed, which is
 * how a bad entry gets cleared. */
int xfsmount_unlink_path(const char *path);

/* Makes a symbolic link at `path` holding `target`.
 *
 * The target lives inside the inode, so it is bounded by the literal area —
 * KERR_NAMETOOLONG past that, rather than a half-written structure. */
int xfsmount_symlink(const char *path, const char *target);

/* A second name for an existing file. KERR_XDEV across volumes, because a
 * hard link is an inode number and that means nothing outside the
 * filesystem that issued it. */
int xfsmount_link_path(const char *from, const char *to);

/* The longest symlink target this mount can store, or zero when nothing is
 * mounted. A caller that must refuse one should ask rather than quote a
 * constant: the bound is the inode's literal area. */
uint32_t xfsmount_symlink_limit(void);

/* Sets a path's permission bits. The file-type bits are preserved, so this
 * cannot change what kind of thing something is. */
int xfsmount_chmod_path(const char *path, uint32_t mode);

/* Sets a path's owner and group; `XFS_OWNER_UNCHANGED` leaves one alone. */
int xfsmount_chown_path(const char *path, uint32_t uid, uint32_t gid);

/* Reads a path's owner and group, which `stat` does not report here. */
int xfsmount_owner_of(const char *path, uint32_t *uid, uint32_t *gid);

/* Writes every dirty buffer to the device. What `fsync` reaches: the cache
 * does not record which buffers belong to which inode, and a sync that
 * missed the inode while writing the data back would be worse than one that
 * writes more than it had to. */
int xfsmount_flush(void);

/* `fsync` for a path. This kernel's own XFS flushes its whole cache (see
 * xfsmount_flush). A mounted filesystem has nothing held here to flush:
 * netfs sends every write and waits for the server to answer it, so what
 * remains is the server's own durability, which no request can change. */
int xfsmount_sync_path(const char *path);

/* Makes a path's file `size` bytes, freeing the blocks it no longer needs. */
int xfsmount_truncate_path(const char *path, uint64_t size);

/* Installs the reader for the creating process's umask.
 *
 * Installed rather than called, because this module must not learn what a
 * process is: `uspace` depends on it for `execve`, so an include the other
 * way would invert the layering. The wall clock arrived here the same way.
 * Until one is installed the mask is zero and a create grants what it
 * always did. */
void xfsmount_set_umask_source(uint32_t (*source)(void));

/* `FITRIM` on the volume holding `path`: every free extent at least
 * `minlen` bytes long within [start, start + len) is discarded. *trimmed is
 * the bytes that reached. KERR_NOTSUP on FAT, or on a device that cannot
 * discard; KERR_INVAL for a range shorter than one block. */
int xfsmount_trim(const char *path, uint64_t start, uint64_t len, uint64_t minlen,
                  uint64_t *trimmed);

/* The volume holding `path`, as block and inode counts. */
int xfsmount_counters(const char *path, struct xfs_counters *out);

/* Sets a path's access and modification times. */
int xfsmount_utimes_path(const char *path, uint64_t atime_s, uint64_t mtime_s);

/* A file's access, modification and change times, in seconds; KERR_NOTSUP
 * off the root XFS's volumes. */
int xfsmount_times(const char *path, uint64_t *atime_s, uint64_t *mtime_s, uint64_t *ctime_s);

/* Moves a name within one volume. KERR_XDEV across two, because a rename is
 * not a copy. This is the mechanism a bounded log file rotates with: there
 * is no ftruncate either, so moving the file aside is the only option. */
int xfsmount_rename_path(const char *from, const char *to);

/* Reads a symbolic link's target into `out`, without a terminator.
 *
 * KERR_INVAL for anything that is not a link, which is what readlink(2)
 * answers: a caller handed a file's contents instead would have no way to
 * tell a link to "hello" from a file containing the word. */
int xfsmount_readlink(const char *path, char *out, uint32_t max, uint32_t *got);

/* A name's inode and its link count, which is how a test of `link` and
 * `unlink` checks what they did rather than that they returned 0. */
int xfsmount_inode_of(const char *name, uint64_t *ino, uint32_t *nlink);

/* Prints the root directory's shape. For a failure in the write path,
 * where "it stopped being a directory" needs to say which field says so. */
void xfsmount_root_shape(void);

/* Chunks of inodes the allocator had to add. Zero means a test that
 * created many files never actually needed one. */
uint64_t xfsmount_inode_chunks_added(void);

/* Leaves split because they filled: what makes a node directory grow
 * rather than merely exist. */
uint64_t xfsmount_leaf_splits(void);

/* Attribute forks that outgrew the inode and became a leaf block. */
uint64_t xfsmount_attrs_to_leaf(void);

/* Allocations that had to leave the group they asked for. */
uint64_t xfsmount_alloc_other_group(void);

/* Puts the allocator's old single-group behaviour back, so that a test can
 * show a full group refusing a write that the search satisfies. */
void xfsmount_single_group(bool on);

/* Directories that changed shape, by the shape reached. A zero in either is
 * the difference between growth that works and growth never reached. */
void xfsmount_growth_counts(uint64_t *to_block, uint64_t *to_leaf, uint64_t *to_node,
                            uint64_t *data_blocks);

#endif
