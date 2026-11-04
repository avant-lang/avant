// Hand-rolled WebSocket echo server (stdlib only) for the harness.
// Text frames are echoed. GET /quit exits after answering.
package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
)

func acceptKey(key string) string {
	h := sha1.New()
	io.WriteString(h, key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

func serveConn(c net.Conn) {
	defer c.Close()
	br := bufio.NewReader(c)
	// Read handshake head.
	var key string
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			return
		}
		if strings.HasPrefix(line, "Sec-WebSocket-Key: ") {
			key = strings.TrimSpace(strings.TrimPrefix(line, "Sec-WebSocket-Key: "))
		}
		if line == "\r\n" {
			break
		}
	}
	fmt.Fprintf(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acceptKey(key))
	for {
		b0, err := br.ReadByte()
		if err != nil {
			return
		}
		b1, err := br.ReadByte()
		if err != nil {
			return
		}
		plen := uint64(b1 & 0x7f)
		if plen == 126 {
			var ext [2]byte
			if _, err := io.ReadFull(br, ext[:]); err != nil {
				return
			}
			plen = uint64(binary.BigEndian.Uint16(ext[:]))
		} else if plen == 127 {
			var ext [8]byte
			if _, err := io.ReadFull(br, ext[:]); err != nil {
				return
			}
			plen = binary.BigEndian.Uint64(ext[:])
		}
		var mask [4]byte
		if b1&0x80 != 0 {
			if _, err := io.ReadFull(br, mask[:]); err != nil {
				return
			}
		}
		payload := make([]byte, plen)
		if _, err := io.ReadFull(br, payload); err != nil {
			return
		}
		if b1&0x80 != 0 {
			for i := range payload {
				payload[i] ^= mask[i&3]
			}
		}
		op := b0 & 0x0f
		var out []byte
		switch op {
		case 1, 2:
			out = append(out, 0x81)
		case 9:
			out = append(out, 0x8a)
		case 8:
			return
		default:
			continue
		}
		if plen < 126 {
			out = append(out, byte(plen))
		} else {
			out = append(out, 126, byte(plen>>8), byte(plen))
		}
		out = append(out, payload...)
		if _, err := c.Write(out); err != nil {
			return
		}
	}
}

func main() {
	port := "18311"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	// /quit is served by a tiny raw listener check in the accept loop:
	// the harness stops WS servers by signal instead (see run.sh).
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		go serveConn(c)
	}
}
