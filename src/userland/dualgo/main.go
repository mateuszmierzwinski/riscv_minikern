// A Go server the way any Go server is written — net.Listen on a port with
// no address — which, once the kernel offers AF_INET6, Go opens as one
// dual-stack socket per protocol. IPv4 clients then arrive as IPv4-mapped
// peers, so this is what proves IPv4 still works through the mapping, and
// that UDP and TCP reach the board over IPv6.
//
// It echoes, upper-cased, a UDP datagram on :7777 and a TCP line on :7778,
// printing each peer, until it has served the counts asked for on the
// command line or a minute has passed.
package main

import (
	"bufio"
	"flag"
	"fmt"
	"net"
	"os"
	"strings"
	"time"
)

var (
	udpWanted = flag.Int("udp", 2, "UDP datagrams to answer before exiting")
	tcpWanted = flag.Int("tcp", 2, "TCP connections to answer before exiting")
	lifetime  = flag.Duration("for", time.Minute, "how long to wait for them")
)

func fail(what string, err error) {
	fmt.Printf("dual: FAILED %s: %v\n", what, err)
	os.Exit(1)
}

// family names the socket family Go chose, from the address it reports.
func family(a net.Addr) string {
	if strings.HasPrefix(a.String(), "[::]") {
		return "dual-stack"
	}
	return "IPv4 only"
}

func serveUDP(pc net.PacketConn, done chan<- string) {
	buf := make([]byte, 512)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			done <- "udp read: " + err.Error()
			return
		}
		if _, err := pc.WriteTo([]byte(strings.ToUpper(string(buf[:n]))), from); err != nil {
			done <- "udp write: " + err.Error()
			return
		}
		fmt.Printf("dual: udp from %v\n", from)
		done <- ""
	}
}

func serveTCP(l net.Listener, done chan<- string) {
	for {
		c, err := l.Accept()
		if err != nil {
			done <- "accept: " + err.Error()
			return
		}
		line, err := bufio.NewReader(c).ReadString('\n')
		if err == nil {
			_, err = c.Write([]byte(strings.ToUpper(line)))
		}
		c.Close()
		if err != nil {
			done <- "tcp: " + err.Error()
			return
		}
		fmt.Printf("dual: tcp from %v\n", c.RemoteAddr())
		done <- ""
	}
}

func main() {
	flag.Parse()
	pc, err := net.ListenPacket("udp", ":7777")
	if err != nil {
		fail("ListenPacket", err)
	}
	l, err := net.Listen("tcp", ":7778")
	if err != nil {
		fail("Listen", err)
	}
	fmt.Printf("dual: udp %v (%s), tcp %v (%s)\n", pc.LocalAddr(), family(pc.LocalAddr()),
		l.Addr(), family(l.Addr()))
	udpDone, tcpDone := make(chan string), make(chan string)
	go serveUDP(pc, udpDone)
	go serveTCP(l, tcpDone)
	fmt.Println("dual: ready")
	deadline := time.After(*lifetime)
	for u, t := 0, 0; u < *udpWanted || t < *tcpWanted; {
		select {
		case e := <-udpDone:
			if e != "" {
				fail("udp", fmt.Errorf("%s", e))
			}
			u++
		case e := <-tcpDone:
			if e != "" {
				fail("tcp", fmt.Errorf("%s", e))
			}
			t++
		case <-deadline:
			fmt.Printf("dual: FAILED waiting: %d udp, %d tcp\n", u, t)
			os.Exit(1)
		}
	}
	fmt.Println("dual: done")
}
