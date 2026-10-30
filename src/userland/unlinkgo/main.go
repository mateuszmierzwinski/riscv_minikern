// unlinkgo removes a file while it is open for writing, then writes to it
// and truncates it through the descriptor: the name must stay gone (not be
// created again by the write), the listing must not show it, and the
// descriptor must still work. After the close nothing is left; the harness
// checks the export from the host. Prints "unlinkgo: every step passed".
package main

import (
	"errors"
	"fmt"
	"os"
)

const dir = "/net/unl"

func gone(path string) error {
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("%s: %v, want not-exist", path, err)
	}
	return nil
}

func empty() error {
	es, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	if len(es) != 0 {
		return fmt.Errorf("%s lists %d entries (%s...)", dir, len(es), es[0].Name())
	}
	return nil
}

func main() {
	path := dir + "/f"
	f, err := os.Create(path)
	if err != nil {
		fmt.Println("unlinkgo: create:", err)
		os.Exit(1)
	}
	steps := []struct {
		name string
		run  func() error
	}{
		{"write before", func() error { _, err := f.WriteString("hello"); return err }},
		{"remove while open", func() error { return os.Remove(path) }},
		{"name is gone", func() error { return gone(path) }},
		{"listing is empty", empty},
		{"write after", func() error { _, err := f.WriteString(" more"); return err }},
		{"ftruncate after", func() error { return f.Truncate(3) }},
		{"still gone after writing", func() error { return gone(path) }},
		{"close", f.Close},
		{"listing still empty", empty},
	}
	failed := 0
	for _, s := range steps {
		if err := s.run(); err != nil {
			failed++
			fmt.Printf("unlinkgo: %s: %v\n", s.name, err)
			continue
		}
		fmt.Printf("unlinkgo: %s ok\n", s.name)
	}
	if failed == 0 {
		fmt.Println("unlinkgo: every step passed")
		return
	}
	os.Exit(1)
}
