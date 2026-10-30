// lsgo lists the directories named in its arguments (or / and /boot), one
// line per entry with its mode and size: a look at a disk before changing it.
package main

import (
	"fmt"
	"os"
)

func list(dir string) {
	es, err := os.ReadDir(dir)
	if err != nil {
		fmt.Printf("lsgo: %s: %v\n", dir, err)
		return
	}
	fmt.Printf("lsgo: %s: %d entries\n", dir, len(es))
	for _, e := range es {
		fi, err := e.Info()
		if err != nil {
			fmt.Printf("lsgo:   %s ?\n", e.Name())
			continue
		}
		fmt.Printf("lsgo:   %v %9d %s\n", fi.Mode(), fi.Size(), e.Name())
	}
}

func main() {
	dirs := os.Args[1:]
	if len(dirs) == 0 {
		dirs = []string{"/", "/boot", "/bin", "/etc"}
	}
	for _, d := range dirs {
		list(d)
	}
}
