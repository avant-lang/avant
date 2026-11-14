// Compiled WebSocket echo loader (stdlib only): the Python loader
// capped near 60k msg/s on this machine, so the harness counts with
// this one. N connections ping-pong text "hi" for D seconds; prints
// the total round-trips. Usage: ws_load PORT CONNS SECONDS, where
// SECONDS may have a fraction (0.3). An argument that does not parse is
// refused with a message and status 2 (Stage 29 W-FIX1f, O593: SECONDS
// was read as a whole number with its error ignored, so 0.3 was 0 and
// such a load sent no message).
package main

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

var total int64
var stop int32

func sendFrame(w io.Writer, payload []byte) error {
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	head := []byte{0x81}
	n := len(payload)
	switch {
	case n < 126:
		head = append(head, 0x80|byte(n))
	case n <= 65535:
		head = append(head, 0x80|126, byte(n>>8), byte(n))
	default:
		head = append(head, 0x80|127)
		var ext [8]byte
		binary.BigEndian.PutUint64(ext[:], uint64(n))
		head = append(head, ext[:]...)
	}
	head = append(head, mask[:]...)
	body := make([]byte, n)
	for i := range payload {
		body[i] = payload[i] ^ mask[i&3]
	}
	if _, err := w.Write(head); err != nil {
		return err
	}
	_, err := w.Write(body)
	return err
}

func recvFrame(br *bufio.Reader) (byte, error) {
	h, err := br.ReadByte()
	if err != nil {
		return 0, err
	}
	b1, err := br.ReadByte()
	if err != nil {
		return 0, err
	}
	plen := uint64(b1 & 0x7f)
	if plen == 126 {
		var ext [2]byte
		if _, err := io.ReadFull(br, ext[:]); err != nil {
			return 0, err
		}
		plen = uint64(binary.BigEndian.Uint16(ext[:]))
	} else if plen == 127 {
		var ext [8]byte
		if _, err := io.ReadFull(br, ext[:]); err != nil {
			return 0, err
		}
		plen = binary.BigEndian.Uint64(ext[:])
	}
	if b1&0x80 != 0 {
		var mask [4]byte
		if _, err := io.ReadFull(br, mask[:]); err != nil {
			return 0, err
		}
	}
	if _, err := io.CopyN(io.Discard, br, int64(plen)); err != nil {
		return 0, err
	}
	return h & 0x0f, nil
}

func worker(port string, wg *sync.WaitGroup) {
	defer wg.Done()
	conn, err := net.DialTimeout("tcp", "127.0.0.1:"+port, 10*time.Second)
	if err != nil {
		fmt.Fprintln(os.Stderr, "dial:", err)
		return
	}
	defer conn.Close()
	keyBytes := make([]byte, 16)
	if _, err := rand.Read(keyBytes); err != nil {
		return
	}
	key := base64.StdEncoding.EncodeToString(keyBytes)
	fmt.Fprintf(conn, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", key)
	br := bufio.NewReader(conn)
	status, err := br.ReadString('\n')
	if err != nil || !strings.Contains(status, "101") {
		fmt.Fprintln(os.Stderr, "handshake:", status, err)
		return
	}
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			return
		}
		if line == "\r\n" {
			break
		}
	}
	var count int64
	for atomic.LoadInt32(&stop) == 0 {
		if err := sendFrame(conn, []byte("hi")); err != nil {
			break
		}
		op, err := recvFrame(br)
		if err != nil {
			break
		}
		if op == 1 || op == 2 || op == 10 {
			count++
		}
	}
	atomic.AddInt64(&total, count)
}

func refuse(msg string) {
	fmt.Fprintf(os.Stderr, "ws_load: %s\nusage: ws_load PORT CONNS SECONDS (SECONDS may have a fraction: 0.3)\n", msg)
	os.Exit(2)
}

func main() {
	if len(os.Args) != 4 {
		refuse(fmt.Sprintf("3 arguments expected, %d given", len(os.Args)-1))
	}
	port := os.Args[1]
	if p, err := strconv.Atoi(port); err != nil || p < 1 || p > 65535 {
		refuse(fmt.Sprintf("PORT is not a port number (1 to 65535): %q", port))
	}
	conns, err := strconv.Atoi(os.Args[2])
	if err != nil || conns < 1 {
		refuse(fmt.Sprintf("CONNS is not a whole number of connections, at least 1: %q", os.Args[2]))
	}
	seconds, err := strconv.ParseFloat(os.Args[3], 64)
	if err != nil || seconds < 0 || math.IsInf(seconds, 0) || math.IsNaN(seconds) {
		refuse(fmt.Sprintf("SECONDS is not a duration in seconds, 0 or more: %q", os.Args[3]))
	}
	var wg sync.WaitGroup
	for i := 0; i < conns; i++ {
		wg.Add(1)
		go worker(port, &wg)
	}
	time.Sleep(time.Duration(seconds * float64(time.Second)))
	atomic.StoreInt32(&stop, 1)
	wg.Wait()
	fmt.Println(total)
}
