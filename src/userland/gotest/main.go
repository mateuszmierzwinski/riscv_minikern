// A Go program that exercises what a kernel has to get right.
//
// Printing "hello" proves a loader. It does not prove threads, and threads
// are where the interesting failures are: a Go runtime starts one during
// initialisation and waits on a futex for it, so a kernel that gets `clone`
// or `futex` subtly wrong does not run Go slowly — it deadlocks before
// `main`. This program therefore spends its time on the things that need
// more than one thread to be real.
//
// It is deliberately standard Go, with no build tags and nothing conditional
// on the kernel underneath it. The point is that it was not written for this
// kernel at all.
package main

import (
	"fmt"
	"runtime"
	"sort"
	"sync"
	"time"
)

// Work that cannot be optimised away and takes long enough that the
// goroutines running it genuinely overlap.
func sumOfPrimesBelow(n int) int {
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
	return total
}

// A mutex under contention from every goroutine at once: the case that needs
// futex waiting and waking to be correct, rather than merely present.
func contendedCounter(goroutines, each int) int {
	var mu sync.Mutex
	var wg sync.WaitGroup
	count := 0
	for i := 0; i < goroutines; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < each; j++ {
				mu.Lock()
				count++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	return count
}

// Values crossing goroutines through a channel, gathered back in order. A
// channel send that blocks parks the goroutine, which is the scheduler's
// own machinery rather than the kernel's — but it only works if the kernel
// kept its promises underneath.
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
	sort.Ints(got)
	return got
}

func main() {
	fmt.Printf("go: %s on %s/%s\n", runtime.Version(), runtime.GOOS, runtime.GOARCH)
	fmt.Printf("go: GOMAXPROCS=%d NumCPU=%d\n", runtime.GOMAXPROCS(0), runtime.NumCPU())

	// Allocation and the garbage collector: megabytes of short-lived
	// objects, which is what makes the runtime call mmap repeatedly and
	// then hand the memory back.
	before := runtime.NumGoroutine()
	kept := make([][]byte, 0, 64)
	for i := 0; i < 2000; i++ {
		b := make([]byte, 4096)
		b[0] = byte(i)
		if i%64 == 0 {
			kept = append(kept, b)
		}
	}
	runtime.GC()
	var stats runtime.MemStats
	runtime.ReadMemStats(&stats)
	fmt.Printf("go: allocated %d MiB total, %d collections, %d blocks kept\n",
		stats.TotalAlloc/(1024*1024), stats.NumGC, len(kept))

	fmt.Printf("go: sum of primes below 200000 is %d\n", sumOfPrimesBelow(200000))

	// Goroutines doing real work in parallel.
	start := time.Now()
	var wg sync.WaitGroup
	results := make([]int, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			results[i] = sumOfPrimesBelow(20000 + i*1000)
		}(i)
	}
	wg.Wait()
	fmt.Printf("go: 8 goroutines finished, first=%d last=%d\n", results[0], results[7])

	fmt.Printf("go: contended counter reached %d\n", contendedCounter(8, 2000))
	fmt.Printf("go: channel values %v\n", throughChannels(6))

	// Sleeping, which is nanosleep, and timekeeping, which is
	// clock_gettime: a sleep that returns immediately or a clock that does
	// not move would both show up here.
	slept := time.Now()
	time.Sleep(50 * time.Millisecond)
	elapsed := time.Since(slept)
	fmt.Printf("go: slept for %v (at least 50ms: %v)\n", elapsed.Round(time.Millisecond),
		elapsed >= 50*time.Millisecond)

	// A panic, recovered: the runtime unwinding its own stack.
	func() {
		defer func() {
			if r := recover(); r != nil {
				fmt.Printf("go: recovered from %v\n", r)
			}
		}()
		var m map[string]int
		m["this map is nil"] = 1
	}()

	fmt.Printf("go: %d goroutines at the end (%d at the start), total time %v\n",
		runtime.NumGoroutine(), before, time.Since(start).Round(time.Millisecond))
	fmt.Println("go: done")
}
