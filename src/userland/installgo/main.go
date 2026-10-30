// installgo installs a system onto the disk the kernel booted with: the tree
// exported at /net/root (files, directories, relative symlinks and their
// modes) is copied into /, and /net/minikern.bin into /boot/minikern.bin,
// so the board can then boot from its own card with nothing on the network.
//
// A file it would overwrite is kept beside itself as <name>.prev the first
// time, so what was on the card before stays recoverable — except one
// already identical (skipped, so a re-run is cheap) and the paths given as
// arguments (an earlier run's partial copies, replaced outright). Prints a
// summary and "installgo: installed" when everything was copied.
package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const (
	source = "/net/root"
	kernel = "/net/minikern.bin"
)

type counts struct{ files, dirs, links, kept, same int }

// replace are paths whose existing copy is not kept (os.Args).
var replace = map[string]bool{}

// identical reports whether dst already holds exactly src's bytes.
func identical(src, dst string) bool {
	a, err := os.ReadFile(src)
	if err != nil {
		return false
	}
	b, err := os.ReadFile(dst)
	return err == nil && bytes.Equal(a, b)
}

func keepPrevious(dst string, c *counts) error {
	if _, err := os.Lstat(dst); errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if replace[dst] {
		return os.Remove(dst)
	}
	if _, err := os.Lstat(dst + ".prev"); err == nil {
		return os.Remove(dst) // an earlier install already kept the original
	}
	c.kept++
	return os.Rename(dst, dst+".prev")
}

func copyFile(src, dst string, mode fs.FileMode, c *counts) error {
	if identical(src, dst) {
		c.same++
		return nil
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := keepPrevious(dst, c); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, mode.Perm())
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	if err := out.Close(); err != nil {
		return err
	}
	c.files++
	if strings.HasPrefix(dst, "/boot/") {
		return nil // FAT: no permission bits to set, and chmod is refused
	}
	return os.Chmod(dst, mode.Perm())
}

func copyLink(src, dst string, c *counts) error {
	target, err := os.Readlink(src)
	if err != nil {
		return err
	}
	if err := keepPrevious(dst, c); err != nil {
		return err
	}
	c.links++
	return os.Symlink(target, dst)
}

func install(c *counts) error {
	return filepath.WalkDir(source, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel := strings.TrimPrefix(path, source)
		dst := "/" + strings.TrimPrefix(rel, "/")
		switch {
		case d.IsDir():
			c.dirs++
			return os.MkdirAll(dst, 0o755)
		case d.Type()&fs.ModeSymlink != 0:
			return copyLink(path, dst, c)
		default:
			fi, err := d.Info()
			if err != nil {
				return err
			}
			return copyFile(path, dst, fi.Mode(), c)
		}
	})
}

func main() {
	var c counts
	for _, p := range os.Args[1:] {
		replace[p] = true
	}
	if err := install(&c); err != nil {
		fmt.Println("installgo: copying the tree:", err)
		os.Exit(1)
	}
	if err := copyFile(kernel, "/boot/minikern.bin", 0o644, &c); err != nil {
		fmt.Println("installgo: the kernel:", err)
		os.Exit(1)
	}
	fmt.Printf("installgo: %d files, %d already in place, %d directories, %d links; %d earlier files kept as .prev\n",
		c.files, c.same, c.dirs, c.links, c.kept)
	fmt.Println("installgo: installed")
}
