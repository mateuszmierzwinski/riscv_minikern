// sdwrite writes two 8 MiB files of distinct known content to the card,
// each with fsync, timing the writes, reads both back and compares them,
// and removes them. Prints "sdw: PASS|FAIL ...", "sdw: <MB/s>" and
// "sdw: done <n> failures".
package main

import (
	"bytes"
	"fmt"
	"os"
	"time"
)

const size = 8 << 20

func content(seed int) []byte {
	b := make([]byte, size)
	for i := range b {
		b[i] = byte(i*31 + seed*97 + i>>9)
	}
	return b
}

func write(path string, b []byte) (time.Duration, error) {
	t0 := time.Now()
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return 0, err
	}
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return time.Since(t0), err
}

func main() {
	_ = os.MkdirAll("/tmp/sdwrite", 0o755)
	failures := 0
	var want [2][]byte
	for i := range want {
		want[i] = content(i + 1)
		p := fmt.Sprintf("/tmp/sdwrite/f%d", i)
		took, err := write(p, want[i])
		if err != nil {
			failures++
			fmt.Printf("sdw: FAIL write %s: %v\n", p, err)
			continue
		}
		fmt.Printf("sdw: wrote %s in %v (%.2f MB/s)\n", p, took.Round(time.Millisecond), float64(size)/took.Seconds()/1e6)
	}
	for i := range want {
		p := fmt.Sprintf("/tmp/sdwrite/f%d", i)
		got, err := os.ReadFile(p)
		if err != nil || !bytes.Equal(got, want[i]) {
			failures++
			fmt.Printf("sdw: FAIL read back %s: %d bytes, err %v\n", p, len(got), err)
		} else {
			fmt.Printf("sdw: PASS read back %s\n", p)
		}
	}
	_ = os.RemoveAll("/tmp/sdwrite")
	fmt.Printf("sdw: done %d failures\n", failures)
}
