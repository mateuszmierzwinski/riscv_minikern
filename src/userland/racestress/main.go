// A reproducer for the four-hart race, built to fail often.
//
// `ROADMAP.md` §0 describes a corruption that a stock Go binary notices about
// once in thirty runs: a buffered channel yields values nobody sent, a
// `WaitGroup` reports its own state as impossible, an interface value is
// garbage by the time the runtime prints it. One failure in thirty boots is
// enough to know something is wrong and far too rare to *compare* anything:
// every experiment — a knob on, a knob off — needs hundreds of runs before
// its number means more than luck, and at a minute a run that is a day per
// question.
//
// So this program does nothing but the operations that have failed, in a
// loop, checking every result, and stops at the first one that is wrong. It
// is not a benchmark and it is not a demonstration: it is an attempt to turn
// "once in thirty boots" into "once in a few seconds", so that a question can
// be answered in minutes.
//
// It is still ordinary Go. Nothing here is conditional on the kernel
// underneath, and nothing avoids a pattern because this kernel is bad at it —
// that would be measuring the test instead of the kernel.
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// Asks the kernel to audit its own page tables, at the moment this program
// has noticed something wrong.
//
// The corruption is transient: by the time anything periodic looks, the
// memory reads correctly again. This program is the only thing that knows
// the moment, so it says so. The number is a private advice value far
// outside the ones Linux defines, which is what makes this harmless
// anywhere else — a real kernel answers EINVAL and the program carries on
// to report what it saw.
func askTheKernelToAudit() {
	_, _, _ = syscall.Syscall(syscall.SYS_MADVISE, 0, 0, 0x4d494e)
}

// The same question about one address: what backs it, and what else reaches
// that frame. Asked at the moment the bytes there were read wrong, which is
// the only moment the answer is still the one that matters.
func askTheKernelAbout(p unsafe.Pointer) {
	_, _, _ = syscall.Syscall(syscall.SYS_MADVISE, uintptr(p), 4096, 0x4d494e)
}

// How many times round. A constant rather than an argument or an environment
// variable: this kernel passes one environment variable to `/init`, and that
// one is needed for the experiments this program exists to make possible
// (`GODEBUG=asyncpreemptoff=1` and the like).
const rounds = 300

// The channel case, which has failed as a `range` yielding zeroes before the
// real values — a slice or a channel read while another goroutine was still
// writing it.
func throughChannels(n int) []int {
	out := make(chan int, 4)
	var wg sync.WaitGroup
	for i := 1; i <= n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			out <- i * i
		}(i)
	}
	go func() { wg.Wait(); close(out) }()
	got := []int{}
	for v := range out {
		got = append(got, v)
	}
	return got
}

// The same values, as a set: the order goroutines finish in is theirs to
// choose, and only the contents are the kernel's business.
func wanted(n int) map[int]bool {
	want := map[int]bool{}
	for i := 1; i <= n; i++ {
		want[i*i] = true
	}
	return want
}

// The interface case. A corrupted interface value is what "panic while
// printing panic value" means, and formatting one through `fmt` is the
// cheapest way to make the runtime dereference it.
type carrier struct {
	label string
	count int
}

func (c carrier) String() string { return fmt.Sprintf("%s/%d", c.label, c.count) }

func throughInterfaces(n int) (string, bool) {
	var mu sync.Mutex
	var wg sync.WaitGroup
	seen := map[string]bool{}
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			var any fmt.Stringer = carrier{label: "c", count: i}
			text := any.String()
			mu.Lock()
			seen[text] = true
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	for i := 0; i < n; i++ {
		text := fmt.Sprintf("c/%d", i)
		if !seen[text] {
			return text, false
		}
	}
	return "", true
}

// Work that cannot be optimised away, allocating as it goes.
//
// **The allocation is the point**, not the arithmetic. The failures this
// program is chasing all happened in a program whose garbage collector was
// working hard, and a collector is the heaviest user of the kernel there is:
// it maps, unmaps and advises constantly, it faults in pages on three harts
// while a fourth is changing the tables, and it stops the world — which is
// where cross-hart agreement about memory gets tested. A reproducer that
// allocates nothing exercises none of that, which is why the first version
// of this program ran four hundred rounds of channels and counters without
// reproducing anything.
func sumOfPrimesBelow(n int) (int, []bool) {
	sieve := make([]bool, n)
	return fillSieve(sieve), sieve
}

// A root in the data segment, held by nothing on any stack. See the call
// site: which kind of root keeps the sieve alive is the question.
var published [16][]bool

// Whether a worker gives its sieve a heap root before filling it. A
// diagnostic: see the comment at the call site.
var publishFirst = os.Getenv("PUBLISH_FIRST") != ""

// The sieve itself, over a slice somebody else owns, so that the caller
// decides what roots the memory while it is being filled.
func fillSieve(sieve []bool) int {
	n := len(sieve)
	total := 0
	for i := 2; i < n; i++ {
		if sieve[i] {
			continue
		}
		total += i
		for j := i * i; j < n; j += i {
			sieve[j] = true
		}
	}
	return total
}

// The same work on every hart at once, which is what makes the overlap real
// rather than a matter of scheduling luck.
func inParallel(workers, n int) ([]int, [][]bool) {
	got := make([]int, workers)
	/* **Where each worker's sieve actually was**, recorded by the worker
	 * itself the moment `make` returned it.
	 *
	 * Two different faults look identical from the outside. The slice's
	 * *bytes* may have been changed under it, or the slice **header** may
	 * have been changed to point at some other span — one the collector
	 * then freed quite correctly, because nothing pointed at it any more.
	 * Reading either gives the freed-span pattern, so the pattern cannot
	 * tell them apart; comparing the address against the one the worker
	 * was given can. A header that still points where `make` put it means
	 * the data moved under a sound pointer. */
	bases := make([]uintptr, workers)
	owned := make([]bool, workers)
	/* **Did `wg.Wait()` actually wait?**
	 *
	 * A `WaitGroup` is a futex underneath, and this kernel's futex is its
	 * own. If a wake arrives that should not have, or an accounting slip
	 * lets `Wait` return while a worker is still running, then everything
	 * read afterwards is simply read too early — a sieve caught part way
	 * through being built, and two passes over it disagreeing because it is
	 * still being written. That is a precise description of what this
	 * program has been reporting, and it is not memory corruption at all.
	 *
	 * So each worker says so itself, as its last act, and `Wait` is held to
	 * its promise rather than trusted. */
	finished := make([]bool, workers)
	/* **Each worker's own sieve, kept.** The first version of this threw
	 * them away and, on a wrong sum, computed a *fresh* sieve to compare
	 * against the reference — which answers a question nobody asked. The
	 * sieve that matters is the one the wrong sum was computed from. */
	sieves := make([][]bool, workers)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			/* **The sieve is published before it is filled.**
			 *
			 * Which root holds it is the whole question. While
			 * `sumOfPrimesBelow` ran, the only reference to its slice was a
			 * local in this running goroutine — a register or a stack slot
			 * — so a collector that scans a running goroutine's stack
			 * imprecisely would free the slice *while the loop was still
			 * writing it*, which is exactly what "the sum was already wrong
			 * when it was computed" looks like. Storing it into the shared
			 * slice first gives it a root on the heap, reachable from
			 * `main`, for the whole computation. If that is enough to make
			 * the corruption stop, the root that was being missed was this
			 * goroutine's own stack. */
			sieve := make([]bool, n)
			if publishFirst {
				/* **A package-level global, which is a different root from
				 * anything tried so far.** The first attempt at this stored
				 * the slice into the shared `sieves`, and that was not the
				 * experiment it looked like: `sieves` is itself a heap
				 * object reachable from `inParallel`'s frame — on a
				 * goroutine's stack. If a goroutine's stack is what the
				 * collector is failing to scan, storing one more pointer
				 * into something reachable only through that stack proves
				 * nothing, which is exactly the result it gave.
				 *
				 * A global lives in the data segment and is scanned as a
				 * root unconditionally, with no stack involved. If that is
				 * enough to keep the sieve alive, the root being missed is
				 * a stack. */
				published[w] = sieve
			}
			/* **A guard the same size, allocated in the same breath.**
			 *
			 * The stamped slabs are checked every round and have never once
			 * been wrong — but they are allocated before anything runs in
			 * parallel and never freed, so they sit in the cold part of the
			 * heap, nowhere near the churn. That makes them evidence about
			 * cold memory and nothing else.
			 *
			 * This one is allocated beside the sieve, lives exactly as
			 * long, and holds a byte no sieve ever writes. If it comes back
			 * wrong, the damage is not something about *this* object being
			 * freed early; it is that memory in the hot part of the heap is
			 * being written by somebody else, and the sieve is simply the
			 * thing that was looking. */
			guard := make([]byte, n)
			/* **Two allocations, one address.**
			 *
			 * The guard was added to ask whether hot memory gets written by
			 * somebody else. It answered something sharper: the guard comes
			 * back *pure* while the sieve beside it sums to zero, and those
			 * two facts fit together in only one way. A sieve whose every
			 * position already reads true skips its whole outer loop and so
			 * writes nothing at all — which is what leaves the guard
			 * pristine — and every position reads true because the bytes
			 * under it are `0xA5`. The sieve is reading the guard.
			 *
			 * The earlier overlap check compared the eight workers' sieves
			 * against each other and never compared a worker's own two
			 * allocations, which is where it is. So this is checked at the
			 * moment both addresses are known, before either is used. */
			if a, b := uintptr(unsafe.Pointer(&sieve[0])), uintptr(unsafe.Pointer(&guard[0])); a < b+uintptr(n) && b < a+uintptr(n) {
				fmt.Printf("stress: **worker %d got one range from two allocations**: sieve %#x, guard %#x, %d bytes each\n",
					w, a, b, n)
				os.Exit(7)
			}
			for i := range guard {
				guard[i] = 0xA5
			}
			sum := fillSieve(sieve)
			/* **The decisive question, asked of the kernel directly.**
			 *
			 * A failing round has the sieve reading the guard's `0xA5`
			 * while the two ranges provably do not overlap, so either two
			 * distinct virtual ranges share physical memory or the bytes
			 * arrived after the runtime cleared the sieve. Those have
			 * different culprits and the program cannot tell them apart —
			 * but the kernel can say which frame backs each address, and
			 * the only moment that answer is still true is now. Two lines
			 * of output: the same frame twice is aliasing, two frames is
			 * not, and either way the guessing stops. */
			if sum != wantedSum {
				askTheKernelAbout(unsafe.Pointer(&sieve[0]))
				askTheKernelAbout(unsafe.Pointer(&guard[0]))
			}
			for i, b := range guard {
				if b != 0xA5 {
					fmt.Printf("stress: **the guard beside worker %d's sieve holds %#02x at %d**\n",
						w, b, i)
					os.Exit(5)
				}
			}
			bases[w] = uintptr(unsafe.Pointer(&sieve[0]))
			/* Checked here, by the goroutine that just built it, before it
			 * is handed anywhere: "already wrong when it was computed" and
			 * "wrong by the time anyone looked" are different bugs. */
			owned[w] = sum == wantedSum
			got[w], sieves[w] = sum, sieve
			finished[w] = true
		}(w)
	}
	wg.Wait()
	for w := 0; w < workers; w++ {
		if !finished[w] {
			fmt.Printf("stress: **Wait() returned while worker %d was still running**\n", w)
			os.Exit(6)
		}
	}
	/* **Did two workers get the same memory?**
	 *
	 * Every sign points at it: the bytes change between two passes of the
	 * same report, and a snapshot of them looks like a sieve part way
	 * through being built. Two owners of one range explains all of it, and
	 * it is one comparison to prove rather than infer — the slices are
	 * right here, and either their addresses overlap or they do not. */
	for w := 0; w < workers; w++ {
		at := uintptr(unsafe.Pointer(&sieves[w][0]))
		if at != bases[w] {
			fmt.Printf("stress: **worker %d's slice header moved**: %#x, was %#x\n",
				w, at, bases[w])
			os.Exit(4)
		}
		if !owned[w] {
			fmt.Printf("stress: worker %d's sum was **already wrong when it computed it**\n", w)
		}
	}
	for a := 0; a < workers; a++ {
		for b := a + 1; b < workers; b++ {
			x := uintptr(unsafe.Pointer(&sieves[a][0]))
			y := uintptr(unsafe.Pointer(&sieves[b][0]))
			if x < y+uintptr(n) && y < x+uintptr(n) {
				fmt.Printf("stress: **workers %d and %d share memory**: %#x and %#x, %d bytes each\n",
					a, b, x, y, n)
				os.Exit(3)
			}
		}
	}
	return got, sieves
}

// Where a wrong sieve went wrong, which is the question a wrong *sum* cannot
// answer.
//
// The sum came back as a page-aligned-looking number far larger than the
// right one, which means composites were counted as primes — the sieve's
// marks were missing rather than the arithmetic being wrong. Whether they
// are missing one at a time, or a cache line at a time, or **a page at a
// time**, is the difference between a lost store and a lost mapping, and it
// is visible only by looking at the slice itself.
// What the damage looks like, not just how much of it there is.
//
// A count of wrong bytes cannot tell a page of foreign data from a
// scattered sprinkle, and those have different causes: a page-sized,
// page-aligned run means a whole mapping went somewhere else, while
// isolated bytes mean something wrote through a stale pointer. So this
// reports the **runs** — how many, the longest, and where the first few
// start and end — and the slice's own address, because a run that begins
// on a page boundary is saying something a run that begins at 1,234 is not.
func describeSieve(sieve, reference []bool) string {
	n := len(sieve)
	base := uintptr(unsafe.Pointer(&sieve[0]))
	wrong, runs, longest := 0, 0, 0
	var where string
	for i := 0; i < n; i++ {
		if sieve[i] == reference[i] {
			continue
		}
		wrong++
		if i == 0 || sieve[i-1] == reference[i-1] {
			runs++
			start := i
			end := i
			for end+1 < n && sieve[end+1] != reference[end+1] {
				end++
			}
			longest = max(longest, end-start+1)
			if runs <= 4 {
				where += fmt.Sprintf(" [%d..%d at +%d%%4096=%d]",
					start, end, start, (base+uintptr(start))%4096)
			}
		}
	}
	if wrong == 0 {
		return "the sieve it summed is **correct**, so the arithmetic lost the number"
	}
	/* **Which way each one is wrong**, which the positions cannot say. A mark
	 * that is missing means a store was lost; a mark that is *extra* at a
	 * prime means something marked a position no pass ever visits, because
	 * every pass starts at p*p. The two have nothing to do with each other,
	 * and a count of "wrong" hides which one this is. */
	var values string
	shown := 0
	for i := 0; i < n && shown < 16; i++ {
		if sieve[i] != reference[i] {
			values += fmt.Sprintf(" %d:%t/%t", i, sieve[i], reference[i])
			shown++
		}
	}
	/* **Whose data is this?** The positions and the direction say the bytes
	 * are foreign; only the bytes themselves say where they came from. A
	 * run of small equal numbers is one of the stamped slabs, 0 and 1 is
	 * another sieve, and eight-byte values that look like addresses are the
	 * Go runtime's own structures. Each points somewhere different. */
	var hist [256]int
	for i := 0; i < n; i++ {
		hist[*(*byte)(unsafe.Add(unsafe.Pointer(&sieve[0]), i))]++
	}
	raw := ""
	for v, c := range hist {
		if c != 0 {
			raw += fmt.Sprintf(" %#02x:%d", v, c)
		}
	}
	zeros := ""
	count := 0
	for i := 0; i < n && count < 24; i++ {
		if !sieve[i] {
			zeros += fmt.Sprintf(" %d", i)
			count++
		}
	}
	raw += "\n          unmarked positions:" + zeros
	return fmt.Sprintf("%d of %d marks wrong in %d runs, longest %d, base %#x;%s\n          got/want:%s\n          byte histogram:%s",
		wrong, n, runs, longest, base, where, values, raw)
}

/* ── the slab test: who else owns this memory? ──────────────────────────── */

// Slabs that stay alive, each stamped with a byte nobody else uses.
//
// **The discriminator the sieve could not be.** A wrong sieve says the bytes
// changed; it cannot say whether the kernel changed them under their only
// owner or whether the allocator handed the same addresses to somebody else.
// A stamp can: every slab is filled with its own number, every slab is kept
// reachable, and every byte is checked every round. Then a byte that is not
// this slab's number is read for *what* it is —
//
//   - another slab's number: the two **overlap**. One range, two owners,
//     and the allocator is the thing that gave it out twice.
//   - zero: something zeroed it, which is what a fresh mapping over live
//     memory looks like from inside.
//   - anything else: the kernel moved bytes nobody in this program wrote.
//
// Those three have different causes and the sieve's symptom is identical for
// all of them, which is why this program now carries both.
// The right answer, computed once before anything runs in parallel, so a
// worker can check its own arithmetic the instant it finishes.
var wantedSum int

const (
	slabCount = 24
	slabBytes = 60000
)

type slabReport struct {
	slab, offset int
	got, want    byte
	otherSlab    int // -1 unless `got` is another slab's stamp
}

func stampSlabs() [][]byte {
	slabs := make([][]byte, slabCount)
	for i := range slabs {
		slabs[i] = make([]byte, slabBytes)
		for j := range slabs[i] {
			slabs[i][j] = byte(i + 1)
		}
	}
	return slabs
}

// Reports the first byte in any slab that is not that slab's stamp.
func checkSlabs(slabs [][]byte) (slabReport, bool) {
	for i, slab := range slabs {
		want := byte(i + 1)
		for j, got := range slab {
			if got == want {
				continue
			}
			other := -1
			if got >= 1 && got <= slabCount {
				other = int(got) - 1
			}
			return slabReport{slab: i, offset: j, got: got, want: want, otherSlab: other}, true
		}
	}
	return slabReport{}, false
}

func (r slabReport) String() string {
	what := "a value no slab stamps"
	switch {
	case r.got == 0:
		what = "**zero** — something mapped fresh memory over live data"
	case r.otherSlab >= 0:
		what = fmt.Sprintf("**slab %d's stamp — the two share this address**", r.otherSlab)
	}
	return fmt.Sprintf("slab %d byte %d (page %d, offset %d) holds %d, wanted %d: %s",
		r.slab, r.offset, r.offset/4096, r.offset%4096, r.got, r.want, what)
}

// The counter case: a mutex and an atomic under contention from every
// goroutine at once, which is where a lost increment shows.
func contended(workers, each int) (int64, int64) {
	var mu sync.Mutex
	var locked int64
	var atomicCount int64
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < each; i++ {
				mu.Lock()
				locked++
				mu.Unlock()
				atomic.AddInt64(&atomicCount, 1)
			}
		}()
	}
	wg.Wait()
	return locked, atomicCount
}

func main() {
	/* **The thread count is the program's, not the machine's.**
	 *
	 * `GOGC=off` stops the corruption, which looked like proof that the
	 * collector is involved — but turning the collector off also stops the
	 * runtime creating threads for it, and the rate plainly tracks the
	 * thread count (one hart reproduces 2 in 6 on its own and 6 in 6 with
	 * eight `P`s). Those are different causes with the same lever, and the
	 * kernel's one-variable `minikern.env=` cannot set both. Fixing the
	 * count here leaves `GOGC` free to be the only thing that changes. */
	runtime.GOMAXPROCS(8)
	fmt.Printf("stress: %d rounds on %d cpus, GOMAXPROCS=%d\n",
		rounds, runtime.NumCPU(), runtime.GOMAXPROCS(0))
	const channelValues = 6
	const workers = 8
	const each = 500
	const sieve = 60000
	want := wanted(channelValues)
	wantSum, reference := sumOfPrimesBelow(sieve)
	wantedSum = wantSum
	slabs := stampSlabs()
	fmt.Printf("stress: %d slabs of %d bytes at %#x..%#x\n", slabCount, slabBytes,
		uintptr(unsafe.Pointer(&slabs[0][0])), uintptr(unsafe.Pointer(&slabs[slabCount-1][0])))
	for round := 1; round <= rounds; round++ {
		if r, bad := checkSlabs(slabs); bad {
			askTheKernelToAudit()
			fmt.Printf("stress: FAILED round %d: %v\n", round, r)
			os.Exit(2)
		}
		/* Every hart busy, allocating, before anything is checked. */
		sums, sieves := inParallel(workers, sieve)
		for w, sum := range sums {
			if sum != wantSum {
				askTheKernelAbout(unsafe.Pointer(&sieves[w][0]))
				askTheKernelAbout(unsafe.Pointer(&sieves[w][30000]))
				askTheKernelToAudit()
				fmt.Printf("stress: FAILED round %d: worker %d summed %d, wanted %d (out by %d)\n",
					round, w, sum, wantSum, sum-wantSum)
				fmt.Printf("stress: the sieve: %s\n", describeSieve(sieves[w], reference))
				os.Exit(1)
			}
		}
		got := throughChannels(channelValues)
		if len(got) != channelValues {
			fmt.Printf("stress: FAILED round %d: channel gave %d values: %v\n",
				round, len(got), got)
			os.Exit(1)
		}
		for _, v := range got {
			if !want[v] {
				fmt.Printf("stress: FAILED round %d: channel gave %v\n", round, got)
				os.Exit(1)
			}
		}
		if missing, ok := throughInterfaces(workers); !ok {
			fmt.Printf("stress: FAILED round %d: interface lost %q\n", round, missing)
			os.Exit(1)
		}
		locked, counted := contended(workers, each)
		if locked != int64(workers*each) || counted != int64(workers*each) {
			fmt.Printf("stress: FAILED round %d: counters %d and %d, wanted %d\n",
				round, locked, counted, workers*each)
			os.Exit(1)
		}
		if round%25 == 0 {
			fmt.Printf("stress: %d rounds done\n", round)
		}
	}
	fmt.Printf("stress: %d rounds, nothing went wrong\n", rounds)
}
