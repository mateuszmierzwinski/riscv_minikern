// pollgo exercises ppoll and pselect6 through raw system calls. It exits 0
// when every step passed.
package main

import (
	"fmt"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

type pollfd struct {
	fd      int32
	events  int16
	revents int16
}

const (
	sysPpoll    = 73
	sysPselect6 = 72
	sysEventfd2 = 19
	pollin      = 0x1
	pollnval    = 0x20
)

func ppoll(fds []pollfd, ts *syscall.Timespec) (int, error) {
	r, _, e := syscall.Syscall6(sysPpoll, uintptr(unsafe.Pointer(&fds[0])), uintptr(len(fds)),
		uintptr(unsafe.Pointer(ts)), 0, 8, 0)
	if e != 0 {
		return -1, e
	}
	return int(r), nil
}

func pselect(n int, rd *[16]uint64, ts *syscall.Timespec) (int, error) {
	r, _, e := syscall.Syscall6(sysPselect6, uintptr(n), uintptr(unsafe.Pointer(rd)), 0, 0,
		uintptr(unsafe.Pointer(ts)), 0)
	if e != 0 {
		return -1, e
	}
	return int(r), nil
}

// sigmask is one thread's blocked set, through rt_sigprocmask.
func sigmask(how int, set *uint64) uint64 {
	var old uint64
	_, _, _ = syscall.RawSyscall6(syscall.SYS_RT_SIGPROCMASK, uintptr(how), uintptr(unsafe.Pointer(set)),
		uintptr(unsafe.Pointer(&old)), 8, 0, 0)
	return old
}

// ppollUnblockingUSR1 blocks SIGUSR1 on a locked thread, then waits in
// ppoll with a temporary mask that lets it through while another goroutine
// sends it to exactly that thread. Linux keeps the temporary mask until the
// handler has run, so the handler sees the signal; afterwards the thread's
// own mask is back. Returns ppoll's error, whether the handler saw the
// signal, and whether SIGUSR1 is blocked again.
func ppollUnblockingUSR1() (error, bool, bool) {
	seen := make(chan os.Signal, 1)
	signal.Notify(seen, syscall.SIGUSR1)
	defer signal.Stop(seen)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	usr1 := uint64(1) << (syscall.SIGUSR1 - 1)
	orig := sigmask(0, &usr1) // SIG_BLOCK
	tid := syscall.Gettid()
	go func() {
		time.Sleep(100 * time.Millisecond)
		_ = syscall.Tgkill(syscall.Getpid(), tid, syscall.SIGUSR1)
	}()
	r, w, _ := os.Pipe()
	defer r.Close()
	defer w.Close()
	fds := []pollfd{{fd: int32(r.Fd()), events: pollin}}
	none := uint64(0)
	_, _, e := syscall.Syscall6(sysPpoll, uintptr(unsafe.Pointer(&fds[0])), 1,
		uintptr(unsafe.Pointer(&syscall.Timespec{Sec: 2})), uintptr(unsafe.Pointer(&none)), 8, 0)
	got := false
	select {
	case <-seen:
		got = true
	case <-time.After(time.Second):
	}
	now := sigmask(0, nil)
	sigmask(2, &orig) // SIG_SETMASK: as it was before the test
	return e, got, now&usr1 != 0
}

func step(name string, err error) {
	if err != nil {
		fmt.Printf("pollgo: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("pollgo: %s ok\n", name)
}

func want(cond bool, format string, args ...any) error {
	if cond {
		return nil
	}
	return fmt.Errorf(format, args...)
}

func main() {
	efd, _, e := syscall.Syscall(sysEventfd2, 0, 0, 0)
	step("eventfd", want(e == 0, "%v", e))
	fds := []pollfd{{fd: int32(efd), events: pollin}}
	ts := syscall.Timespec{Nsec: 50_000_000}
	start := time.Now()
	n, err := ppoll(fds, &ts)
	took := time.Since(start)
	step("ppoll times out", err)
	step("nothing ready", want(n == 0, "%d ready", n))
	step("waited ~50 ms", want(took >= 40*time.Millisecond && took < 500*time.Millisecond, "%v", took))
	step("time left written back", want(ts.Sec == 0 && ts.Nsec < 50_000_000, "%+v", ts))
	one := uint64(1)
	_, err = syscall.Write(int(efd), (*[8]byte)(unsafe.Pointer(&one))[:])
	step("eventfd write", err)
	ts = syscall.Timespec{Sec: 1}
	n, err = ppoll(fds, &ts)
	step("ppoll ready", err)
	step("POLLIN", want(n == 1 && fds[0].revents&pollin != 0, "n %d revents %x", n, fds[0].revents))
	bad := []pollfd{{fd: 999, events: pollin}}
	n, err = ppoll(bad, &syscall.Timespec{})
	step("closed fd is POLLNVAL", want(err == nil && n == 1 && bad[0].revents&pollnval != 0, "n %d %x %v", n, bad[0].revents, err))
	f, err := os.Open("/net/hello.txt")
	step("open file", err)
	file := []pollfd{{fd: int32(f.Fd()), events: pollin}}
	n, err = ppoll(file, &syscall.Timespec{})
	step("file always ready", want(err == nil && n == 1, "n %d %v", n, err))
	var rd [16]uint64
	rd[efd/64] |= 1 << (efd % 64)
	n, err = pselect(int(efd)+1, &rd, &syscall.Timespec{Sec: 1})
	step("pselect6 ready", want(err == nil && n == 1 && rd[efd/64]&(1<<(efd%64)) != 0, "n %d %v", n, err))
	var none [16]uint64
	none[999/64] |= 1 << (999 % 64)
	_, err = pselect(1000, &none, &syscall.Timespec{})
	step("pselect6 closed fd is EBADF", want(err == syscall.EBADF, "%v", err))
	perr, handled, reblocked := ppollUnblockingUSR1()
	step("ppoll interrupted by the signal its mask let in", want(perr == syscall.EINTR, "%v", perr))
	step("the handler ran under ppoll's mask", want(handled, "SIGUSR1 never reached the handler"))
	step("the thread's own mask is back", want(reblocked, "SIGUSR1 left unblocked"))
	fmt.Println("pollgo: every step passed")
}
