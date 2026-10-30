// rawgo drives the console's modes with keys the harness types
// (board.py --send): a canonical line, then raw mode as x/term's MakeRaw
// sets it — every byte at once, ^C and CR as data — a VTIME timeout, a
// refused flag, untranslated output, and the old mode restored.
//
// The harness sends, in order: "hello\r" after "rawgo: type a line",
// "a\x03\r" after "rawgo: raw keys", "bye\r" after "rawgo: canonical again".
// Prints "rawgo: every step passed".
package main

import (
	"errors"
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

func get() (syscall.Termios, error) {
	var t syscall.Termios
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, 0, syscall.TCGETS, uintptr(unsafe.Pointer(&t)))
	if e != 0 {
		return t, e
	}
	return t, nil
}

func set(t syscall.Termios) error {
	_, _, e := syscall.Syscall(syscall.SYS_IOCTL, 0, syscall.TCSETS, uintptr(unsafe.Pointer(&t)))
	if e != 0 {
		return e
	}
	return nil
}

// makeRaw is x/term's MakeRaw, flag for flag.
func makeRaw(t syscall.Termios) syscall.Termios {
	t.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP | syscall.INLCR |
		syscall.IGNCR | syscall.ICRNL | syscall.IXON
	t.Oflag &^= syscall.OPOST
	t.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	t.Cflag &^= syscall.CSIZE | syscall.PARENB
	t.Cflag |= syscall.CS8
	t.Cc[syscall.VMIN] = 1
	t.Cc[syscall.VTIME] = 0
	return t
}

func readN(n int) ([]byte, error) {
	var got []byte
	buf := make([]byte, 16)
	for len(got) < n {
		k, err := os.Stdin.Read(buf)
		if err != nil {
			return got, err
		}
		got = append(got, buf[:k]...)
	}
	return got, nil
}

func line(prompt, want string) error {
	fmt.Println(prompt)
	b, err := readN(len(want))
	if err != nil {
		return err
	}
	if string(b) != want {
		return fmt.Errorf("read %q, want %q", b, want)
	}
	return nil
}

func rawKeys(orig syscall.Termios) error {
	if err := set(makeRaw(orig)); err != nil {
		return fmt.Errorf("MakeRaw refused: %v", err)
	}
	os.Stdout.WriteString("rawgo: raw keys\n")
	b, err := readN(3)
	if err != nil {
		return err
	}
	if string(b) != "a\x03\r" {
		return fmt.Errorf("raw read %q, want %q", b, "a\x03\r")
	}
	return nil
}

func vtimeTimesOut(orig syscall.Termios) error {
	t := makeRaw(orig)
	t.Cc[syscall.VMIN], t.Cc[syscall.VTIME] = 0, 5
	if err := set(t); err != nil {
		return err
	}
	start := time.Now()
	n, _ := os.Stdin.Read(make([]byte, 8))
	if took := time.Since(start); n != 0 || took < 400*time.Millisecond || took > 1500*time.Millisecond {
		return fmt.Errorf("VTIME 5 read %d bytes after %v", n, took)
	}
	return nil
}

func refusesIXON(orig syscall.Termios) error {
	t := orig
	t.Iflag |= syscall.IXON
	if err := set(t); !errors.Is(err, syscall.EINVAL) {
		return fmt.Errorf("IXON (not built) answered %v, want EINVAL", err)
	}
	return nil
}

func main() {
	orig, err := get()
	if err != nil || orig.Lflag&syscall.ICANON == 0 {
		fmt.Println("rawgo: TCGETS", err, orig.Lflag)
		os.Exit(1)
	}
	steps := []struct {
		name string
		run  func() error
	}{
		{"canonical line", func() error { return line("rawgo: type a line", "hello\n") }},
		{"raw keys", func() error { return rawKeys(orig) }},
		{"untranslated output", func() error {
			_, err := os.Stdout.WriteString("rawgo-opost:x\ny:end\r\n")
			return err
		}},
		{"VTIME timeout", func() error { return vtimeTimesOut(orig) }},
		{"unbuilt flag refused", func() error { return refusesIXON(orig) }},
		{"canonical again", func() error {
			if err := set(orig); err != nil {
				return err
			}
			return line("rawgo: canonical again", "bye\n")
		}},
	}
	failed := 0
	for _, s := range steps {
		if err := s.run(); err != nil {
			failed++
			fmt.Printf("rawgo: %s: %v\n", s.name, err)
			continue
		}
		fmt.Printf("rawgo: %s ok\n", s.name)
	}
	_ = set(orig)
	if failed == 0 {
		fmt.Println("rawgo: every step passed")
		return
	}
	os.Exit(1)
}
