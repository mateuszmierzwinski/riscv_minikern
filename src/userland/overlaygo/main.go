// overlaygo mounts an overlay of two directories on the netfs export and
// changes it through ordinary Go file calls, checking after each step that
// the merged view is right AND that the lower layer was never touched.
//
// Fixture (reset by the harness before a run), under /net/ovl:
//
//	lower/a.txt "lower a", lower/gone.txt "gone", lower/sub/b.txt "b"
//	upper/      empty
//
// Prints one line per step and "overlaygo: every step passed".
package main

import (
	"errors"
	"fmt"
	"os"
	"slices"
	"syscall"
)

const (
	lower  = "/net/ovl/lower"
	upper  = "/net/ovl/upper"
	target = "/ovl"
)

type step struct {
	name string
	run  func() error
}

func want(path, content string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if string(b) != content {
		return fmt.Errorf("%s holds %q, want %q", path, b, content)
	}
	return nil
}

func absent(path string) error {
	if _, err := os.Stat(path); !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("%s: %v, want not-exist", path, err)
	}
	return nil
}

func names(dir string) ([]string, error) {
	es, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	slices.Sort(out)
	return out, nil
}

func listing(dir string, wanted ...string) error {
	got, err := names(dir)
	if err != nil {
		return err
	}
	slices.Sort(wanted)
	if !slices.Equal(got, wanted) {
		return fmt.Errorf("%s lists %v, want %v", dir, got, wanted)
	}
	return nil
}

func all(errs ...error) error { return errors.Join(errs...) }

func refused(err error, want syscall.Errno, what string) error {
	if !errors.Is(err, want) {
		return fmt.Errorf("%s: %v, want %v", what, err, want)
	}
	return nil
}

func steps() []step {
	return []step{
		{"bad options refused", func() error {
			return all(
				refused(syscall.Mount("overlay", target, "overlay", 0, "lowerdir="+lower), syscall.EINVAL, "no upperdir"),
				refused(syscall.Mount("overlay", "/net/ovl", "overlay", 0, "lowerdir="+lower+",upperdir="+upper), syscall.EINVAL, "layer under target"))
		}},
		{"mount", func() error {
			return syscall.Mount("overlay", target, "overlay", 0, "lowerdir="+lower+",upperdir="+upper+",workdir=/tmp")
		}},
		{"lower files show through", func() error {
			return all(want(target+"/a.txt", "lower a"), want(target+"/sub/b.txt", "b"), listing(target, "a.txt", "gone.txt", "sub"))
		}},
		{"a write copies up and leaves lower alone", func() error {
			return all(os.WriteFile(target+"/a.txt", []byte("changed"), 0o644),
				want(target+"/a.txt", "changed"), want(upper+"/a.txt", "changed"), want(lower+"/a.txt", "lower a"))
		}},
		{"remove whites out", func() error {
			return all(os.Remove(target+"/gone.txt"), absent(target+"/gone.txt"),
				want(lower+"/gone.txt", "gone"), listing(target, "a.txt", "sub"))
		}},
		{"a new file goes up", func() error {
			return all(os.WriteFile(target+"/new.txt", []byte("new"), 0o644), want(upper+"/new.txt", "new"),
				absent(lower+"/new.txt"), listing(target, "a.txt", "new.txt", "sub"))
		}},
		{"a removed name can come back", func() error {
			return all(os.WriteFile(target+"/gone.txt", []byte("back"), 0o644), want(target+"/gone.txt", "back"),
				listing(target, "a.txt", "gone.txt", "new.txt", "sub"))
		}},
		{"mkdir in a lower directory", func() error {
			return all(os.Mkdir(target+"/sub/deeper", 0o755), listing(target+"/sub", "b.txt", "deeper"), absent(lower+"/sub/deeper"))
		}},
		{"rename a lower file", func() error {
			return all(os.Rename(target+"/sub/b.txt", target+"/b2.txt"), want(target+"/b2.txt", "b"),
				absent(target+"/sub/b.txt"), want(lower+"/sub/b.txt", "b"))
		}},
		{"rename a lower directory is EXDEV", func() error {
			return refused(os.Rename(target+"/sub", target+"/sub2"), syscall.EXDEV, "rename sub")
		}},
		{"truncate copies up", func() error {
			return all(os.Truncate(target+"/b2.txt", 0), want(target+"/b2.txt", ""), want(lower+"/sub/b.txt", "b"))
		}},
		{"umount", func() error { return all(syscall.Unmount(target, 0), absent(target+"/a.txt")) }},
	}
}

func main() {
	failed := 0
	for _, s := range steps() {
		if err := s.run(); err != nil {
			failed++
			fmt.Printf("overlaygo: %s: %v\n", s.name, err)
			continue
		}
		fmt.Printf("overlaygo: %s ok\n", s.name)
	}
	if failed == 0 {
		fmt.Println("overlaygo: every step passed")
		return
	}
	os.Exit(1)
}
