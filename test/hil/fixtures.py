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
import shutil
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


def small_directory_defaults(at):
    """`small_directory`, made the way anyone would make it: `mkfs.xfs` with
    **no feature options at all**.

    Its defaults turn on what the fixture above turns off — the reverse-map
    tree, reflink, sparse inodes, 64-bit extent counts, big timestamps — and
    every write the kernel makes then has to keep the reverse map exact.
    The host's `xfs_write_test` judges those writes with `xfs_repair`; this
    runs the same writer on the board, through the kernel's own self-test."""
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, "protodefaults")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\nseed ---644 0 0 {data}\n$\n")
    img = os.path.join(at, "defaults.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    subprocess.run(["mkfs.xfs", "-q", "-f", "-p", proto, img], check=True)
    return img


# Two filesystems on one disk, labelled so that automount has a decision to
# make. The sizes are forced by `mkfs.xfs` refusing anything under 300 MB.
VOLUME_MB = 320
VOLUME_FIRST_SECTOR = 2048  # 1 MiB in, which is where a partitioner starts


def _labelled_xfs(at, label, data):
    """One XFS volume file labelled `label`, carrying `/on<label>`."""
    proto = os.path.join(at, f"proto{label}")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\non{label} ---644 0 0 {data}\n$\n")
    one = os.path.join(at, f"vol{label}.img")
    with open(one, "wb") as f:
        f.truncate(VOLUME_MB * 1024 * 1024)
    subprocess.run(MKFS + ["-L", label, "-n", "ftype=1", "-p", proto, one], check=True)
    return one


def _boot_fat(at):
    """A FAT32 boot partition file holding a stand-in `minikern.bin`."""
    one = os.path.join(at, "volBOOT.img")
    with open(one, "wb") as f:
        f.truncate(BOOT_MB * 1024 * 1024)
    subprocess.run(["mkfs.vfat", "-F", "32", "-n", "BOOT", one], check=True,
                   stdout=subprocess.DEVNULL)
    kernel = os.path.join(at, "minikern.bin")
    with open(kernel, "wb") as f:
        f.write(b"not a kernel, only a file in the boot partition\n")
    subprocess.run(["mcopy", "-i", one, kernel, "::minikern.bin"], check=True)
    return one


def _gpt_image(img, volumes):
    """Lays `volumes` — (name, file) pairs, in partition order — into a GPT
    image at consecutive sector offsets, without root: `sfdisk` writes the
    table into the file and each volume is copied in at its offset."""
    first, lines = VOLUME_FIRST_SECTOR, []
    for name, one in volumes:
        sectors = os.path.getsize(one) // 512
        lines.append(f"start={first}, size={sectors}, name={name}\n")
        first += sectors
    with open(img, "wb") as f:
        f.truncate((first + VOLUME_FIRST_SECTOR) * 512)
    subprocess.run(["sfdisk", "--quiet", img], input="label: gpt\n" + "".join(lines),
                   text=True, check=True)
    with open(img, "r+b") as out:
        at = VOLUME_FIRST_SECTOR
        for _, one in volumes:
            out.seek(at * 512)
            with open(one, "rb") as src:
                shutil.copyfileobj(src, out, 1 << 20)
            at += os.path.getsize(one) // 512
    return img


def two_labelled_volumes(at):
    """A partitioned image with a ROOT volume and a DATA volume.

    **This is the fixture the mount policy needs and nothing else provides.**
    A single filesystem cannot show the difference between "mounted at /
    because it is labelled ROOT" and "mounted at / because it is the only
    thing there". So there are two, labelled, and the DATA one carries a file
    that can only be reached through `/volumes/DATA`.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    volumes = [(label, _labelled_xfs(at, label, data)) for label in ("ROOT", "DATA")]
    return _gpt_image(os.path.join(at, "twovol.img"), volumes)


# A boot partition big enough to hold a kernel and small enough to stage.
BOOT_MB = 64


def boot_partition_first(at):
    """**FAT as partition 1**, then ROOT, then DATA: the layout a disk that
    boots locally has, because U-Boot cannot read XFS (docs/BOOTING.md).

    The root must be found by its label, not by being partition 1 — the
    kernel used to mount partition 1 as the root and would have tried the
    FAT here.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    volumes = [("BOOT", _boot_fat(at))]
    volumes += [(label, _labelled_xfs(at, label, data)) for label in ("ROOT", "DATA")]
    return _gpt_image(os.path.join(at, "bootfirst.img"), volumes)


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


# An external log has to be at least this big, which `mkfs.xfs` enforces
# and is the reason the log device dwarfs what it holds.
EXTLOG_MB = 80


def v4_filesystem(at):
    """A **real v4 filesystem**, the pre-checksum on-disk format.

    Built by `mkfs.xfs -m crc=0`, which still works here and prints a
    deprecation warning — so this is the genuine older layout rather than a
    v5 image with its version number altered, which is the only way to test
    the shorter headers and absent checksums for real.

    The feature flags live inside `versionnum` on v4 rather than in separate
    words, and this image comes out as `0xb4a4`: v2 directories, v2 inodes,
    aligned allocation, a v2 log and extent flags. `-n ftype=1` is left off
    because the file-type byte is a v5-era feature and a v4 entry is one
    byte shorter without it.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, "protov4")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\nseed ---644 0 0 {data}\n$\n")
    img = os.path.join(at, "v4.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    subprocess.run(
        ["mkfs.xfs", "-q", "-f", "-m", "crc=0", "-b", "size=4096", "-p", proto, img],
        check=True,
    )
    return img


def v4_filesystem_big_inodes(at):
    """The same v4 format, with **512-byte inodes like the v5 fixture**.

    This exists so that the write case compares the *format* and nothing
    else. `mkfs.xfs` defaults to 256-byte inodes, which leaves a v4 inode a
    156-byte literal area — nine extents — against the 336 bytes a
    512-byte inode gives. A directory growing past nine extents needs a
    b-tree data fork, which this writer refuses by design, so the default
    image stops early for a reason that has nothing to do with v4.

    The default-size image is still used by the read case, and is what
    found the bug this bound now prevents: the directory writer limited the
    extents it would write by the size of a local array rather than by what
    the inode holds, which overflows on **any** small inode, v5 included.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, "protov4big")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\nseed ---644 0 0 {data}\n$\n")
    img = os.path.join(at, "v4big.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    subprocess.run(
        ["mkfs.xfs", "-q", "-f", "-m", "crc=0", "-i", "size=512", "-b", "size=4096",
         "-p", proto, img],
        check=True,
    )
    return img


def external_log_volume(at):
    """A filesystem whose log is on a **separate device**, and that device.

    Returns both paths: the data image has `sb_logstart == 0`, which is how
    XFS says "my log is elsewhere", and nothing in it names where. That is
    the point of the configuration and the reason the kernel has to be told
    — Linux is told with `-o logdev=`.

    `mkfs.xfs` will not make an external log under 64 MB, so the log device
    is 80 MB for a 320 MB filesystem. Both are mostly zeroes and compress
    to nothing.
    """
    os.makedirs(at, exist_ok=True)
    data = payload(at)
    proto = os.path.join(at, "protoext")
    with open(proto, "w") as f:
        f.write(f"dummy\n0 0\nd--755 0 0\nseed ---644 0 0 {data}\n$\n")
    img = os.path.join(at, "ext.img")
    log = os.path.join(at, "extlog.img")
    with open(img, "wb") as f:
        f.truncate(320 * 1024 * 1024)
    with open(log, "wb") as f:
        f.truncate(EXTLOG_MB * 1024 * 1024)
    subprocess.run(MKFS + ["-n", "ftype=1", "-l", f"logdev={log}", "-p", proto, img],
                   check=True)
    return img, log


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


# QuokkaOS's userland, on a filesystem the board can actually boot.
#
# **Why a protofile and not a loop mount.** Putting files into an XFS image
# normally means `losetup` and `mount`, which need privileges this account
# does not have. `mkfs.xfs -p` takes a *protofile*: a description of the tree
# to create while the filesystem is being made, including directories and
# **symlinks** — which is what makes this fixture possible at all, because a
# runlevel is a directory of symlinks and QuokkaOS's whole enable/disable
# model is built on them.
#
# The tree is the one the board test asserts on: three services where b
# needs a and c uses b, a becoming ready only after it writes a file, and a
# fourth that dies on purpose so the restart can be observed.
QUOKKA_MB = 320


def _proto_line(mode, name, source):
    """One protofile entry. The format is unforgiving about spacing."""
    return f"{name} {mode} 0 0 {source}"


def quokka_userland(at, init_binary, rc_binary, service_binary, tiny_binary=None):
    """An XFS image holding init, rc, a test service and a default runlevel.

    The service files are written to the host first and copied in by
    `mkfs.xfs`, so what the board parses is the same bytes this function
    wrote — there is no second copy to drift.
    """
    os.makedirs(at, exist_ok=True)
    services = {
        "a": ('description = "becomes ready late"\n'
              'command = ["/bin/qservice", "-name", "a", "-touch", "/run/a.ready",'
              ' "-delay", "400ms"]\n'
              "[ready]\n"
              'file = "/run/a.ready"\n'
              'timeout = "20s"\n'),
        "b": ('description = "needs a"\n'
              'command = ["/bin/qservice", "-name", "b"]\n'
              "[depend]\n"
              'need = ["a"]\n'),
        "c": ('description = "uses b"\n'
              'command = ["/bin/qservice", "-name", "c"]\n'
              "[depend]\n"
              'use = ["b"]\n'),
        "dies": ('description = "exits on purpose, to be restarted"\n'
                 'command = ["/bin/qservice", "-name", "dies", "-die-after", "1s"]\n'
                 "[restart]\n"
                 'policy = "always"\n'
                 'backoff = "100ms..200ms"\n'
                 'max_in_window = "20/60s"\n'),
    }
    written = {}
    for name, body in services.items():
        path = os.path.join(at, f"{name}.toml")
        with open(path, "w") as f:
            f.write(body)
        written[name] = path

    # The protofile. Indentation is irrelevant; the `$` tokens close
    # directories and are what the format uses instead of braces.
    lines = ["dummy", "0 0", "d--755 0 0"]
    lines.append("bin d--755 0 0")
    lines.append(_proto_line("---755", "qservice", service_binary))
    lines.append(_proto_line("---755", "rc", rc_binary))
    if tiny_binary is not None:
        # A freestanding program with its own _start, for the one comparison
        # a Go binary cannot make: whether execve itself works.
        lines.append(_proto_line("---755", "tiny", tiny_binary))
    lines.append("$")
    lines.append("etc d--755 0 0")
    lines.append("init.d d--755 0 0")
    for name, path in written.items():
        lines.append(_proto_line("---644", f"{name}.toml", path))
    lines.append("$")
    lines.append("conf.d d--755 0 0")
    lines.append("$")
    # The marker that asks init to run rc at the end of the boot. A real
    # root does not carry it; this image does, which is how the board test
    # gets rc's own output onto the console without the kernel having to
    # pass init any arguments.
    lines.append("quokka d--755 0 0")
    lines.append(_proto_line("---644", "rc-check", written["a"]))
    lines.append("$")
    lines.append("runlevels d--755 0 0")
    lines.append("default d--755 0 0")
    for name in written:
        # A symlink, which is what "enabled" means — and the reason this
        # fixture needs the kernel's symlinkat and readlinkat at all.
        lines.append(f"{name}.toml l--777 0 0 /etc/init.d/{name}.toml")
    lines.append("$")
    lines.append("$")
    lines.append("$")
    lines.append("var d--755 0 0")
    lines.append("log d--755 0 0")
    lines.append("$")
    lines.append("$")
    lines.append("run d--755 0 0")
    lines.append("quokka d--755 0 0")
    lines.append("$")
    lines.append("$")
    lines.append(_proto_line("---755", "init", init_binary))
    lines.append("$")
    proto = os.path.join(at, "quokka.proto")
    with open(proto, "w") as f:
        f.write("\n".join(lines) + "\n")

    img = os.path.join(at, "quokka.img")
    with open(img, "wb") as f:
        f.truncate(QUOKKA_MB * 1024 * 1024)
    subprocess.run(MKFS + ["-L", "ROOT", "-n", "ftype=1", "-p", proto, img], check=True)
    return img
