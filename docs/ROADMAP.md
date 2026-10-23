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

- [ ] **A four-hart race that corrupts user memory.** (M) **Still open**, and
      measured again rather than remembered. A stock Go binary fails its own
      concurrency checks: a contended counter ends short, a channel yields
      values nobody sent, a `WaitGroup` reports its own state as impossible,
      or a thread faults at an address no program could have produced. Down
      from one run in three after two fixes (a thread slot freed while still
      in use; an `mmap` hint that checked only the first page of its range).
      A minimal four-thread C program using the same primitives — raw
      `clone`, atomics and a futex mutex — **passes**, so what remains is
      something a Go runtime does that it does not.
      **Today's rates**, measured in one session with one binary:

      | Configuration | Runs | Failures |
      |---|---|---|
      | Board | 24 | 1 |
      | Board, `minikern.onetid=1` | 16 | 1 |
      | QEMU | 60 | 1 |
      | QEMU, `minikern.onetid=1` | 60 | 1 |

      So it is now **about one run in twenty on the board**, against the one
      in eight this file recorded, and the emulator and the board agree for
      the first time. What changed in between is this session's work; which
      part of it is not established, and the table is here so the next
      measurement has something to be compared against.
      **One thing it is not**: the thread-id defect fixed in §1. Every
      thread used to report id 1, so a Go runtime preempted goroutines with
      `tgkill(pid, 1, SIGURG)` and every preemption signal reached whichever
      thread was numbered 1. That looked like an excellent candidate — and
      `minikern.onetid=1` puts it back, which is what the second and fourth
      rows are. They are indistinguishable from the first and third. The
      lever stays in the tree because a cause that has been *excluded by
      measurement* is worth more than one that was never suspected, and
      because the first small sample (1 in 8 against 0 in 16) pointed the
      other way: it took 40 more runs to stop believing it.
      **A sharper signature than before**, from the one board failure:
      `store access fault at sepc 0x4020f3fe, stval 0x40000000` with
      `sp 0x407b7e30`, reported as a fault *in user code*, after 306,450
      signals and 3 million syscalls. That pc is inside the kernel's own
      text and that sp is in the kernel's identity map: the thread was
      returned to user mode with **kernel registers**, which no program can
      arrange. `uspace_impossible_frames` now tests exactly that on every
      return to user mode and reports the frame where it is restored rather
      than where it faults.
      `CLAUDE.md` lists everything ruled out and the detectors left in the
      tree.
- [x] **An embedded program's stack was not in its own address-space
      record.** (S) **Found and fixed** by §2's `mremap` test, which is the
      first embedded program to both use its stack and call `mmap`. The raw
      program path mapped 64 stack pages and recorded none of them, so
      placement believed those addresses were free and handed out **the top
      eight pages of the running program's own stack** — and `mmap` drops the
      pages of a range before recording it, so the stack under the program
      went back to the allocator. The ELF path records them, which is why
      real programs were never affected and this sat in the tree unseen.
      Fourth instance of "the record has to include everything in the
      address space", and the pattern is now clear: it breaks once per
      **path** that builds one, because each maps something the others do
      not. `uspace_stale_records` turns the next one into a report — it
      counts a page found mapped at an address the record called free, which
      only a move is in a position to notice.
- [x] **`sched_end_siblings` leaves threads running on other harts.** (S)
      **Fixed, and it was two defects and a hole, not one.** It marked the
      siblings and the caller carried straight on — and what a caller does
      next is replace the address space (`execve`) or free it
      (`exit_group`), so a sibling still executing user code on another hart
      ran against page tables being taken apart underneath it. Nothing
      faults: it reads and writes pages that by then belong to something
      else.
      **Marking is not stopping.** A thread is told to leave its hart by a
      per-hart preemption flag, which the trap path reads on its way back to
      user mode — so it only works if a trap happens. A thread spinning in
      user mode makes no syscalls, and under dynamic ticks its hart may have
      no deadline armed at all, so nothing was going to interrupt it. It now
      gets an interrupt aimed at it: the opposite of `poke_idle_harts`'s
      rule, which skips a busy hart because a busy hart finds work by
      itself, and is why it is a separate function with the reasoning
      written down.
      **And `sched_yield_now` undid the marking**: it set `THREAD_RUNNABLE`
      unconditionally, and the preemption path is exactly where a thread
      that has been ended or stopped finds out — so a dying thread came back
      as runnable and was re-claimed. Measured before the fix: the kernel
      waited 6,179 µs for a thread to leave a hart it was never going to
      leave, getting its answer only from the window between a yield and the
      next claim. The same line meant a `SIGSTOP` could not stop a thread
      running on another hart, which no test had tried.
      Then the caller **waits**, blocked rather than spinning: this hart
      holds the kernel lock, and the sibling cannot reach the kernel to
      notice it has been ended until that lock is free, so spinning is a
      deadlock and yielding hands the lock straight back. The departing
      hart wakes the waiter, with the deadline as the backstop — waking on
      *every* switch instead, which is where it started, kept the waiting
      thread runnable so the hart never idled and the lock was never
      released: a livelock rather than a slow wait.
      `execve` refuses rather than risking it if a thread will not leave;
      `exit_group` keeps the address space instead of freeing it, because a
      process's worth of pages is a bounded loss and freeing memory another
      hart is writing through is not.
      Verified on the board (`minikern.endsib=1`): a child process with a
      spinning thread that makes no syscalls, ended by `exit_group` from its
      main thread. **5**, with `1 poke aimed, 1 woken by a departure, 1
      look` and a **20 µs** wait where the broken mechanism took 6,179 µs —
      four runs for four, after a rate of roughly one hang in two.
      **The process-group test had to be rewritten**, and that is part of
      the result rather than collateral. It sent `SIGSTOP` to *its own*
      group, which includes the sender — so once `SIGSTOP` could actually
      stop a running thread, the test stopped itself and deadlocked. It had
      passed only because the stop did not work. It now does what a shell
      does: puts the child in a group of its own with `setpgid`, stops that
      group, and **proves the stop** with a `WNOHANG` probe showing the
      child has not finished although its own work was far shorter than the
      wait. The exit status alone could not tell "stopped" from "ran
      through", so the old test was not checking what it claimed.
- [x] **An idle hart was never told the process had ended.** (S) **Found by
      the test above**, and it is the reason that test hung in silence
      rather than failing. The scheduling loop reads "is the process over?"
      only after a wake, so a hart asleep in `wfi` learns it when something
      interrupts it — and with nothing else due, that was whatever tick
      happened to come next, or nothing at all. The hart stayed in the
      scheduler, and `uspace_run` waits for every hart to leave before
      freeing what they are standing on, so the machine stopped with **no
      output of any kind**: the least diagnosable state this kernel has, and
      indistinguishable from a dead board. It is the rule the scheduler
      already follows for work appearing, applied to work running out.
      Two changes: the harts are poked when the process ends, and the wait
      in `uspace_run` — which cannot be bounded, since there is nothing
      better to do than wait — now **says who has not left and what they
      are running** rather than hanging quietly. That report is what caught
      it, after several runs whose only evidence was an empty log.
- [x] **A poke to the boot hart was never delivered.** (S) **Found by the
      above.** Software interrupts are enabled per hart, and only the
      secondaries' path (`timer_init_cpu`) enabled them — the boot hart
      enabled the timer alone. It cost nothing for as long as pokes only
      ever went to *idle* harts, because a hart wakes from `wfi` on a poke
      whether or not the interrupt is enabled; the moment one was aimed at a
      busy hart to make it stop, the one aimed at the boot hart went
      nowhere. `trap_software_interrupts` counts the pokes actually **taken**
      as traps, as against sent, because that difference is invisible
      otherwise.

---

## 1. Process model

- [x] **Real `fork` with copy-on-write.** (L) **Done.** The child gets an
      address space of its own, made of the parent's pages copy-on-write;
      nothing is copied until one of them writes. Underneath: refcounted
      physical frames (saturating, and sticky at saturation), Sv39's
      software bits carrying the copy-on-write mark, and a fault handler
      that copies or takes over depending on how many holders remain.
      `src/userland/forktest` is the proof rather than a demonstration —
      both processes write the same global, the same stack local and the
      same mapped page, and each checks it sees only its own.
      **The "still owed" note here was wrong and is withdrawn.** It said
      `wait4` does not wake a forked child and leaves the parent blocked.
      Measured on the board: a child exits with 7, the parent's `wait4`
      wakes **after one round**, reaps it, and reads the status back through
      the `WEXITSTATUS` shift. `minikern.wait=1` is that test.
      What the measurement did find was a different bug, in the raw
      embedded-program path rather than in `wait4`: `uspace_build` mapped
      its text page without **recording** it, and `give_address_space`
      walks the record and nothing else — so a forked child resumed with no
      text and took an instruction page fault at its own entry. The ELF
      path records its segments, which is why real programs forked
      correctly and only the hand-written ones did not. Fixed, and it is
      the third instance of the rule this kernel keeps relearning: the
      address-space record has to include everything in the address space,
      because placement reads it too.
- [x] **Process groups and sessions.** (M) **Done.** `setpgid`, `getpgid`,
      `setsid`, `getsid`, and `kill` — which did not exist at all before
      this, only `tgkill` — with the three forms a shell uses: a pid, a
      group as a negative pid, and 0 for the caller's own group. Group and
      session are inherited at `fork` and survive `execve`, so a shell can
      put a job in a group before exec'ing into it, and a fresh process
      leads its own group and session, which makes the first process a
      leader with no special case.
      **`SIGSTOP` and `SIGCONT` came with it**, which `PREEMPTION_MODEL.md`
      had listed as not built. They act on the process rather than on a
      handler, so they are the scheduler's: a new `THREAD_STOPPED` state
      rather than a flag, because a thread stopped during a sleep must stay
      stopped when the sleep expires, and a state gets that right without
      every wake path remembering to check.
      Measured on the board (`minikern.pgrp=1`): a parent confirms it leads
      its own group, forks, sends `SIGSTOP` to the **group**, sleeps long
      enough that a child which ignored it would have finished, sends
      `SIGCONT`, and reaps the child's status. Exit 7 means every step
      worked — a job-control signal reaching only part of a group is worse
      than one reaching none, so the child's own observation is what is
      checked rather than the syscall's return.
      **Not done:** `SIGTSTP` and `SIGTTIN` need a controlling terminal to
      be raised *from*, which is the next item; and `kill` raises on the
      process's first live thread, since there is no process-wide pending
      set — right for what a shell sends, wrong for a multi-threaded
      program catching them, and said so in the code.
- [x] **A controlling terminal.** (M) **Done.** Canonical mode with echo,
      `^C`/`^Z`/`^D`/erase/kill, `TIOCGWINSZ`/`TIOCSWINSZ`,
      `TIOCGPGRP`/`TIOCSPGRP`, and `SIGTTIN` for a background reader —
      because two programs reading one keyboard would each get an arbitrary
      half of every line.
      The editing rules are **pure and host-tested** (`core/tty.c`,
      `test/tty_test.c`): every mistake in them is a plausible behaviour
      that needs a human's fingers to notice — a backspace that erases the
      prompt, an echo that prints a control character as itself, a `^D`
      that throws away the line it was meant to deliver.
      The kernel half does what only it can: pull bytes off the wire,
      raise the key's signal **on the foreground group**, and block a
      reader. `ioctl` exists now and answers `ENOTTY` for anything that is
      not a device, which is how a program finds out whether its output is
      a terminal.
      Verified on the board by **typing at it**: a program asks its window
      size, blocks on `read`, and a line typed over the serial line arrives
      through the discipline and comes back out. That needed a harness that
      sends as well as captures.
- [ ] **`SIGTSTP` from the keyboard to a shell's job.** (S) The terminal
      raises it on the foreground group already; what is missing is
      anything that sets a foreground group, which is a shell's job and
      waits for one to exist.
- [x] **Users and groups.** (M) **Mostly done, and the gap is named.**
      Real and effective uid/gid on every process, inherited at `fork` and
      surviving `execve`, with `getuid`/`geteuid`/`getgid`/`getegid` and the
      privileged `setuid`/`setgid`. The first process is root, because
      something has to be — a machine whose first program cannot read its
      own filesystem does not boot.
      `setuid` changes **both** ids: setting only the effective one leaves a
      real uid of 0, and a process with that can take privilege back, which
      is the difference between dropping it and appearing to. Verified on
      the board (`minikern.uid=1`): root, drop to 1000, both ids moved, and
      `setuid(0)` refused.
      The permission rule is pure and host-tested (`core/access.c`), and the
      case it exists for is the **exclusivity**: a mode of 0077 denies its
      own owner, which only happens if the owner's class is the whole
      answer. A check that fell through from a denied owner to the group
      bits would make that mode mean its opposite.
      **XFS now parses ownership**, which it did not — `mode` was read and
      `uid`/`gid` were not, which is precisely why permissions were "stored
      and ignored". `xfsmount_stat` reports them.
      **What is missing:** nothing *calls* the rule yet, because there is no
      regular-file `openat` to call it from (§3). The check belongs where a
      process exists rather than in the filesystem layer — putting it in
      `xfsmount_read_file` was tried and reverted, since that would have the
      hexagon reaching outward and the dependency would be circular. Also
      missing: the unprivileged `setuid` that returns to a real uid one
      already has, and `setresuid`/saved ids.
- [x] **Resource limits.** (S) **Done for the two this kernel can enforce,
      and honest about the rest.** `prlimit64` answered every question with
      "infinite" and refused every attempt to set anything, so a program
      read a number nothing would hold it to.
      It now gets and sets, with POSIX's privilege rule — lower either,
      raise the **soft** limit up to the hard one, raise the **hard** limit
      only as root. Without that last part a limit is a suggestion a
      program can decline.
      **Enforced**, which is the half that was missing: `RLIMIT_NOFILE`
      bounds `fd_install`, and `RLIMIT_NPROC` is checked before a process
      slot is taken — counted **by uid**, because that is what the limit
      means. A per-process count would let every child start again from
      zero and bound nothing, which is exactly the shape a fork bomb has.
      `RLIMIT_STACK` reads the real stack size; everything else still reads
      as infinite, because that is the truth about it rather than a stored
      number nothing consults.
      Verified on the board (`minikern.rlimit=1`): read as 64 (the table's
      size, not infinity), lowered to 5, the fifth descriptor refused with
      `EMFILE` and the sixth never reached, and the hard limit unraisable
      after dropping privilege.

- [x] **`getpid` returned the constant 1.** (S) **Done**, and it is worth
      recording how it survived. It was written when there was one process
      and never revisited, which made it the quietest kind of wrong answer:
      every process believed it was pid 1, so anything comparing two pids
      compared two 1s and agreed. It was found by running §1's own
      process-group test as the **third** program in a boot rather than the
      first — `getpgid(0)` gave the real group and `getpid()` gave 1, and a
      test that had only ever passed because both numbers happened to be 1
      failed honestly.
      `gettid` was the same function, so every thread of a process reported
      the same id — and a Go runtime records that as its thread's identity
      and then preempts a goroutine with `tgkill(pid, that id, SIGURG)`.
      Since `tgkill` resolves by thread id, **every preemption signal was
      delivered to whichever thread was numbered 1**: the right signal, to
      the wrong thread. It is now the calling thread's own id.
      Verified on the board: the process-group test gives **7** as the third
      program in a boot as well as the first, which is the thing that was
      not true before — in a single boot that also runs the syscall-surface
      suite (0), `wait4` (7), the identity model (0), resource limits (0),
      `mremap` (63), the `mmap` flags (127) and the ending test (5).

---

## 2. Memory

- [x] **Demand paging.** (M) **Done.** A mapping now costs its pages when
      they are *touched*: `mmap` records the region and backs nothing, and
      the first fault on each page backs it with the region's rights.
      It needs the **region record** rather than the page tables, which is
      the whole reason this could not be done before `vma.c` was wired in:
      to the page tables an unbacked page and an address nobody ever asked
      for are identical, and backing the second would turn a wild pointer
      into a silently working one. `vma_lookup` answers what the tables
      cannot. A `PROT_NONE` reservation stays unbacked, and a write to a
      read-only region still faults — the rights come from the region.
      Measured on the board: **1396 pages** backed on first touch in a run
      of the Go program, which is the number that tells "lazy" from "eager
      somewhere else".
      **One interaction, and it is the important part of this change.** The
      kernel validates a user pointer by walking the process's page tables,
      so a buffer the program has not written to yet had no entry and a
      valid pointer read as `EFAULT`. The networking test caught it
      immediately — a socket read failing with "bad address" into a freshly
      allocated buffer. The validator now backs the page itself, which makes
      "may the process touch this" and "will a touch succeed" the same
      question. It backs rather than merely permitting because the kernel is
      about to write through the pointer, and a kernel fault is a panic
      where a program's would be ordinary.
- [x] **Copy-on-write.** (L) **Done** for anonymous memory, with `fork`
      above. Not done for `MAP_PRIVATE` file mappings, which need
      file-backed mappings first.
- [ ] **File-backed mappings.** (M) `mmap` of a file is not supported, so a
      dynamic loader cannot work and neither can anything that maps a file
      to read it.
- [x] **`mremap`.** (S) **Done.** Shrinking unmaps the tail; growing takes
      the pages past the end when they are free and otherwise moves the
      mapping, which needs `MREMAP_MAYMOVE` — a caller that cannot cope with
      its memory changing address says so by withholding it. A caller-chosen
      destination (`MREMAP_FIXED`) is honoured inside the window `mmap`
      places into and refused outside it, because the program's break is not
      in the region record and an overlap there is one nothing could see.
      `MREMAP_DONTUNMAP` is refused rather than approximated.
      **Nothing is copied**: the frames that exist are re-pointed at the new
      addresses and the holes stay holes, so a move costs page-table entries
      and no memcpy. `uspace_moved_pages` makes that a measurement rather
      than a claim — on the board, a four-page mapping grown to eight across
      a blocker reports **4 pages moved and none newly backed**.
      Growing in place is `vma_insert` of the tail, and the insert **is** the
      test of whether the space is free: a range that overlaps is refused and
      one that is adjacent with the same rights is merged into the region it
      extends. Two answers kept in step is how they drift apart.
      Verified on the board (`minikern.mremap=1`), whose exit status is a
      bitmask of six checks so a partial failure says which half works:
      **63**.
- [x] **The record `munmap` never cleared.** (S) **Done**, and it was a live
      defect rather than a tidy-up. Nothing removed a region from the
      address-space record when a program unmapped it. While mappings were
      backed eagerly that was an address-space leak — the addresses stayed
      spoken for, so the gap search never offered them again. Demand paging
      made it worse than a leak: a fault inside a recorded region is backed
      on the spot, so a read of memory the program had **returned** found a
      region that still claimed it and was handed a fresh zero page. A
      use-after-free that silently works is the opposite of what unmapping
      is for.
      The two halves of unmapping are now separate functions, because the
      callers want different ones: a fixed `mmap` drops pages and records a
      new region over the same addresses, `mprotect` to `PROT_NONE` drops the
      pages of a region that is still there as a reservation, and a
      shrinking `brk` drops pages the record never described. Only `munmap`
      wants both. The record goes first, since it is the only step that can
      fail having changed nothing.
- [ ] **Swap.** (L) Not planned; listed so the absence is deliberate.
- [x] **Huge pages for user space.** (M) **Done**, for 2 MiB pages, and only
      when a program asks: `MAP_HUGETLB` was in the refused list until there
      were huge pages to give, which is the right place for a flag to wait.
      Sv39 needs nothing special for one — a leaf at level 1 **is** a 2 MiB
      page — but it does require the virtual *and* physical addresses to be
      2 MiB aligned, so the frames are asked for with that alignment and
      **placement had to learn about it too**: aligning an address after a
      gap has been found is not the same as finding an aligned gap, because
      the aligned address can fall out of the bottom of a gap that was only
      just big enough and land on the region below. `vma_find_free` takes
      the alignment, and the host test covers the case where the only gap
      cannot hold an aligned mapping — the answer there is no, not an
      address inside somebody else's memory.
      **Not transparent, on purpose.** A Go runtime reserves tens of
      megabytes of `PROT_NONE` arena and commits corners of it; backing any
      large mapping with 2 MiB pages would commit 2 MiB per first touch and
      waste most of it. The flag is the program saying it wants that trade.
      A block that cannot be allocated contiguously falls back to 4 KiB
      pages rather than failing — a mapping that works slowly is worth more
      than one that does not exist — so `uspace_huge_pages` counts the
      entries that were really huge. It has to, because **a program cannot
      see the difference**: the only thing distinguishing this from a
      quietly 4 KiB mapping is that number.
      Three paths had to learn the difference between a page and a mapping.
      Freeing reads the span: `pgtable_lookup` reports the physical address
      *inside* a megapage, so freeing what it returns would hand back one
      frame from the middle of a block and leak 511 while the unmap took all
      512 out of the address space. A partial `munmap` or an `mremap` of a
      huge region is **refused** rather than splitting it, because splitting
      is the opposite of what the caller asked for. And `fork` **copies** a
      touched block: copy-on-write marks live in the page-table entry, and
      one entry covers all 2 MiB, so a copy-on-write fault would have to
      copy the whole thing anyway or split the page and stop being what the
      program asked for — paying it at `fork`, once and predictably, is the
      better half of that trade. An untouched block is not copied.
      Verified on the board (`minikern.huge=1`): **127**, all seven checks
      — accepted, 2 MiB aligned, both blocks writable, the last word 4 MiB
      in, a partial unmap refused, a forked child seeing the markers, and
      the child's write *not* reaching the parent's copy — with the kernel
      reporting **4 huge pages**: two backed in the parent and two copied
      into the child.
- [x] **`MAP_FIXED_NOREPLACE`, `MAP_NORESERVE`, `MAP_POPULATE`.** (S)
      **Done**, and the item turned out to be about more than three flags.
      What it said — "flags are largely ignored rather than honoured or
      refused" — was the defect: the code had three constants and a comment
      claiming the rest were advisory on Linux too, which was true of some
      and a quiet lie about the others. Each flag now has one of **three**
      answers, written down where it can be checked:
      **Honoured.** `MAP_POPULATE` backs the whole mapping at `mmap`, which
      is what a caller asks for when it would rather pay up front than take
      a fault per page — the thing demand paging took away. `MAP_LOCKED`
      populates too, because that is what locking does on Linux and a locked
      mapping whose pages arrived later would not have been locked. Advisory
      both there and here: running out part-way does not fail the mapping,
      since the rest faults in as it would have anyway.
      `MAP_FIXED_NOREPLACE` is `MAP_FIXED`'s insistence on an address with
      the opposite answer to a collision — `EEXIST` rather than destruction
      — and that is the whole reason it exists: without it a program's only
      way to learn whether an address was free was to take it, and a wrong
      guess cost whatever was there.
      **Vacuous.** `MAP_NORESERVE` asks for memory not to be committed up
      front, which since demand paging is what *every* mapping does; there
      is no swap, so nothing can be evicted either. `MAP_STACK`,
      `MAP_NONBLOCK`, `MAP_DENYWRITE` and `MAP_EXECUTABLE` ask for nothing
      this kernel has. Recording that is not the same as ignoring them, and
      writing it down is what stops the next reader "implementing" one.
      **Refused**, which is the half that matters: a flag accepted and not
      delivered leaves the caller's assumption standing. `MAP_SHARED`
      without shared frames is a private mapping two processes each believe
      the other can see, so their writes go nowhere; `MAP_HUGETLB` answered
      with 4 KiB pages breaks the one assumption that is the reason to ask;
      `MAP_GROWSDOWN` would promise a mapping that grows, and nothing here
      grows one. Each is `EINVAL` with the name in the log, once, since
      `EINVAL` alone tells a program nothing.
      It also found a wrong bound in the fixed path: `at + bytes >
      USER_STACK_TOP` **wraps**, so an address a few pages below 2^64
      compared as being inside user space. It was not a hole — the page
      tables refuse a non-canonical address — which is how a wrong check
      survives.
      Verified on the board (`minikern.mapflags=1`): **127**, all seven
      checks. The populate half is the kernel's own counter, because a
      program cannot see it: `4 populated up front` with the demand count
      unmoved, so the write to the first page took no fault.

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
- [x] **Frequency scaling.** (M) **Done**, and it needed one register rather
      than the clock-tree work this entry assumed: the board's four
      operating points are 1500 MHz divided by 1, 2, 3 and 4, and the system
      generator's word at index 1 is that divider. Measured on the board at
      100%, 50%, 33% and 25% of the top rate. `drivers/cpufreq_jh7110.c`.
- [ ] **Voltage scaling.** (M) The remaining part of DVFS and where the
      power saving is: three of the four operating points want 800 mV while
      the board runs at 1040 mV for the top one, so every low point is
      currently run at a wasteful voltage.
      **The I²C driver is no longer the blocker — it is written and
      verified** (`drivers/i2c_dw.c`, identity register `44570140`), the
      PMIC answers, and its chip id reads `0x54` at register `0x03`. What is
      missing is which register holds the core rail. The device tree says
      the cores want 1040 mV, which encodes as `0x36` under this family's
      documented 0.5 V + 10 mV a step, and **no register on the part holds
      `0x36`**. `0x20..0x2b`, where a related AXP keeps its DCDC voltages,
      read `0x0b` almost uniformly — and the identity registers prove the
      read path is real and per-register, so that block is not the voltage
      map here.
      This is the one peripheral where a wrong write destroys the part
      rather than hanging the machine, so it waits for an authoritative
      AXP15060 register map. `minikern.probe=1` prints the whole map for
      whoever has one.
- [ ] **The thermal half.** (S–M) Nothing reads `sfctemp`, so the
      governor's thermal thresholds are configured and inert — which the
      code says of itself rather than looking live. The policy underneath
      them is built and host-tested now, including that an impossible
      reading is treated as the worst case, so this is a sensor driver
      rather than a design.
- [ ] **USB.** (L) The board's VL805 is enumerated on PCIe and otherwise
      ignored. No XHCI driver, no HID, no mass storage.
- [ ] **eMMC.** (S) The DesignWare driver already binds to both controllers;
      the on-board eMMC socket is simply empty here. Worth testing with a
      module fitted.
- [ ] **SPI, I²C, PWM, RTC, watchdog.** (M each) None exist.
- [ ] **Interrupt-driven storage and network.** (M) Both drivers poll. The
      PLIC is wired and the tick uses it, but no device does — virtio-net
      registers a handler and the poll is still what drives it.
      **This is now the last periodic interrupt in the system.** With
      dynamic ticks a busy thread takes 1 timer interrupt in 10 seconds and
      an idle hart 101; turn a network on and the machine pays a 10 ms beat
      for ever, because a polled device has to be asked. Measured:
      ~1,200 interrupts in 10 seconds against 304 with no network. The beat
      also exists for the stack's own timers (retransmit, DHCP renewal, ARP
      expiry), and those do not need polling either — they need a
      **deadline**: if the stack reported when it next wants to be called,
      the scheduler would arm a one-shot and an idle network would cost
      nothing at all. Doing both would make the kernel genuinely tickless.
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
- [x] **A CVE regression suite (Rule 25).** (M) **Run, and it found three
      real defects.** Four surfaces parse untrusted input here. Three are
      pure parsers tested adversarially on the host — the device tree, XFS,
      the partition table and now the ELF loader (`test/cve/`) — grouped as
      `//:cve`. The fourth, **syscall arguments**, needs page tables and a
      process, so it is an embedded program run on the board
      (`minikern.hostile=1`) whose exit status is a bitmask of the hostile
      arguments the kernel *accepted*; it reads 0.
      What it found: a NULL dereference in `elf_parse`'s entry condition,
      and two from the bottom-half mapping below — user space could read
      and write the kernel's own memory through any syscall that took a
      pointer.
- [ ] **Extend it as the kernel grows.** (S, repeatedly) Rule 25.2 says the
      audit is repeated and not a one-time event. The syscall-surface
      program covers pointers and one arithmetic argument; it does not yet
      cover TOCTOU across a syscall, descriptor-table exhaustion, or the
      signal-delivery path. Every new syscall is a new case — the suite
      caught `sched_setaffinity` as *already correct* only because it was
      written days before the suite.
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
      works around — and that workaround has now cost **three** bugs: one
      memory-corruption bug in the teardown path, and two security holes
      the CVE suite found, where the kernel's own addresses passed every
      `< USER_TOP` pointer check in the syscall layer. Those are fixed by
      checking permissions instead of a constant, which is the right fix
      regardless; moving the kernel out of the user half would make the
      whole class unrepresentable.
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
