// netfsgo exercises a netfs export from user space through ordinary Go file
// calls. It exits 0 when every step passed and prints the first that did not.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"syscall"
)

const root = "/net"

func step(name string, err error) {
	if err != nil {
		fmt.Printf("netfsgo: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("netfsgo: %s ok\n", name)
}

// chmodTo sets path's permission bits and checks a fresh stat reports them:
// the export stores modes, so the round trip is what proves it.
func chmodTo(path string, mode os.FileMode) error {
	if err := os.Chmod(path, mode); err != nil {
		return err
	}
	fi, err := os.Stat(path)
	if err == nil && fi.Mode().Perm() != mode {
		err = fmt.Errorf("mode %v after chmod %v", fi.Mode().Perm(), mode)
	}
	return err
}

// refused is nil when err is the errno wanted.
func refused(err error, want syscall.Errno) error {
	if errors.Is(err, want) {
		return nil
	}
	return fmt.Errorf("got %v, want %v", err, want)
}

func main() {
	entries, err := os.ReadDir(root)
	step("list /net", err)
	for _, e := range entries {
		fmt.Printf("netfsgo:   %s dir=%v\n", e.Name(), e.IsDir())
	}
	hello, err := os.ReadFile(filepath.Join(root, "hello.txt"))
	step("read hello.txt", err)
	fmt.Printf("netfsgo:   %q\n", hello)
	payload := bytes.Repeat([]byte("quokka-"), 20000) // 140 KB: several READ/WRITE frames
	dir := filepath.Join(root, "scratch")
	step("mkdir", os.MkdirAll(dir, 0o755))
	step("write", os.WriteFile(filepath.Join(dir, "big"), payload, 0o644))
	back, err := os.ReadFile(filepath.Join(dir, "big"))
	if err == nil && !bytes.Equal(back, payload) {
		err = fmt.Errorf("read back %d bytes, differ", len(back))
	}
	step("read back", err)
	step("rename", os.Rename(filepath.Join(dir, "big"), filepath.Join(dir, "moved")))
	fi, err := os.Stat(filepath.Join(dir, "moved"))
	if err == nil && fi.Size() != int64(len(payload)) {
		err = fmt.Errorf("size %d", fi.Size())
	}
	step("stat", err)
	step("chmod 0600", chmodTo(filepath.Join(dir, "moved"), 0o600))
	step("chmod 0644", chmodTo(filepath.Join(dir, "moved"), 0o644))
	step("setuid refused", refused(os.Chmod(filepath.Join(dir, "moved"), os.ModeSetuid|0o755), syscall.EPERM))
	step("remove", os.Remove(filepath.Join(dir, "moved")))
	_, err = os.Stat(filepath.Join(dir, "moved"))
	if os.IsNotExist(err) {
		err = nil
	} else if err == nil {
		err = fmt.Errorf("still there")
	}
	step("gone", err)
	step("mount at /srv", syscall.Mount("192.168.86.119:8081", "/srv", "netfs", 0, ""))
	again, err := os.ReadFile("/srv/hello.txt")
	if err == nil && !bytes.Equal(again, hello) {
		err = fmt.Errorf("read %q through /srv", again)
	}
	step("read through /srv", err)
	step("unmount /srv", syscall.Unmount("/srv", 0))
	_, err = os.Stat("/srv/hello.txt")
	if err == nil {
		err = fmt.Errorf("still visible after unmount")
	} else {
		err = nil
	}
	step("gone after unmount", err)
	err = syscall.Mount("x", "/srv", "ext4", 0, "")
	if err == nil {
		err = fmt.Errorf("an unknown type was mounted")
	} else {
		err = nil
	}
	step("unknown type refused", err)
	fmt.Println("netfsgo: every step passed")
}
