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
)

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
	return total, sieve
}

// The same work on every hart at once, which is what makes the overlap real
// rather than a matter of scheduling luck.
func inParallel(workers, n int) ([]int, [][]bool) {
	got := make([]int, workers)
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
			got[w], sieves[w] = sumOfPrimesBelow(n)
		}(w)
	}
	wg.Wait()
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
func describeSieve(sieve, reference []bool) string {
	n := len(sieve)
	first, last, wrong := -1, -1, 0
	for i := 0; i < n; i++ {
		if sieve[i] != reference[i] {
			if first < 0 {
				first = i
			}
			last = i
			wrong++
		}
	}
	if wrong == 0 {
		return "the sieve it summed is **correct**, so the arithmetic lost the number"
	}
	return fmt.Sprintf("%d of %d marks wrong, first at %d (page %d, offset %d), last at %d",
		wrong, n, first, first/4096, first%4096, last)
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
	fmt.Printf("stress: %d rounds on %d cpus, GOMAXPROCS=%d\n",
		rounds, runtime.NumCPU(), runtime.GOMAXPROCS(0))
	const channelValues = 6
	const workers = 8
	const each = 500
	const sieve = 60000
	want := wanted(channelValues)
	wantSum, reference := sumOfPrimesBelow(sieve)
	for round := 1; round <= rounds; round++ {
		/* Every hart busy, allocating, before anything is checked. */
		sums, sieves := inParallel(workers, sieve)
		for w, sum := range sums {
			if sum != wantSum {
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
