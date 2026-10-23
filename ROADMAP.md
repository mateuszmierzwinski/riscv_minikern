# ROADMAP — what is missing

What this kernel does **not** do yet, so that the gaps are a list rather than
a discovery. `FEATURES.md` is the other side of this: what works today, and
what proves it.

Each item says what it is, why it matters, and roughly what it costs. The
sizes are honest estimates, not promises:

- **S** — a day or less; mostly mechanical.
- **M** — a few days; a new subsystem or a real protocol.
- **L** — a week or more; changes something structural.

Ordering inside a section is roughly by what unblocks the most.

---

## 0. Known defects

- [ ] **A four-hart race that corrupts user memory.** (M) A stock Go binary
      fails its own concurrency checks about 3 runs in 40 under emulation
      and 1 in 8 on the board: a contended counter ends short, or a channel
      yields values nobody sent. Down from one run in three after two fixes
      (a thread slot freed while still in use; an `mmap` hint that checked
      only the first page of its range). A minimal four-thread C program
      using the same primitives — raw `clone`, atomics and a futex mutex —
      **passes**, so what remains is something a Go runtime does that it
      does not. `CLAUDE.md` lists everything ruled out and the detectors
      left in the tree.
- [ ] **`sched_end_siblings` leaves threads running on other harts.** (S)
      It marks them `THREAD_DYING` and the process is torn down; a thread
      still executing on another hart runs until its next trap with its
      address space being freed underneath it. It has not been seen to
      fail, which is not the same as being right.

---

## 1. Process model

- [ ] **Real `fork` with copy-on-write.** (L) Today `clone` makes either a
      thread or a **vfork** child that borrows its parent's address space
      until `execve`. Everything that expects `fork` — a shell, `os/exec`
      with a pipe between parent and child, anything that forks and keeps
      running — does not work. Needs COW in the page tables, a fault handler
      that copies, and refcounted physical pages.
- [ ] **Process groups and sessions.** (M) `setpgid`, `getpgid`, `setsid`,
      and the job-control signals (`SIGTSTP`, `SIGCONT`, `SIGTTIN`). Needed
      before a shell is usable.
- [ ] **A controlling terminal.** (M) Line discipline, canonical mode,
      `TIOCGWINSZ`. Today the console is a write-only sink with a read path
      that is not a terminal.
- [ ] **Users and groups.** (M) There is one privilege level: everything
      runs as root with no checks. `getuid` and friends are absent; file
      permissions are stored and ignored.
- [ ] **Resource limits.** (S) `prlimit64` answers but enforces nothing.

---

## 2. Memory

- [ ] **Demand paging.** (M) Pages are backed eagerly at `mmap` time, so a
      large reservation that is never touched still costs nothing (it is
      left unmapped), but a large *mapping* costs all of it immediately.
- [ ] **Copy-on-write.** (L) Needed by `fork`, and by `MAP_PRIVATE` file
      mappings. Shares the refcounting work with `fork`.
- [ ] **File-backed mappings.** (M) `mmap` of a file is not supported, so a
      dynamic loader cannot work and neither can anything that maps a file
      to read it.
- [ ] **`mremap`.** (S) A runtime that grows a mapping has to unmap and
      remap instead.
- [ ] **Swap.** (L) Not planned; listed so the absence is deliberate.
- [ ] **Huge pages for user space.** (M) The kernel's own identity map uses
      1 GiB pages; user mappings are 4 KiB only.
- [ ] **`MAP_FIXED_NOREPLACE`, `MAP_NORESERVE`, `MAP_POPULATE`.** (S) Flags
      are largely ignored rather than honoured or refused.

---

## 3. Filesystem and VFS

- [ ] **A VFS layer.** (L) There is exactly one filesystem, mounted at one
      place, with no mount table. No `mount`/`umount`, no second filesystem,
      no overlay.
- [ ] **`/proc`, `/sys`, `/dev`.** (M) Nothing synthetic exists. A runtime
      that reads `/proc/self/maps` or `/sys/kernel/mm/transparent_hugepage`
      gets `ENOENT`, which Go tolerates and most other things do not.
- [ ] **Device nodes.** (M) No `/dev/null`, `/dev/zero`, `/dev/urandom` as
      *files* — the descriptor layer has the objects, but nothing opens them
      by path.
- [ ] **The directory and file syscalls.** (M) Missing: `getdents64`,
      `lseek`, `fstat`/`fstatat`/`statx`, `mkdirat`, `unlinkat`, `renameat`,
      `linkat`, `symlinkat`, `readlinkat`, `truncate`/`ftruncate`, `fsync`,
      `statfs`, `chdir`/`getcwd`, `access`/`faccessat`, `umask`, `chmod`,
      `chown`, `utimensat`. `openat` exists but only for reading an existing
      regular file.
- [ ] **`ioctl`.** (S for a stub, M to mean anything.)
- [ ] **`poll`/`ppoll` and `select`/`pselect`.** (S) `epoll` is implemented;
      these are not, and plenty of software uses them instead.

### XFS specifically

What is read today: v5 only, shortform and single-block directories, *local*
and *extents* inode formats, a clean log, `reflink`/`rmapbt` read-only,
sparse inodes, `nrext64`, exchange-range, parent pointers, `bigtime`,
`meta_uuid`, `ftype`, `finobt`, `inobtcnt`.

- [ ] **B-tree format forks** (`XFS_DINODE_FMT_BTREE`). (M) **The biggest
      gap.** A file with enough extents — any large or fragmented file —
      moves its extent map into a b-tree and is refused outright. A
      filesystem that has been written to for a while is full of them.
- [ ] **Leaf and node directories.** (M) A directory that outgrows a single
      filesystem block becomes `leaf` and then `node` format. Neither is
      read, so a directory of more than a few dozen entries cannot be
      listed.
- [ ] **Multi-block directory blocks** (`-n size=` greater than the block
      size). (S) Refused at mount.
- [ ] **Log replay.** (M) A dirty log is refused rather than replayed, so an
      unclean shutdown needs `xfs_repair` on another machine. Deliberate for
      now — guessing at a log is how data is lost — but it is a gap.
- [ ] **Extended attributes.** (M) Not read at all, which is also why parent
      pointers are accepted but unusable.
- [ ] **External logs, quotas, the realtime subvolume, v4 filesystems,
      inode sizes other than 256 and 512.** (M each) Refused.
- [ ] **Writing beyond the simple cases.** (M) Create, unlink and write
      exist; rename, hard links, directory growth into leaf format, and
      allocation under fragmentation do not.

---

## 4. Drivers and board hardware

- [ ] **The board's Ethernet (DesignWare GMAC).** (L) The network stack and
      sockets work over virtio-net; on the VisionFive 2 there is no driver,
      so the board has no network. The cable is in the **second** controller
      (`ethernet@16040000`), which is the one that negotiates.
- [ ] **GPIO pad-function selection (pinmux).** (M) Driving a pin works, but
      only for a pin the firmware already left as GPIO. Claiming a pad's
      function means the `sys_iomux` registers, which should be *measured*
      on the board rather than taken from a datasheet — see how the
      pad-config block was found (`CLAUDE.md`).
- [ ] **Thermal and frequency scaling.** (M) `sfctemp` and the clock tree.
      Nothing reads the temperature, so nothing throttles.
- [ ] **USB.** (L) The board's VL805 is enumerated on PCIe and otherwise
      ignored. No XHCI driver, no HID, no mass storage.
- [ ] **eMMC.** (S) The DesignWare driver already binds to both controllers;
      the on-board eMMC socket is simply empty here. Worth testing with a
      module fitted.
- [ ] **SPI, I²C, PWM, RTC, watchdog.** (M each) None exist.
- [ ] **Interrupt-driven storage and network.** (M) Both drivers poll. The
      PLIC is wired and the tick uses it, but no device does.
- [ ] **DMA for SD.** (S–M) The DesignWare controller has an internal DMA
      engine; the driver uses the FIFO, a word at a time. That is most of
      what stands between 8.2 MB/s and the bus limit.

---

## 5. Networking

- [ ] **The GMAC driver** — see above. Everything else here is behind it on
      real hardware.
- [ ] **IPv6.** (L) The stack is IPv4 only.
- [ ] **A routing table.** (S) One interface, one gateway, no routes.
- [ ] **TCP SACK.** (M) Deliberately not planned: with a reassembly queue a
      loss already costs one round trip.
- [ ] **Unix-domain sockets.** (M) Nothing local can talk over a socket.
- [ ] **`netlink`.** (M) No way for a program to ask what interfaces exist.
- [ ] **DNS.** (S) The resolver address is learned from DHCP and stored; no
      resolver exists in the kernel, which is correct — but nothing in user
      space can resolve a name either.

---

## 6. Security

- [ ] **Any privilege separation at all.** (L) Everything runs with full
      rights. No uid/gid checks, no capabilities, no `seccomp`.
- [ ] **ASLR.** (M) The stack, the heap and `mmap` are all at fixed
      addresses. `AT_RANDOM` is filled from the entropy pool, but nothing is
      actually randomised.
- [ ] **A CVE regression suite (Rule 25).** (M) **This gate has not been
      run for this project.** The claim that it "carries over from the ARM
      project" was false and is withdrawn in `CLAUDE.md`: the ARM tree was
      deleted and nothing was carried. What exists is `tcp_hostile_test`,
      which is one class of one. The product class is a kernel that parses
      untrusted input — a filesystem on a **removable card**, an ELF a user
      supplies, packets off a network — and that is what the suite has to
      cover.
- [ ] **W^X enforcement for user mappings.** (S) `mprotect` is honoured, but
      nothing refuses a mapping that is both writable and executable.

---

## 7. Boot and delivery

- [ ] **A real SD-card boot image.** (M) The kernel is loaded over the
      network by U-Boot today. An image with U-Boot, a partition table and
      the kernel, plus the documented flash procedure, is what makes it a
      machine someone else can start.
- [ ] **Netboot of its own.** (M) The ARM kernel had an authenticated
      netboot protocol (MKNB, Ed25519-verified offers). The code carried
      over; nothing is wired to it.
- [ ] **The kernel in the top half of the address space.** (M) It still runs
      identity-mapped in the bottom half, which `share_kernel_mappings`
      works around — and that workaround has already cost one
      memory-corruption bug.
- [ ] **A device-tree overlay or command-line way to describe a board**
      this kernel has not met. (S)

---

## 8. Performance

Measured on the board, so these are gaps with numbers rather than guesses.

- [ ] **SD beyond 8.2 MB/s.** (M) Multi-block transfers and the card's
      standard 25 MHz got it from 2.5. Next is high-speed mode (`CMD6`, 50
      MHz) and the controller's internal DMA instead of a word-at-a-time
      FIFO.
- [ ] **NVMe beyond 334 MB/s.** (M) One queue, one command in flight,
      polled. Several queues and interrupt completion is the next step.
- [ ] **Interrupt-driven I/O generally.** (M) Everything polls, so a hart is
      busy for the whole of every transfer.
- [ ] **The big kernel lock.** (L) One hart in the kernel at a time. It is
      what makes four harts safe without auditing every structure, and it is
      the ceiling on anything syscall-heavy. Splitting it is a large change
      and should wait until something is actually measured to be blocked on
      it.
- [ ] **`me()` scans the hart table on every call.** (S) A cached index in
      `sscratch` would make it a load.

---

## 9. Testing and tooling

- [ ] **The CVE suite** — see §6.
- [ ] **A fuzzing harness.** (M) The monorepo has a central corpus and a
      staging convention; nothing here feeds it. The filesystem and ELF
      parsers are the obvious targets, and both already have hostile-input
      tests that could seed it.
- [ ] **Coverage.** (S) No coverage is collected for this project.
- [ ] **A benchmark suite that runs on the board** rather than a
      command-line flag. (S) `minikern.diskbench` exists and is useful;
      making it a test that records numbers over time would catch a
      regression.
