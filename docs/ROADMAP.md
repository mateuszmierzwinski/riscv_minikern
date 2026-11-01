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
- [x] **A death by signal reported as one** — `wait4` gave a child killed by SIGSEGV as `WIFEXITED` with code 245 (the stored −11's low byte), and `waitid` said `CLD_EXITED`: a shell or Go's `os/exec` could not tell a crash from an exit. Now `WTERMSIG` / `CLD_KILLED` with the signal; `exit` keeps its code's low byte (`exit(-1)` is 255, never a "signal"), and an unhandled user fault is recorded as SIGSEGV rather than `-EFAULT`. Board: the sigframe case reads its children's SIGSEGVs through `wait4`; "§P2: wait4 …" and QuokkaOS's `rc` unchanged.

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

- [~] **Realtime files on the board — needs the operator** — truncate, holes and unwritten extents in realtime files are built and host-judged (`xfs_repair -n -r`, extent sizes 4 and 16 KiB). A board run needs a second block device for the realtime subvolume; the board has the operator's NVMe and one SD card.

### Done

- [x] **An extent map of any height** — `xfs_fork.c` loads a tree of up to five levels and stores the least-height tree whose root fits the inode, each level filled evenly; the map holds 65,536 extents (was 4,096, and two levels: ~5,000 with 512-byte inodes). Host: `xfs_write_test` `tallfork` (7,000 extents → root level 2, cut to 900 → 1, to 5 → in the inode) on every image, `xfs_repair -n` clean. Board 2026-11-01: `/w_tall` in `fswrite` — 7,000 extents, root level 2, read back, removed — on the v5, `mkfs.xfs`-defaults and v4 RAM-disk cases.
- [x] **A log Linux left dirty is replayed** (was "needs the operator": a Linux-written dirty log is now made without root, by the host's own kernel under QEMU — `scripts/make_linux_log.sh`, fixture `test/testdata/linux_dirty_log.img.xz`). The replay reads items of any number of regions, operations split across records, and applies inode items with their forks (inline, extents, b-tree roots converted from the in-core layout, device numbers), buffer items with their checksums recomputed by block type (Linux logs buffers unsealed) and an inode buffer's unlinked pointers only, buffer cancellations, inode creates, dquots, and extent-free intents left unfinished; any other intent without its done item refuses the replay. The superblock's lazy counters are recounted and its quota fields re-read after a replay. Five older bugs found by the fixture: **transaction operations were stamped `LOG`, which Linux's recovery skips** — a log this kernel left dirty would have been ignored by Linux; the logged-core conversion lost the 64-bit extent count and swapped big timestamps' halves; the dquot version was taken to be 3 (it is 1), so every Linux quota record was refused; quota records were placed end to end (they are packed per block) and read through the file size (Linux leaves it 0); and the quota fields were taken from before the replay, so nothing was charged on a filesystem Linux had put quotas on. Host: `xfs_linuxlog_test` (replay, the files as the log left them, a write over the result, `xfs_repair -n` clean), `xfs_write_test` `efi` (an unfinished free finished, a finished one not repeated; `xfs_logprint` reads the items as EFI/EFD). Board: "§3: a log Linux left dirty is replayed" — 3 transactions, 247 items, and the file Linux truncated reads at its new size. **The fuzzer then found** that the superblock's log fields were shift counts before they were checked (§9).
- [x] **Inodes whose fork is past the inode's 412-byte cache** — read, not refused: `xfs_inode_fork` hands a reader the cached copy, or the inode's own block with that block's format and extent count (a caller's older copy of the inode with fresh bytes cut off the newest extent); the extent and b-tree lookups, inline file data, shortform listing and the shortform-to-block conversion use it, attribute buffers cover any literal area, and the writer's cache guards are gone. Host: `xfs_write_test` runs every step on 1 KiB and 2 KiB inodes, and `bigfork` reads and grows an 848-byte shortform directory — `xfs_repair -n` clean. Board: "§3: the write path on 1024-byte inodes" — `/big`'s 848-byte fork read, every `fswrite` step on 1 KiB inodes; its attribute-leaf step needed 80 fillers, not 12, because a 1 KiB inode now keeps a whole fork.
- [x] **Attributes past one leaf block** — an attribute fork in node format, read and written (`xfs_attrnode.c`): a full leaf is compacted, then split, and the fork's one leaf becomes a node index at block 0, on the da-tree shared with directories (`xfs_danode.c`, now behind a port: each owner supplies its block I/O); the fork's extent map becomes a b-tree past the inode (`xfs_fork.c` serves either fork); the last attribute removed leaves an empty shortform fork; a remote value's blocks are given back when its attribute goes. Fixed on the way: removing a remote attribute zeroed the leaf's `usedbytes`. Host: `xfs_write_test` `manyattrs` — 1,500 attributes, every tenth 3,000 bytes, node fork in a b-tree, listed and read, half kept for `xfs_repair` to judge, a second file emptied — on every image. Board 2026-11-01: `fswrite` sets 600 attributes on one file (node format — no single leaf holds them), lists them, reads a sample across a remount and removes them all, on the v5, `mkfs.xfs`-defaults and v4 cases.
- [x] **Directories of any size** — the node directory's hash index is a da-b-tree of any height (`xfs_danode.c`: path descent, node splits up to a root that grows a level in place, keys kept equal to each child's highest hash, emptied nodes removed), and the free index a run of free blocks (each describing 2,016 data blocks, an emptied one given back). Was: one node (~177,000 names) and one free block (~338,000). Removal finds its data block through the hash index (2.7 → ~0.3 ms a name at 300,000). Host: `xfs_write_test` `bigdir` — 200,000 40-byte names, root level 2, a second free block, every other name removed and both halves looked up, the rest removed — `xfs_repair -n` clean; 600,000 names checked by hand. Board 2026-11-01: the rewritten node path in the v5, `mkfs.xfs`-defaults and v4 `fswrite` cases (1,200 names, 5 leaf splits) and the 8 KiB-directory-block case; the multi-level index itself is host-proven only.
- [x] **A split leaf's index update used a stale inode** — `split_leaf` added the new leaf to the fork and then updated the index through the caller's copy of the inode; once the directory's fork was a b-tree that a store rebalances, the copy named the wrong block and a create at 108,698 names answered `ENOENT`, half-made. The inode is re-read after the fork changes.
- [x] **A full free list refused frees** — a truncate of ~900 scattered extents merged reverse-map blocks onto the group's free list until `xfs_agfl_ensure` answered `NOSPC` for the next free; the list is now drained back to free space (`drain`, raw free and raw unmap, as Linux's `xfs_alloc_fix_freelist`). Host: `xfs_write_test` (defaults image, every step then `tallfork`) failed before, passes after, `xfs_repair -n` clean.
- [x] **A VFS layer, `mount`/`umount2`, overlay** — `core/vfs.c` port and longest-prefix mount table (FAT, netfs, overlay); `mount("netfs")` under `CAP_SYS_ADMIN`; overlay with copy-up and whiteouts. Board 2026-10-29 (`overlaygo`, FAT `/boot` cases).
- [x] **`/proc`, `/sys`, `/dev` as filesystems** — `kernel/synthfs.c` (stat, listing, read; writes `EPERM`) over `procfiles.c`: `/proc/self/{maps,status,comm}`, `stat`, `uptime`, `loadavg`, `cpuinfo`, `mounts`, `version`, THP and CPU files under `/sys`. Board `procgo` (re-run 2026-11-01).
- [x] **`/proc` depth** — per-hart `cpuN`, `ctxt`, `btime`, `processes`, `procs_running`; user/system split from trap stamps; `/proc/<pid>/`. Board `procgo`, `cpugo`.
- [x] **`/proc/ps/`** — `by-pid/`, `by-user/`, `by-name/`, one value per file. Board `procgo`, `namego`.
- [x] **Device nodes by path** — `/dev/null`, `zero`, `urandom`, `random`, `console`, `tty`; stat as character devices, list in `/dev`. Board `procgo`.
- [x] **`ioctl` and raw terminal mode** — `FIOCLEX`/`FIONCLEX`/`FIONBIO`/`FIONREAD`, termios with canonical and raw modes (`VMIN`/`VTIME`); an unhonoured bit is refused whole. Board `ioctlgo` (re-run 2026-11-01), `rawgo`.
- [x] **`ppoll` and `pselect6`** — over each object's readiness, sigmask kept until a handler runs. Board `pollgo` (re-run 2026-11-01).
- [x] **`.` and `..` resolved against the tree** — `core/pathres.c`, one component at a time, links expanded first, `ELOOP` at 40. Host `pathres_test`; board `dotgo`.
- [x] **Timestamps, `UTIME_OMIT`, `ftruncate`, unlinked-while-open files** — `stat`/`statx` report times (bigtime honoured at all 30 sites), a removed file a writer holds is renamed aside until the last close. Board `timesgo`, `unlinkgo`.
- [x] **The writer's speed** — `xfs_write_test` ~4½ min → 48 s (slice-by-8 CRC32C; gprof).
- [x] **`chmod` over netfs** — `OP_CHMOD` in the protocol, netfsd sets permission bits through `os.Root` and refuses setuid/setgid/sticky (`EPERM`, not dropped); the kernel's `vfs_ops.chmod`. Host: `service_test`, `fsadapter_test` (escape refused); board `netfsgo` 2026-11-01: 0600 and 0644 round-trip through `stat`, setuid refused with `EPERM`.

- [x] **Directory and file syscalls** — `getdents64`, `fstat`, `newfstatat`, `statx` (honest `stx_mask`), `statfs`, `faccessat`, `mkdirat`, `unlinkat`, `renameat2`, `linkat`, `symlinkat`, `readlinkat`, `fchmodat`, `fchownat`, `utimensat`, `lseek`, `fsync`/`fdatasync`, `openat` create/truncate; board-proven.
- [x] **`truncate`/`ftruncate`, `umask`, `chdir`/`getcwd`** — free-block accounting asserted on the board; `umask` read by create (`0022`→`0644`); relative paths resolve in `copy_in_path`; all inherited across fork/exec.
- [x] **B-tree format forks** — keyed descent in `xfs_bmbt.c` behind `xfs_bmap`, every disk number checked; board: 3,000- and 40,000-entry directories list completely, payload checksums exact.
- [x] **Directory hash index** — `xfs_dirhash.c` name hash, leaf binary search and node descent with collision runs; board requires `0 walked` lookups.
- [x] **B-tree board case** — the expectation was wrong, not the reader; the case asserts `cat: hash index answered`.
- [x] **8 KiB directory blocks** — passes on the board (2026-10-31: `/p2999`, 12 hashed, 0 walked, checksum exact). Not a reader bug: the reader at `9edc86b7` reads the same `mkfs.xfs -n size=8192` image on the host (3,000 files); the board failures predate the single-address kernel.
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
- [~] **Voltage scaling — needs the operator** (M) — Three of four operating points want 800 mV and run at 900 mV (measured, below). **The register map is now sourced** from mainline Linux (`include/linux/mfd/axp20x.h`, `drivers/regulator/axp20x-regulator.c`, `jh7110-common.dtsi`): the core rail `vdd_cpu` is **DCDC2, register 0x14**, 7-bit selector (bit 7 preserved), 500 mV + 10 mV × sel up to 0x46, then 1220 mV + 20 mV × (sel − 0x47) to 0x57; **800 mV = 0x1E, 1040 mV = 0x36** — which is where the old "0x36" came from (it is also the PMIC's I²C address on `i2c5` and the power-key register). Enable is 0x10 bit 1 (never clear it); 0x1A bit 6 says DCDC2/3 run dual-phase. Linux writes the selector in one step with no ramp. **Measured read-only (2026-11-01): 0x14 reads 0xA8 — selector 0x28, 900 mV** (bit 7 set, preserved by any write); DCDC1 and DCDC6 decode to 3.3 V, which confirms the map against the hardware; 0x1A = 0, not dual-phase. So the firmware already runs the cores at 900 mV at 1.5 GHz, not the 1040 mV the tree names, and the saving left is 900 → 800 mV at the three low points (`minikern.probe` prints the decoded rail). Writing it is the operator's call: raise voltage before clock and lower clock before voltage, clamped to 0x1E..0x28 (800..900 mV, never above what the firmware chose).
- [~] **USB** (L) — The XHCI driver runs the VL805 (5 ports, 32 slots, port map read from capabilities), but USB2 port 1 completes its reset and stays in Polling (`40000ee1`) with `PED` clear. Next: the VL805 errata or a missing hub-level step, starting from the recorded trace. HID and mass storage come after that.
- [~] **eMMC — needs the operator** (S) — The DesignWare driver binds to both controllers, but the eMMC socket is empty. Test it once a module is fitted.
- [ ] **Tickless network** (M) — The polled network keeps a 10 ms beat (~1,200 interrupts in 10 s against 304 without a network). Next: an RX interrupt for idle traffic, plus the stack reporting its next timer deadline (retransmit, DHCP, ARP) so the scheduler can arm a one-shot.

### Done
- [x] **SPI and PWM beyond the probe** — judged through the operator's existing 60–63 jumper by routing the signals onto those pads (signal numbers from Linux's `jh7110-pinfunc.h`). `/dev/spidev0.0`: `SPI_IOC_MESSAGE` (up to four full-duplex transfers), mode and word-size queries (mode 0, 8-bit; others refused), maximum speed; with `minikern.spi.jumper` (TXD out on 63, RXD in from 60) a program's bytes come back down the wire (63), unrouted they do not (39, the control). `/dev/pwmchip0`: Linux 6.13's PWM character device rather than sysfs (this kernel's synthetic files are not writable) — request/free per descriptor, round, exact-or-`EDOM`, set, read back, no phase (127). The counter runs at **51.2 MHz**, measured; and with `minikern.pwm.jumper` the kernel samples channel 0 on pad 60: at 25 % it reads 75 % high, at 75 % 25 % — the JH7110's PWM is inverted-polarity only, as StarFive's driver says. Mainline Linux has no driver for this PWM at all. (RTC: none fitted on this board.)
- [x] **I²C beyond the probe** — `/dev/i2c-N` for every bus the tree's `/aliases` name (four come up), numbered as Linux numbers them, root-only: `I2C_RDWR` with one message or a write-then-read pair to one address (repeated start), `I2C_FUNCS`; anything else refused rather than split. The bus driver's transfer is general now (`i2c_dw_transfer`), its old register calls facades over it. Board, read-only: "§4: /dev/i2c-5 from a program, the PMIC read" — the id registers `10 00 00 54` and the core rail `0xa8` through the user interface, three messages `EINVAL`, a bad array `EFAULT` (127).
- [x] **GPIO pad-function selection (pinmux)** — the 2-bit function-select field per sys pad (function 0 = GPIO), the layer under DOUT/DOEN this driver never wrote. Its layout comes from Linux's JH7110 sys pinctrl table (irregular, so tabulated: `gpio_jh7110_funcsel_of`, every run's ends host-tested; GPIO0-5 have none), and the direction calls now claim the pad first, as Linux's do. Measured: on this board's firmware **no** sys pad starts in a dedicated function (`minikern.gpio.funcs`) — StarFive's U-Boot routes JTAG through the input table, not the function select — and the pad-config bits were never the blocker either (0x09 is input-enable plus pull-up, 0x01 input-enable alone). Board: pad 63 put in function 1 reads back 1, and through the 60–63 jumper the wire works both ways once driving claims it; the control (`minikern.gpio.noclaim`) fails both ways. `minikern.gpio.claim=<c>:<pin>,…` claims named pads.
- [x] **The watchdog, armed and fed** — measured first: the counter runs at 24 MHz, and control bit 1 is the reset (armed and starved, the board came back through U-Boot; `minikern.wdtprobe`/`wdtreset` kept for the next person). `minikern.watchdog=<s>` arms it and the kernel feeds it once a second from the beat, so a kernel wedged on its lock is reset; `/dev/watchdog` (root, 0600) hands the feeding to a process as Linux's does — keep-alives, `WDIOC_GET/SETTIMEOUT`, one holder (`EBUSY`), the magic close handing it back; a close without it leaves it unfed. It is stopped at power-off, which fails on this board and would otherwise turn every halt into a reboot. Board: "§4: the watchdog, fed by the kernel", "§4: /dev/watchdog, held and handed back" (63), "§4: the watchdog resets a board nothing feeds". Found on the way: a refused second open ran `release` and dropped the real holder's claim.
- [x] **The first SD data read overran the FIFO** — the cause was U-Boot's internal DMA controller, which reads this card before handing over and was only disabled (`BMOD` 0), never reset. Found by measuring rather than by the two theories tried first: draining the FIFO in bursts (fewer register reads per word) changed nothing, and a trace of the failure (`minikern.mmctrace`, now kept) showed the card had sent exactly 512 bytes and the error came after the last word — with a host-side byte count of 8,420. Bring-up now resets the engine (`BMOD.SWR`) and clears `IDSTS`. Board, `minikern.disk=sd minikern.nomount`: a retry in 3 of 3 boots before, 0 of 4 after. The retry stays as a counted safety net.
- [x] **The thermal half** — the governor runs: `core/governor.c` asks the host-tested policy every 250 ms of the housekeeping beat and carries the answer to the divider (`kernel/govern.c`), with the die sensor's reading (an unreadable or implausible one is taken as critical) and the busiest hart's load (four U74s share one divider). Thermal mode by default — load taken as 100 %, so the board stays at 1.5 GHz unless hot and no benchmark moves; `minikern.governor=ondemand` follows load, `=off` stops it, `minikern.governor.throttle=<mC>` moves the throttle point. Host: `governor_test` (8 cases, fake sensor/load/divider). Board: "§4: the governor throttles on the die sensor" (throttle 20 °C, die 38 °C: 1.5 GHz → 750 → 500 → 375 MHz in 0.8 s, each step logged with its reason) and "§4: ondemand follows the load" (375 MHz idle, back to 1.5 GHz within 0.75 s of the spinner). **Found on the way: the beat had room for 4 hooks** and a board boot registers up to 8, every caller ignoring the refusal — the console poll, the HDMI refresh or `cpuwhy` could vanish silently by registration order. 12 now, and a refusal prints.

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

### Done
- [x] **Bits lost across a blocking `recvfrom`** — no longer reproduces. It failed 4 of 4 runs when recorded; on 2026-11-01 the program was put back in its original shape (no `volatile` on the mask, no prints between the steps — the two things that had been changed to study it) and passed 4 of 4, and in its current shape 2 of 2. Which of the context-switch and signal fixes since then cured it is not identified; the case ("§5: two threads over a unix-domain socket") keeps guarding it.
- [x] **GMAC strict unicast filter** — the cause was the order of two register writes: the address registers reach the receive clock domain when the **low** word is written, and the driver wrote the low word first, so the filter matched against a high half (and an `AE` bit) it never received — while both read back perfectly from the register side. High word first, as Linux's stmmac does; the strict filter is now the default and the board runs promiscuous only with `minikern.netpromisc`. Board: with `minikern.netfilter` (the old lever) a netfs-loaded `timego` passed every step over unicast TCP; then as the default, "§5: a DHCP lease…", the QuokkaOS case and VNC from the PC pass, with `not-ours` down from a stream of other hosts' frames to 1.
- [x] **GMAC as the network device** — DHCP, ARP, ping, TCP and DNS all work on the board (see the unicast filter workaround above).
- [x] **Routing table** — Eight prefixes, longest-prefix match ranked together with the on-link subnet and the default route. Masks with holes are refused (`test/route_test.c`, 10 host cases).
- [x] **TCP SACK** — Kept by operator decision. The receiver builds coalesced blocks from the reassembly queue. The sender stops retransmitting at the first SACKed block, and forged blocks are discarded. There is no full RFC 6675 scoreboard (host test, both counters).
- [x] **Unix-domain sockets** — Stream and datagram, as a third `sock_ops` with names in a kernel table (exact for the abstract namespace). `src/userland/unixsock` exits 127 on the board.
- [x] **`netlink`** — `RTM_GETLINK`, `RTM_GETADDR` and `RTM_GETROUTE` answered synchronously, with `NLMSG_ERROR` for unimplemented requests. Board program 127/127, so Go's `net.Interfaces` works.
- [x] **DNS** — `/etc/resolv.conf` is synthesised from the DHCP lease. `src/userland/resolve` exits 31 on the board. This work also added the GMAC interrupt (`interrupts-extended`) and fixed the `AE` bit.

---

## 6. Security

### Open

### Done
- [x] **The CVE suite's three named gaps** — `sigframe.elf` (board, "§6: hostile signal frames, a full descriptor table, a path pulled away", 127): a signal delivered onto a stack pointer aimed at the kernel or at nothing, `rt_sigreturn` through such a pointer, the descriptor table filled to `EMFILE` and recovered, and a path argument unmapped and remapped by another thread through 3,000 `openat`s. Each hostile case runs in a child; the parent reporting is the kernel surviving. **It found the worst bug of the round: signal delivery wrote its 1,088-byte frame wherever the thread's `sp` pointed, unchecked** — the control (check removed) wrote it over kernel memory at `ffffffc0003ffbc0` without a fault. Delivery now writes only where `uspace_can_write` allows and otherwise ends the process with SIGSEGV (handler reset first, as Linux's `force_sigsegv`), `rt_sigreturn` reads only where `uspace_can_read` allows, a forged or clobbered frame ends the process instead of returning (its `ecall` never steps, so it re-ran for ever), and each refusal is printed — the case requires the "not written" lines, which the control lacks. The standing obligation — a case for every new syscall and every CVE class (Rule 25.2) — is now a rule in CLAUDE.md §5, not an item.
- [x] **W^X for user mappings** — `mmap` and `mprotect` refuse a mapping both writable and executable with `EACCES` (Linux's answer under SELinux `execmem`); the JIT's write-then-flip to read-and-execute still works. `minikern.allowwx` is the control. Board: "§6: W^X — no page both writable and executable" exits 63 (all six checks); the control exits 58 (the two refusals let through, nothing else changed).
- [x] **Permission checks on every mutating path** — `kernel/perm.c`: write-open, create (`O_CREAT`/`O_EXCL`), mkdir, unlink, rename, link, symlink, chmod, chown, utimensat, truncate, execve; every `*at` call resolves its `dirfd`. Board `permgo` (re-run 2026-11-01): as uid 1000 every write to a root-owned file is refused.
- [x] **The kernel's direct map is randomised** — a random 1 GiB slot per boot, seeded from `/chosen` `kaslr-seed`/`rng-seed` and the timer (`minikern.nokaslr` is the control). The kernel image itself stays at its link address: tried as a PIE and reverted, by the operator's decision (2026-10-29).
- [x] **Capabilities and `seccomp`** — 41 capabilities with full sets, `capget`/`capset` v3, and seccomp strict plus classic-BPF filter mode checked before the syscall table. There are no file capabilities. Board: `minikern.privsep` exits 0.
- [x] **ASLR** — Stack, trampoline, `mmap` floor and heap gap are each randomised by ~12 bits, with the trap path feeding the entropy pool. Proved on the board across two runs. `minikern.noaslr` turns it off.
- [x] **CVE regression suite (Rule 25)** — Host parser cases for the device tree, XFS, partitions and ELF (`//:cve`), plus `minikern.hostile=1` on the board (accepted-bitmask 0). It found and fixed three real defects.

---

## 7. Boot and delivery

### Open

### Done
- [x] **Local boot from SD** — the board boots unattended from the card's FAT partition (saved `bootcmd` + boot menu default, `docs/BOOTING.md`); root by `ROOT` label, or netfs. A FAT-only card no longer takes the FAT as its root. Board 2026-10-29: power-on to QuokkaOS serving `statsd` in 55 s; netboot 30/30 clean after the YT8531 RGMII-delay fix. The raw-partition variant is dropped (operator).
- [x] **Own netboot** — `minikern.netboot[=<ip>]` fetches the root image over MKNB (`kernel/netboot.c`, up to 64 MiB, `minikern.rootfs.sha256=` and `minikern.netboot.key=` pin an Ed25519-signed server) into the RAM-disk root; server `src/go/tools/mknbserver`.
- [x] **Describe an unknown board** — `minikern.mem=`, Linux `earlycon=uart8250,…`, `minikern.timebase=` replace the tree's values (`core/boardspec.c`). Host `boardspec_test`; board: all three applied.
- [x] **Kernel in the top half** (M) — The kernel runs only high (linked at `0xffffffc000200000`), and since step C a process's table carries only the kernel's top-half root entries: the identity map of RAM is not reachable from user tables at all. Every physical-address-as-pointer use goes through `mmu_phys_to_virt` (page tables, frames, kernel stacks, the loader, swap, disk buffers); `mmu: N identity addresses converted after the switch` counts any that are missed (0 on every board case). `minikern.lowmap` restores the old sharing as the control. `copy_in`/`copy_out` take user addresses and were rightly left alone — converting them was the likely cause of the old attempt's `fork` hang.

---

## 8. Performance

### Open
- [ ] **NVMe beyond 334 MB/s** (M) — One queue, one command in flight. Interrupt completion now works (opt-in, see Done). Next: several commands in flight and several queues; measured on reads only, since the drive holds the operator's data.

### Done
- [x] **2 MiB pages zeroed out of the lock** — the longest hold left in a Go run after the console work was a store page fault zeroing a transparent huge page (gone with `minikern.nothp`). Two steps: frames are zeroed and copied a 64-bit word at a time, not a byte (4.85 → 2.56 ms; what is left is DRAM bandwidth), and a pool of four zeroed 2 MiB blocks is kept — filled at boot, refilled by idle harts 256 KiB at a time **with the lock released**, after the hart has unloaded any process table. Filling only from idle harts left one fault zeroing inline in every run whatever the pool size (2, 4 and 8 tried): idle harts reach the idle loop only when a program starts. The program's longest hold: **2.0 ms → 184–234 µs** (`go_lock_hold_us`; control `minikern.nozeroahead`, 2.0 ms); every huge page of the run came from the pool. Go speedup unchanged (2.1–3.3×).
- [x] **Long work out of the tick — console output, measured and moved** — the record's "8.2 ms in the timer interrupt" was stale; measured per cause on 2026-11-01 (`klock` now names an external interrupt's PLIC source, and keeps a second longest-hold record from the program's start, so boot's one-offs stop hiding the steady state). In a Go run the holds were, in turn: **`mmap` 62 ms** — a per-4 KiB lookup over a 64 MiB `MAP_FIXED` arena; ranges now stride over empty table spans (`pgtable_empty_span`, host-tested, also used by `fork`'s copy) → gone; **`clone` 8.5 ms** — a leftover diagnostic `kprintf` at 115200 baud; **`write` 8.0 ms** — the program's own console output sent byte by byte with the lock held. Program output is now **queued** (`core/txring`, 16 KiB) and the UART's transmit interrupt feeds the FIFO, whose depth was measured (16 bytes: whole at 16, lines lost at 32 — the part's parameter register reads 0). Kernel lines stay synchronous so they survive a hang, and send the queue first to keep order; the routine ones that landed inside programs (a futex `EAGAIN` trace, the deliberate nil-dereference's `demand:` line, the per-hart timer check, the boot's ping replies) are now behind `minikern.errors`, said only when wrong, or counted for the end report. The program's longest hold: **8.0 → 4.85 ms** (`go_lock_hold_us` in `bench.py`); `minikern.consolesync` is the control (8.0 ms in `write`). Boot's own longest is the DHCP reply in the GMAC interrupt, once.
- [x] **Interrupt-driven I/O** — SD completions by interrupt are now the default (`minikern.mmcpoll` keeps polling): 15.6 MB/s at 50 MHz and a mounted boot's writes clean through it. **NVMe's interrupt works** (`minikern.nvmeirq`, first waited command woken, 64 MiB at 291 MB/s against 331 polled). What had been recorded as a PCIe demultiplexer problem and an `irqwait` that ignored its deadline was neither: measured, a completion sets INTA (bit 24 of `+184`, as assumed) and reaches PLIC source 57, and `irqwait` timed out at exactly its 2 s. The fault was in the order of two calls — the handler masked its source and the trap then completed the claim, and **the PLIC ignores a completion for a source that is not enabled**, so the gateway never forwarded 57 again after the stale assertion that fires at arming. Handlers now ask for the mask *after* completion (`irq_mask_when_completed`), and `submit` re-enables the source per command. NVMe interrupts stay opt-in: the default would put every write to the operator's data drive through a path proven on reads only.
- [x] **SD at 16.2 MB/s, from 8.2** — high-speed mode: `CMD6` switches the card (its 64-byte status read and checked; `CMD6` gets an explicit data flag, since `ACMD6` shares its index), then the 50 MHz input undivided. At 50 MHz the stream failed its data CRC in the third block every time, with 126 of 128 FIFO words held — the FIFO filling mid-block — so the card read threshold now holds the card between blocks until one fits (a 32-phase sample sweep tried first passed at every phase and was dropped). The ceiling after that was the CPU's drain, two register reads per word: the FIFO is now emptied in bursts by its level. Board, `minikern.diskbench=16:16`: 8,308 → **16,173 KiB/s**; the control `minikern.sdhs=0` gives 9,486 (the burst drain alone); a write round trip and a mounted boot (FAT and XFS, log rotation) clean at 50 MHz. Internal DMA would be the next step past this.
- [x] **The spinner's hart: 0 timer interrupts, not ~400** — the benchmark's first run read the `smp: switches/ticks` line, which counts from boot, and so folded the boot hart's periodic start-up into a figure meant for one busy thread; every image back to `b266fce` read the same, which looked like an old regression. The spin test now reports its own window (`spin: timer interrupts in the window by hart`): the spinner's hart takes **0** in 15 s, the idle harts ~607 each (the beat, which thermal sampling keeps alive on every boot, moving between them), and `minikern.periodic=1` gives 1,503–1,505 on every hart. `bench.py` reads the window and holds the spinner to a ceiling of 5.
- [x] **Big kernel lock — not split (operator decision)** — Measured on 4 harts: 8.1% of acquisitions wait and the average hold is ~6 µs, so the outliers are one long section. `klock_tag` names the cause (board).
- [x] **`me()` without a scan** — `index_by_id[]` is filled after hart enumeration, which leaves `sscratch` untouched. `smp_hart_misses` is now reported (0 on four harts, board).

---

## 9. Testing and tooling

### Open

### Done
- [x] **Board benchmark suite** — `test/hil/bench.py` boots the board for every figure in `docs/BENCHMARKS.md` (Go 8-unit speedup ×5, NVMe and SD reads, the spinner's interrupts, missed pokes and lost deadlines), reads each with the expression the doc gives a person, appends every reading to `docs/benchmarks.tsv`, and fails on a measure that regressed past its own run-to-run spread, on a must-be-zero counter that is not, and on a measure that produced no reading. Disks are only read: `minikern.nomount` (new) attaches the device and mounts nothing. First run: 6 of 6, row `9edc86b77`+ in BENCHMARKS.md — and it found the item below.
- [x] **Fuzzing harness** — four libFuzzer harnesses (`test/fuzz/`), one per parser of outside input: ELF, device tree, EDID, and the start of an XFS disk (mount, replay, walk the root, read attributes and file heads). Each is built twice: `<name>_replay`, a `cc_test` in the `cve` suite that replays every committed input under the monorepo's central corpus (`//testdata/fuzz/src/c/experiments/riscv_minikern/<Name>/`, staged by the same `fuzz_corpus` rule the Go fuzz tests use), and `<name>`, the `manual` libFuzzer binary `gen_fuzz_c.sh <name> [seconds]` builds with the whole tree instrumented (coverage, ASan, UBSan; clang in its own output base, because the host toolchain is gcc) and harvests crashers from. The XFS harness reseals every v5 checksum it recognises, or the fuzzer spends its run proving the CRC check works (482 edges → 640). **Found on its first run: the superblock's log fields were used as shift counts before they were checked** — `dirblklog` 255 is undefined behaviour in `take_geometry`, and `inodesize` 0 divided by zero in `check_geometry`; both bounded (`pow2_or_zero`), the crasher kept as a seed. Then clean: EDID 84 M, ELF 100 M, FDT 1.6 M, XFS 2.6 M + 18 M inputs.
- [x] **Coverage** — `gen_coverage.sh`: `bazel coverage` over the host tests (the rest is cross-built and cannot link a coverage runtime), LCOV kept as `coverage.lcov`, a per-directory summary printed. First reading: **86.9 %** of the 13,467 lines the host tests compile; XFS 85.8 %. Not sent to Sonar: the server has no C analyser (CLAUDE.md §2).
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
  On a TV, also boot with `minikern.hdmimode=hdmi` (HDMI mode with an AVI infoframe) and check it shows.
  And `minikern.hdmicursor`: a white arrow with a black outline at the screen's centre.
  And `minikern.hdmi=auto`: the log should name the monitor and the mode it prefers.
  Last, whether the controller snoops the L2: a build with the beat's flush removed — if `fbdemo`'s square
  still moves smoothly, the flushes are unnecessary.
- [ ] **Acceleration** — the cursor plane is done (below). Next: the overlay planes and the scaler, which mainline does not cover (only `OVL_CONFIG_EX(n) = 0x2540 + 4n`); the DC8200's planes are the real 2D acceleration (overlays, scaler, hardware cursor);
  there is no blitter. The PowerVR BXE-4-32 is an XL project of its own (closed MIPS firmware, ~35k-line
  reference driver, unsupported in Mesa). Measure memset/memcpy and a full-frame flush first.

### Scrapped for now (operator, 2026-11-01)

- **GL in user space, and the C stack on top of the kernel (musl).** No further work on user-space GL
  or on growing the musl-based C runtime. What exists stays: musl 1.2.5 and the compiler-rt routines
  remain vendored under `third_party/c/`, because `fbtest.elf` and `fbdemo.elf` link against them and
  their board cases pass. The item as it stood: own small software GL or Mesa's software renderers;
  Mesa would need a minimal KMS (`/dev/dri/card0`: dumb buffers, page flip).

### Done
- [x] **Cache maintenance, smarter** — dirty rectangles, as DRM's `DIRTYFB`: `MINIKERN_FBIO_DIRTY` on `/dev/fb0` names a changed rectangle, the kernel keeps the union of the rows marked (clipped by `fbinfo_dirty_rows`, host-tested), and the 10 Hz beat flushes only those — while any descriptor that marked is open; a program that never marks keeps the whole-frame flush. Board: "§10: only the rows a program marked are flushed" — 32 rows, 245,760 of 8,294,400 bytes in 269 µs against 2,227 µs for a frame; the plain framebuffer client unchanged. Whether the controller snoops the L2 at all (which would make the flush unnecessary) still needs a monitor: it joins the by-eye check.
- [x] **Mode from the EDID** — `minikern.hdmi=auto` powers and clocks the display before any framebuffer exists, reads the monitor's EDID, and takes its preferred timing when the transmitter has settings for that size and the pixel clock is the mode's within 1 % (`display_mode_for_monitor`, host-tested: 1080p and 720p at 60 and 59.94 Hz chosen; 1366×768 and 1080p30 not guessed at); otherwise 1920×1080, saying why. `/dev/fb0` is then sized from the chosen mode (both modes' lines are multiples of the controller's 128-byte pitch). Board, no monitor attached: "§10: HDMI mode chosen from the monitor, or the default without one" — hot-plug low, the default, scanning out. With a monitor it is part of the by-eye check above.
- [x] **The hardware cursor** — the first of the DC8200's planes in use: a 32×32 ARGB8888 arrow (`display_cursor_arrow`, host-tested) in the kernel's image (below 4 GiB, as the controller's 32-bit addresses need), loaded and placed through display 0's cursor block from the mainline cross-check (`dc8200_cursor_show`). `minikern.hdmicursor` puts it at the screen's centre. Board: "§10: the hardware cursor, placed and read back" — config `0x0E`, our address, 960,540. Whether it shows is part of the by-eye check above.
- [x] **A way to see the VFB from the PC** — QuokkaOS `vncd` (`src/go/quokkaos/cmd/vncd`): RFB 3.8, view only, raw encoding in any true-colour format a viewer asks for, incremental updates by 64-pixel tiles, behind VNC authentication with the password re-read from a 0600 file for every attempt (an exposed file is refused), a per-address lockout after five failures, a session cap and a handshake deadline. Host: `rfb_test` and `rfb_cve_test` (the RealVNC "None" bypass, the LibVNCServer cut-text and encoding-count lengths, impossible pixel formats, off-screen update rectangles, brute force, slow handshakes, connection floods). Board: "§10: the VFB seen from the PC over VNC" — vncd off netfs on the board, judged from this machine by `vncprobe`: a wrong password refused, then 640x480 "minikern" and a full update of 1,228,800 bytes.
- [x] **AVI infoframe / HDMI mode** — `minikern.hdmimode=hdmi` frames the mode in HDMI mode (0x52 bit 1) and loads the AVI infoframe for its VIC into the packet buffer (0x9F = 6, bytes from 0xA0, version and checksum included — the vendor's buffer omits both); DVI stays the default, as every monitor takes it. Board: "§10: HDMI mode with an AVI infoframe" — the mode bit set, the infoframe for VIC 16 read back as sent, the scanout at 30 frames in 0.5 s. Whether a TV accepts it joins the operator's by-eye check above.
- [x] **Permissions on `/dev/fb0`** — root's and the `video` group's, 0660, as on Linux (gid 39, Fedora's; `minikern.videogid=` overrides). A registered device now carries an owner and mode that `stat` reports (synthfs) and `open` enforces (`uspace_may_access`); the memory devices and the console stay 0666. Board: in "§10: /dev/fb0 reads, writes and seeks…", a uid-1000 child outside the group gets `EACCES`, one in group 39 is let in.
- [x] **`read`/`write`/`lseek` on `/dev/fb0`** — a positional device keeps its offset in the descriptor (`struct file.pos`), `read`/`write` are `pread`/`pwrite` there, and `lseek` works on any device with `pread` and a `size` op (`SEEK_END` = the framebuffer's length); `fdobjects_seek` split into `seek_span`/`seek_store`. Board: the same case exits 127 — write, seek back, read the same bytes, position 4, `SEEK_END` = `smem_len`, 0 at the end.

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
