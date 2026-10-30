// pipego checks what os/exec relies on to capture a child's output: a pipe
// whose read end is waited on by Go's poller (epoll) in a goroutine, and a
// child process writing into it. Exits 0 when both arrive.
package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"time"
)

func step(name string, err error) {
	if err != nil {
		fmt.Printf("pipego: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("pipego: %s ok\n", name)
}

func main() {
	if len(os.Args) > 1 && os.Args[1] == "child" {
		fmt.Println("from the child")
		return
	}
	r, w, err := os.Pipe()
	step("pipe", err)
	got := make(chan string, 1)
	go func() {
		var b bytes.Buffer
		_, _ = io.Copy(&b, r)
		got <- b.String()
	}()
	_, err = w.WriteString("through the pipe\n")
	step("write", err)
	w.Close()
	select {
	case s := <-got:
		step("read in a goroutine", map[bool]error{true: nil, false: fmt.Errorf("got %q", s)}[s == "through the pipe\n"])
	case <-time.After(5 * time.Second):
		step("read in a goroutine", fmt.Errorf("nothing after 5 s: the poller never woke"))
	}
	var out bytes.Buffer
	cmd := exec.Command(os.Args[0], "child")
	cmd.Stdout = &out
	done := make(chan error, 1)
	go func() { done <- cmd.Run() }()
	select {
	case err = <-done:
	case <-time.After(10 * time.Second):
		err = fmt.Errorf("the child's output never arrived (10 s)")
	}
	if err == nil && out.String() != "from the child\n" {
		err = fmt.Errorf("captured %q", out.String())
	}
	step("child's output captured", err)
	fmt.Println("pipego: every step passed")
}
