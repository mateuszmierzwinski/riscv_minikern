// The program that gets run. It exists to be started by another program, so
// everything it does is evidence of something the kernel had to get right.
package main

import (
	"fmt"
	"os"
)

func main() {
	// Its arguments, which the parent chose: proof that argv crossed an
	// execve rather than being inherited from whatever was there before.
	fmt.Printf("child: argc=%d argv=%v\n", len(os.Args), os.Args)
	// Its own pid, and its parent's: proof that it is a different process
	// from the one that started it, and that it knows which one that was.
	fmt.Printf("child: pid=%d ppid=%d\n", os.Getpid(), os.Getppid())
	// Standard output arrived through a pipe the parent put on descriptor 1
	// before the exec — so this line being readable by the parent is the
	// whole of dup3 working.
	fmt.Println("child: this line came back through a pipe")
	// And an exit status the parent must read back exactly.
	os.Exit(7)
}
