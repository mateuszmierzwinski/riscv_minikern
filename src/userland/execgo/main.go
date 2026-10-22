// Running a program from a program: Go's `os/exec`, unchanged.
//
// This is the last thing the kernel this migrated from could do that this one
// could not. It needs, all at once: a second process, a pipe, descriptors
// duplicated onto chosen numbers, `execve` replacing an address space while
// the parent keeps its own, and a status collected afterwards.
//
// Go does not use `fork` for this. It uses `vfork` semantics — the child
// borrows its parent's address space and the parent is suspended until the
// child execs — which is why the parent's output below appearing *after* the
// child's is not an accident but the mechanism.
package main

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
)

func main() {
	fmt.Println("parent: starting /child")
	cmd := exec.Command("/child", "first", "second")
	out, err := cmd.CombinedOutput()
	fmt.Printf("parent: child said %q\n", strings.TrimSpace(string(out)))
	if err == nil {
		fmt.Println("parent: FAILED expected a non-zero exit status, got none")
		os.Exit(1)
	}
	// The status must come back exactly. A kernel that writes it in the
	// wrong bits of the word reports 0 for every program that exits with
	// anything under 256, which looks like success.
	exitErr, ok := err.(*exec.ExitError)
	if !ok {
		fmt.Printf("parent: FAILED unexpected error %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("parent: child exited with %d\n", exitErr.ExitCode())

	// A program that does not exist must fail cleanly, not hang and not
	// take the parent with it: the child's exec failed, and the only way it
	// can say so is the error pipe the runtime set up for exactly this.
	if _, err := exec.Command("/does-not-exist").Output(); err == nil {
		fmt.Println("parent: FAILED running a missing program should have failed")
		os.Exit(1)
	} else {
		fmt.Printf("parent: a missing program failed cleanly\n")
	}

	fmt.Println("parent: done")
}
