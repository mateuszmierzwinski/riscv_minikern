// allocstress — the §0 corruption with nothing else in the way.
//
// `racestress` reproduces the defect, and it does several things at once:
// eight goroutines, channels, interfaces, a mutex, an atomic. Every one of
// those was in it because it had been seen to fail, and together they make
// a reproducer rather than an experiment — when it fails, the only honest
// statement is "something went wrong".
//
// This is the same failure with everything removed that could be removed.
// **One goroutine**, no channels, no locks, no sharing: allocate, fill,
// read back, compare. If this fails then no concurrency in the program is
// involved at all, and what is left is the runtime's own collector, the
// threads it keeps, and this kernel — which is a far smaller place to look.
//
// The arithmetic is a sieve because a sieve reads back what it wrote: it
// writes marks across every page of a large allocation and then sums what
// it finds, so a lost store, a stale read and a page that arrived with
// somebody else's bytes in it all come out as a wrong total. `GOMAXPROCS`
// is set here rather than left to the machine so that turning the collector
// off changes only the collector: the runtime keeps the same number of
// threads either way.
package main

import (
	"fmt"
	"os"
	"runtime"
)

const (
	rounds = 2000
	limit  = 60000
)

// Sum of the primes below n, and the sieve it was read from.
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

// Where a wrong sieve differs from a right one, in the terms that have told
// this hunt the most: how many marks are wrong, whether the bytes are still
// booleans, and what the first few of them are.
func describe(sieve, reference []bool) string {
	wrong, zeros, ones, other := 0, 0, 0, 0
	for i := range sieve {
		if sieve[i] != reference[i] {
			wrong++
		}
	}
	for _, b := range sieve {
		switch b {
		case false:
			zeros++
		default:
			ones++
		}
	}
	return fmt.Sprintf("%d of %d marks wrong (%d false, %d true, %d neither)",
		wrong, len(sieve), zeros, ones, other)
}

func main() {
	runtime.GOMAXPROCS(8)
	want, reference := sumOfPrimesBelow(limit)
	fmt.Printf("alloc: %d rounds, one goroutine, GOMAXPROCS=%d, want %d\n",
		rounds, runtime.GOMAXPROCS(0), want)
	for round := 1; round <= rounds; round++ {
		got, sieve := sumOfPrimesBelow(limit)
		if got != want {
			fmt.Printf("alloc: FAILED round %d: summed %d, wanted %d (out by %d)\n",
				round, got, want, got-want)
			fmt.Printf("alloc: the sieve: %s\n", describe(sieve, reference))
			os.Exit(1)
		}
		if round%250 == 0 {
			fmt.Printf("alloc: %d rounds done\n", round)
		}
	}
	fmt.Printf("alloc: %d rounds, nothing went wrong\n", rounds)
}
