// Go's `net` package, on this kernel.
//
// This is the test that needs the most of the kernel: a descriptor layer, a
// real epoll (the runtime waits on readiness and will block for ever if the
// kernel reports none), sockets over the IPv4 stack, and non-blocking
// semantics throughout — Go opens every socket with O_NONBLOCK and drives it
// from the netpoller.
//
// Nothing here is written for this kernel. It is the `net` package as any
// program uses it.
package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"
)

func fail(what string, err error) {
	fmt.Printf("net: FAILED %s: %v\n", what, err)
	os.Exit(1)
}

// UDP both ways, through the loopback address: the simplest round trip that
// exercises bind, sendto and recvfrom with the kernel choosing the reply's
// source address.
func udpRoundTrip() {
	server, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		fail("ListenUDP", err)
	}
	defer server.Close()
	fmt.Printf("net: udp listening on %v\n", server.LocalAddr())

	done := make(chan string, 1)
	go func() {
		buf := make([]byte, 128)
		n, from, err := server.ReadFromUDP(buf)
		if err != nil {
			done <- "read error: " + err.Error()
			return
		}
		if _, err := server.WriteToUDP([]byte(strings.ToUpper(string(buf[:n]))), from); err != nil {
			done <- "write error: " + err.Error()
			return
		}
		done <- ""
	}()

	client, err := net.DialUDP("udp4", nil, server.LocalAddr().(*net.UDPAddr))
	if err != nil {
		fail("DialUDP", err)
	}
	defer client.Close()
	if _, err := client.Write([]byte("hello over udp")); err != nil {
		fail("udp write", err)
	}
	reply := make([]byte, 128)
	if err := client.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		fail("SetReadDeadline", err)
	}
	n, err := client.Read(reply)
	if err != nil {
		fail("udp read", err)
	}
	if msg := <-done; msg != "" {
		fail("udp server", fmt.Errorf("%s", msg))
	}
	fmt.Printf("net: udp round trip returned %q\n", string(reply[:n]))
}

// TCP, which needs the state machine, a listener's backlog, accept, and a
// connect that completes asynchronously — Go's dial returns EINPROGRESS and
// waits for writability, then confirms with getpeername because it does not
// trust the poller alone.
func tcpRoundTrip() {
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		fail("Listen", err)
	}
	defer listener.Close()
	fmt.Printf("net: tcp listening on %v\n", listener.Addr())

	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		buf := make([]byte, 64)
		n, _ := conn.Read(buf)
		_, _ = conn.Write([]byte("echo:" + string(buf[:n])))
	}()

	conn, err := net.DialTimeout("tcp4", listener.Addr().String(), 5*time.Second)
	if err != nil {
		fail("DialTimeout", err)
	}
	defer conn.Close()
	fmt.Printf("net: tcp connected %v -> %v\n", conn.LocalAddr(), conn.RemoteAddr())
	if _, err := conn.Write([]byte("hello over tcp")); err != nil {
		fail("tcp write", err)
	}
	// A half-close, which is what a request/response protocol does and what
	// needs `shutdown` rather than `close`.
	if tc, ok := conn.(*net.TCPConn); ok {
		if err := tc.CloseWrite(); err != nil {
			fail("CloseWrite", err)
		}
	}
	reply, err := io.ReadAll(conn)
	if err != nil {
		fail("tcp read", err)
	}
	fmt.Printf("net: tcp round trip returned %q\n", string(reply))
}

// A transfer big enough to say whether retransmissions scale with the data
// (something is actually being lost) or are a fixed cost per connection
// (something about how a connection starts or ends). The distinction is not
// visible in a 14-byte exchange, and the answer decides whether there is a
// bug or an inefficiency.
func bulkTransfer(bytes int) {
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		fail("bulk Listen", err)
	}
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		block := make([]byte, 4096)
		for i := range block {
			block[i] = byte(i)
		}
		for sent := 0; sent < bytes; sent += len(block) {
			if _, err := conn.Write(block); err != nil {
				return
			}
		}
	}()
	conn, err := net.DialTimeout("tcp4", listener.Addr().String(), 5*time.Second)
	if err != nil {
		fail("bulk Dial", err)
	}
	defer conn.Close()
	got, err := io.ReadAll(conn)
	if err != nil {
		fail("bulk read", err)
	}
	fmt.Printf("net: bulk transfer received %d of %d bytes\n", len(got), bytes)
}

func main() {
	fmt.Printf("net: %s on %s/%s\n", "go", "linux", "riscv64")
	udpRoundTrip()
	tcpRoundTrip()
	bulkTransfer(64 * 1024)
	fmt.Println("net: done")
}
