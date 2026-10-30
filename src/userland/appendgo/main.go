// appendgo appends to a file twice, as QuokkaOS writes a service log, and
// reads it back. Exits 0 when both writes are there.
package main

import (
	"fmt"
	"os"
)

func main() {
	path := "/net/append.test"
	_ = os.Remove(path)
	for _, line := range []string{"first\n", "second\n"} {
		f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o640)
		if err != nil {
			fmt.Println("appendgo: open:", err)
			os.Exit(1)
		}
		n, err := f.WriteString(line)
		fmt.Printf("appendgo: wrote %d bytes, err %v\n", n, err)
		f.Close()
	}
	b, err := os.ReadFile(path)
	fmt.Printf("appendgo: read back %q, err %v\n", b, err)
	if string(b) != "first\nsecond\n" {
		os.Exit(1)
	}
	fmt.Println("appendgo: every step passed")
}
