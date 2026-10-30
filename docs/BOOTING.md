# Booting from an SD card or NVMe

How to start minikern on a VisionFive 2 without the network. **Option B (FAT)
was booted end to end on the board on 2026-10-28**: U-Boot loaded the kernel
from the card's FAT partition, and the kernel mounted the `ROOT` XFS from
partition 2 and ran `/init`. Option A uses the same steps with a raw read in
place of `fatload`, and has not been run.

## Why the kernel cannot be loaded from XFS

The board's U-Boot (2025.04, in SPI flash) reads three filesystems:

```
StarFive # fstypes
Supported filesystems: fat, ext4, btrfs
```

Upstream U-Boot has no XFS driver, so it cannot load the kernel as a file from
an XFS volume. It can read raw blocks: `mmc read` and `nvme read` both return
the XFS superblock (`XFSB`). That leaves two ways to boot:

| Option | Needs | Trade-off |
|---|---|---|
| **A. Raw kernel partition** | a small partition holding the image byte for byte | no FAT; the kernel is `dd`'d, not copied as a file |
| **B. FAT boot partition** | a FAT32 partition holding the image as a file | ordinary file copy; verified |

Reading the kernel's blocks straight out of an XFS file is possible in principle
(contiguous extents, `xfs_bmap`), but it breaks the first time the file is
rewritten. Don't do it.

## The disk layout

- **The root is the XFS labelled `ROOT`, on any partition of any disk.** The
  kernel attaches devices in order (memory, NVMe, SD, virtio). On each, it reads
  block 0 of every partition and takes the first XFS labelled `ROOT`. So the boot
  partition can be partition 1, and the root can be on the card or on the NVMe
  drive. `minikern.disk=` limits the search to one kind of device.
- With no `ROOT` anywhere, the first volume of the first device is the root **only
  if it is XFS** (unlabelled disks, test images). Otherwise the disk has no root:
  its volumes are still mounted (the FAT at `/boot`) and `/` is the command line's.
- **A card as bought works as it is**: one FAT partition (MBR, type `0c`) holding
  `minikern.bin`, nothing else. The kernel boots from it and the root comes from
  wherever the user points it — an XFS `ROOT` elsewhere (NVMe), or netfs:
  `minikern.disk=sd minikern.netfs=<ip>:<port> minikern.netroot minikern.init`.
  Verified on the board with a stock layout (2026-10-29): "no XFS root on sd …
  / is the command line's", FAT at `/boot`, QuokkaOS from netfs.
- **`mkfs.xfs`'s defaults are fine**, reverse map and reflink included: every
  write keeps both, and the result passes `xfs_repair` (`test/xfs_write_test.sh`).
  Files shared by `cp --reflink` are copied on write, as on Linux.
- Other XFS partitions are mounted by label at `/volumes/<label>`; the FAT boot
  partition at `/boot` (below).

On a Linux machine (`/dev/sdX` is the SD card or NVMe namespace; this erases it):

```bash
printf 'label: gpt\nstart=2048, size=512MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=BOOT\ntype=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=ROOT\n' \
    | sudo sfdisk /dev/sdX
sudo mkfs.vfat -F 32 -n BOOT /dev/sdX1
sudo mkfs.xfs -L ROOT /dev/sdX2     # then copy /init and the rest in
```

For NVMe the partitions are `/dev/nvme0n1p1` and `/dev/nvme0n1p2`. XFS cannot
shrink, so an existing single-partition drive has to be backed up and re-created.

The image is the board target's output, about 0.4 MiB:

```bash
img=$(bazel cquery --output=files //:minikern_board)
```

Despite its `.elf` name, that file is the flat boot image: `file` reports
`Linux kernel RISC-V boot executable Image`, and it is the same file netboot
serves as `minikern.bin`. Below it is called `minikern.bin`; any name works if
the U-Boot command uses the same one.

## Option B — FAT partition (verified)

```bash
sudo mount /dev/sdX1 /mnt && sudo cp "$img" /mnt/minikern.bin && sudo umount /mnt
```

At the U-Boot prompt (SD is `mmc 1`; NVMe is `nvme 0:1`):

```
fatload mmc 1:1 0x82000000 minikern.bin
setenv bootargs "console=sbi minikern.init=1"
booti 0x82000000 - ${fdtcontroladdr}
```

Measured: `421952 bytes read in 25 ms`, then the kernel's own boot. The load
address and the `booti` line are the ones netboot uses.

**The running kernel mounts this partition at `/boot`**, read-write: the FAT
labelled `BOOT`, or the first FAT on the disk when none is labelled. So a new
kernel can be installed from the running system — copy it over
`/boot/minikern.bin` and the next boot loads it. FAT has no owners or
permission bits: everything there is root's, directories `0755`, files `0644`,
and `chmod`, `chown`, links and symlinks are refused.

## Option A — raw partition

Give the kernel its own small partition 1 (a few MiB, any type) instead of FAT,
and write the image into it raw:

```bash
sudo dd if="$img" of=/dev/sdX1 bs=1M conv=fsync
```

```
mmc dev 1
part start mmc 1 1 ks
mmc read 0x82000000 ${ks} 0x800                       # 0x800 blocks = 1 MiB
setenv bootargs "console=sbi minikern.init=1"
booti 0x82000000 - ${fdtcontroladdr}
```

From NVMe: `nvme scan`, `part start nvme 0 1 ks`, `nvme read 0x82000000 ${ks} 0x800`.

## Booting without typing

`bootcmd` is `bootflow scan` today. Replace it in the SPI environment (option B
shown):

```
setenv bootcmd 'fatload mmc 1:1 0x82000000 minikern.bin; setenv bootargs "console=sbi minikern.init=1"; booti 0x82000000 - ${fdtcontroladdr}'
saveenv
```

`bootdelay` stays 2, so the netboot harness can still interrupt autoboot, and
`env default bootcmd; saveenv` undoes the change. This build has no
`sysboot`/extlinux, and whether `bootflow scan` would find a `boot.scr` is
unverified, so the saved `bootcmd` is the reliable route.

## Kernel command line

| Option | Meaning |
|---|---|
| `console=sbi` | the firmware's console, the safe choice (`console=uart` uses the kernel's own driver) |
| `minikern.disk=sd` / `nvme` | search only that kind of device for `ROOT`. Without it every device is searched, NVMe before SD |
| `minikern.init=1` | run `/init` from the root filesystem |
| `minikern.time=<unix seconds>` | the epoch; the board has no RTC, and the harness supplies it |
| `minikern.halt=poweroff` | power off when done; for tests, not for a machine meant to stay up |

## Open

- Option A has not been run on the board.
- The first SD data read after bring-up overruns the controller's FIFO. The
  driver retries it and says so (`disk: N SD read(s) needed a second attempt`).
  The cause is not found (ROADMAP §4).
