/* A kernel-level check of the structures this reader walks.
 *
 * **Run before a filesystem whose log is dirty is mounted**, which is the
 * case it exists for: a machine that lost power has metadata that is behind
 * its log, and the question is not "is this filesystem perfect" but "is it
 * sound enough to read". Answering it needs the structures looked at rather
 * than assumed, because the alternative — refusing every unclean
 * filesystem — makes a power cut as bad as a broken disk.
 *
 * What it is **not**: a repair. Nothing here writes. `xfs_repair` rebuilds
 * trees from the reverse-mapping data and second-guesses the allocator;
 * this reads each structure the kernel will later rely on and says whether
 * it holds together. The distinction matters because a check that quietly
 * "fixed" something would be the most dangerous code in the tree — on a
 * filesystem whose log says something newer happened, a fix is a guess
 * overwriting a fact.
 *
 * It is bounded by the group count rather than the filesystem's size: a few
 * blocks per group, so a 2 TB disk costs the same per group as a 320 MB
 * one. Walking every inode would be the thorough thing and would make boot
 * time a function of how full the disk is, which is the trade `xfs_repair`
 * makes and a boot path should not.
 */
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* What the pass accumulates: a count, and the first thing that was wrong.
 *
 * The count is reported rather than a bool because "this filesystem has one
 * bad group out of forty" and "nothing in it holds together" are different
 * situations, and a reader that printed only the first fault would make
 * them look alike. */
struct report {
    uint32_t faults;
    const char *what;
    uint32_t where;
};

static void fault(struct report *r, const char *what, uint32_t where) {
    r->what = r->faults == 0 ? what : r->what;
    r->where = r->faults == 0 ? where : r->where;
    r->faults++;
}

/* Verifies one of a group's headers: magic, checksum and UUID.
 *
 * All three, because each catches a different accident. A wrong magic is a
 * block that was never written or was written by something else; a bad
 * checksum is a block torn mid-write, which is exactly what a power cut
 * does; a foreign UUID is a block belonging to a *different* filesystem,
 * which is what a partition table edited by hand produces.
 *
 * **`sector` is an offset inside the group's first block, not a block to
 * add to it.** A group's headers share one filesystem block, one per
 * sector — so the address is `data + sector * sectsize`, and adding the
 * sector to the *block* number reads ordinary filesystem blocks instead.
 * Written the wrong way first, it reported twenty faults on a filesystem
 * `mkfs.xfs` had just made, which is the useful kind of wrong: a checker
 * that cries corruption on a good disk would have had its verdict ignored
 * on a bad one. */
static void header_ok(struct xfs *fs, uint32_t agno, const uint8_t *at, uint32_t magic,
                      uint32_t crc_off, uint32_t uuid_off, struct report *r,
                      const char *name) {
    if (xfs_be32(at) != magic || !xfs_crc_ok(fs, at, fs->sectsize, crc_off) ||
        !xfs_uuid_ok(fs, at + uuid_off)) {
        fault(r, name, agno);
    }
}

/* Verifies a b-tree root block exists and carries the magic it should.
 *
 * The root alone, not the tree. A descent would be the thorough check and
 * costs a read per node per group; the root is where a torn write lands
 * when a tree is grown, which is the failure this is looking for. */
static void tree_root_ok(struct xfs *fs, uint32_t agno, uint32_t block, uint32_t magic,
                         struct report *r, const char *name) {
    struct bbuf *b;
    if (block == 0 || block >= fs->agblocks) {
        fault(r, name, agno);
        return;
    }
    /* **At whatever height it is.** This used `xfs_tree_get`, which only
     * hands over a lone leaf — so any filesystem used long enough to grow a
     * tree past one block was reported faulty here. */
    if (xfs_meta_block(fs, ((uint64_t)agno << fs->agblklog) | block, magic, 0, XFS_BTREE_CRC,
                       XFS_BTREE_UUID, &b) != KERR_OK) {
        fault(r, name, agno);
        return;
    }
    bcache_put(fs->cache, b);
}

/* Checks the counts a group states about itself are possible.
 *
 * Not that they are *right* — proving that means walking the trees — but
 * that they are not impossible, which is what a torn header gives: more
 * free blocks than the group has, a longest free extent longer than the
 * group, more free inodes than inodes. Each of those would later be
 * believed by the allocator. */
static void counts_possible(struct xfs *fs, uint32_t agno, const uint8_t *agf,
                            const uint8_t *agi, struct report *r) {
    if (xfs_be32(agf + XFS_AGF_FREEBLKS) > fs->agblocks ||
        xfs_be32(agf + XFS_AGF_LONGEST) > fs->agblocks) {
        fault(r, "group free-space counts", agno);
    }
    if (xfs_be32(agi + XFS_AGI_FREECOUNT) > xfs_be32(agi + XFS_AGI_COUNT)) {
        fault(r, "group inode counts", agno);
    }
}

/* The trees the two headers root, and the counts they state. */
static void check_roots(struct xfs *fs, uint32_t agno, const uint8_t *agf, const uint8_t *agi,
                        struct report *r) {
    tree_root_ok(fs, agno, xfs_be32(agf + XFS_AGF_ROOTS), xfs_magic_for(fs, XFS_BLK_BNOBT), r,
                 "free space by block");
    tree_root_ok(fs, agno, xfs_be32(agf + XFS_AGF_ROOTS + 4u), xfs_magic_for(fs, XFS_BLK_CNTBT), r,
                 "free space by count");
    tree_root_ok(fs, agno, xfs_be32(agi + XFS_AGI_ROOT), xfs_magic_for(fs, XFS_BLK_INOBT), r, "inodes");
    /* No free-inode tree on v4, and `agi_free_root` is past the end of the
     * AGI as v4 defined it — so checking it would read whatever follows in
     * the sector and count a fault that does not exist. */
    if (fs->has_finobt) {
        tree_root_ok(fs, agno, xfs_be32(agi + XFS_AGI_FREE_ROOT),
                     xfs_magic_for(fs, XFS_BLK_FINOBT), r, "free inodes");
    }
    counts_possible(fs, agno, agf, agi, r);
}

/* One group: both its headers, the four b-tree roots they name, and the
 * counts they state.
 *
 * The roots and the counts are read *out of* the headers, so they are only
 * looked at once those headers have validated — taking a root block number
 * from a header that failed its checksum is reading a number somebody else
 * wrote. That ordering is the difference between a checker and a crash.
 *
 * The two headers are got separately: with 512-byte sectors they share the
 * group's first block, with 4 KiB sectors each has its own. */
static void check_group(struct xfs *fs, uint32_t agno, struct report *r) {
    struct bbuf *fb, *ib;
    uint8_t *agf, *agi;
    uint32_t before = r->faults;
    if (xfs_group_header(fs, agno, XFS_AGF_SECTOR, &fb, &agf) != KERR_OK) {
        fault(r, "the group's free-space header block", agno);
        return;
    }
    if (xfs_group_header(fs, agno, XFS_AGI_SECTOR, &ib, &agi) != KERR_OK) {
        bcache_put(fs->cache, fb);
        fault(r, "the group's inode header block", agno);
        return;
    }
    header_ok(fs, agno, agf, XFS_AGF_MAGIC, XFS_AGF_CRC, XFS_AGF_UUID, r,
              "group free-space header");
    header_ok(fs, agno, agi, XFS_AGI_MAGIC, XFS_AGI_CRC, XFS_AGI_UUID, r,
              "group inode header");
    if (r->faults == before) {
        check_roots(fs, agno, agf, agi, r);
    }
    bcache_put(fs->cache, ib);
    bcache_put(fs->cache, fb);
}

/* The root directory must read and must be a directory.
 *
 * Worth its own step: every path this kernel resolves starts here, so a
 * sound set of groups with an unreadable root is a filesystem that would
 * fail on its first lookup instead of at mount. */
/* `noinline` because it holds an inode, which is the largest object this
 * code puts on a stack. Folded into `xfs_check`'s frame the two together
 * cross the 1 KiB limit, and the limit is what stops a kernel stack being
 * quietly overrun. */
__attribute__((noinline)) static void check_root(struct xfs *fs, struct report *r) {
    struct xfs_inode root;
    if (xfs_inode_read(fs, fs->rootino, &root) != KERR_OK) {
        fault(r, "the root inode", 0);
        return;
    }
    if (!xfs_is_dir(&root)) {
        fault(r, "the root inode is not a directory", 0);
    }
}

int xfs_check(struct xfs *fs) {
    struct report r = {0, NULL, 0};
    /* **Nothing here returns a device error**, and that is deliberate. A
     * checker exists to count what is wrong, so a structure it cannot read
     * is a fault like any other — stopping at the first one would report a
     * filesystem with forty bad groups as a filesystem with one, and it was
     * written that way first: the test that caught it was asking for a
     * fault count and getting `-5`. */
    for (uint32_t agno = 0; agno < fs->agcount; agno++) {
        check_group(fs, agno, &r);
    }
    check_root(fs, &r);
    if (r.faults != 0) {
        kprintf("xfs: check found %u fault(s) across %u group(s); first: %s (group %u)\n",
                r.faults, fs->agcount, r.what, r.where);
        return KERR_DIRTY;
    }
    kprintf("xfs: check: %u group(s) sound, root readable\n", fs->agcount);
    return KERR_OK;
}
