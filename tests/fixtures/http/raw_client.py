# Raw-socket client for tests/cases/test_std_http_safety.av: what a test
# needs to say byte by byte and std.http's own client cannot (pipelined
# requests in one write, a close with answers unread, a reader that
# stalls, repeated headers). Usage: raw_client.py PORT MODE [ARG]
# Prints one line, MODE=RESULT; the test compares it.
import socket
import sys
import time

port = int(sys.argv[1])
mode = sys.argv[2]
arg = sys.argv[3] if len(sys.argv) > 3 else ""


def connect():
    return socket.create_connection(("127.0.0.1", port), timeout=10)


def get(path, extra=""):
    return ("GET %s HTTP/1.1\r\nHost: x\r\n%s\r\n" % (path, extra)).encode()


def read_answers(s, want):
    """Reads `want` answers framed by Content-Length; returns their
    (status, body) pairs, fewer when the server closes first."""
    buf = b""
    out = []
    while len(out) < want:
        end = buf.find(b"\r\n\r\n")
        if end >= 0:
            head = buf[:end].decode("latin1").split("\r\n")
            length = 0
            for line in head[1:]:
                name, _, value = line.partition(":")
                if name.strip().lower() == "content-length":
                    length = int(value.strip())
            if len(buf) >= end + 4 + length:
                out.append((int(head[0].split(" ")[1]), buf[end + 4:end + 4 + length]))
                buf = buf[end + 4 + length:]
                continue
        try:
            chunk = s.recv(65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
    return out


def ping_ok():
    """A new connection gets its answer: the server is still there."""
    try:
        s = connect()
        s.sendall(get("/ping"))
        got = read_answers(s, 1)
        s.close()
        return got == [(200, b"pong")]
    except OSError:
        return False


def close_unread(count):
    # O240: `count` pipelined requests in one write, then the client
    # closes without reading one answer. The first answer meets a closed
    # socket and is reset; the next write on that connection must fail
    # quietly instead of raising SIGPIPE.
    s = connect()
    s.sendall(get("/ping") * count)
    s.close()
    time.sleep(0.1)
    return "alive" if ping_ok() else "gone"


try:
    if mode == "close_unread":
        result = close_unread(int(arg))
    else:
        result = "unknown mode"
except Exception as e:  # a failure is a result the test prints
    result = "BAD:%s:%s" % (type(e).__name__, e)
print("%s=%s" % (mode, result))
