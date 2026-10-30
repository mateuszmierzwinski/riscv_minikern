// timego checks file times on the root XFS. It exits 0 when every step
// passed.
package main

import (
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

const utimeOmit = (1 << 30) - 2

func step(name string, err error) {
	if err != nil {
		fmt.Printf("timego: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("timego: %s ok\n", name)
}

func want(cond bool, format string, args ...any) error {
	if cond {
		return nil
	}
	return fmt.Errorf(format, args...)
}

func times(path string) (atime, mtime int64, err error) {
	var st syscall.Stat_t
	if err = syscall.Stat(path, &st); err != nil {
		return 0, 0, err
	}
	return st.Atim.Sec, st.Mtim.Sec, nil
}

func main() {
	_, m, err := times("/")
	step("stat /", err)
	fmt.Printf("timego:   / mtime %v\n", time.Unix(m, 0).UTC())
	path := "/timego.test"
	step("create", os.WriteFile(path, []byte("x"), 0o644))
	_, m, err = times(path)
	step("stat new file", err)
	now := time.Now().Unix()
	step("new file's mtime is now", want(m > now-60 && m < now+60, "mtime %d, now %d", m, now))
	a0, m0 := time.Unix(1700000000, 0), time.Unix(1750000000, 0)
	step("chtimes", os.Chtimes(path, a0, m0))
	a, m, err := times(path)
	step("stat back", err)
	step("times read back", want(a == a0.Unix() && m == m0.Unix(), "atime %d mtime %d", a, m))
	ts := [2]syscall.Timespec{{Sec: 1600000000}, {Nsec: utimeOmit}}
	_, _, e := syscall.Syscall6(syscall.SYS_UTIMENSAT, uintptr(0xffffff9c),
		uintptr(unsafe.Pointer(syscall.StringBytePtr(path))), uintptr(unsafe.Pointer(&ts[0])), 0, 0, 0)
	step("utimensat UTIME_OMIT", want(e == 0, "%v", e))
	a, m, err = times(path)
	step("stat after omit", err)
	step("mtime kept, atime set", want(a == 1600000000 && m == m0.Unix(), "atime %d mtime %d", a, m))
	step("remove", os.Remove(path))
	fmt.Println("timego: every step passed")
}
