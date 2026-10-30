// ioctlgo exercises ioctl through raw system calls. It exits 0 when every
// step passed.
package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

type termios struct {
	Iflag, Oflag, Cflag, Lflag uint32
	Line                       uint8
	Cc                         [19]uint8
}

const (
	tcgets   = 0x5401
	tcsets   = 0x5402
	fionread = 0x541b
	fionbio  = 0x5421
	fionclex = 0x5450
	fioclex  = 0x5451
	icanon   = 0o2
	echo     = 0o10
)

func ioctl(fd uintptr, req uintptr, arg unsafe.Pointer) error {
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, fd, req, uintptr(arg))
	if e != 0 {
		return e
	}
	return nil
}

func fcntl(fd, cmd uintptr) uintptr {
	r, _, _ := syscall.Syscall(syscall.SYS_FCNTL, fd, cmd, 0)
	return r
}

func step(name string, err error) {
	if err != nil {
		fmt.Printf("ioctlgo: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("ioctlgo: %s ok\n", name)
}

func want(cond bool, format string, args ...any) error {
	if cond {
		return nil
	}
	return fmt.Errorf(format, args...)
}

func main() {
	var t termios
	step("TCGETS on stdin", ioctl(0, tcgets, unsafe.Pointer(&t)))
	step("canonical", want(t.Lflag&icanon != 0, "lflag %o", t.Lflag))
	quiet := t
	quiet.Lflag &^= echo
	step("echo off", ioctl(0, tcsets, unsafe.Pointer(&quiet)))
	var back termios
	_ = ioctl(0, tcgets, unsafe.Pointer(&back))
	step("echo reads off", want(back.Lflag&echo == 0, "lflag %o", back.Lflag))
	step("echo on again", ioctl(0, tcsets, unsafe.Pointer(&t)))
	raw := t
	raw.Lflag &^= icanon
	step("raw accepted", ioctl(0, tcsets, unsafe.Pointer(&raw)))
	_ = ioctl(0, tcgets, unsafe.Pointer(&back))
	step("raw reads back", want(back.Lflag&icanon == 0, "lflag %o", back.Lflag))
	step("canonical again", ioctl(0, tcsets, unsafe.Pointer(&t)))
	fast := t
	fast.Cflag ^= 0o17 // another line speed: not a thing this console can be set to
	step("speed change refused", want(ioctl(0, tcsets, unsafe.Pointer(&fast)) == syscall.EINVAL, "accepted"))
	f, err := os.Open("/net/hello.txt")
	step("open", err)
	fd := f.Fd() // once: each Fd() call puts the descriptor back into blocking mode
	var n int32
	step("FIONREAD", ioctl(fd, fionread, unsafe.Pointer(&n)))
	step("FIONREAD count", want(n == 17, "%d bytes", n))
	step("FIOCLEX", ioctl(fd, fioclex, nil))
	step("cloexec set", want(fcntl(fd, syscall.F_GETFD) == 1, "not set"))
	step("FIONCLEX", ioctl(fd, fionclex, nil))
	step("cloexec clear", want(fcntl(fd, syscall.F_GETFD) == 0, "still set"))
	on := int32(1)
	step("FIONBIO", ioctl(fd, fionbio, unsafe.Pointer(&on)))
	step("nonblock set", want(fcntl(fd, syscall.F_GETFL)&syscall.O_NONBLOCK != 0, "not set"))
	step("unknown is ENOTTY", want(ioctl(fd, 0x1234, nil) == syscall.ENOTTY, "not ENOTTY"))
	fmt.Println("ioctlgo: every step passed")
}
