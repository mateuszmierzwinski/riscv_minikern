// volgo exercises what fssd needs from the kernel's storage, printing one
// "volgo: PASS|FAIL <what>" line per check and "volgo: done <n> failures".
//
// The volume it mounts is mounted read-only and never written: the board's
// NVMe drive is the operator's data.
package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"
	"syscall"
)

const target = "/volumes/volgo"

var failures int

func check(what string, err error) {
	if err != nil {
		failures++
		fmt.Printf("volgo: FAIL %s: %v\n", what, err)
		return
	}
	fmt.Printf("volgo: PASS %s\n", what)
}

func want(what string, err, expected error) {
	if !errors.Is(err, expected) {
		check(what, fmt.Errorf("got %v, want %v", err, expected))
		return
	}
	check(what, nil)
}

type volume struct{ name, fstype, label, mountpoint, flags string }

func volumes() ([]volume, error) {
	f, err := os.Open("/proc/volumes")
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var out []volume
	s := bufio.NewScanner(f)
	for s.Scan() {
		fmt.Printf("volgo: /proc/volumes: %s\n", s.Text())
		p := strings.Fields(s.Text())
		if len(p) != 7 {
			return nil, fmt.Errorf("line %q has %d fields", s.Text(), len(p))
		}
		out = append(out, volume{p[0], p[1], p[2], p[5], p[6]})
	}
	return out, s.Err()
}

// unmounted is a volume nobody mounted whose filesystem the kernel reads.
func unmounted(vs []volume) (volume, bool) {
	for _, v := range vs {
		if v.mountpoint == "-" && v.fstype != "unknown" && !strings.Contains(v.flags, "boot") {
			return v, true
		}
	}
	return volume{}, false
}

func mountCycle(v volume) {
	dev := "/dev/" + v.name
	check("mount "+dev+" read-only", syscall.Mount(dev, target, v.fstype, syscall.MS_RDONLY, ""))
	want("a second mount of it is EBUSY", syscall.Mount(dev, "/volumes/volgo2", v.fstype, syscall.MS_RDONLY, ""), syscall.EBUSY)
	entries, err := os.ReadDir(target)
	check(fmt.Sprintf("list it (%d entries)", len(entries)), err)
	for _, e := range entries {
		info, ierr := e.Info()
		fmt.Printf("volgo: entry %q dir=%v type=%v info=%v err=%v\n", e.Name(), e.IsDir(), e.Type(), info != nil && info.Mode().IsRegular(), ierr)
		_, lerr := os.Lstat(target + "/" + e.Name())
		fmt.Printf("volgo: lstat %q: %v\n", e.Name(), lerr)
	}
	vs, _ := volumes()
	for _, w := range vs {
		if w.name == v.name && (w.mountpoint != target || !strings.HasPrefix(w.flags, "ro")) {
			check("/proc/volumes shows it", fmt.Errorf("%+v", w))
		}
	}
	readOneFile(entries)
	d, err := os.Open(target)
	check("open its root directory", err)
	want("umount while it is open is EBUSY", syscall.Unmount(target, 0), syscall.EBUSY)
	if d != nil {
		_ = d.Close()
	}
	check("umount once closed", syscall.Unmount(target, 0))
	check("mount it again (the slot is reused)", syscall.Mount(dev, target, "auto", syscall.MS_RDONLY, ""))
	check("umount again", syscall.Unmount(target, 0))
}

// readOneFile opens and reads the first regular file on the mounted
// volume: a file on a second volume could be listed but not opened.
func readOneFile(entries []os.DirEntry) {
	for _, e := range entries {
		if !e.Type().IsRegular() {
			continue
		}
		f, err := os.Open(target + "/" + e.Name())
		check("open a file on it for reading", err)
		if err == nil {
			n, rerr := io.Copy(io.Discard, io.LimitReader(f, 1<<20))
			check(fmt.Sprintf("read it (%d bytes)", n), rerr)
			_ = f.Close()
		}
		return
	}
}

func refusals(vs []volume) {
	want("no such volume is ENOENT", syscall.Mount("/dev/nosuch9", target, "xfs", syscall.MS_RDONLY, ""), syscall.ENOENT)
	if len(vs) > 0 {
		want("a target outside /volumes is EINVAL", syscall.Mount("/dev/"+vs[0].name, "/tmp/x", "auto", syscall.MS_RDONLY, ""), syscall.EINVAL)
		want("a nested target is EINVAL", syscall.Mount("/dev/"+vs[0].name, "/volumes/a/b", "auto", syscall.MS_RDONLY, ""), syscall.EINVAL)
		want("the wrong type is EINVAL", syscall.Mount("/dev/"+vs[0].name, target, "ext4", syscall.MS_RDONLY, ""), syscall.EINVAL)
	}
	want("the root cannot be unmounted", syscall.Unmount("/", 0), syscall.EBUSY)
	want("umount of nothing is EINVAL", syscall.Unmount("/volumes/nothing", 0), syscall.EINVAL)
}

// streaming reads a file larger than the whole-file limit, from the start
// and at an offset, and compares its digest with the one beside it.
func streaming(path string) {
	f, err := os.Open(path)
	check("open a 20 MiB file for reading", err)
	if err != nil {
		return
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	check(fmt.Sprintf("read it through (%d bytes)", n), err)
	sum, _ := os.ReadFile(path + ".sha256")
	if got := hex.EncodeToString(h.Sum(nil)); !strings.HasPrefix(string(sum), got) {
		check("its digest", fmt.Errorf("got %s, want %s", got, strings.TrimSpace(string(sum))))
	} else {
		check("its digest", nil)
	}
	end, err := f.Seek(0, io.SeekEnd)
	check(fmt.Sprintf("seek to its end (%d)", end), err)
	buf := make([]byte, 4096)
	_, err = f.ReadAt(buf, end-4096)
	check("read its last page at an offset", err)
	_, err = f.Write([]byte("x"))
	check("a write to it is refused", map[bool]error{true: nil, false: errors.New("accepted")}[err != nil])
}

func longPath(dir string) {
	long := dir + "/" + strings.Repeat("d", 60) + "/" + strings.Repeat("e", 60) + "/" + strings.Repeat("f", 40) + ".txt"
	b, err := os.ReadFile(long)
	check(fmt.Sprintf("read a %d-byte path", len(long)), err)
	if err == nil && strings.TrimSpace(string(b)) != "long" {
		check("its content", fmt.Errorf("%q", b))
	}
}

// modes checks a create's mode is honoured (less the umask) and that
// File.Chmod (fchmod) works — fssd's key file depends on both.
func modes(path string) {
	_ = os.MkdirAll("/tmp", 0o1777)
	_ = os.Remove(path)
	syscall.Umask(0o022)
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
	check("create a file 0600", err)
	if err != nil {
		return
	}
	st, _ := f.Stat()
	if st == nil || st.Mode().Perm() != 0o600 {
		check("it is 0600", fmt.Errorf("mode %v", st.Mode().Perm()))
	} else {
		check("it is 0600", nil)
	}
	check("fchmod it to 0640", f.Chmod(0o640))
	st, _ = os.Stat(path)
	if st == nil || st.Mode().Perm() != 0o640 {
		check("it is 0640", fmt.Errorf("mode %v", st.Mode().Perm()))
	} else {
		check("it is 0640", nil)
	}
	_ = f.Close()
	g, err := os.OpenFile(path+"2", os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o666)
	check("create one 0666 under umask 022", err)
	if err == nil {
		st, _ = g.Stat()
		_ = g.Close()
		if st.Mode().Perm() != 0o644 {
			check("it is 0644", fmt.Errorf("mode %v", st.Mode().Perm()))
		} else {
			check("it is 0644", nil)
		}
	}
	_ = os.Remove(path)
	_ = os.Remove(path + "2")
}

// removeTree checks os.RemoveAll empties and removes a tree: it recurses
// only when unlinkat on a directory answers EISDIR.
func removeTree(base string) {
	_ = os.MkdirAll(base+"/a/b", 0o755)
	for _, f := range []string{"/x", "/a/y", "/a/b/z"} {
		_ = os.WriteFile(base+f, []byte("1"), 0o644)
	}
	check("os.RemoveAll of a tree", os.RemoveAll(base))
	if _, err := os.Stat(base); !os.IsNotExist(err) {
		check("the tree is gone", fmt.Errorf("stat: %v", err))
	} else {
		check("the tree is gone", nil)
	}
	want("unlinkat of a file with AT_REMOVEDIR is ENOTDIR", func() error {
		_ = os.WriteFile(base+".f", []byte("1"), 0o644)
		defer os.Remove(base + ".f")
		return syscall.Rmdir(base + ".f")
	}(), syscall.ENOTDIR)
}

func main() {
	vs, err := volumes()
	check(fmt.Sprintf("/proc/volumes (%d volumes)", len(vs)), err)
	if v, ok := unmounted(vs); ok {
		mountCycle(v)
	} else {
		fmt.Println("volgo: SKIP no unmounted volume to mount")
	}
	refusals(vs)
	streaming("/net/volgo/big.bin")
	longPath("/net/volgo")
	modes("/tmp/volgo-mode")
	removeTree("/tmp/volgo-tree")
	m, _ := os.ReadFile("/proc/mounts")
	fmt.Printf("volgo: /proc/mounts:\n%s", m)
	fmt.Printf("volgo: done %d failures\n", failures)
}
