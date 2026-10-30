// timesgo checks that stat reports file times on the filesystems that are
// not the root XFS: a netfs file whose time the harness set on the host to
// 1700000000 must say exactly that, and a file on the FAT boot partition
// must have a plausible date rather than 1970. Prints "timesgo: every step
// passed".
package main

import (
	"fmt"
	"os"
	"time"
)

func mtime(path string) (int64, error) {
	fi, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	return fi.ModTime().Unix(), nil
}

func firstFile(dir string) (string, error) {
	es, err := os.ReadDir(dir)
	if err != nil {
		return "", err
	}
	for _, e := range es {
		if !e.IsDir() {
			return dir + "/" + e.Name(), nil
		}
	}
	return "", fmt.Errorf("%s holds no file", dir)
}

func main() {
	failed := 0
	check := func(name string, ok bool, detail string) {
		if !ok {
			failed++
			fmt.Printf("timesgo: %s: %s\n", name, detail)
			return
		}
		fmt.Printf("timesgo: %s ok (%s)\n", name, detail)
	}
	t, err := mtime("/net/stamped.txt")
	check("netfs time is the server's", err == nil && t == 1700000000, fmt.Sprintf("%d %v", t, err))
	path, err := firstFile("/boot")
	if err == nil {
		t, err = mtime(path)
	}
	lo, hi := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC).Unix(), time.Now().Unix()+86400
	check("FAT time is a date", err == nil && t > lo && t < hi, fmt.Sprintf("%s %d %v", path, t, err))
	if failed == 0 {
		fmt.Println("timesgo: every step passed")
		return
	}
	os.Exit(1)
}
