// procgo reads the synthetic files under /proc, /sys and /dev through
// ordinary Go calls. It exits 0 when every step passed.
package main

import (
	"fmt"
	"io"
	"os"
	"strings"
)

func step(name string, err error) {
	if err != nil {
		fmt.Printf("procgo: %s: %v\n", name, err)
		os.Exit(1)
	}
	fmt.Printf("procgo: %s ok\n", name)
}

// readHas reads path and checks it contains want.
func readHas(path, want string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	first, _, _ := strings.Cut(string(b), "\n")
	fmt.Printf("procgo:   %s: %q\n", path, first)
	if !strings.Contains(string(b), want) {
		return fmt.Errorf("no %q in %q", want, b)
	}
	return nil
}

func lists(dir string, want ...string) error {
	es, err := os.ReadDir(dir)
	if err != nil {
		return err
	}
	seen := map[string]bool{}
	for _, e := range es {
		seen[e.Name()] = true
	}
	for _, w := range want {
		if !seen[w] {
			return fmt.Errorf("%s lacks %s (has %v)", dir, w, seen)
		}
	}
	return nil
}

func main() {
	step("maps", readHas("/proc/self/maps", "[stack]"))
	step("status", readHas("/proc/self/status", "Pid:"))
	step("uptime", readHas("/proc/uptime", "."))
	step("loadavg", readHas("/proc/loadavg", "/"))
	step("cpuinfo", readHas("/proc/cpuinfo", "processor"))
	step("mounts", readHas("/proc/mounts", "/proc"))
	step("version", readHas("/proc/version", "minikern"))
	step("thp enabled", readHas("/sys/kernel/mm/transparent_hugepage/enabled", "always"))
	step("thp size", readHas("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "2097152"))
	step("cpu online", readHas("/sys/devices/system/cpu/online", "0"))
	step("list /proc", lists("/proc", "self", "meminfo", "cpuinfo", "uptime"))
	step("list /proc/self", lists("/proc/self", "maps", "status"))
	step("list /sys/kernel/mm", lists("/sys/kernel/mm", "transparent_hugepage"))
	step("list /dev", lists("/dev", "null", "zero", "urandom", "virtual"))
	fi, err := os.Stat("/dev/null")
	if err == nil && fi.Mode()&os.ModeCharDevice == 0 {
		err = fmt.Errorf("mode %v", fi.Mode())
	}
	step("stat /dev/null", err)
	f, err := os.Open("/dev/urandom")
	if err == nil {
		b := make([]byte, 32)
		_, err = io.ReadFull(f, b)
		f.Close()
		zero := true
		for _, c := range b {
			zero = zero && c == 0
		}
		if err == nil && zero {
			err = fmt.Errorf("32 zero bytes")
		}
	}
	step("read /dev/urandom", err)
	err = os.WriteFile("/proc/version", []byte("x"), 0o644)
	if err == nil {
		err = fmt.Errorf("a write to /proc succeeded")
	} else {
		err = nil
	}
	step("write to /proc refused", err)
	step("stat cpu0", readHas("/proc/stat", "cpu0 "))
	step("stat ctxt", readHas("/proc/stat", "\nctxt "))
	step("stat processes", readHas("/proc/stat", "\nprocesses "))
	pid := fmt.Sprint(os.Getpid())
	step("list /proc has my pid", lists("/proc", pid))
	step("per-pid status", readHas("/proc/"+pid+"/status", "Pid:\t"+pid+"\n"))
	step("list /proc/<pid>", lists("/proc/"+pid, "maps", "status"))
	_, err = os.Stat("/proc/99999/status")
	if err == nil {
		err = fmt.Errorf("a dead pid answered")
	} else {
		err = nil
	}
	step("dead pid absent", err)
	uid := fmt.Sprint(os.Getuid())
	step("list /proc/ps", lists("/proc/ps", "by-pid", "by-user"))
	step("list by-pid", lists("/proc/ps/by-pid", pid))
	step("list by-user", lists("/proc/ps/by-user", uid))
	step("list by-user/<uid>", lists("/proc/ps/by-user/"+uid, pid))
	base := "/proc/ps/by-pid/" + pid + "/"
	step("list by-pid/<pid>", lists(base, "pid", "memory_bytes", "time_alive", "niceness", "fds_opened"))
	step("ps pid", readHas(base+"pid", pid+"\n"))
	step("ps memory_bytes", readHas(base+"memory_bytes", "\n"))
	step("ps time_alive", readHas(base+"time_alive", "\n"))
	step("ps niceness", readHas(base+"niceness", "0\n"))
	step("ps fds_opened", readHas(base+"fds_opened", "\n"))
	step("ps state", readHas(base+"state", "R\n"))
	step("by-user path", readHas("/proc/ps/by-user/"+uid+"/"+pid+"/uid", uid+"\n"))
	_, err = os.Stat("/proc/ps/by-user/4242/" + pid + "/pid")
	if err == nil {
		err = fmt.Errorf("the wrong uid answered")
	} else {
		err = nil
	}
	step("wrong uid absent", err)
	fmt.Println("procgo: every step passed")
}
