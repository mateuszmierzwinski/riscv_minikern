// sdconcurrent reads the card's largest programs one at a time (the
// reference: sequential reads were always right), then three at a time for
// three rounds, comparing each concurrent read with its reference. Nothing
// is written: the card's own files are the data. Prints "sd: PASS|FAIL ..."
// and "sd: done <n> failures".
package main

import (
	"bytes"
	"fmt"
	"os"
	"sort"
	"sync"
)

// largest is the three biggest regular files in /bin on the card.
func largest() []string {
	es, _ := os.ReadDir("/bin")
	type f struct {
		name string
		size int64
	}
	var fs []f
	for _, e := range es {
		if info, err := e.Info(); err == nil && info.Mode().IsRegular() {
			fs = append(fs, f{"/bin/" + e.Name(), info.Size()})
		}
	}
	sort.Slice(fs, func(i, j int) bool { return fs[i].size > fs[j].size })
	var out []string
	for i := 0; i < len(fs) && i < 3; i++ {
		out = append(out, fs[i].name)
	}
	return out
}

func main() {
	names := largest()
	ref := map[string][]byte{}
	for _, n := range names {
		b, err := os.ReadFile(n)
		fmt.Printf("sd: reference %s %d bytes err=%v\n", n, len(b), err)
		ref[n] = b
	}
	var mu sync.Mutex
	failures := 0
	if len(names) < 2 {
		failures++
		fmt.Println("sd: FAIL fewer than two programs on the card to read at once")
	}
	for round := 0; round < 3; round++ {
		var wg sync.WaitGroup
		for _, n := range names {
			wg.Add(1)
			go func(n string) {
				defer wg.Done()
				got, err := os.ReadFile(n)
				mu.Lock()
				defer mu.Unlock()
				if err != nil || !bytes.Equal(got, ref[n]) {
					failures++
					fmt.Printf("sd: FAIL round %d %s: %d bytes, err %v\n", round, n, len(got), err)
					return
				}
				fmt.Printf("sd: PASS round %d %s\n", round, n)
			}(n)
		}
		wg.Wait()
	}
	fmt.Printf("sd: done %d failures\n", failures)
}
