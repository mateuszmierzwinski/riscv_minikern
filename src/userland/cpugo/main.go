// cpugo checks /proc/stat's user/system split: a second spent spinning in
// user mode must show up as user time, a second spent in syscalls as system
// time, and the per-hart lines must add up to the total. The spin is not
// pure user time: each clock read is a syscall here (no vDSO) and Go's
// background threads run on the other harts, so it needs twice as much
// user as system, not more. Prints one line
// per check and "cpugo: every step passed".
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// ticks are one cpu line's user, system and idle fields.
type ticks struct{ user, system, idle uint64 }

func read() (map[string]ticks, error) {
	b, err := os.ReadFile("/proc/stat")
	if err != nil {
		return nil, err
	}
	out := map[string]ticks{}
	for _, line := range strings.Split(string(b), "\n") {
		f := strings.Fields(line)
		if len(f) < 5 || !strings.HasPrefix(f[0], "cpu") {
			continue
		}
		u, _ := strconv.ParseUint(f[1], 10, 64)
		s, _ := strconv.ParseUint(f[3], 10, 64)
		i, _ := strconv.ParseUint(f[4], 10, 64)
		out[f[0]] = ticks{u, s, i}
	}
	return out, nil
}

func spinUser(d time.Duration) {
	end := time.Now().Add(d)
	x := 1
	for time.Now().Before(end) {
		for i := 0; i < 100000; i++ {
			x = x*31 + i
		}
	}
	_ = x
}

func spinKernel(d time.Duration) {
	end := time.Now().Add(d)
	var st syscall.Stat_t
	for time.Now().Before(end) {
		for i := 0; i < 50; i++ {
			_ = syscall.Stat("/proc/stat", &st)
		}
	}
}

func delta(before, after map[string]ticks) ticks {
	b, a := before["cpu"], after["cpu"]
	return ticks{a.user - b.user, a.system - b.system, a.idle - b.idle}
}

func check(name string, ok bool, detail string) int {
	if ok {
		fmt.Printf("cpugo: %s ok (%s)\n", name, detail)
		return 0
	}
	fmt.Printf("cpugo: %s FAILED (%s)\n", name, detail)
	return 1
}

func main() {
	failed := 0
	t0, err := read()
	if err != nil {
		fmt.Println("cpugo:", err)
		os.Exit(1)
	}
	spinUser(time.Second)
	t1, _ := read()
	spinKernel(time.Second)
	t2, _ := read()
	u, k := delta(t0, t1), delta(t1, t2)
	failed += check("user spin is user time", u.user >= 80 && u.user > 2*u.system, fmt.Sprintf("user %d system %d", u.user, u.system))
	failed += check("syscalls are system time", k.system >= 30 && k.system > k.user, fmt.Sprintf("user %d system %d", k.user, k.system))
	var sum ticks
	for name, v := range t2 {
		if name != "cpu" {
			sum.user += v.user
			sum.system += v.system
			sum.idle += v.idle
		}
	}
	failed += check("harts add up to the total", sum == t2["cpu"], fmt.Sprintf("%+v vs %+v", sum, t2["cpu"]))
	if failed == 0 {
		fmt.Println("cpugo: every step passed")
		return
	}
	os.Exit(1)
}
