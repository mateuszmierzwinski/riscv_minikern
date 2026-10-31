# ROADMAP — what is missing

What this kernel does **not** do yet. `FEATURES.md` is the other side: what
works today and what proves it. Sizes are estimates: **S** a day or less,
**M** a few days, **L** a week or more. Within a section, items are ordered by
what they unblock. **The board is the gate** (`bazel test //…:hil_test`); the
QEMU suite is repaired but sidelined. The full history of closed items is in
git, and the pitfalls are in `CLAUDE.md` §5.

---

## 0. Known defects

### Open

- [ ] **The SD card's `/init` is no longer the Go test binary — needs the operator** — `a stock Go binary off the card` fails because `/init` on the card is a 9,760-byte C program that exits 42 (seen 2026-10-29); the kernel ran it correctly. Needed: put the Go test binary (`src/userland/gotest`) back as `/init` on the card's ROOT, or say which `/init` the card should carry and the case will follow it. **2026-10-30:** `/init` on the card is now QuokkaOS init (2,949,282 bytes; it ran and started qmcd and statsd), and **`/swapfile` is gone** (`swap: cannot use /swapfile (rc -2)`), so `swap and a file-backed mapping` and `part of a transparent huge page sent to swap and back` fail too. Needed as well: a 64 MiB non-sparse `/swapfile` on ROOT (`dd if=/dev/zero of=/mnt/swapfile bs=1M count=64`).

### Done

- [x] **`sched_arm_timer` arming a past instant** — not a defect: the interrupt follows within 158 µs (board, `waitloop`: 243 such arms, 3 superseded). Reported every boot as `sched: N deadlines armed already past`.
- [x] **The sleeper hang** — the two-address bug: wakes named one alias, sleepers the other. Board: `waitloop` with four live sleeping children, 200 rounds of each `wait4` form, 0 kernel faults.
- [x] **Kernel objects had two addresses** — the kernel is now linked at `0xffffffc000200000` and runs only in the top half: `start.S` jumps high before any C runs, and secondaries do the same in assembly. A child's exit no longer wakes the parent's low alias. Step C is in §7.
- [x] **Kernel faults on the housekeeping beat** — harts no longer idle on a process's page table, and `uspace_destroy` takes the table off every hart first (`sched_forget_address_space`). This ended the `dwmac_poll`/`plic_claim`/`temp_jh7110_sample` faults.
- [x] **Forked children died** — `give_every_mapped_page` is on by default (`minikern.noforkleaves` is the control). On the board, waitloop's 400 rounds pass in about 7 s with 0 kernel faults.
- [x] **Filesystem image mounted from DRAM** — `minikern.disk=ram minikern.diskat=<addr>:<len>`. Images come from `test/hil/fixtures.py` (`mkfs.xfs`, gzipped, payload `0..255`) and are fetched by `board.py -i`.
- [x] **Board harness in the tree** — `test/hil/board.py` (one boot) and `suite.py` (cases) run as `hil_test`. Each run rebuilds and digest-checks the staged image, and all self-tests run in one boot.
- [x] **`MADV_DONTNEED`/`MADV_FREE` ignored** — `uspace_discard_range` now discards the pages, and the next touch reads zero. Proven on the board: `minikern.dontneed` exits 0, and the `minikern.keepadvice` control exits 200.
- [x] **User-memory corruption under Go** — the cause was the ignored `MADV_DONTNEED`. On the board, 5,000 clean rounds with the fix, while the `minikern.keepadvice` control fails by round 9.
- [x] **Embedded program's stack unrecorded** — the raw program path now records its 64 stack pages, so `mmap` no longer places a mapping on the live stack. `uspace_stale_records` reports the next case.
- [x] **`sched_end_siblings` left threads running** — a busy sibling now gets an aimed interrupt, `sched_yield_now` no longer resurrects ended threads, and the caller blocks until siblings leave. Proven on the board with `minikern.endsib=1`.
- [x] **Kernel register set entering user mode** — interrupts are now off between arming `sscratch` and `sret` in `uspace_enter_frame`. `trap_mismatched_entries` checks the invariant.
- [x] **Detectors reported before any program ran** — the detector counters are now printed after each program finishes, not at the end of hart startup.
- [x] **Idle hart never told the process had ended** — harts are poked when a process ends, and the wait in `uspace_run` reports which harts have not left.
- [x] **Poke to the boot hart never delivered** — the boot hart now enables software interrupts. `trap_software_interrupts` counts pokes taken against pokes sent.

---

## 1. Process model

### Open


### Done

- [x] **Process-wide pending signals** — `kill` raises on the process's pending set, and the thread woken is one with the signal unblocked; a thread that unblocks it later takes it. It used to go to the first thread, so a program whose main thread blocks a signal never received it. Board: `§1: a signal to the process is taken by a thread that can take it` (15).
- [x] **`nanosleep` interrupted, `clock_nanosleep`, `CLOCK_REALTIME`** — an interrupted sleep returned 0 after sleeping short, and now returns `EINTR` with `rem`. `clock_nanosleep` (relative and `TIMER_ABSTIME`) was missing. `CLOCK_REALTIME` returned time since boot, not the date. Board: `§1: nanosleep interrupted…` (255); hostile cases for `getresuid` and `clock_nanosleep`.

- [x] **Saved ids and unprivileged `setuid`** — `setresuid`/`getresuid` (syscalls 147/148, both missing) and a saved uid. Capabilities follow Linux's rule: cleared for good when all three ids are non-zero, only the effective set when the effective uid leaves 0, restored from the permitted set when it returns. An unprivileged `setuid` may move to its real or saved uid. Board: `§1: saved ids` (127), `process model self-tests`, `hostile syscall arguments`.

- [x] **QuokkaOS init on the board** — three runlevels up, a dying service restarted, `rc list` answered every round (the `quokka:` suite case).
- [x] **`execve` started programs one instruction late** — `execve` was missing from `syscall_steps_over_ecall`'s exceptions; proven by `minikern.execone` and `/bin/tiny` on the board.
- [x] **`execve` dropped address-space fields; concurrent `execve`s shared one scratch** — exec adopts a whole seeded scratch, one per process slot. Proven by QuokkaOS init starting services in parallel.
- [x] **`wait4` after `fork`** — `EFAULT` on copy-on-write pages fixed (`uspace_break_cow` shared by the fault and copy-out paths); both `wait4(pid)` and `wait4(-1)` proven on the blocking path, `waitloop` 400 rounds on the board.
- [x] **`fork` with copy-on-write** — uses refcounted frames, a COW bit in the PTE software bits and a COW fault handler. Proven on the board by `src/userland/forktest`; `minikern.wait=1` proves `wait4` reaps the child.
- [x] **Process groups and sessions** — adds `setpgid`/`getpgid`/`setsid`/`getsid`, `kill` (pid, group, 0), and `SIGSTOP`/`SIGCONT` as a `THREAD_STOPPED` state. Proven on the board with `minikern.pgrp=1`, which exits 7.
- [x] **Controlling terminal** — canonical mode, echo, `^C`/`^Z`/`^D`, `TIOCG/SWINSZ`, `TIOCG/SPGRP` and `SIGTTIN`. The line discipline is host-tested (`core/tty.c`, `test/tty_test.c`), and the board passed a real typed-line test.
- [x] **`SIGTSTP` from the keyboard** — one `uspace_deliver_signal` now stops processes on `SIGTSTP`/`TTIN`/`TTOU`, and the one-second `nanosleep` clamp is gone. Proven on the board by the `runtype.py` `^Z` test: 7 with the key, 174 without.
- [x] **Users and groups** — adds real and effective uid/gid, `setuid`/`setgid` (both ids), and XFS ownership. The permission check (`core/access.c`) is host-tested and called from `openat`. Proven on the board with `minikern.uid=1`.
- [x] **Resource limits** — `prlimit64` gets and sets limits under POSIX privilege rules. `RLIMIT_NOFILE` and `RLIMIT_NPROC` (per uid) are enforced. Proven on the board with `minikern.rlimit=1`.
- [x] **`getpid`/`gettid` returned 1** — both now return the real pid and tid, so Go's `tgkill` preemption reaches the right thread. Proven on the board: the pgrp test passes as the third program in a boot.

---

## 2. Memory

### Open


- [x] **`pgtable_map` could write through a borrowed kernel table** — since step C a process borrows only the kernel's top half, and its own mappings are all below `USER_TOP`, so no walk for a user address reaches a kernel table. The boot counter stays (`pgtable: no process wrote into the kernel's tables`).
- [x] **ELF segments are in the region record** — `load_segment` records each segment (`record_segment`); a page two segments share gets the union of their rights in the earlier region, as its mapping does. The `wait4` case that hung at round one under the old layout passes its 200 rounds.

### Done

- [x] **Cutting a file mapping: one file reference per piece** — a partial `mprotect` cut a file region into pieces sharing one reference, and each piece gave it back when it went: the file was freed under the regions still reading it. Now each new half takes its own (`vma_set_split_hook`), and partial `munmap` of a file mapping, which was refused for want of this, works. Host: `vma_test`. Board: `§2: a file mapped in 2 MiB pages` (63).

- [x] **Transparent huge pages** (operator priority 2026-10-28) — an ordinary anonymous private region gets a 2 MiB page on first touch of an aligned window it covers whole and nothing has touched (Linux's `always`; `minikern.nothp` turns it off). Discard, unmap, `MAP_FIXED`, `mprotect`, `mremap`, `fork` and swap split a megapage they cover only in part (`split_huge`: same frames, same rights), so no byte a program reads can change. A first version allocated and zeroed 2 MiB before asking whether the window was free: Go's stress ran 15× slower. Now `pgtable_megapage_free` is asked first, and the run takes the same 33 s as with THP off. Board: `§2: transparent huge pages…` (127, 7 huge, 5 split), the `nothp` control, the 2,500-round §0 reproducer.
- [x] **Huge file mappings** — a private file mapping's window, 2 MiB aligned in address and file and wholly inside the file, gets one megapage filled from the file. Board: `§2: a file mapped in 2 MiB pages` (15; 2 from a file).
- [x] **Huge pages in swap** — swap-out splits a megapage and sends out only the page chosen; the other 511 stay. Board: `§2: part of a transparent huge page sent to swap and back` (1 out, 1 back in).
- [x] **`mprotect` of part of a huge page** — re-mapped 4 KiB over the 2 MiB leaf, which dropped the other 511 pages' contents. This affected `MAP_HUGETLB` too. Now a megapage covered whole is changed in place, and one covered in part is split, or refused (`EINVAL`) in a `MAP_HUGETLB` region.

- [x] **`fence.i` after loading code** — issued after segments are loaded, behind a data fence; board-verified on exec.
- [x] **Leaf walker read swapped-out PTEs as addresses** — `pgtable_for_each_leaf` requires `PTE_V` itself.

- [x] **Demand paging** — pages are backed on first touch from the `vma.c` region record, and the user-pointer validator backs pages too. On the board, 1,396 pages were demand-backed in a Go run.
- [x] **Copy-on-write** — done for anonymous memory. Private file mappings need no COW because each page is filled privately, and `MAP_SHARED` is refused.
- [x] **File-backed mappings** — `MAP_PRIVATE` read-only mappings over a regular-file `openat` (16 MiB cap). The region holds a reference to its file. Proven on the board: `minikern.filemap=1 minikern.disk=sd` exits 63.
- [x] **`mremap`** — shrinks, grows in place, or moves without copying (`MREMAP_MAYMOVE`/`FIXED`). `MREMAP_DONTUNMAP` is refused. Proven on the board: `minikern.mremap=1` exits 63, with 4 pages moved and none newly backed.
- [x] **`munmap` never cleared the region record** — record removal and page dropping are now separate steps, and `munmap` does both. This closes a use-after-free that silently returned zero pages.
- [x] **Swap** — a swapfile set up with root-only `swapon`/`swapoff`, swappiness in the flags, `MADV_PAGEOUT`, a free-page watermark and a clock victim policy using `PTE_A` (A-bit faults are handled in software). Proven on the board with a 64 MiB `/swapfile`.
- [x] **Huge pages for user space** — `MAP_HUGETLB` gives 2 MiB pages with aligned placement (`vma_find_free`) and falls back to 4 KiB. A partial unmap is refused, and `fork` copies blocks the parent has touched. Proven on the board: `minikern.huge=1` exits 127.
- [x] **`mmap` flags** — every flag is now honoured (`MAP_POPULATE`/`LOCKED`/`FIXED_NOREPLACE`), vacuous (`NORESERVE`, `STACK`, …) or refused (`SHARED`, `GROWSDOWN`). The wrapping bound in the fixed path is fixed. Proven on the board: `minikern.mapflags=1` exits 127.

---

## 3. Filesystem and VFS

### Open

- [ ] **A file in an 8 KiB-directory-block filesystem cannot be read** (M) — lookup succeeds through the hash index, then the read returns `EIO` (`cat: /p2999 cannot be read (rc -5, 14 hashed, 0 walked)`); the 4 KiB variant reads the same file. Pre-existing (fails identically at `c4856ea1`). Reproducer: suite case `§3: 8 KiB directory blocks`.
- [~] **A VFS layer** (L) — The mount table holds XFS and FAT volumes, and each path-based `xfsmount_*` call asks it which kind a path is on (`volumes_resolve_fat` → `fatmount.c`). There is still no filesystem port, no `mount`/`umount` and no overlay. Next step: a port struct in `src/core` once a third filesystem needs one.
- [ ] **`/proc`, `/sys`, `/dev`** (M) — Nothing synthetic exists beyond a few `/dev` names and `/dev/virtual/kernel_logger`; `/proc/self/maps` or `/sys/kernel/mm/...` give `ENOENT`, which Go tolerates and most software does not. Next step: extend the synthetic-path dispatch in `fdobjects_open_path`.
- [ ] **`/proc` depth** (M) — `/proc/stat` lacks `ctxt`, `processes` and per-hart `cpuN` (needs idle accounting in `struct hart`); all busy time reports as `system` (a split needs trap entry/exit stamping); `/proc/uptime`, `loadavg`, `cpuinfo` and `/proc/<pid>/` are missing.
- [ ] **`/proc/ps/` — processes as files for the Go coreutils** (M) — Views `by-pid/<pid>/`, `by-name/<name>/<pid>/`, `by-user/<uid>/<pid>/`, one value per file in bytes (`memory_bytes`, `pid`, `time_alive`, `niceness`, `fds_opened`, later `ppid`/`uid`/`state`/...). Known-path reads can ride the `fdobjects_open_path` dispatch first; directory listing needs the VFS.
- [ ] **Device nodes** (M) — `/dev/null`, `/dev/zero`, `/dev/urandom` exist as descriptor objects but cannot be opened by path (regular files can). Next step: register them in the synthetic-path dispatch.
- [ ] **`ioctl`** (S for a stub, M to mean anything).
- [ ] **`poll`/`ppoll` and `select`/`pselect`** (S) — `epoll` exists; much software uses these instead. Next step: implement them over the existing per-object readiness used by `epoll`.
- [~] **Path resolution of `.` and `..`** — Any path containing a dot component is refused with `ENOENT` (board-tested) because textual `..` collapsing escapes sandboxes through symlinks. Next step: resolve components against the tree, never by string operation.
- [~] **`utimensat` `UTIME_OMIT` and `ftruncate` limits** — `UTIME_OMIT` is refused because `stat` reports no timestamps; `ftruncate` works only on write descriptors, not on read descriptors or files unlinked while open. Next step: report timestamps in `stat`, and let descriptors hold the inode.
- [~] **Realtime files: truncate, holes, unwritten extents** — Refused (`ENOTSUP`): their blocks belong to the realtime bitmap, not a group. Unlink frees them to the bitmap.
- [~] **XFS log replay coverage** — Only inode and buffer log items replay; extent-free intents, inode-create and quota items abandon replay and leave the filesystem read-only. Next step: add item types as Linux-written dirty logs require them.
- [~] **XFS writer depth limits** — Node directories are written one node level deep; an extent b-tree fork is written two levels deep (up to ~5,000 extents with 512-byte inodes; a third level is `NOSPC`); a single map edit holds at most 4,096 extents; the attribute fork is not converted to a b-tree; the rt summary file is not maintained. The reader walks every depth.
- [ ] **The writer's speed** (M) — Each change re-reads the trees it touches from the root, and a b-tree fork is rewritten whole per block appended. Correct, measured by `xfs_repair`, and slow: `xfs_write_test` takes ~4½ minutes on the host. Next step: profile it.

### XFS

- [~] **Inodes whose fork exceeds `XFS_FORK_MAX` (412 bytes)** — `struct xfs_inode` caches the fork and the 1 KiB frame limit forbids a larger cache, so such inodes are refused per inode (never the whole filesystem); in practice rare. The refusal path is untested: it needs a fixture with 1024-byte inodes (unreachable at 512). Next step: build that fixture; lifting the limit means reading forks on demand at ~20 write-path call sites.

### Done

- [x] **Directory and file syscalls** — `getdents64`, `fstat`, `newfstatat`, `statx` (honest `stx_mask`), `statfs`, `faccessat`, `mkdirat`, `unlinkat`, `renameat2`, `linkat`, `symlinkat`, `readlinkat`, `fchmodat`, `fchownat`, `utimensat`, `lseek`, `fsync`/`fdatasync`, `openat` create/truncate; board-proven.
- [x] **`truncate`/`ftruncate`, `umask`, `chdir`/`getcwd`** — free-block accounting asserted on the board; `umask` read by create (`0022`→`0644`); relative paths resolve in `copy_in_path`; all inherited across fork/exec.
- [x] **B-tree format forks** — keyed descent in `xfs_bmbt.c` behind `xfs_bmap`, every disk number checked; board: 3,000- and 40,000-entry directories list completely, payload checksums exact.
- [x] **Directory hash index** — `xfs_dirhash.c` name hash, leaf binary search and node descent with collision runs; board requires `0 walked` lookups.
- [x] **B-tree board case** — the expectation was wrong, not the reader; the case asserts `cat: hash index answered`.
- [x] **Multi-block directories** — leaf/node directories with several data blocks are read and written (dirblocks).
- [x] **Dirty filesystem checked, mounted read-only** — `xfs_check` validates group headers, b-tree roots, counts and root inode, writing nothing; board-proven with a log this kernel left dirty.
- [x] **Log replay** — all-or-nothing replay of inode and buffer items in the block cache, host-byte-order inode conversion; board: dirty log replayed, pre-crash file intact, writable, with read-only control.
- [x] **Automount by label** — `ROOT` at `/`, others at `/volumes/<label>`, virtual `/volumes`, longest-prefix resolution, one `struct xfs` per device; board-proven on a two-volume GPT image.
- [x] **The root is found by its `ROOT` label** — on any partition of any attached device (NVMe before SD), not partition 1; FAT boot partitions are skipped. Board: FAT p1 + ROOT p2, from a RAM image and from the real card.
- [x] **Extended attributes** — shortform, leaf (with free map), remote values and b-tree attribute forks (host-tested); namespaces as flags; board: 14 attrs across a remount.
- [x] **Directory growth through every shape** — shortform → block → leaf → node, multiple data blocks, leaf splits, `bests`/free blocks maintained; board: 400 names added/found/removed twice.
- [x] **Inode chunk and multi-group allocation** — new contiguous inode chunks (`xfs_alloc_extent` smallest fit), `xfs_alloc_somewhere` across allocation groups; board-proven with counters and a `single_group` control.
- [x] **XFS format coverage** — v4 and v5 read/write (real `mkfs.xfs -m crc=0` image), `mkfs.xfs` defaults read **and written** (sparse inodes, `nrext64`, exchange-range, parent pointers, reverse map, reflink), inode sizes 256–2048; board-proven.
- [x] **Writes on a filesystem made with `mkfs.xfs` defaults** — The reverse map is kept on every allocation and free (`xfs_rmap.c` over a full B+tree with split, borrow, merge and root collapse, `xfs_btree.c`; blocks from a refilled AGFL). Judged by `xfs_repair -n` in `xfs_write_test`: minimal, defaults-without-rmap and defaults images, 1,800 files in and 1,700 out (rmap tree 1 → 2 → 1 levels), clean, with a control that fails when the map is not kept. Board: `§3: the write path on a filesystem made with mkfs.xfs defaults`.
- [x] **Writer bugs `xfs_repair` found once it was the judge** — None of these was visible to the hand-built fixtures:
  - inode chunks not aligned to `sb_inoalignmt`;
  - v5 `blkno` never stamped on directory and remote-attribute blocks;
  - `bestfree` never updated, and free spans never joined;
  - the attribute extent count (`di_anextents`) never written;
  - the data extent count written to the wrong field under `nrext64`;
  - the remote-attribute header laid out wrong;
  - attribute entry sizes rounded to 8 instead of 4;
  - every directory entry typed as a regular file;
  - sparse inode records written as a 32-bit count;
  - a free block's owner over its UUID, and its `nused` miscounted;
  - node keys stale after a removal;
  - a first remote attribute failing `ENOENT`;
  - file growth mapping blocks at `di_nblocks` instead of the written offset;
  - unlink freeing b-tree forks, realtime blocks and attribute blocks wrongly or not at all.
- [x] **Every XFS b-tree written at any height** — free space (both trees), inodes and free inodes, reverse map and reference counts on one engine (`xfs_btree.c`), blocks from a refilled free list or the allocator. A chunk Linux filled — absent from the free-inode tree — can have inodes freed again. Host: 1,200 free extents in a group, 17,000 inodes, two-level trees in and out; `xfs_repair` clean.
- [x] **Extent maps that outgrow the inode** — files and directories become b-tree forks (`xfs_fork.c`); host: a 1,024-extent file and a 2,400-name directory, written, truncated, removed.
- [x] **Writes into holes, unwritten extents and shared blocks** — a hole gets a zeroed block, an unwritten block is zeroed and written, a reflinked block is copied first (`xfs_fill.c`); frees of shared extents lower the counts (`xfs_reflink.c`). `xfs_reflink_clone` (`FICLONE`'s semantics) exists, used by the host test.
- [x] **The log is a ring** — records go at the head, in its cycle, padded to the log's stripe unit, wrapping with the cycle; replay runs from the tail. It used to write at block 0 in cycle 1, which a log Linux had wrapped reads as dirty. Host: a log taken round past its end (cycle 1 → 2), a record split across the wrap, a crash replayed after it.
- [x] **4 KiB-sector disks** — group headers each in their own block (`xfs_group_header`); they were read past the end of block 0's buffer.
- [x] **Directories give back emptied blocks** — data blocks and hash leaves (`xfs_dirshrink.c`); `xfs_repair` rejects an empty data block.
- [x] **FAT at `/boot`** — FAT12/16/32 with long names, read and written (`src/core/fat/`); the FAT labelled `BOOT`, or else the first FAT, is mounted at `/boot`. Host: `fsck.fat -n` and mtools on all three types (`fat_write_test`). Board: `§3: the FAT boot partition, read and written at /boot`.
- [x] **`lseek` and `ftruncate` on write descriptors** — both went through a lookup that only answers for read descriptors, so `lseek` was `EBADF` and `ftruncate` found no path; `SEEK_END` now uses the file's size on disk.
- [x] **Quotas, realtime, external logs** — quotas read and enforced incl. soft limits with grace via `minikern.time` clock port; realtime subvolume read/write (`xfs_rt_alloc`); external log attach/replay.
- [x] **`NEEDSREPAIR` refused** — a filesystem flagged as needing repair is refused at mount; covered by `xfs_qemu_test`.

---

## 4. Drivers and board hardware

### Open
- [~] **TRIM on the NVMe drive — needs the operator** (S) — Deallocate is built and gated on `ONCS`. The board's drive reports it supported, with deallocated blocks reading back undefined (`DLFEAT`). But the command itself has not run on silicon: the drive is one XFS holding the operator's data (`2026-09-12 20-49-06.mp4`), and a trim bug there loses it. **Needed from the operator:** a scratch partition or drive, or permission to trim this drive's free space. Then: `minikern.disk=nvme minikern.autotrim` on the board, and make `autotrim` the default.
- [ ] **TRIM on USB drives** (M) — After USB mass storage (below). Use SCSI `UNMAP` or `WRITE SAME(16)` per `READ CAPACITY(16)` `LBPME` and VPD `B0h`/`B2h`, as Linux's `sd` driver chooses. Fallback: ATA TRIM through SAT `ATA PASS-THROUGH(16)` (the channel SMART uses through a bridge), gated on IDENTIFY word 169 bit 0. Plus a deny-list by USB VID:PID, because some bridges advertise `UNMAP` and mishandle it. The block port's `discard` is what it plugs into.
- [ ] **USB mass storage** (L) — BOT and UAS, and a SCSI layer, after the XHCI port leaves Polling (below). USB discard depends on it.
- [ ] **GPIO pad-function selection (pinmux)** (M) — Only pins the firmware already left as GPIO can be driven. Claiming a pad's function means the `sys_iomux` registers; measure them on the board (dump the window) rather than trust the datasheet.
- [ ] **Voltage scaling** (M) — Three of four operating points want 800 mV but run at 1040 mV, so every low point wastes power. I²C and the PMIC (chip id `0x54`) work, but no register holds the core-rail value (`0x36`). A wrong write can destroy the part, so this waits for an authoritative AXP15060 register map (`minikern.probe=1` dumps it).
- [~] **The thermal half** (S–M) — The sensor driver works and `/proc/thermal` reports it, and the governor policy is host-tested (an impossible reading counts as the worst case). The governor's thresholds still don't read the sensor. Next: connect the governor to `temp_jh7110_sample` and prove a throttle on the board.
- [~] **USB** (L) — The XHCI driver runs the VL805 (5 ports, 32 slots, port map read from capabilities), but USB2 port 1 completes its reset and stays in Polling (`40000ee1`) with `PED` clear. Next: the VL805 errata or a missing hub-level step, starting from the recorded trace. HID and mass storage come after that.
- [ ] **The first SD data read overruns the FIFO** (S) — `RINTSTS 0x82c` (`FRUN`) on the first read after bring-up, correct data, `EIO`; resetting the FIFO first does not prevent it. `dw_read` retries once and the boot log counts it. Next: find what bring-up leaves the controller in.
- [ ] **eMMC** (S) — The DesignWare driver binds to both controllers, but the eMMC socket is empty. Test it once a module is fitted.
- [ ] **Tickless network** (M) — The polled network keeps a 10 ms beat (~1,200 interrupts in 10 s against 304 without a network). Next: an RX interrupt for idle traffic, plus the stack reporting its next timer deadline (retransmit, DHCP, ARP) so the scheduler can arm a one-shot.
- [ ] **RTC, watchdog, SPI, I²C, PWM beyond the probe** — Deferred by the operator until thermals are done. The board has no RTC, and the watchdog's reset stays unarmed because nothing feeds it.

### Done

- [x] **TRIM** — a `discard` operation on the block port (through partitions and scaling), NVMe Dataset Management deallocate (gated on `ONCS`, `DLFEAT` recorded), an XFS trim pass over each group's free-space tree (flushed first, so only blocks free on disk are trimmed), `FITRIM` on a directory (`CAP_SYS_ADMIN`), and `minikern.autotrim` at boot (opt-in until the NVMe item above). No kernel threads, so a running system trims with `FITRIM` from user space, as `fstrim.timer` does. Host: `xfs_write_test` checks every free block is discarded and nothing else (the fake drive poisons what it discards). Board: `§4: TRIM on the RAM disk, judged by the device` and `§4: fstrim through FITRIM, from a program`. Writing it found two stale-data leaks, now fixed: a new block, and a block's tail after a truncate, showed the disk's old bytes.
- [x] **Ethernet driver (DesignWare GMAC 5.20)** — Link up at 1 Gbit/s on `@16040000`. The YT8531 PHY address is found by search, the MAC address is the one the firmware set, and MAC loopback works both ways (proved on the board, `drivers/dwmac.c`).
- [x] **Network stack over the GMAC** — `bring_up_network` falls back from virtio to dwmac. The board takes a real DHCP lease (192.168.86.247, 86400 s).
- [x] **SoC temperature sensor via `/proc`** — `starfive,jh7110-temp` reads ~38 °C. Out-of-range values are refused. `/proc` is a table of generators that snapshot at open (`kernel/procfs.c`, plausibility verdict checked on the board).
- [x] **Frequency scaling** — Four operating points (1500 MHz ÷1–4) set through one divider word in the system clock generator. Measured on the board at 100/50/33/25% (`drivers/cpufreq_jh7110.c`).
- [x] **SPI, I²C, PWM, watchdog probes** — pl022 loopback `a5→a5`, DW I²C signature `44570140`, PWM and watchdog counters move. Watchdog offsets were measured, not taken from the docs. No RTC is fitted (board).

---

## 5. Networking

### Open
- [ ] **IPv6** (L) — The stack is IPv4 only.
- [ ] **GMAC strict unicast filter** — With the strict filter the controller accepts no unicast frames, even though its registers match the working run. The default is promiscuous (`PR`) as a workaround. Next: find the DWMAC 5.20 / JH7110 cause. `minikern.netfilter` reproduces it in one boot.
- [ ] **Bits lost across a blocking `recvfrom`** — In `unixsock`, a `.bss` mask lost its low four bits in 4 of 4 runs when no syscall came between the steps. The socket code, the exit path, register save, sibling exit and stack overruns are all ruled out. This looks like a coherency or scheduling window like `endsib`. The test now carries intermediate prints.

### Done
- [x] **GMAC as the network device** — DHCP, ARP, ping, TCP and DNS all work on the board (see the unicast filter workaround above).
- [x] **Routing table** — Eight prefixes, longest-prefix match ranked together with the on-link subnet and the default route. Masks with holes are refused (`test/route_test.c`, 10 host cases).
- [x] **TCP SACK** — Kept by operator decision. The receiver builds coalesced blocks from the reassembly queue. The sender stops retransmitting at the first SACKed block, and forged blocks are discarded. There is no full RFC 6675 scoreboard (host test, both counters).
- [x] **Unix-domain sockets** — Stream and datagram, as a third `sock_ops` with names in a kernel table (exact for the abstract namespace). `src/userland/unixsock` exits 127 on the board.
- [x] **`netlink`** — `RTM_GETLINK`, `RTM_GETADDR` and `RTM_GETROUTE` answered synchronously, with `NLMSG_ERROR` for unimplemented requests. Board program 127/127, so Go's `net.Interfaces` works.
- [x] **DNS** — `/etc/resolv.conf` is synthesised from the DHCP lease. `src/userland/resolve` exits 31 on the board. This work also added the GMAC interrupt (`interrupts-extended`) and fixed the `AE` bit.

---

## 6. Security

### Open
- [ ] **Extend the CVE suite as the kernel grows** (S, repeatedly) — The syscall-surface program covers pointers and one arithmetic argument. It doesn't yet cover TOCTOU across a syscall, descriptor-table exhaustion or the signal-delivery path. Every new syscall needs a case (Rule 25.2).
- [ ] **W^X for user mappings** (S) — `mprotect` is honoured, but nothing refuses a mapping that is both writable and executable.

### Done
- [x] **Capabilities and `seccomp`** — 41 capabilities with full sets, `capget`/`capset` v3, and seccomp strict plus classic-BPF filter mode checked before the syscall table. There are no file capabilities. Board: `minikern.privsep` exits 0.
- [x] **ASLR** — Stack, trampoline, `mmap` floor and heap gap are each randomised by ~12 bits, with the trap path feeding the entropy pool. Proved on the board across two runs. `minikern.noaslr` turns it off.
- [x] **CVE regression suite (Rule 25)** — Host parser cases for the device tree, XFS, partitions and ELF (`//:cve`), plus `minikern.hostile=1` on the board (accepted-bitmask 0). It found and fixed three real defects.

---

## 7. Boot and delivery

### Open
- [~] **Local boot from SD or NVMe** (M) — U-Boot cannot read XFS, so the kernel sits on a FAT partition (or a raw one) beside the `ROOT` XFS; booted end to end from a FAT partition on the board, recipe in `docs/BOOTING.md`. Left: a saved `bootcmd` so it boots unattended, and the raw-partition variant.
- [ ] **Own netboot** (M) — The ARM project's authenticated MKNB protocol (Ed25519-verified offers) came over as code, but nothing is wired to it.
- [x] **Kernel in the top half** (M) — The kernel runs only high (linked at `0xffffffc000200000`), and since step C a process's table carries only the kernel's top-half root entries: the identity map of RAM is not reachable from user tables at all. Every physical-address-as-pointer use goes through `mmu_phys_to_virt` (page tables, frames, kernel stacks, the loader, swap, disk buffers); `mmu: N identity addresses converted after the switch` counts any that are missed (0 on every board case). `minikern.lowmap` restores the old sharing as the control. `copy_in`/`copy_out` take user addresses and were rightly left alone — converting them was the likely cause of the old attempt's `fork` hang.
- [ ] **Describe an unknown board** (S) — A device-tree overlay or a command-line way to describe a board this kernel hasn't met.

---

## 8. Performance

### Open
- [ ] **SD beyond 8.2 MB/s** (M) — Multi-block transfers and 25 MHz took it from 2.5 MB/s. Next: high-speed mode (`CMD6`, 50 MHz) and the controller's internal DMA in place of word-at-a-time FIFO reads.
- [ ] **NVMe beyond 334 MB/s** (M) — One queue, one command in flight, polled. Next: several queues and interrupt completion (blocked by the item below).
- [~] **Interrupt-driven I/O** (M) — The external-interrupt path and `irqwait` work. SD reads and writes complete via `minikern.mmcirq` (still opt-in), after the fix to wait for `tran` with `CMD13` after a write. NVMe IRQ is blocked on the JH7110/PLDA PCIe interrupt demultiplexer: source 57 fires with no INTx bit set, and `irqwait_wait` doesn't honour its deadline. `minikern.nvmeirq` stays off. Next: dump `0x2c000000` across a completion and fix the `irqwait` deadline.
- [ ] **Long work out of the tick** (M) — The worst kernel-lock hold (~8.2 ms) is inside the supervisor timer interrupt: network processing and console output in the housekeeping beat. Next: measure which dominates and move it out of the critical section. Also measured 2026-10-26: in a Go run off the card a slice deadline was 323 ms stale when armed and a past arm waited 28.8 ms for its interrupt — long kernel sections with interrupts off (polled SD loading `/init`). `waitloop` sleeps wake up to 19 ms late; the console is the first suspect.

### Done
- [x] **Big kernel lock — not split (operator decision)** — Measured on 4 harts: 8.1% of acquisitions wait and the average hold is ~6 µs, so the outliers are one long section. `klock_tag` names the cause (board).
- [x] **`me()` without a scan** — `index_by_id[]` is filled after hart enumeration, which leaves `sscratch` untouched. `smp_hart_misses` is now reported (0 on four harts, board).

---

## 9. Testing and tooling

### Open
- [ ] **Fuzzing harness** (M) — Nothing feeds the monorepo's central corpus and staging convention. Next: fuzz the filesystem and ELF parsers, seeded from their existing hostile-input tests.
- [ ] **Coverage** (S) — No coverage is collected for this project.
- [ ] **Board benchmark suite** (S) — `minikern.diskbench` is a flag. Make it a test that records numbers over time so regressions are caught.

### Done
- [x] **QEMU suite repaired** — the timestamped log and the kernel's move broke all ten `*_qemu_test` targets; fixed, kept `manual` and sidelined.
- [x] **Fault reports blamed user code for kernel faults** — the origin is decided from the kernel's own table (`PTE_U` absent = kernel); board-verified both ways.
- [x] **Stale symbol twin** — `scripts/whereis.sh` rebuilds `minikern_board_elf` before symbolising.

## 10. Display and graphics

Built bottom to top (operator, 2026-10-31): a framebuffer the kernel owns, then 2D output on the board's
HDMI, then GL in user space. Research: the HDMI bring-up register sequence and the acceleration options are
summarised in `docs/DISPLAY.md`.

### Open

- [~] **HDMI: a picture confirmed by eye — needs the operator** (S) — the scanout runs on silicon (below), but
  no monitor is attached to the board (hot-plug low, so no EDID either). With a monitor on the HDMI port:
  boot `minikern.hdmi=1920x1080` with `fbdemo.elf` and check the colour-bar order (white, yellow, cyan, green,
  magenta, red, blue, black), a smooth grey ramp and a moving white square; then the EDID line in the log.
- [ ] **AVI infoframe / HDMI mode** (S) — the transmitter runs in DVI mode (register 0x52 = 0). Some TVs want
  HDMI mode with a valid AVI infoframe (`display_avi_infoframe` builds it; nothing sends it yet).
- [ ] **Mode from the EDID** (S) — the mode is the command line's; the monitor's preferred timing is read and
  only reported. Only 1080p60 and 720p60 have PLL settings.
- [ ] **Cache maintenance, smarter** (M) — the whole frame is flushed from the L2 ten times a second (~2 ms each,
  ~2% of a hart at 1080p). Better: flush only what changed (a dirty-rectangle `ioctl`, as DRM's `DIRTYFB`), or
  the TRM's uncached DRAM view at `0x4_4000_0000` once its semantics and PMP access are measured. Whether the
  DC snoops the L2 at all is unmeasured — it needs eyes on a monitor with flushing off.
- [ ] **Permissions on `/dev/fb0`** (S) — any process may open and map it today; Linux gives it to root and the
  `video` group.
- [ ] **`read`/`write`/`lseek` on `/dev/fb0`** (S) — only `pread`/`pwrite` and `mmap` now (`cat /dev/fb0` fails).
- [ ] **A way to see the VFB from the PC** (M) — a QuokkaOS service that maps `/dev/fb0` and serves it over VNC.
- [ ] **GL in user space** (L) — own small software GL, or Mesa's software renderers (Alpine's riscv64 musl
  packages run unchanged on this ABI). Mesa needs a minimal KMS (`/dev/dri/card0`: dumb buffers, page flip).
- [ ] **Acceleration** — the DC8200's planes are the real 2D acceleration (overlays, scaler, hardware cursor);
  there is no blitter. The PowerVR BXE-4-32 is an XL project of its own (closed MIPS firmware, ~35k-line
  reference driver, unsupported in Mesa). Measure memset/memcpy and a full-frame flush first.

### Done

- [x] **HDMI scanout of `/dev/fb0`** — `minikern.hdmi=1920x1080` or `1280x720` (`drivers/display_jh7110.c`): HPD
  and the PMIC rails read, DDC/HPD pins given to the transmitter (`gpio_jh7110_set_function`/`route_input`),
  VOUT powered (`pmu_jh7110.c`), syscrg and voutcrg clocks and resets (`CRG_VOUT`), the transmitter's PLLs
  locked in DVI mode (`inno_hdmi.c`), the DC8200 primary plane on display 0 (`dc8200.c`), the frame flushed
  from the L2 (`ccache_jh7110.c`). Board: DC revision 0x5720, PLLs locked, 31 vsyncs in 0.5 s at both modes,
  no underflow; fbtest 255 at 1080p while scanned out. Timings, register words, infoframe and EDID are pure
  and host-tested (`display_mode_test`), the EDID reader is in the CVE suite (`edid_cve_test`, guard pages).
- [x] **Virtual framebuffer `/dev/fb0`** — `minikern.vfb=<w>x<h>`, XRGB8888, one contiguous run of frames.
  `FBIOGET_VSCREENINFO`/`FSCREENINFO`, `FBIOPUT_VSCREENINFO` (the current mode only), `FBIOPAN_DISPLAY` (origin),
  `FBIOBLANK`; `pread`/`pwrite`; `mmap(MAP_SHARED)` maps the device's own frames, shared on `fork`. Board: a
  musl fbdev client passes all eight checks (`§10:` cases, 255).
- [x] **`MAP_SHARED` for devices** — `file_ops.map_page`, `VMA_SHARED`; refused (and said once) for anonymous
  memory and regular files, `EACCES` past the descriptor's mode, `EINVAL` past the device's end.
- [x] **musl user space** — `third_party/c/musl` (1.2.5) + compiler-rt binary128 routines; `musl_binary`. Board:
  stdio, malloc and `strtold` (`§10: a program built against musl runs`).
- [x] **`mprotect` after `fork` broke copy-on-write** — found while building the above: a still-shared page was
  re-mapped writable. Fixed (`protected_bits`); board control `minikern.protectshares` fails the check (191).
