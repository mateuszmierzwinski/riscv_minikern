#!/usr/bin/env python3
"""Filesystem images with the shapes the reader has to cope with.

**Built here rather than kept somewhere**, because an image checked into a
tree is an image nobody can say how to remake, and `mkfs.xfs` is on the
machine that runs the board. Each one is a *shape*: a directory big enough
that its extent map stops fitting in the inode, a file big enough to span
blocks, contents a test can predict.

The payload is `0..255` repeated. It is deliberately neither zeroes nor
random: zeroes make a checksum that cannot tell a correct read from a read
of empty memory, and random bytes make an image that will not compress, so
the whole 320 MB has to cross the network. A repeating pattern is non-zero,
has a checksum anyone can compute, and gzips to nothing.

`mkfs.xfs` refuses a filesystem under 300 MB, which is why these are all
320 MB and why it matters that they are mostly holes.
"""
import os
import subprocess
import sys

PAYLOAD_BYTES = 65536
# 256 repetitions of 0..255, whose sum is 255*256/2 = 32640.
PAYLOAD_CHECKSUM = (PAYLOAD_BYTES // 256) * 32640

# The naming option is added per image, because a fixture may want a
# different directory block size.
MKFS = [
    "mkfs.xfs", "-q", "-f",
    "-m", "crc=1,finobt=1,reflink=0,rmapbt=0,bigtime=0,inobtcount=0",
    "-i", "sparse=0,size=512,nrext64=0",
    "-b", "size=4096",
]


def payload(at):
    """Writes the predictable payload, and returns its path."""
    path = os.path.join(at, "payload")
    with open(path, "wb") as f:
        f.write(bytes(i % 256 for i in range(PAYLOAD_BYTES)))
    return path


def big_directory(at, entries, dirblksize=None):
    """An image whose root directory has `entries` files in it.

    Past a few hundred entries the directory's own extent map moves into a
    b-tree (`XFS_DINODE_FMT_BTREE`) and the directory itself into leaf and
    then node format — the two shapes this reader refused outright until the
    b-tree walk existed. `xfs_db -c 'inode 96' -c 'p core.format'` is how to
    confirm a given count still produces one."""
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, f"proto{entries}")
    with open(proto, "w") as f:
        f.write("dummy\n0 0\nd--755 0 0\n")
        for i in range(1, entries + 1):
            f.write(f"p{i} ---644 0 0 {data}\n")
        f.write("$\n")
    tag = f"{entries}" if dirblksize is None else f"{entries}n{dirblksize}"
    img = os.path.join(at, f"dir{tag}.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    naming = ["-n", "ftype=1" if dirblksize is None else f"ftype=1,size={dirblksize}"]
    subprocess.run(MKFS + naming + ["-p", proto, img], check=True)
    return img


def small_directory(at):
    """An image whose root directory still fits inside its inode.

    The write path starts from a **shortform** directory — one that lives in
    the inode's literal area — and grows it from there, so the fixture is
    nearly empty and the test does the growing.

    It is also what the group-crossing test fills. `mkfs.xfs` here will not
    make a filesystem under 300 MB, and raising the group count to shrink a
    group makes it demand a 64 MB log that will not fit in one — so the
    groups are the default four of eighty megabytes, and the test writes its
    way out of the first. That is twenty thousand blocks, which costs
    nothing against a RAM disk and would be unreasonable against a card.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, "protosmall")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\nseed ---644 0 0 {data}\n$\n")
    img = os.path.join(at, "small.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    subprocess.run(MKFS + ["-n", "ftype=1", "-p", proto, img], check=True)
    return img


# Two filesystems on one disk, labelled so that automount has a decision to
# make. The sizes are forced by `mkfs.xfs` refusing anything under 300 MB.
VOLUME_MB = 320
VOLUME_FIRST_SECTOR = 2048  # 1 MiB in, which is where a partitioner starts


def two_labelled_volumes(at):
    """A partitioned image with a ROOT volume and a DATA volume.

    **This is the fixture the mount policy needs and nothing else provides.**
    A single filesystem cannot show the difference between "mounted at /
    because it is labelled ROOT" and "mounted at / because it is the only
    thing there" — and the second is what the kernel used to do. So there
    are two, labelled, and the DATA one carries a file that can only be
    reached through `/volumes/DATA`.

    Built without root: `mkfs.xfs` writes each filesystem into its own
    file, `sfdisk` writes a partition table into a third, and the two are
    copied in at their sector offsets. `losetup` would be the ordinary way
    and needs privileges this account does not have.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    parts = []
    for label in ("ROOT", "DATA"):
        proto = os.path.join(at, f"proto{label}")
        with open(proto, "w") as f:
            f.write(f"dummy\n0 0\nd--755 0 0\non{label} ---644 0 0 {data}\n$\n")
        one = os.path.join(at, f"vol{label}.img")
        with open(one, "wb") as f:
            f.truncate(VOLUME_MB * 1024 * 1024)
        subprocess.run(MKFS + ["-L", label, "-n", "ftype=1", "-p", proto, one], check=True)
        parts.append(one)
    img = os.path.join(at, "twovol.img")
    sectors = VOLUME_MB * 1024 * 1024 // 512
    with open(img, "wb") as f:
        f.truncate((VOLUME_FIRST_SECTOR + 2 * sectors + VOLUME_FIRST_SECTOR) * 512)
    script = (f"label: gpt\n"
              f"start={VOLUME_FIRST_SECTOR}, size={sectors}, name=ROOT\n"
              f"start={VOLUME_FIRST_SECTOR + sectors}, size={sectors}, name=DATA\n")
    subprocess.run(["sfdisk", "--quiet", img], input=script, text=True, check=True)
    with open(img, "r+b") as out:
        for i, one in enumerate(parts):
            out.seek((VOLUME_FIRST_SECTOR + i * sectors) * 512)
            with open(one, "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    out.write(chunk)
    return img


# A page file big enough to be worth having and small enough to stage: 64
# MiB is 16,384 slots, far more than any test here pages out.
SWAPFILE_MB = 64


def swap_volume(at):
    """An image carrying a **real** `/swapfile`.

    `swap_enable` resolves a swapfile's blocks once and then only ever
    overwrites them, so the file has to be *allocated* rather than sparse —
    a hole has nowhere to put a page, and the kernel refuses one. So the
    host file is written as real zeroes and `mkfs.xfs` copies it in, which
    is what `dd if=/dev/zero` does on a running machine and the same thing
    the operator did by hand on the SD card.

    Zeroes compress to nothing, so the 64 MiB inside a 320 MB image costs
    almost nothing to send to the board.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    swapfile = os.path.join(at, "swapfile")
    with open(swapfile, "wb") as f:
        f.write(b"\0" * (SWAPFILE_MB * 1024 * 1024))
    proto = os.path.join(at, "protoswap")
    with open(proto, "w") as f:
        f.write("dummy\n0 0\nd--755 0 0\n"
                f"seed ---644 0 0 {data}\n"
                f"swapfile ---600 0 0 {swapfile}\n$\n")
    img = os.path.join(at, "swap.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    subprocess.run(MKFS + ["-n", "ftype=1", "-p", proto, img], check=True)
    return img


def main():
    at = sys.argv[1] if len(sys.argv) > 1 else "/tmp/minikern-fixtures"
    entries = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    dirblksize = int(sys.argv[3]) if len(sys.argv) > 3 else None
    img = small_directory(at) if entries == 0 else big_directory(at, entries, dirblksize)
    print(img)
    print(f"payload: {PAYLOAD_BYTES} bytes, checksum {PAYLOAD_CHECKSUM}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
