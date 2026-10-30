// namego checks that a process is named after its program: /proc/self/comm,
// the Name: line of /proc/self/status, /proc/ps/by-pid/<pid>/name, and the
// /proc/ps/by-name/<name>/<pid>/ view — with a second, exec'd copy of
// itself listed beside it. Prints "namego: every step passed".
package main

import (
	"fmt"
	"os"
	"os/exec"
	"slices"
	"strconv"
	"strings"
	"time"
)

func read(path string) (string, error) {
	b, err := os.ReadFile(path)
	return strings.TrimSpace(string(b)), err
}

func expect(path, want string) error {
	got, err := read(path)
	if err != nil {
		return err
	}
	if got != want {
		return fmt.Errorf("%s = %q, want %q", path, got, want)
	}
	return nil
}

func statusName() error {
	s, err := read("/proc/self/status")
	if err != nil {
		return err
	}
	if !strings.HasPrefix(s, "Name:\tnamego\n") {
		return fmt.Errorf("status starts %q", s[:min(len(s), 30)])
	}
	return nil
}

func listed(dir string, want ...string) error {
	es, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	var got []string
	for _, e := range es {
		got = append(got, e.Name())
	}
	for _, w := range want {
		if !slices.Contains(got, w) {
			return fmt.Errorf("%s lists %v, missing %s", dir, got, w)
		}
	}
	return nil
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "child" {
		time.Sleep(3 * time.Second)
		return
	}
	me := strconv.Itoa(os.Getpid())
	child := exec.Command("/net/bin/namego", "child")
	if err := child.Start(); err != nil {
		fmt.Println("namego: start child:", err)
		os.Exit(1)
	}
	time.Sleep(500 * time.Millisecond)
	kid := strconv.Itoa(child.Process.Pid)
	checks := []struct {
		name string
		err  error
	}{
		{"comm", expect("/proc/self/comm", "namego")},
		{"status Name", statusName()},
		{"by-pid name", expect("/proc/ps/by-pid/"+me+"/name", "namego")},
		{"by-name lists the name", listed("/proc/ps/by-name", "namego")},
		{"by-name lists both copies", listed("/proc/ps/by-name/namego", me, kid)},
		{"by-name file", expect("/proc/ps/by-name/namego/"+kid+"/pid", kid)},
		{"child comm", expect("/proc/"+kid+"/comm", "namego")},
		{"wrong name refused", func() error {
			if _, err := os.Stat("/proc/ps/by-name/nosuch/" + me + "/pid"); err == nil {
				return fmt.Errorf("a pid was found under a name it does not have")
			}
			return nil
		}()},
	}
	failed := 0
	for _, c := range checks {
		if c.err != nil {
			failed++
			fmt.Printf("namego: %s: %v\n", c.name, c.err)
			continue
		}
		fmt.Printf("namego: %s ok\n", c.name)
	}
	_ = child.Wait()
	if failed == 0 {
		fmt.Println("namego: every step passed")
		return
	}
	os.Exit(1)
}
