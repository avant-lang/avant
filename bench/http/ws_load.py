#!/usr/bin/env python3
"""Dependency-free WebSocket echo loader for the harness.

Opens N connections, does the RFC 6455 handshake, then ping-pongs
text messages ("hi") sequentially per connection for D seconds and
prints the total round-trips. Usage: ws_load.py PORT CONNS SECONDS
"""
import base64
import os
import socket
import struct
import sys
import threading
import time

TOTAL = 0
LOCK = threading.Lock()
STOP = False


def send_frame(sock, payload, opcode=1):
    data = bytearray()
    data.append(0x80 | opcode)
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        data.append(0x80 | n)
    elif n <= 65535:
        data.append(0x80 | 126)
        data += struct.pack(">H", n)
    else:
        data.append(0x80 | 127)
        data += struct.pack(">Q", n)
    data += mask
    data += bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    sock.sendall(bytes(data))


def recv_frame(sock):
    def need(n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    h = need(2)
    plen = h[1] & 0x7F
    if plen == 126:
        plen = struct.unpack(">H", need(2))[0]
    elif plen == 127:
        plen = struct.unpack(">Q", need(8))[0]
    if h[1] & 0x80:
        need(4)
    return h[0] & 0x0F, need(plen)


def worker(port):
    global TOTAL
    count = 0
    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall(
            (
                "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n" % key
            ).encode()
        )
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake closed")
            buf += chunk
        if b"101" not in buf.split(b"\r\n")[0]:
            raise ConnectionError("handshake refused: %r" % buf[:80])
        while not STOP:
            send_frame(sock, b"hi")
            op, _payload = recv_frame(sock)
            if op in (1, 2, 10):
                count += 1
    except Exception as exc:  # noqa: BLE001 - bench loader reports and ends
        print("worker error: %s" % exc, file=sys.stderr)
    finally:
        with LOCK:
            TOTAL += count


def main():
    global STOP
    port = int(sys.argv[1])
    conns = int(sys.argv[2])
    seconds = float(sys.argv[3])
    threads = [threading.Thread(target=worker, args=(port,)) for _ in range(conns)]
    start = time.time()
    for t in threads:
        t.start()
    time.sleep(seconds)
    STOP = True
    for t in threads:
        t.join(timeout=15)
    elapsed = time.time() - start
    print(TOTAL)
    print("%.1f" % elapsed, file=sys.stderr)


if __name__ == "__main__":
    main()
