// dotgo resolves paths with `.` and `..` through ordinary Go calls. It
// exits 0 when every step passed.
package main

import (
	"fmt"
	"os"
	"strings"
	"time"
)

var last = time.Now()

func step(name string, err error) {
	took := time.Since(last)
	if err != nil {
		fmt.Printf("dotgo: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("dotgo: %s ok (%v)\n", name, took.Round(time.Millisecond))
	last = time.Now()
}

func readsHello(path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if !strings.HasPrefix(string(b), "hello from netfs") {
		return fmt.Errorf("read %q", b)
	}
	return nil
}

func main() {
	step("absolute ..", readsHello("/net/bin/../hello.txt"))
	step("absolute .", readsHello("/net/./hello.txt"))
	step("mixed", readsHello("/net/bin/./../bin/../hello.txt"))
	step("chdir", os.Chdir("/net/bin"))
	step("relative ..", readsHello("../hello.txt"))
	_, err := os.Stat("./dotgo")
	step("relative .", err)
	step("proc ..", func() error { _, e := os.ReadFile("/proc/self/../uptime"); return e }())
	step("root ..", readsHello("/../../net/hello.txt"))
	fmt.Println("dotgo: every step passed")
}
