# A WebSocket server for one connection, for
# tests/cases/test_std_http_safety.av (Stage 29 W-FIX1g): it reads the
# whole handshake, however long, with a 64 KiB receive buffer and 0.3 s
# late (so a long handshake is more than the client's socket takes at
# once), answers 101, reads one text frame and answers "target of N
# bytes, message M". Usage: ws_target_server.py PORT_FILE. Ends after
# one connection, or after 60 s.
import base64
import hashlib
import os
import socket
import struct
import sys
import time

pf = sys.argv[1]
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
srv.bind(("127.0.0.1", 0))
srv.listen(4)
srv.settimeout(60)
with open(pf + ".tmp", "w") as f:
    f.write("%d\n" % srv.getsockname()[1])
os.rename(pf + ".tmp", pf)
try:
    c, _ = srv.accept()
except socket.timeout:
    sys.exit(1)
c.settimeout(10)
time.sleep(0.3)
buf = b""
try:
    while b"\r\n\r\n" not in buf:
        chunk = c.recv(1 << 16)
        if not chunk:
            break
        buf += chunk
except socket.timeout:
    pass
if b"\r\n\r\n" not in buf:
    print("handshake cut: %d bytes" % len(buf))
    sys.exit(0)
head, buf = buf.split(b"\r\n\r\n", 1)
lines = head.split(b"\r\n")
target = lines[0].split(b" ")[1]
key = [l.split(b":", 1)[1].strip() for l in lines if l.lower().startswith(b"sec-websocket-key:")][0]
acc = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
while len(buf) < 2:
    buf += c.recv(4096)
n = buf[1] & 127
while len(buf) < 6 + n:
    buf += c.recv(4096)
mask = buf[2:6]
msg = bytes(x ^ mask[i & 3] for i, x in enumerate(buf[6:6 + n]))
ans = b"target of %d bytes, message %s" % (len(target), msg)
c.sendall(struct.pack("!BB", 0x81, len(ans)) + ans)
time.sleep(0.5)
c.close()
print("answered: " + ans.decode())
