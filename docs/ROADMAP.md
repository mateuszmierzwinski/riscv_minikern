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

- [x] **A filesystem image can be mounted from DRAM.** (S) **The fixture
      the filesystem work could not proceed without.** There was exactly one
      filesystem to read — the operator's SD card — so every gap in the
      reader could be described and not exercised, because producing a
      filesystem with the awkward shape means writing a card, and that is
      the operator's to do and was not available.

      `minikern.disk=ram` over `minikern.diskat=<address>:<length>`: the
      boot loader fetches an image into DRAM, the kernel reserves the range
      before the frame allocator runs and presents it as 512-byte sectors,
      so the partition scanner, the 4 KiB bridge and the block cache all see
      what they see on a real disk. `test/hil/fixtures.py` builds the images
      with `mkfs.xfs` and a protofile — no root, no mounting — and
      `board.py -i` fetches one.

      Two details that are the whole difference between a fixture that works
      and one that lies. `mkfs.xfs` refuses a filesystem under 300 MB, and
      such an image is nearly all holes, so it **gzips to about a megabyte**
      and U-Boot's `unzip` expands it: a 320 MB transfer becomes a trivial
      one. And the payload is `0..255` repeated rather than zeroes or random
      — zeroes make a checksum that cannot tell a correct read from a read
      of empty memory, random bytes make an image that will not compress,
      and the board proved the second one by overwriting the archive with
      its own decompression output and reporting it as a corrupt log.

- [x] **The board harness lives in the tree.** (S) **Done**, and overdue:
      every piece of hardware work this project has done drove the board
      with a throwaway script that was never committed, so each session
      rediscovered the serial ports, the relay commands, the U-Boot sequence
      and the load address — and staged a stale image more than once because
      nothing checked. For a kernel whose emulator "agrees with whatever you
      believed", that is the wrong thing to leave outside version control.

      `test/hil/board.py` is one boot: cut the power, restore it, catch the
      autoboot window, `dhcp`, fetch the image over HTTP, set the command
      line, `booti`, capture. Every step waits for U-Boot's prompt rather
      than sleeping a guessed interval, which is how the original lost a
      command inside the previous one's output and presented it as
      `Bad Linux RISCV Image magic!`. It adds `console=sbi
      minikern.halt=poweroff` to every boot, because a run that cannot be
      heard or does not stop is not a test, and the suite's first version
      forgot both.

      `test/hil/suite.py` is the cases, and `bazel test //…:hil_test` runs
      them. Two things make it quick enough to use constantly: the kernel
      runs **every self-test named on its command line in one boot**, so a
      dozen programs cost one power cycle; and the image is **built, hashed
      and re-staged before every run**, compared against what the web server
      will actually hand out. That check earns its place — `bazel cquery`
      prints an output's path without building it, and staging that way
      produced a board result for code that was not on the board during the
      `MADV_DONTNEED` work.

      Its first run reported six failures, all of which were **passes**: these
      self-tests exit with a bitmask of the checks that passed, and the ones
      that report a child's status exit with whatever the child chose. The
      expectations now come from each program's own exit paths. A suite that
      invents regressions is worse than no suite, because the next person
      debugs working code.

- [x] **`MADV_DONTNEED` and `MADV_FREE` were accepted and ignored.** (S)
      **Fixed, and verified on the board.** A real defect, found while
      hunting the item below — and *not* established as its cause, which is
      the next entry's business.

      Their name says advice and their contract does not. For private memory
      Linux **guarantees** that the pages are discarded and that the next
      read of those addresses returns zero. This kernel accepted the call,
      answered 0 and kept the pages, with a comment in `sys_madvise`
      explaining that discarding was optional and that "a runtime copes
      with" either choice. A runtime that has been told its memory is zero
      does not cope with being handed the last owner's bytes.

      `uspace_discard_range` is the fix: the pages go, the addresses stay,
      and the next touch faults a fresh zero page in the ordinary way. A
      file-backed range re-reads its file on the next fault, which is what
      Linux does and what `fill_from_file` already implemented. A range that
      would split a 2 MiB mapping is **declined rather than rounded**:
      discarding all of a huge page because a quarter of it was named would
      throw away memory the program still wants. `MADV_FREE` is honoured the
      same way — discarding at once is a correct implementation of the lazy
      form, and the fault it costs on the next touch is what the caller has
      already decided it can afford.

      **The test is behavioural and it fails without the fix.**
      `minikern.dontneed` writes a non-zero word into each of sixteen pages,
      **proves it is really there**, discards the range, requires every word
      to read zero, and writes and reads the addresses again to show the
      mapping survived. On the **board**: exit **0**, and exit **200** with
      `minikern.keepadvice`, which puts the old behaviour back for exactly
      this purpose. A test of a guarantee that cannot fail when the
      guarantee is broken is not a test.

      Its first board run exited **216** — an index the mapping does not
      have. The zero-checking loop had borrowed its limit from the filling
      loop, which counts 1..16 because it writes the index plus one, so the
      check read one page *past* the mapping; the emulator answered that
      page with zero and the board did not. A loop bound copied from a loop
      that starts somewhere else is the oldest off-by-one there is, and the
      board found it the first time it ran.

- [x] **The corruption of user memory — it was `MADV_DONTNEED`.** (M)
      **Fixed, and proved on the board with a control.** The defect is the
      entry above: `madvise(MADV_DONTNEED)` was accepted, answered 0 and
      ignored, and a Go runtime relies on what that call promises about what
      those addresses read as afterwards.

      | On the VisionFive 2, 2,500 rounds a boot | Result |
      |---|---|
      | the fix, run 1 | **2,500 rounds, nothing went wrong** (678,480 syscalls) |
      | the fix, run 2 | **2,500 rounds, nothing went wrong** (693,404 syscalls) |
      | `minikern.keepadvice`, run 1 | **failed at round 7** |
      | `minikern.keepadvice`, run 2 | **failed at round 9** |

      Five thousand clean rounds against a control that fails inside ten.
      `minikern.keepadvice` exists for exactly this: it puts the old kernel
      back, so the gate was watched failing before it was trusted passing.
      Both are standing cases in the board suite now.

      **This entry said, in the commit before this one, that the fix had not
      closed it.** That was measured under the emulator — four failures in
      four with the guarantee honoured — and it was wrong. The board says
      the opposite, unambiguously, with a control. Why the emulator
      disagreed is not established; the likeliest explanation is the
      staleness class of mistake that bit the board twice in the same
      session, because the QEMU scripts take the kernel's path from
      `bazel cquery`, which prints an output without building it, and they
      have no check of their own. What is certain is that the retraction was
      itself an emulator artefact, and that the operator's instruction to
      stop using the emulator arrived while this was being got wrong.

      **What the runtime was relying on**, stated at the level it can be
      defended: Go returns memory to the kernel with `madvise` and then
      hands those addresses out again without clearing them, because the
      call it made promised what they would read as. Keeping the pages is
      therefore not declining optional advice — it is telling a program its
      memory is zero while it still holds the last owner's bytes. The exact
      bookkeeping inside the runtime that turns the broken promise into an
      un-zeroed allocation is the runtime's business; the promise was the
      kernel's, and it was not kept.

      Everything the hunt excluded along the way stays excluded and stays
      instrumented — no frame reachable from two user addresses (among
      2,570 user pages on the board), no page backed twice, no mapping
      placed on a claimed range, the kernel lock exclusive, 28 user
      registers preserved across both kinds of context switch, and
      `minikern.mapchurn` showing that two separate mappings never become
      one piece of memory. None of them was the cause, and each is now the
      standing answer to a question this hunt had to re-argue from scratch.

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
- [x] **A kernel register set entering user mode.** (S) **Fixed**, and it
      was the one failure in this family whose mechanism is exact.
      `uspace_enter_frame` arms `sscratch` — the trap path's one source of
      truth for "this trap came from user mode" — and then spends five
      instructions in the kernel with interrupts on. A trap in that window
      took the *user* path, switched `sp` to the thread's kernel stack top,
      and wrote the kernel's own register set **over the frame that function
      was in the middle of loading**: the frame lives at exactly that
      address. On the way out the handler correctly saw `sstatus.SPP` say
      supervisor and returned, and the `sret` then entered user mode with a
      kernel program counter and a kernel stack pointer.
      It was named by its own address: an endless instruction page fault at
      `sepc 0x80204be8`, which `nm` places five instructions into
      `uspace_enter_frame`. The board had shown the same thing as
      `sepc 0x4020f3fe` with `sp 0x407b7e30` and called it a fault in user
      code. Interrupts are now off across the window — `sret` restores them
      from `SPIE` — and the window is **closed rather than narrowed**, since
      no instruction both arms `sscratch` and returns.
      `trap_mismatched_entries` is the invariant for the class: the entry
      code records which path it took in the frame's spare word, and C
      compares it against what the hardware says. The two disagreeing is
      precisely `sscratch` armed while the kernel is still running.
- [x] **Every detector was reported before the program ran.** (S) The whole
      block — unrecorded regions, replaced pages, slot aliases, stack
      overflows, spawn failures, hart misses, and everything added while
      hunting the race — sat at the end of *hart startup*, so it printed
      zeros in every run and could not have printed anything else. This file
      said those detectors "all read zero in the failing runs"; they did,
      because they were read before the run. **A detector reported at the
      wrong moment is worse than no detector**, because it is evidence of
      the wrong thing. They are printed after a program finishes now, which
      is how the dirty-frame report above was finally seen.
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
- [x] **`SIGTSTP` from the keyboard to a shell's job.** (S) **Done, and
      walking the path found two defects** — which is the argument for
      walking a path that every piece of is already built.
      **`SIGTSTP` killed the job instead of stopping it.** What a signal
      *does* to a process by default had two copies, one in `kill` and one
      in the terminal's group-signal path, and both had only half of it:
      `SIGSTOP` and `SIGCONT` were handled, and `SIGTSTP`, `SIGTTIN` and
      `SIGTTOU` fell through to ordinary delivery — where a signal nothing
      handles is fatal. So `^Z` ended the foreground job, and a background
      `read` killed the reader rather than stopping it. There is now one
      `uspace_deliver_signal`, and the three catchable stop signals stop a
      process only when it has not claimed them, because a shell catches
      `SIGTSTP` to record the job before it stops.
      **`nanosleep` clamped every sleep to one second**, with no reason
      written down. That is not a cautious approximation: the call returned
      early and reported success, which to a program is indistinguishable
      from time having passed, so `time.Sleep(5s)` came back after one.
      Verified on the board with the harness typing the key
      (`runtype.py … "tstp: the keyboard is the child" '\x1a'`): the program
      puts a child in a group of its own, makes that group the terminal's
      foreground with `TIOCSPGRP`, prints a marker and waits six seconds.
      **7** with the keystroke — the child was stopped and continued — and
      **174** without it, which is the control that makes the 7 mean
      something. Three versions of this test passed while proving nothing:
      the first slept 2,000,000,000 **nanoseconds**, which is not a long
      sleep but an invalid argument; the second had a child whose work was
      over before the key arrived; only the third has both margins the right
      way round.
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
      **The rule has a caller now.** This entry used to end "nothing *calls*
      the rule yet, because there is no regular-file `openat` to call it
      from" — §2's file-backed mappings needed that `openat`, and it checks
      the mode from the filesystem's own inode against the caller's identity
      before handing out a descriptor. The check sits where a process
      exists, not in the filesystem layer: putting it in
      `xfsmount_read_file` was tried and reverted, since that would have the
      hexagon reaching outward and the dependency would be circular.
      Still missing: the unprivileged `setuid` that returns to a real uid one
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
      above. The note that followed — "not done for `MAP_PRIVATE` file
      mappings, which need file-backed mappings first" — is **withdrawn
      rather than completed**: a private file mapping here fills each page
      from the file into a page of its own, so there is nothing shared
      between two processes for copy-on-write to postpone. A *shared* file
      mapping would need it, and `MAP_SHARED` is refused.
- [x] **File-backed mappings.** (M) **Done** for `MAP_PRIVATE`, read-only,
      which is what "maps a file to read it" needs. A region records the
      file and the offset it starts at, and each page is filled from the
      file at its first touch — the page is already zero, which is exactly
      what the bytes past the end of the file have to read as.
      It needed the dependency this entry named: **`openat` of a regular
      file**, which did not exist (only `/dev/null`, `/dev/zero` and the
      console did). A file is read whole into the kernel's heap when it is
      opened, because `xfsmount` reads a file by name and not a block of
      one by offset — so there is nothing to be lazy with yet, and the cost
      is the file's size in kernel memory while the descriptor is open,
      with a stated 16 MiB limit rather than a discovered one.
      **That opening is also the permission rule's first caller.** §1 built
      `access_permitted` and `uspace_may_access` and nothing asked them
      anything, because there was no file to be denied; the mode now comes
      from the filesystem's own inode and a file the caller may not read
      does not open.
      **The mapping holds its own reference to the file**, which is the
      whole of why `mmap` does not need the descriptor to stay open — and
      that reference is let go of wherever a region goes: `munmap`, a fixed
      `mmap` over it, `execve`, and the end of the process. `fork` takes
      one per region, because the child's record is a copy of the parent's
      pointers and sharing one reference would let whichever process
      finished first take the file's bytes out from under the other.
      A partial `munmap` of one is **refused**, for the same reason a huge
      mapping's is: splitting the region would make two of them share one
      reference, and the first to go would free the file under the second.
      So is an unaligned offset, and so is a mapping reaching past the
      file's last page — Linux answers a touch there with `SIGBUS`, this
      kernel has none to raise, and handing out zeroes instead would be
      inventing file contents.
      Verified on the board (`minikern.filemap=1 minikern.disk=sd`): **63**,
      all six checks, mapping `/init` and comparing against its ELF magic —
      which is a better marker than anything the test could write, because
      it is the file's. The check that matters reads the word at offset 4096
      **through the descriptor**, closes it, and compares against the same
      offset **through the mapping**: a page that has to be faulted in from
      a file the program can no longer name. Re-reading the first page would
      have proved nothing, since it was already backed. The kernel reports
      `2 pages from a file`, one before the close and one after.
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
- [x] **Swap.** (L) **Done, watermark and victim policy included** — the operator asked for it,
      "Windows style": a **file** on the filesystem rather than a partition,
      with the path and the pressure root's to choose, and **off by
      default**.
      A file is the better shape here, and not only because it was asked
      for: a partition is a second thing to create and a table to agree
      about, while a file is one `dd` on a filesystem that already exists.
      It also needs **no filesystem write path**, which this kernel does not
      have for the board's filesystem: a swapfile's blocks are allocated
      once by whoever created it and are then only ever *overwritten*, which
      changes no metadata at all. So the blocks are resolved once at enable
      time (`xfsmount_map_file`) and read and written as blocks after that —
      which is how Linux's `swapon` works, and for the same reason. A sparse
      file is refused: a hole has nowhere to put a page.
      **Pages go out and come back.** A page that is out leaves its slot
      number in the page table entry it used to be in, with the valid bit
      clear — so the hardware ignores it, every existing reader is told "not
      mapped", and `pte_is_leaf`/`pte_is_table` cannot see it because both
      require that bit. The marker is what separates it from an entry that
      is simply **absent**: without one, a fault on a swapped page would be
      backed with a fresh zero page and the page's contents would be gone
      silently, which is the worst way to lose memory. For the same reason
      the fault handler asks about swap **before** demand paging.
      A page comes back with its *region's* rights rather than the ones its
      entry had, because the region is the authority on what a process may
      do with an address and a page that went out read-only and came back
      writable would be a permission appearing from nowhere.
      **Giving the slots back** matters as much as taking them: a page that
      is out still costs a slot, so `munmap` releases the slots in its range
      — the loop that frees pages cannot see them, since a swap entry is not
      a mapping — and a process that ends releases every slot its regions
      still hold. Without those, a program that pages memory out and then
      frees it fills the file with pages nobody will ask for.
      **Root's to turn on**: `swapon(path, flags)` and `swapoff(path)`, both
      `EPERM` for anybody else — a real check, because this kernel has had
      real identities since §1. The flags carry the swappiness in the low
      seven bits, which is where Linux puts a swap device's priority: it
      keeps the call's shape and avoids inventing a syscall for one number.
      The *size* is the file's, which is the point of a page file — the
      operator chooses it with `dd` and the kernel uses what is there.
      **`madvise(MADV_PAGEOUT)` now does something**, and it is what makes
      swap testable at all: on a machine with 8 GiB the pressure that would
      cause paging is unreachable, and Linux has exactly this advice for
      exactly this purpose, so a program asking for it is asking an ordinary
      question. The rest of the advice is still accepted and ignored, which
      is honest — advice is advice, and the only thing a kernel may not do
      is *claim* to have taken it.
      Verified **on the board**, with a 64 MiB `/swapfile` the operator
      created on the card: 16,384 slots resolved from the file's extents, a
      page out to slot 0 and back byte for byte, and a program that wrote a
      marker into each of sixteen pages, asked for them to be paged out, and
      read all sixteen back — **exit 0**, with the kernel reporting `16
      pages sent out by a process, 16 faulted back in` and no slot left
      taken. The Go program runs unchanged with swap enabled.
      **Both of those are done now.**

      The **watermark** is the free-page count below which the kernel pages
      out unasked, scaled by swappiness and **zero at zero** — the knob has
      to include "off", because that is the setting an operator checks for.
      It reclaims **the deficit**, not a fixed amount: paging out a page
      frees a frame, so the shortfall falls as it is served and the sweep
      stops of its own accord. Asking for a fixed batch instead is a thrash
      loop by construction, which is what the first version did — page out
      thirty-two, have them faulted straight back in, page out thirty-two
      more — and on the board it looked exactly like a hang.

      It runs **on a fault and nowhere else**. The first version reclaimed
      inside `back_one_page`, which every mapping path goes through —
      including the ELF loader building a process, where the *kernel* writes
      into the new process's pages directly rather than through a fault. The
      clock could free a frame the loader was still filling.

      The **victim choice** is a clock. `PTE_A` says whether a page has been
      touched, so a page whose bit is clear has not been used since the
      sweep that cleared it; a page whose bit is set gets it cleared and a
      second chance. The cursor is a **virtual address**, not a region
      index, because regions come and go between sweeps and an index would
      point somewhere different each time — which is how a rotating policy
      quietly becomes the fixed one it replaced. Clearing bits needs one TLB
      flush per sweep: a hart holding a translation whose `A` is already set
      never writes the table again, so without the flush a cleared bit stays
      clear however hard the page is used, and the clock would evict the
      busiest page on the machine.

      **And it found that this core does not update `A` in hardware**, which
      this project had recorded as a measured fact the other way round. See
      `CLAUDE.md` §5: the evidence for the old claim was a Go binary running,
      which proves nothing when `pgtable_map` pre-sets `A` on every mapping
      and no page is ever reached with it clear. Clearing one bit hung the
      machine silently — mapped, permitted, not stale, so the retry path
      flushed and retried for ever. The kernel now sets `A` on such a fault
      and counts it.

      **On the board**, with a 64 MiB `/swapfile` in the image: a program
      that asks for nothing touches a megabyte twice under a watermark set
      just above the free count, and reports `315 out unasked, 419 second
      chance(s), 104 A-bit fault(s)` — pages going out proves the watermark
      fired, second chances prove the *clock* chose them rather than a scan
      from the beginning, and the A-bit faults are the core's behaviour
      measured rather than assumed. The control, in the same run: with
      swappiness at zero, nothing goes out at all.

      That control was itself wrong at first and worth recording: it set the
      watermark **override** to zero, and zero there means "derive it from
      swappiness", not "off". With swappiness 60 the derived mark was about
      sixty thousand pages and free was two million, so the control passed
      because the machine was empty — proving nothing, while reading as
      proof that the switch worked.
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
      **A regular file does open by path now** (§2's file-backed mappings
      needed it): `openat` reads it off the filesystem, checks the mode
      against the caller's identity, and gives a descriptor that `read`
      works on. What is still missing here is the *synthetic* ones.
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

- [x] **B-tree format forks** (`XFS_DINODE_FMT_BTREE`). (M) **Done, and
      verified on the board.** A fork that outgrows the inode moves into a
      b-tree and leaves only its root behind — and this was not the exotic
      case the old entry implied: **3,000 files in a directory is enough**,
      so the root directory of any filesystem with a few thousand entries
      was refused outright (`inode 96 has a btree fork`).

      `xfs_bmbt.c` descends it: the root inside the inode behind a four-byte
      header, every other block behind a 72-byte v5 header, and in both the
      keys and pointers are **separate arrays sized by what fits** — so the
      pointers begin after the maximum number of keys rather than after the
      ones in use, and a reader that assumes otherwise follows a key as a
      block number. A keyed descent rather than a walk of every leaf,
      because the question is always "which extent covers this block" and
      answering it by reading the whole map would make each block of a large
      file cost all of it.

      It goes in `xfs_bmap`, which is the one place both shapes of extent
      map meet: everything that asks where a block is — a file read, a
      directory listing, a symlink — comes through there, so nothing else
      had to learn about b-trees. `list_extents` was renamed
      `list_data_blocks`, since its name had been a claim about the fork.

      Every number from the disk is checked before it is used: the v5 magic
      (the pre-v5 one is refused rather than carrying a second header size
      for a case a v5 filesystem cannot contain), the CRC and the UUID, that
      a child's level is below its parent's, that a count fits the block it
      came from, that a pointer is inside the filesystem, and a hard cap on
      depth — a tree that points at itself must be an error, not a loop. The
      host tests cover a tree that reads, a root claiming to be a leaf, a
      leaf whose checksum is wrong, and a pointer outside the filesystem.

      **On the board**: a 3,000-entry directory lists completely and
      `/p2999` reads back 65,536 bytes with the checksum `fixtures.py`
      computes, which is a standing case in the board suite. A 40,000-entry
      directory lists all 40,002 entries.

- [x] **The directory hash index.** (M) **Done, and proved to be in use.**
      The entry this replaces claimed a directory past `leaf` format "cannot
      be listed", which was wrong — listing and lookup both walk the data
      blocks by offset, so they worked once the extent map did. What was
      missing is what leaf and node format exist *for*: the index, so a
      lookup does not read every entry.

      `xfs_dirhash.c` computes the format's name hash — an on-disk interface
      fact, and a reader that computes it differently finds nothing, which
      looks exactly like an empty directory — then binary-searches the leaf,
      or descends the node blocks first when there is more than one leaf.

      Two details are the difference between correct and nearly correct.
      **Hashes collide**, so finding one is the start of a scan: every entry
      with that hash is resolved and its name compared, and a run of equal
      hashes can continue into the next leaf, which is why `forw` is
      followed. And an **address is a byte offset divided by eight**, so it
      resolves to a block *and an offset within it*.

      The fallback stays for shortform and block-format directories, which
      have no index block to search and a handful of entries to walk — the
      walk is the right path for those, not a lesser one. `KERR_NOTSUP`
      distinguishes "this directory has no index" from "the name is not
      here", and confusing the two would make every lookup in a small
      directory fail.

      **The bug worth recording** is that a directory's data blocks and its
      index blocks do not agree about where anything is: a data block has a
      32-bit magic at offset 0 with its checksum at 4, while a leaf or node
      block is an `xfs_da3_blkinfo` with a **16-bit magic at offset 8** and
      its checksum at 12. Verifying one as the other reads part of a block
      number as a magic and rejects every block, which is what it did — so
      the getter is two functions, and one magic parameter cannot express
      the difference.

      **And it is measured, not assumed.** The two paths return the same
      inode, so a test of the index passes just as happily when the index is
      never reached — `xfs` counts `hash_lookups` against `walked_lookups`
      and the board case requires `0 walked`. On hardware, both geometries:
      `2 hashed, 0 walked`, with the payload checksum exact.

- [x] **A dirty filesystem is checked before it is mounted, not refused.**
      (M) **Done, and proved with a log this kernel dirtied itself.** A
      filesystem whose log held uncommitted work was refused outright, which
      made a power cut as damaging as a broken disk: the machine could not
      read its own root filesystem.

      It is now **checked and mounted read-only**, which is what Linux's
      `norecovery` does. The two facts underneath are worth separating,
      because a kernel that got either wrong would look fine:

      - such a filesystem **is readable** — everything committed before the
        cut is intact, and refusing it protects nothing;
      - it **must not be written** — its metadata is behind a log saying
        something newer happened, so a write would build on a state the log
        has already superseded.

      `xfs_check` is the pass: both headers of every group, the four b-tree
      roots they name, the counts each group states about itself, and the
      root inode. It **writes nothing** — a repair that guessed would be the
      most dangerous code in the tree, because on a filesystem whose log
      says something newer happened, a fix is a guess overwriting a fact.
      It is bounded by the group count rather than the filesystem's size;
      walking every inode is what `xfs_repair` does and would make boot time
      a function of how full the disk is.

      Two things it got wrong first, both instructive. It **returned the
      first device error instead of counting it**, so a filesystem with
      forty bad groups reported as one — a checker exists to count, and a
      structure it cannot read is a fault like any other. And it addressed a
      group's headers as `block + sector` when a **sector is an offset
      inside the group's first block**: it reported twenty faults on a
      filesystem `mkfs.xfs` had just made. That is the useful kind of wrong,
      because a checker that cries corruption on a good disk would have had
      its verdict ignored on a bad one.

      **On the board**: the kernel logs a transaction and omits the unmount
      record — what a machine losing power mid-write actually leaves — then
      mounts again from the disk with the cache dropped. The log reads
      dirty, the check reports four groups sound, the mount is read-only,
      a write is refused, and the file written before the cut reads back. A
      hand-patched log would have proved the parser; a log this kernel wrote
      and failed to close proves the path a real machine takes.

- [x] **Automount: a volume is mounted where its label says.** (M)
      **Done, and proved with two labelled filesystems on one disk.** This
      kernel mounted exactly one filesystem — whatever the first partition
      held — and every path lookup went to it regardless of what the path
      said. There was no mount table, so `/volumes/DATA/x` and `/x` reached
      the same filesystem.

      The policy is the **label**: `ROOT` is the root filesystem and is
      mounted at `/`; anything else is mounted at `/volumes/<label>`. That
      is the operator's instruction, and it is the right rule for a reason
      worth recording: the alternative — mounting whatever the firmware
      booted from at `/` — makes the root filesystem a property of the
      **boot order** rather than of the disk, so fitting a second drive can
      silently change which filesystem is the system.

      **One `struct xfs` per device**, which the first version got wrong.
      It automounted every candidate *and* let `xfsmount` mount the one it
      adopts, so that device had two independent block caches, both able to
      write — one holding stale metadata while the other changed it.
      Nothing had broken only because no path resolved into the duplicate,
      which is luck rather than design. Automount now starts from the
      **second** candidate and the root filesystem enters the table through
      `volumes_register`, which stores a *pointer* to `xfsmount`'s mount. A
      consequence worth noting: the listing had to move after
      `xfsmount_attach`, since before it the table describes every
      filesystem on the machine except the one it is running from.

      `/volumes` is a **virtual directory**, synthesised from the table. A
      directory that exists because the kernel says so needs no block, no
      inode and no space, and cannot disagree with what is actually mounted.

      Resolution takes the **longest matching prefix**, at a path boundary —
      so `/volumes/DATA` is not matched by a volume at `/volumes/DAT`, and
      `/`, which matches everything, is the weakest match rather than the
      first. The first version sorted the table instead and had to copy a
      `struct volume` to swap two entries: three kilobytes on the stack,
      which the frame guard refused, and a block cache moved out from under
      whatever held a buffer in it.

      `struct xfs` carried **85 KB of directory scratch**, which made a
      mount table the largest thing in the kernel. It is now shared, which
      is sound only because this kernel takes one big lock around every
      directory operation — said in the struct, because splitting that lock
      would have to split this with it.

      **On the board**, with a GPT image carrying a `ROOT` volume and a
      `DATA` volume: both mount, `ROOT` at `/` and `DATA` at
      `/volumes/DATA`, `/volumes` lists both, and each volume's own file is
      read **through its own mount point** — including the negative, that
      `DATA`'s file is *not* reachable at `/onDATA`. Without that last
      check the test would pass on a kernel that ignored mount points
      entirely, which is precisely the kernel this replaced.

      The harness cost two fixes, both the same shape as a defect already in
      this list. The compressed image was staged at `0x8000_0000` and a
      642 MB fixture reaches past it; and then the image at `0x6000_0000`
      ran through `0x8200_0000`, **where the kernel itself is loaded** — so
      U-Boot decompressed the filesystem over the kernel it was about to
      boot and said `Bad Linux RISCV Image magic!`. The overlap check this
      project added after the first collision did not have the kernel's own
      address in it. A range check that does not check every range is the
      same defect twice.

- [x] **Log replay.** (M) **Done, and it took four board runs to believe —
      three of which found something.** A dirty filesystem was checked and
      mounted read-only, which made it readable; replay is what makes it
      *writable* again: apply what the log says, write an unmount record,
      and the filesystem is clean.

      It understands **inode items** (what this kernel's writer produces)
      and **buffer items** (what Linux logs most metadata as). Everything
      else — extent-freeing intents, inode creation, quotas — is refused,
      and the filesystem stays read-only until a kernel that understands
      them has a turn.

      **All or nothing, because this is the most dangerous code here.**
      Every other operation acts on what it is told; this one acts on what
      it infers from a structure a crash was in the middle of writing, and
      getting it wrong does not fail to mount — it writes plausible rubbish
      and the damage surfaces later, somewhere else. So: a transaction
      without its `COMMIT_TRANS` is dropped (a crash leaves exactly one of
      those, at the end); an item type it does not understand abandons the
      **whole** replay rather than being skipped, because applying a
      transaction's other items produces a state the filesystem never had;
      and nothing reaches the disk until the entire log has read clean —
      the writes land in the block cache, which is flushed once at the end
      and invalidated on abort. An abort is not a failure to mount: it
      falls back to read-only, and a kernel that refused the disk outright
      over an unrecognised item would be strictly worse.

      One thing replay must undo that is easy to miss: the writer stamps
      the record's cycle number over the **first four bytes of every data
      sector**, so that a torn write is detectable, keeping the originals in
      `h_cycle_data`. Without putting them back, the first operation header
      in the second sector has its length and client id read out of the
      middle of a cycle number.

      **The bug that mattered, and how it was caught.** A logged inode is
      *not* an on-disk inode: each field keeps its offset but is written in
      the **host's** byte order, because the log is replayed by the machine
      that wrote it. The first version copied the logged bytes straight in,
      writing a little-endian core over a big-endian one — **corrupting
      every inode it touched while reporting success**, log marked clean and
      filesystem declared writable. What found it was the test asserting
      something replay never touches: a file written *before* the simulated
      crash. A test that checked only "did replay run" and "is it writable"
      would have passed green on a filesystem-eating defect. The conversion
      is an involution over the writer's own field table, and it writes
      **only** the fields in that table — a wholesale 176-byte copy would
      zero `di_next_unlinked`, which the log does not carry.

      **Three more, all found by the control** that proves the read-only
      fallback still works, and none of them in replay:

      - the log writer reached `clear_after` only through `write_unmount`,
        so skipping the unmount record left the *previous* write's unmount
        record further down the log — `find_newest` found it and called the
        log clean. The first dirty-log test passed only because its
        transaction happened to be **larger** than the one before and
        overwrote the stale record; a later one of equal size did not, and
        the instrument had quietly stopped working;
      - `xfsmount_remount` zeroed the whole `struct xfs`, wiping the flag
        meant to govern that very mount;
      - and `xfs_mount` zeroed it **again**, a few lines before reading it.
        Three places, each looking like a different bug. The reset is now
        deliberate about what it keeps, at the one chokepoint.

      **On the board**: the kernel logs a transaction, omits the unmount
      record — what a machine losing power mid-write leaves — and mounts
      again from the disk with the cache dropped. The log reads dirty, the
      check reports four groups sound, the log is replayed, the filesystem
      is **writable**, the file written before the cut reads back byte for
      byte, and a new file can be created. Then the control, in the same
      run: with replay refused, the same filesystem mounts **read-only**. (M) A dirty log is refused rather than replayed, so an
      unclean shutdown needs `xfs_repair` on another machine. Deliberate for
      now — guessing at a log is how data is lost — but it is a gap.
- [x] **Extended attributes.** (M) **Done for the shortform fork, read and
      written, and board-proven across a mount.** This was recorded as
      blocked on testability rather than difficulty: setting an xattr needs
      a mounted filesystem, which needs root, and `mkfs.xfs`'s protofile
      cannot express one — so there was no way to make a sample to read.

      The way in was to notice that **shortform attributes live entirely
      inside the inode's literal area**. The writer allocates nothing, so
      writing them is what makes reading them testable: set, read back,
      list, remove, and then **mount the filesystem again** and read them
      once more. That last step is the one that matters, because everything
      before it would pass on a kernel that kept attributes in a variable —
      the writer and the reader are the same code and would agree with each
      other about anything.

      Three facts the format insists on, each of which a reader written
      alone would have got wrong silently:

      - **The namespace is a flag, not part of the name.** Linux presents
        `user.foo`; XFS stores `foo` with no flag, `trusted.foo` as `foo`
        with ROOT, `security.foo` as `foo` with SECURE. Keeping the prefix
        in the name would find nothing Linux wrote and write nothing Linux
        could find — while passing any test that used only its own output,
        which is why the test uses all three namespaces and checks that
        `user.level` is *absent* after `trusted.level` was set. A prefix
        this kernel does not know is **refused** rather than stored as a
        user attribute.
      - **`forkoff` of zero means there is no attribute fork**, not "it
        starts at the beginning". The first version computed
        `(data_fork_bytes + 7) / 8`, which for a freshly created empty file
        is exactly zero: it stored the fork and then stamped "no fork here"
        over it. The board read back `KERR_NOENT` for an attribute it had
        just written.
      - **Replace is remove-then-add.** Entries are packed with no gaps, so
        a new value of a different length cannot be written over the old
        one; doing it anyway eats the first bytes of the entry after it.

      The default offset is **half the literal area**, as Linux does, and
      for a reason rather than symmetry: the data fork cannot be moved once
      attributes sit above it — `extend_fork` refuses to grow past its
      bound, which is what stops a growing file from eating them — so a
      file given an attribute while empty would otherwise never hold a
      single extent.

      **The leaf format is written too**, so an attribute set is no longer
      capped by the inode. When the shortform fork fills, it becomes a leaf
      block — taking the new attribute with it rather than converting as a
      step of its own, because converting first and adding after would leave
      a leaf on disk that does not hold what the caller asked for, having
      grown the file's metadata for nothing. Entries stay sorted by hash
      (the same `xfs_da_hashname` directories use), pairs pack downward from
      the end of the block while the entry array grows up from the header,
      and the **free map** is maintained: it is what `xfs_repair` reads to
      decide whether a block has room, so leaving it wrong is invisible here
      and visible to every other tool. A removal leaves a **hole** rather
      than compacting — packing a pair out would mean moving every pair
      below it and rewriting every `nameidx`, and the format carries a
      `holes` flag precisely to tolerate gaps until a writer needs the
      space.

      **Writing it is what tested the reader, and the reader was wrong.**
      `xfs_attr_leaf_name_local` is `{valuelen be16; namelen u8; nameval[]}`
      — the *value's* length first. The reader had `namelen` at offset zero,
      which costs nothing visible on a short name and mangles every longer
      one, and it had shipped that way because there was no leaf block to
      read it against. This is the "code that compiled and never ran"
      pattern that cost this project five bugs in one session, caught the
      one way it can be: by making the thing it claims to read.

      A second trap was in **this code's own interface**. The listing
      callback handed out the raw flag byte, and `XFS_ATTR_LOCAL` is a
      *storage* property — set for a local value in a leaf, not set in a
      shortform fork — while `ROOT`/`SECURE` are the namespace. So a `user.`
      attribute's flags read 0 in the inode and 1 in a leaf, and the first
      consumer of the callback concluded it had changed namespace. The
      lookup paths already masked correctly; only the listing leaked the
      bits, so only the listing could mislead. It reports the namespace
      alone now.

      **A value too large to share a block with its name goes into blocks
      of its own**, read and written. Each block carries its own offset into
      the value and its byte count, and both are checked while the value is
      reassembled: a block that is sound in isolation but belongs at a
      different offset is what a misplaced write produces, and concatenating
      blindly would give back a value wrong in the middle and right at both
      ends — the hardest kind to notice. The blocks are asked for as **one
      extent**, so the fork costs one record however long the value is.

      Two mistakes here, both about *order*. The dispatch asked what shape
      the fork was before asking how big the value was, so a three-kilobyte
      value on an already-converted leaf went to the local writer and came
      back `KERR_NOSPC` **on a filesystem with 300 MB free** — where a value
      lives is a property of the value, not of the fork. And
      `store_attr_extent` cleared only the sixteen bytes of the extent it
      was writing when the fork changed format from shortform to extents,
      leaving the old attribute *text* in the slots above: `attr_fork_end`
      unpacked it and reported the fork ending at logical block
      2,981,740,977. **A format change must clear its fork**, because an
      extent list's zero tail is how both the appender and the reader find
      its end — a fork whose tail is not cleared reads as whatever it used
      to be.

      **A b-tree attribute fork is walked too.** An inode has two forks and
      either can outgrow the inode into a b-tree — a file with hundreds of
      attributes has one of exactly that shape, and `mkfs` and Linux both
      produce them. The descent is the *same* one the data fork uses, so it
      is handed the fork rather than reimplemented for it:
      `xfs_bmbt_lookup_in` takes the fork and its length, and the two
      facades name the two callers (Rule 5.1/5.2). A second implementation
      would have been a second place to get the key arithmetic wrong.

      It is **host-tested, because nothing here can produce one**: a b-tree
      attribute fork needs hundreds of attributes on one inode, and this
      writer answers `KERR_NOSPC` long before the fork would convert, so
      there is no board fixture to be had. The inode is built by hand, which
      is the honest way to test a reader for a structure nothing available
      can make — the alternative was leaving the format refused and the code
      unwritten. The test also asserts the same refusals the data fork's
      tree gets, since it is the same walk.

      The attribute fork needs its own mapper (`xfs_attr_bmap`) whatever its
      shape: it is a separate extent list, and reading an attribute block
      through `xfs_bmap` would return a block of the file's own contents.

      **On the board**: two attributes in two namespaces set in the inode,
      a dozen more added until the fork is pushed into a leaf (`1 to leaf`,
      counted, because a set that happened to fit would exercise none of
      it), all fourteen listed and one read back byte for byte **after a
      remount**, one removed without disturbing the rest, and a name in an
      unknown namespace refused.

- [x] **Directory growth: shortform into block format.** (M) **Done, and
      verified on the board.** A directory stopped accepting names at about
      a dozen — `KERR_NOSPC`, with a comment saying it would have to become
      a block directory. It becomes one now, which takes it to roughly a
      hundred and twenty.

      The conversion builds the block once from the old entries **and the
      new one together**, which is why it takes the new entry as an
      argument: converting first and adding after would write the block
      twice and leave a directory briefly holding neither answer. It also
      has to create what shortform does not store — `.` and `..` are
      synthesized by a reader there and are real entries here — and compute
      every entry's hash, because the block carries the sorted array a
      larger directory's lookup will search.

      Adding and removing afterwards are done **in place**: an entry goes
      into a free span, its hash is inserted into the sorted array, and the
      tail's count rises; removal leaves a free span and takes the pair out.
      No compaction, because free spans are what the format expects — and
      compacting would need a second buffer, which there is not.

      `xfs_dir_add` and `xfs_dir_remove` are where the shape is decided, so
      create, link, rename and unlink do not know about formats. A block
      directory is not shrunk back when it empties, which is what Linux does
      and what stops a directory that is repeatedly filled and emptied
      converting twice per entry.

      **Two bugs it found, both in code that had never run.** The hash array
      grows *downwards* as entries are added, so a free span reaching it
      stops being entirely free — the first version left the span's recorded
      length alone and eventually wrote an entry over the hashes. And
      `take_inode` composed an inode number as `start | offset` where the
      format means `start + offset`: a group's first chunk commonly starts
      at inode 96, so offset 32 produced 96 again — **the root directory** —
      whose inode was then overwritten with the new file's. Twenty-eight
      creates worked first, because every offset below 32 happens to share
      no bits with 96.

      **On the board**: sixty names added, all sixty found, all sixty
      removed, twice over — the second pass being what proves a removed
      entry's space is reused, and sixty being what also forces a second
      chunk of inodes.

      A third bug, found by raising that number from twenty to sixty:
      `find_room` required a free span to lie **entirely** below the hash
      array, and the trailing span records the length it had when it was
      written while the array grows down into it. After one addition the
      only span there is was rejected and the directory reported itself full
      with three and a half kilobytes free. It uses the part of the span
      that is still free.

- [x] **Directory growth: block format into leaf.** (M) **Done, and both
      conversions measured on the board.** Past about a hundred and twenty
      names one block is full, and the entries need blocks of their own with
      the hashes in a block of their own. That is leaf format — what `mkfs`
      gives any directory with a few hundred files in it, and the shape this
      kernel could read and not produce.

      The conversion is mostly a **re-labelling**: the block that was the
      whole directory becomes data block zero, its magic changing from
      `XDB3` to `XDD3`, and the space its hash array and tail occupied
      becomes free. The hashes move to a new block at 32 GiB of logical
      offset — which is why a leaf directory's *size* counts only its data,
      and why asking the extent map whether a block exists up there is how
      the two shapes are told apart. Inferring it from the size would not
      work: a leaf directory with one data block has the same size as a
      block directory.

      Adding and removing afterwards put the entry in the data block and its
      hash in the leaf, and maintain the one structure this reader never
      consults: the leaf's **`bests`** array, one value per data block
      giving its largest free span, with a `bestcount` tail. `xfs_repair`
      and Linux read it and will call a filesystem corrupt if it lies.

      **And more than one data block**, added in the same round. When every
      block it has is full the directory gains one at the next logical
      offset — far below the leaf's 32 GiB, which is the whole reason the
      format puts the leaf up there — and three things move with it: the
      directory's size, a data extent in the fork, and a slot in `bests`.

      Two details that only break once there is a *second* of something, and
      so were right by accident until now. A leaf entry's **address is a
      byte offset within the directory's data, not within its block**: the
      reader has always divided it by the directory block size to decide
      which block to read (`name_at`), and a block-relative offset agreed
      with that for exactly as long as there was one block. And `bests` is
      indexed **upwards from its start while the array is anchored at the
      tail**, so growing it moves every existing value *down* — the other
      direction writes block zero's free span into block one's slot, which
      nothing here would notice and `xfs_repair` would.

      Data extents are **merged when contiguous**, which is not tidiness: a
      256-byte inode's literal area holds five extents, so a directory that
      never merged would run out of *inode* long before it ran out of disk.

      A removal does not give a block back. XFS itself keeps them and reuses
      the space; reclaiming one means removing an extent and a `bests` slot,
      which is a step of its own.

- [x] **Directory growth: leaf format into node.** (M) **Done, and every
      shape measured on the board.** Leaf format keeps every hash in one
      block, so it ended at about five hundred names however many data
      blocks it had. Node format puts a **node** block above several
      `LEAFN` blocks and indexes them by hash, so the ceiling becomes the
      node's own: five hundred leaves of five hundred hashes.

      Three things move, and the middle one is the surprise:

      - the one `LEAF1` block's hashes split across **two** `LEAFN` blocks,
        chained by `forw`/`back` so a run of equal hashes can continue into
        the next;
      - the block that *was* the leaf becomes the **node root**, and the
        leaves move to new addresses. The root has to keep its block number
        because that is where a search starts, so the conversion moves the
        contents out rather than putting the root somewhere new;
      - `bests` cannot stay, a `LEAFN` block being all hashes. It moves to a
        **free block** at 64 GiB of logical offset — a third address space,
        above the data and the leaves.

      A full leaf then **splits**: the new one takes the upper hashes and
      goes after the old one in the chain, so the order the chain exists to
      preserve is preserved. One node level is written; a deeper tree is
      refused. The *reader* walks any depth, so a filesystem made elsewhere
      is still read — the limit is on what is written, which is the safer
      way round.

      **Three bugs, and all three were the same mistake about what a stale
      copy is worth:**

      - `xfs_dir_fork_insert` took the extent count from the **caller's
        inode snapshot**. The conversion adds three extents in a row, so
        the second write put the fork back one extent short and silently
        dropped the block the first had just added.
      - The dispatcher chose a shape by **calling an add path and falling
        back on its error** — but an add writes the entry into a data block
        *before* it touches the index, so the wrong guess left an entry on
        disk that the other path then wrote again. `xfs_dir_index_shape`
        only reads, and is asked first: a probe is how you choose between
        operations that write.
      - And twice, a block that had **just been created could not be
        mapped**, because the inode it was mapped through predated it. On
        the board that read as a *create* answering `KERR_NOENT` — "no such
        file" for a file being made. `refresh` is now called wherever the
        fork changes, and named, because it was written out by hand in two
        places before it was given a name.

      A fourth was found in the leaf format while reaching this one: a leaf
      add placed the entry **before** checking the leaf had room for its
      hash, so a full leaf would have left an entry visible to a walk and
      invisible to a lookup. It had never been reached, because until node
      format existed there was nothing after "the leaf is full" — which is
      the only reason it was not a bug in practice. A step that cannot be
      undone has to be the last step.

      Also: `struct xfs` carried **85 KB of directory scratch**, which the
      mount table made expensive. It is shared now, sound only because this
      kernel takes one big lock around every directory operation.

      The pairs are held in the filesystem rather than on a stack: the frame
      guard refused a 16 KiB array, which is the second time that rule has
      caught this work.

      **On the board**: four hundred names added, found and removed, twice
      over — with `1 to block, 1 to leaf, 1 data block(s)` and six inode
      chunks, so every growth path is known to have run rather than
      assumed. The second pass adds to a directory that is *already* a leaf
      with two data blocks, since neither is ever given back, which is what
      exercises the add and remove rather than the conversion.
- [x] **A second inode chunk.** (S–M) **Done, and measured to have run.**
      `take_inode` allocated from the inode b-tree records that existed, so
      when every chunk in a group was full it answered `KERR_NOSPC` — a
      fresh filesystem took about sixty files and then stopped, the limit
      being the one chunk `mkfs` had made rather than anything about the
      disk.

      It makes another now: sixty-four inodes' worth of **contiguous**
      blocks, each written as a *valid empty inode* rather than zeroes —
      `xfs_inode_read` checks the magic, the version and the checksum, and a
      reader meeting a zeroed inode cannot tell "never used" from
      "corrupt" — then a record into each of the two inode b-trees, and the
      group's and superblock's counts.

      Contiguity is why the allocator grew an extent interface:
      `xfs_alloc_extent` takes `count` blocks and picks the **smallest free
      extent that fits** rather than the largest. Taking the largest run for
      every request is harmless when every request is one block and wasteful
      the moment one asks for eight, because it breaks up the one run a
      later caller may need whole. `xfs_alloc_block` is now that function
      with `count` of one.

      A b-tree **leaf**, singular: a full leaf would have to split and that
      is refused rather than approximated. A group's first leaf holds a few
      hundred records — tens of thousands of inodes — so the refusal is far
      away, and a wrong b-tree is much worse than a filesystem that stops
      growing.

      `fs->inode_chunks_added` is counted and the board case requires it to
      be **1**, because sixty names need a second chunk only if the first
      runs out and a run where it did not would report every step passing
      while never touching this path.
- [x] **Allocation beyond one group.** (M) **Done, with a control.** This
      entry used to say the gap was fragmentation — one contiguous run or
      nothing — and reading the code to close it found something worse and
      simpler: **nothing ever asked a second allocation group.** `agcount`
      appeared nowhere in the allocator. Every caller passed the group its
      inode lived in, and `xfs_alloc_extent` answers `KERR_NOSPC` for the
      whole filesystem when that one group is full — so a default
      four-group filesystem refused writes at about **a quarter full**, with
      three-quarters of the disk free.

      Fragmentation itself turned out to be already handled, which is why
      the entry was wrong: `grow_file` allocates **one block at a time** and
      merges each into the previous extent when it is contiguous, so a file
      on a fragmented filesystem collects several extents rather than
      failing. The description had been written from the allocator's
      signature instead of from its caller.

      `xfs_alloc_somewhere` prefers a group and then tries every other one,
      reporting which answered — because an inode number, a file's extents
      and a directory's blocks all have to be composed from it. The locality
      hint is dropped once the search leaves the preferred group, an address
      inside one group meaning nothing in another. An inode **chunk** is the
      case that needed care: it may land in a group other than the one the
      directory asked for, so the group travels back out and `take_inode` is
      retried in the one that answered — retrying in the original would find
      it just as full and recurse until the stack ran out.

      **On the board**: a file written until the allocator leaves its group,
      which happens at `81600 KiB` — the group's size — and the last chunk,
      which lives in the *second* group, read back byte for byte, because an
      allocation from another group is also a second extent and a file whose
      extents are right in the inode and wrong on disk reads back as
      somebody else's data. **And its control in the same run**: with the
      search switched off (`single_group`, kept for this) the same write is
      refused with `KERR_NOSPC`. Without that control the test would show
      only that writing works.

---

## 4. Drivers and board hardware

- [x] **The board's Ethernet (DesignWare GMAC).** (L) **The link comes up
      and frames go both ways on the board.**

      `dwmac: /soc/ethernet@16040000, version 00004152` /
      `address 6c:cf:39:00:b6:89` / `PHY at 0, id 4f51e91b` /
      `link up at 1000 Mbit, full duplex` — on the first attempt, because
      everything it needed had been measured first rather than guessed (see
      `CLAUDE.md` §3 and the probe's six runs).

      Three choices in it that came from measurements rather than from how
      such a driver usually looks:

      - **`@16040000` is tried before `@16030000`**, not in tree order,
        because the cable is in the second controller — the first bring-up
        measured it negotiating while the other timed out. A driver binding
        in tree order reports a dead link on a working board.
      - **The PHY address is searched, not read from `phy-handle`**: the
        probe found the same PHY answering at two addresses, so one number
        would have been right by luck.
      - **The link bit is read twice before it is believed**, because it
        latches low: one read says "the link was lost at some point", and
        the second says what is true now.

      The MAC address is taken from the registers the firmware programmed
      rather than invented, so this kernel appears on the network as the
      same machine U-Boot was.

      **And frames now go both ways**, proved in MAC loopback:
      `frame of 64 bytes, 6c:cf:39:00:b6:89 -> ff:ff:ff:ff:ff:ff type 0806`
      and `1 sent, 1 received, 0 dropped`. Eight descriptors each way,
      2 KiB buffers, store-and-forward both directions.

      Three details that are where a dwmac driver silently does nothing:
      the ring-length registers hold **one less** than the ring (the
      controller reads it as the last valid index); **writing the tail
      pointer** is what starts a transfer, so a descriptor published without
      moving it sits there for ever; and the **ownership bit goes last**,
      fenced with `fence w, w` as `virtio.c` does, because the controller
      may act the instant it sees that bit and a descriptor published before
      its buffer address is one the hardware fills at whatever address was
      there.

      The DMA software reset had to move **before** the MAC is configured:
      on this controller it resets the whole MAC, so resetting it where it
      naturally belongs — with the rings — wiped the speed and duplex the
      PHY had just negotiated. That fails silently: the link stays up,
      frames leave at the wrong rate, and none arrive.

      **The first test could not have failed usefully.** It sent a broadcast
      ARP and waited for anything at all, assuming a real segment is never
      quiet; when nothing came back, a broken receive ring and a switch port
      with nobody else on it looked identical — and the DMA status was
      consistent with both. Loopback removes the network from the question:
      the frame sent is the frame that must arrive. A test whose outcome
      depends on what else is plugged in was never a test of this driver.

      Two of this driver's own comments had to be corrected after being
      written as though measured: that enhanced addressing (`EAME`) was the
      cause of the receive failure — the buffers came back at
      `0x4084_6000`, inside 32 bits, so it never was, though it stays
      because an 8 GiB board *can* return higher memory — and that `dma
      status` bit 10 was a fatal bus error. Bit 12 is; bit 10 is the early
      transmit interrupt, so the `0x405` that was read as a fault was
      `TI | TBU | ETI` and entirely normal.

- [ ] **The board's Ethernet: the stack over it.** (M) The network stack and
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
