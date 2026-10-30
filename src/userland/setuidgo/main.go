// setuidgo calls Go's syscall.Setgid and syscall.Setuid with several threads
// running: the runtime applies each to every thread (a signal to each, the
// call made in the handler, the caller waiting for all of them), so a lost
// signal or wake hangs it. Checks every thread then reports the new ids
// through /proc/self/status. Prints "setuidgo: every step passed".
package main

import (
	"fmt"
	"os"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
)

func busyThreads(n int, stop <-chan struct{}, wg *sync.WaitGroup) {
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			runtime.LockOSThread()
			for {
				select {
				case <-stop:
					return
				default:
					time.Sleep(time.Millisecond)
				}
			}
		}()
	}
}

func within(d time.Duration, f func() error) error {
	done := make(chan error, 1)
	go func() { done <- f() }()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		return fmt.Errorf("did not return within %v", d)
	}
}

func main() {
	stop := make(chan struct{})
	var wg sync.WaitGroup
	busyThreads(4, stop, &wg)
	time.Sleep(100 * time.Millisecond)
	failed := 0
	for _, s := range []struct {
		name string
		run  func() error
	}{
		{"Setgid", func() error { return within(10*time.Second, func() error { return syscall.Setgid(1000) }) }},
		{"Setuid", func() error { return within(10*time.Second, func() error { return syscall.Setuid(1000) }) }},
		{"every thread changed", func() error {
			st, err := os.ReadFile("/proc/self/status")
			if err != nil {
				return err
			}
			if !strings.Contains(string(st), "Uid:\t1000\t1000\t1000") {
				return fmt.Errorf("status: %q", st)
			}
			return nil
		}},
	} {
		if err := s.run(); err != nil {
			failed++
			fmt.Printf("setuidgo: %s: %v\n", s.name, err)
			continue
		}
		fmt.Printf("setuidgo: %s ok\n", s.name)
	}
	close(stop)
	wg.Wait()
	if failed == 0 {
		fmt.Println("setuidgo: every step passed")
		return
	}
	os.Exit(1)
}
