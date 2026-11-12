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


BIG = 8 * 1024 * 1024


def big_body():
    letters = bytes(range(97, 123))
    return (letters * (BIG // 26 + 1))[:BIG]


def small_rcvbuf():
    # A small receive buffer, so that the kernel cannot hold a whole
    # large answer for a client that is not reading: the server's write
    # comes back short and its output queue is what is tested.
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    s.settimeout(20)
    s.connect(("127.0.0.1", port))
    return s


def slow_big():
    # P191: an 8 MiB answer to a client that starts reading late. Every
    # byte arrives, and the connection still answers the next request.
    s = small_rcvbuf()
    s.sendall(get("/big"))
    time.sleep(0.1)
    got = read_answers(s, 1)
    if got != [(200, big_body())]:
        return "cut:%s" % [(st, len(b)) for st, b in got]
    s.sendall(get("/ping"))
    after = read_answers(s, 1)
    s.close()
    return "whole,then pong" if after == [(200, b"pong")] else "whole,then %r" % after[:1]


def pile():
    # P191: 64 pipelined requests for 64 KiB answers, each filled with
    # its own digit, and a last small one; nothing is read for a while.
    # Every answer arrives whole and in request order.
    s = small_rcvbuf()
    n = 64
    s.sendall(b"".join(get("/n?%d" % k) for k in range(n)) + get("/ping"))
    time.sleep(0.2)
    got = read_answers(s, n + 1)
    s.close()
    want = [(200, bytes([48 + k % 10]) * 65536) for k in range(n)] + [(200, b"pong")]
    if got == want:
        return "65 whole,in order"
    for k in range(len(want)):
        if k >= len(got) or got[k] != want[k]:
            return "answer %d of %d wrong (%d received)" % (k, n + 1, len(got))


def close_big():
    # P191: Connection: close with an answer larger than the socket
    # takes at once: the connection closes after the whole answer.
    s = small_rcvbuf()
    s.sendall(get("/big", "Connection: close\r\n"))
    time.sleep(0.1)
    got = read_answers(s, 1)
    if got != [(200, big_body())]:
        return "cut:%s" % [(st, len(b)) for st, b in got]
    rest = s.recv(16)
    s.close()
    return "whole,then eof" if rest == b"" else "whole,then %r" % rest


def half_close():
    # P191: two pipelined large requests, then the client shuts down its
    # sending side and reads: both answers arrive whole, then the end.
    s = small_rcvbuf()
    s.sendall(get("/big") * 2)
    s.shutdown(socket.SHUT_WR)
    time.sleep(0.1)
    got = read_answers(s, 2)
    if got != [(200, big_body())] * 2:
        return "cut:%s" % [(st, len(b)) for st, b in got]
    rest = s.recv(16)
    s.close()
    return "2 whole,then eof" if rest == b"" else "2 whole,then %r" % rest


def flood():
    # P191, memory: a client that sends requests for 64 KiB answers as
    # fast as it can and reads nothing. The server stops taking requests
    # from it and stops reading it, so the client's own writes stop
    # being accepted long before 64 MiB of requests have gone out.
    s = small_rcvbuf()
    s.setblocking(False)
    chunk = get("/f") * 4096
    limit = 64 * 1024 * 1024
    sent = 0
    stalled = None
    while sent < limit:
        try:
            sent += s.send(chunk)
            stalled = None
        except BlockingIOError:
            now = time.time()
            if stalled is None:
                stalled = now
            elif now - stalled > 0.5:
                break
            time.sleep(0.01)
        except OSError:
            break
    s.close()
    time.sleep(0.1)
    if not ping_ok():
        return "gone"
    return "held back" if sent < limit // 2 else "the server took %d bytes of requests" % sent


def wait_closed(s, limit):
    """True when the server closes the connection (end or reset) within
    `limit` seconds; what arrives before that is thrown away."""
    s.settimeout(0.25)
    end = time.time() + limit
    while time.time() < end:
        try:
            if s.recv(65536) == b"":
                return True
        except socket.timeout:
            continue
        except OSError:
            return True
    return False


def idle():
    # P192: the server's timeout is an idle timeout. The test's server
    # runs with it shortened to 8 s (two of uSockets' 4-second ticks, so
    # a connection is closed between 4 and 8 s after its last activity).
    # Four connections, opened together: `busy` sends a request every
    # 0.4 s; `quiet` never sends; `once` sends one request and reads its
    # answer; `stalled` asks for 8 MiB and never reads. The server must
    # close `quiet`, `once` and `stalled` (idle, idle after activity,
    # stalled with output queued) while `busy` goes on being answered:
    # with a timeout armed once at accept all four die in the same sweep.
    busy = connect()
    quiet = connect()
    once = connect()
    stalled = small_rcvbuf()
    once.sendall(get("/ping"))
    if read_answers(once, 1) != [(200, b"pong")]:
        return "once: no answer"
    stalled.sendall(get("/big"))
    quiet.settimeout(0.4)
    start = time.time()
    closed = False
    while time.time() - start < 12:
        busy.sendall(get("/ping"))
        if read_answers(busy, 1) != [(200, b"pong")]:
            return "busy closed after %.0f s" % (time.time() - start)
        try:
            if quiet.recv(16) == b"":
                closed = True
                break
        except socket.timeout:
            pass
        except OSError:
            closed = True
            break
    if not closed:
        return "quiet still open after 12 s"
    waited = time.time() - start
    # The sweep that closed `quiet` has run: `busy` must have survived it.
    time.sleep(0.2)
    try:
        busy.sendall(get("/ping"))
        alive = read_answers(busy, 1) == [(200, b"pong")]
    except OSError:
        alive = False
    if not alive:
        return "busy closed with the idle ones after %.0f s" % waited
    # `once` and `stalled` were last active just before or after `quiet`
    # was accepted: the same sweep or the next one (4 s later) closes them.
    if not wait_closed(once, 6):
        return "once still open"
    if not wait_closed(stalled, 6):
        return "stalled still open"
    busy.close()
    return "idle closed,busy alive"


def exchange(raw):
    """Sends raw on a new connection; returns (status of the first
    answer or 0, everything received until the server closes or 1 s
    passes, whether the server closed)."""
    s = connect()
    s.sendall(raw)
    s.settimeout(1)
    data = b""
    closed = False
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                closed = True
                break
            data += chunk
    except socket.timeout:
        pass
    except OSError:
        closed = True
    s.close()
    status = int(data.split(b" ", 2)[1]) if data.startswith(b"HTTP/1.1 ") else 0
    return status, data, closed


def lengths():
    # P193: Content-Length lines that disagree are two readings of where
    # the body ends (request smuggling): 400 and close, the body's tail
    # never runs as a request. Lines repeating one number are one value.
    hidden = get("/hidden")
    post = b"POST /echo HTTP/1.1\r\nHost: x\r\n"
    out = []

    def case(name, headers, body, want_status, want_closed):
        status, data, closed = exchange(post + headers + b"\r\n" + body)
        ok = status == want_status and closed == want_closed and b"smuggled" not in data
        out.append("%s:%s" % (name, status if ok else "BAD(%d,%s,%r)" % (status, closed, data[-40:])))

    tail = b"abc" + hidden
    case("differ", b"Content-Length: 3\r\nContent-Length: %d\r\n" % len(tail), tail, 400, True)
    case("differ-reversed", b"Content-Length: %d\r\nContent-Length: 3\r\n" % len(tail), tail, 400, True)
    case("third-differs", b"Content-Length: 3\r\nContent-Length: 3\r\nContent-Length: %d\r\n" % len(tail), tail, 400, True)
    case("case", b"content-length: 3\r\nCONTENT-LENGTH: %d\r\n" % len(tail), tail, 400, True)
    case("folded", b"Content-Length: 3\r\n 7\r\n", tail, 400, True)
    case("list", b"Content-Length: 3, 3\r\n", b"abc", 400, True)
    s = connect()
    s.sendall(post + b"Content-Length: 3\r\nContent-Length: 3\r\n\r\nabc" + get("/ping"))
    got = read_answers(s, 2)
    s.close()
    out.append("same:%s" % (200 if got == [(200, b"abc"), (200, b"pong")] else "BAD(%r)" % got))
    return ",".join(out)


try:
    if mode == "close_unread":
        result = close_unread(int(arg))
    elif mode == "slow_big":
        result = slow_big()
    elif mode == "pile":
        result = pile()
    elif mode == "close_big":
        result = close_big()
    elif mode == "half_close":
        result = half_close()
    elif mode == "flood":
        result = flood()
    elif mode == "idle":
        result = idle()
    elif mode == "lengths":
        result = lengths()
    else:
        result = "unknown mode"
except Exception as e:  # a failure is a result the test prints
    result = "BAD:%s:%s" % (type(e).__name__, e)
print("%s=%s" % (mode, result))
