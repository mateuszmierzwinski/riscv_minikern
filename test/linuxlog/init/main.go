// init for a throwaway Linux guest: loads xfs, mounts /dev/vda with quotas,
// makes changes that log inode creates, extent-free intents and quota
// updates, then forces the log out and shuts the filesystem down without
// writing the metadata back — leaving a dirty log Linux wrote.
package main

import (
	"fmt"
	"os"
	"syscall"
	"time"
	"unsafe"
)

const xfsIocGoingdown = 0x8004587d // _IOR('X', 125, uint32)

func must(what string, err error) {
	if err != nil {
		fmt.Printf("linuxlog: %s: %v\n", what, err)
		off()
	}
}

func off() {
	syscall.Sync()
	syscall.Reboot(syscall.LINUX_REBOOT_CMD_POWER_OFF)
}

// fragmented writes a file of thousands of one-block extents (interleaved
// with a second file), then truncates it to nothing while another goroutine
// keeps forcing the log: the frees take many transaction rolls, and a
// checkpoint between an extent-free intent and its done item keeps both in
// the log rather than eliding them.
func fragmented() {
	a, err := os.Create("/mnt/frag")
	must("create frag", err)
	b, err := os.Create("/mnt/spacer")
	must("create spacer", err)
	blk := make([]byte, 4096)
	for i := 0; i < 3000; i++ {
		_, err = a.WriteAt(blk, int64(i)*4096)
		must("frag", err)
		_, err = b.WriteAt(blk, int64(i)*4096)
		must("spacer", err)
	}
	must("fsync", a.Sync())
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			select {
			case <-stop:
				return
			default:
				_ = b.Sync()
			}
		}
	}()
	must("truncate frag", a.Truncate(0))
	time.Sleep(500 * time.Millisecond)
	close(stop)
	<-done
}

func main() {
	f, err := os.Open("/xfs.ko")
	must("open xfs.ko", err)
	_, _, e := syscall.Syscall(313, f.Fd(), uintptr(unsafe.Pointer(&[]byte{0}[0])), 0) // finit_module
	if e != 0 {
		must("finit_module", e)
	}
	must("devtmpfs", syscall.Mount("devtmpfs", "/dev", "devtmpfs", 0, ""))
	must("mkdir", os.MkdirAll("/mnt", 0o755))
	must("mount", syscall.Mount("/dev/vda", "/mnt", "xfs", 0, "uquota,gquota"))
	payload := make([]byte, 64<<10)
	for i := range payload {
		payload[i] = byte(i)
	}
	must("mkdir d", os.Mkdir("/mnt/d", 0o755))
	for i := 0; i < 200; i++ {
		p := fmt.Sprintf("/mnt/d/f%03d", i)
		must("write", os.WriteFile(p, payload[:4096*(1+i%8)], 0o644))
		must("chown", os.Chown(p, 1000+i%3, 100+i%2))
	}
	for i := 0; i < 200; i += 2 {
		must("remove", os.Remove(fmt.Sprintf("/mnt/d/f%03d", i)))
	}
	time.Sleep(3 * time.Second) // the removed inodes' blocks are freed in the background (inodegc)
	fragmented()
	must("big", os.WriteFile("/mnt/big", payload, 0o644))
	must("truncate", os.Truncate("/mnt/big", 8192))
	d, err := os.Open("/mnt")
	must("open mnt", err)
	flag := uint32(1) // XFS_FSOP_GOING_FLAGS_LOGFLUSH: the log out, nothing else
	_, _, e = syscall.Syscall(syscall.SYS_IOCTL, d.Fd(), xfsIocGoingdown, uintptr(unsafe.Pointer(&flag)))
	if e != 0 {
		must("goingdown", e)
	}
	fmt.Println("linuxlog: log flushed, filesystem shut down")
	off()
}
