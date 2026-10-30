// bulkgo reads one large file off netfs again and again, timing each read:
// the reproducer for the intermittent stall. Exits 0 when all reads were
// whole and none took over a second.
package main

import (
	"fmt"
	"os"
	"time"
)

func main() {
	var want int
	slow := 0
	for i := 0; i < 20; i++ {
		start := time.Now()
		b, err := os.ReadFile("/net/bin/timego")
		took := time.Since(start)
		if err != nil {
			fmt.Printf("bulkgo: read %d: %v after %v\n", i, err, took)
			os.Exit(1)
		}
		if i == 0 {
			want = len(b)
		}
		if len(b) != want {
			fmt.Printf("bulkgo: read %d: %d bytes, want %d\n", i, len(b), want)
			os.Exit(1)
		}
		if took > time.Second {
			slow++
		}
		fmt.Printf("bulkgo: read %d: %d bytes in %v\n", i, len(b), took.Round(time.Millisecond))
	}
	if slow > 0 {
		fmt.Printf("bulkgo: %d slow reads\n", slow)
		os.Exit(2)
	}
	fmt.Println("bulkgo: every read was whole and quick")
}
