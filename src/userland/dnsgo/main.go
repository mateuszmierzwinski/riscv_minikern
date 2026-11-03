// dnsgo resolves a name the way any Go program does (net.LookupHost, the
// pure-Go resolver) and prints "dns: PASS ..." or "dns: FAIL ...".
package main

import (
	"fmt"
	"net"
	"os"
)

func main() {
	if st, err := os.Stat("/etc/resolv.conf"); err != nil || !st.Mode().IsRegular() {
		fmt.Printf("dns: FAIL stat /etc/resolv.conf: %v\n", err)
	}
	addrs, err := net.LookupHost("pool.ntp.org")
	if err != nil || len(addrs) == 0 {
		fmt.Printf("dns: FAIL lookup: %v\n", err)
		return
	}
	fmt.Printf("dns: PASS pool.ntp.org is %v\n", addrs)
}
