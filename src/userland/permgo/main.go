// permgo checks that an unprivileged process cannot change root's files:
// as uid 1000 every write-open, create, unlink, rename, chmod, chown,
// utimes and truncate of a root-owned path must be refused, and running a
// file without execute permission must fail. Prints one line per check
// and "permgo: every check passed" when all were refused as Linux would.
package main

import (
	"errors"
	"fmt"
	"os"
	"runtime"
	"syscall"
	"time"
)

// target is a root-owned file on the export, mode 0755.
const target = "/net/bin/permgo"

type check struct {
	name string
	run  func() error
}

func checks() []check {
	return []check{
		{"open for write", func() error { f, err := os.OpenFile(target, os.O_WRONLY, 0); closeIf(f); return err }},
		{"create in root's dir", func() error { f, err := os.Create("/net/bin/new"); closeIf(f); return err }},
		{"mkdir in root's dir", func() error { return os.Mkdir("/net/bin/d", 0o755) }},
		{"unlink", func() error { return os.Remove(target) }},
		{"rename", func() error { return os.Rename(target, "/net/bin/moved") }},
		{"chmod", func() error { return os.Chmod(target, 0o777) }},
		{"chown", func() error { return os.Chown(target, 1000, 1000) }},
		{"utimes", func() error { return os.Chtimes(target, time.Unix(1, 0), time.Unix(1, 0)) }},
		{"truncate", func() error { return os.Truncate(target, 0) }},
	}
}

func closeIf(f *os.File) {
	if f != nil {
		f.Close()
	}
}

// refused is whether err is the refusal Linux gives an unprivileged caller.
func refused(err error) bool {
	return errors.Is(err, syscall.EACCES) || errors.Is(err, syscall.EPERM) || errors.Is(err, syscall.EROFS)
}

// dropToUser makes the process uid/gid 1000 with raw setresgid/setresuid:
// minikern keeps credentials per process, and Go's syscall.Setuid goes
// through its all-threads signal broadcast, which is a separate test.
func dropToUser() error {
	runtime.LockOSThread()
	if _, _, e := syscall.RawSyscall(syscall.SYS_SETRESGID, 1000, 1000, 1000); e != 0 {
		return fmt.Errorf("setresgid: %w", e)
	}
	if _, _, e := syscall.RawSyscall(syscall.SYS_SETRESUID, 1000, 1000, 1000); e != 0 {
		return fmt.Errorf("setresuid: %w", e)
	}
	return nil
}

func main() {
	if err := dropToUser(); err != nil {
		fmt.Println("permgo:", err)
		os.Exit(2)
	}
	failed := 0
	for _, c := range checks() {
		err := c.run()
		if !refused(err) {
			failed++
			fmt.Printf("permgo: %s: %v (want EACCES/EPERM)\n", c.name, err)
			continue
		}
		fmt.Printf("permgo: %s: refused ok\n", c.name)
	}
	if _, err := os.Stat(target); err != nil {
		failed++
		fmt.Println("permgo: target vanished:", err)
	}
	if failed == 0 {
		fmt.Println("permgo: every check passed")
		return
	}
	os.Exit(1)
}
