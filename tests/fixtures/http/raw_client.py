# Raw-socket client for tests/cases/test_std_http_safety.av: what a test
# needs to say byte by byte and std.http's own client cannot (pipelined
# requests in one write, a close with answers unread, a reader that
# stalls, repeated headers). Usage: raw_client.py PORT MODE [ARG]
# Prints one line, MODE=RESULT; the test compares it.
import select
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
    # `count` pipelined requests in one write, then the client
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
    # An 8 MiB answer to a client that starts reading late. Every
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
    # 64 pipelined requests for 64 KiB answers, each filled with
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
    # Connection: close with an answer larger than the socket
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
    # Two pipelined large requests, then the client shuts down its
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
    # Memory: a client that sends requests for 64 KiB answers as
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
    # The server's timeout is an idle timeout. The test's server
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
    # Content-Length lines that disagree are two readings of where
    # the body ends (request smuggling): 400 and close, the body's tail
    # never runs as a request. Repeats are refused too, identical or not,
    # in both spellings of the list.
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
    case("same", b"Content-Length: 3\r\nContent-Length: 3\r\n", tail, 400, True)
    case("same-list", b"Content-Length: %d, %d\r\n" % (len(tail), len(tail)), tail, 400, True)
    s = connect()
    s.sendall(post + b"Content-Length: 3\r\n\r\nabc" + get("/ping"))
    got = read_answers(s, 2)
    s.close()
    out.append("one:%s" % (200 if got == [(200, b"abc"), (200, b"pong")] else "BAD(%r)" % got))
    return ",".join(out)


def hosts():
    # RFC 9112 3.2: an HTTP/1.1 request without Host, and a
    # request of any version with two Host lines (identical or not, in
    # any case) or a folded one, are answered 400 and the connection is
    # closed: a request behind it is never answered. HTTP/1.0 may omit
    # Host; an empty Host is valid.
    hidden = get("/hidden")
    out = []

    def case(name, raw, want_status, want_closed):
        status, data, closed = exchange(raw)
        ok = status == want_status and closed == want_closed and b"smuggled" not in data
        out.append("%s:%s" % (name, status if ok else "BAD(%d,%s,%r)" % (status, closed, data[-40:])))

    case("none", b"GET /ping HTTP/1.1\r\n\r\n" + hidden, 400, True)
    case("two", b"GET /ping HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n" + hidden, 400, True)
    case("same", b"GET /ping HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n" + hidden, 400, True)
    case("case", b"GET /ping HTTP/1.1\r\nhost: x\r\nHOST: y\r\n\r\n" + hidden, 400, True)
    case("folded", b"GET /ping HTTP/1.1\r\nHost: x\r\n y\r\n\r\n" + hidden, 400, True)
    case("ws-none", b"GET /ping HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400, True)
    case("1.0-two", b"GET /ping HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", 400, True)
    case("1.0-none", b"GET /ping HTTP/1.0\r\n\r\n", 200, True)
    case("empty", b"GET /ping HTTP/1.1\r\nHost:\r\nConnection: close\r\n\r\n", 200, True)
    # The value is uri-host [ ":" port ] (RFC
    # 9112 3.2, RFC 3986): anything else is 400 and close.
    for name, value in [("space", b"a b"), ("at", b"user@a"), ("path", b"a/b"), ("query", b"a?b"),
                        ("colons", b"a:1:2"), ("port-alpha", b"a:8x"), ("pct-bad", b"a%4"),
                        ("pct-hex", b"a%zz"), ("v6-open", b"[::1"), ("v6-empty", b"[]"),
                        ("v6-tail", b"[::1]x"), ("v6-space", b"[::1 ]"), ("quote", b"a\"b"),
                        ("high", b"\xc3\xa9.example")]:
        case("bad-" + name, b"GET /ping HTTP/1.1\r\nHost: " + value + b"\r\n\r\n" + hidden, 400, True)
    for name, value in [("name", b"example.com"), ("port", b"example.com:8080"), ("v4", b"127.0.0.1:80"),
                        ("v6", b"[::1]:8080"), ("v6-bare", b"[2001:db8::7]"), ("pct", b"a%41b"),
                        ("subdelims", b"a!$&'()*+,;=-._~b"), ("port-empty", b"a:")]:
        case("ok-" + name, b"GET /ping HTTP/1.1\r\nHost: " + value + b"\r\nConnection: close\r\n\r\n", 200, True)
    s = connect()
    s.sendall(get("/ping") + get("/ping"))
    got = read_answers(s, 2)
    s.close()
    out.append("one:%s" % (200 if got == [(200, b"pong"), (200, b"pong")] else "BAD(%r)" % got))
    return ",".join(out)


def header_split(port_file):
    # tests/fixtures/http/header_split.av writes its port to
    # port_file and answers GET /h with a header its handler sets. What
    # reaches the wire: "nothing" when the server sent no byte, else the
    # answers' count, the first one's status and its header lines.
    global port
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            text = open(port_file).read()
            if text.endswith("\n"):
                port = int(text)
                break
        except OSError:
            pass
        time.sleep(0.02)
    else:
        return "no port"
    s = connect()
    s.sendall(b"GET /h HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    s.settimeout(5)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    s.close()
    if not data:
        return "nothing"
    answers = data.count(b"HTTP/1.1 ")
    lines = data.split(b"\r\n\r\n", 1)[0].split(b"\r\n")
    status = lines[0].split(b" ")[1].decode("latin1") if b" " in lines[0] else "?"
    head = lines[1:]
    try:
        s = connect()
        s.sendall(get("/__stop"))
        read_answers(s, 1)
        s.close()
    except OSError:
        pass
    # Each header line with a byte that is not printable ASCII escaped.
    return "%d answer(s) %s %s" % (answers, status, "|".join(h.decode("latin1").encode("unicode_escape").decode("ascii") for h in head))


def ws_frame(s, op, payload):
    # One masked client frame.
    import struct
    m = b"\x01\x02\x03\x04"
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | op, 0x80 | n)
    elif n <= 65535:
        head = struct.pack("!BBH", 0x80 | op, 0x80 | 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | op, 0x80 | 127, n)
    s.sendall(head + m + bytes(x ^ m[i & 3] for i, x in enumerate(payload)))


def ws_read_frame(s, buf):
    # One server frame: (opcode, payload, the bytes after it).
    import struct

    def need(n):
        nonlocal buf
        while len(buf) < n:
            chunk = s.recv(1 << 20)
            if not chunk:
                raise EOFError("closed after %d of %d bytes" % (len(buf), n))
            buf += chunk

    need(2)
    op, n, off = buf[0] & 15, buf[1] & 127, 2
    if n == 126:
        need(4)
        n, off = struct.unpack("!H", buf[2:4])[0], 4
    elif n == 127:
        need(10)
        n, off = struct.unpack("!Q", buf[2:10])[0], 10
    need(off + n)
    return op, buf[off:off + n], buf[off + n:]


def budget():
    # The test's server has a body budget of 1000 bytes. A body of 600
    # waiting for its bytes holds 600 of it; a second one of 600 is
    # answered 503 and closed; once the first is complete (200) a third
    # of 600 fits again.
    post = b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 600\r\n\r\n"
    first = connect()
    first.sendall(post + b"a" * 100)
    time.sleep(0.2)
    status, data, closed = exchange(post + b"b" * 100)
    second = "%d %s" % (status, "closed" if closed else "open")
    first.sendall(b"a" * 500)
    got = read_answers(first, 1)
    first.close()
    third = connect()
    third.sendall(post)
    time.sleep(0.2)
    third.sendall(b"c" * 600)
    got3 = read_answers(third, 1)
    third.close()
    return "second:%s,first:%s,third:%s" % (
        second, got[0][0] if got else 0, got3[0][0] if got3 else 0)


def ceiling(port_file):
    # tests/fixtures/http/handler_trap.av with a memory ceiling of 1
    # byte: once its sweep has read the memory (within a second), every
    # new request is answered 503 and closed.
    if not wait_port(port_file):
        return "no port"
    time.sleep(1.5)
    status, data, closed = exchange(get("/ping"))
    return "%d %s" % (status, "closed" if closed else "open")


def conn_limit():
    # The test's server keeps 3 connections. a, b and c each have an
    # answer and wait idle; a fourth, d, closes the longest idle (a,
    # the oldest of equals) and is answered, as b and c still are.
    # Then b, c and d each hold half a head (busy, not idle): a fifth is
    # closed at once.
    def one(sock):
        sock.sendall(get("/ping"))
        got = read_answers(sock, 1)
        return got == [(200, b"pong")]
    conns = []
    for _ in range(3):
        c = connect()
        if not one(c):
            return "no answer"
        conns.append(c)
        time.sleep(0.05)
    d = connect()
    d_ok = one(d)
    a_closed = wait_closed(conns[0], 2)
    b_ok, c_ok = one(conns[1]), one(conns[2])
    for c in (conns[1], conns[2], d):
        c.sendall(b"GET /ping HTTP/1.1\r\n")
    time.sleep(0.1)
    e = connect()
    e_closed = wait_closed(e, 2)
    return "d:%s,a closed:%s,b:%s,c:%s,e closed:%s" % (d_ok, a_closed, b_ok, c_ok, e_closed)


def ws_silent():
    # A WebSocket client that never answers the server's close frame and
    # never closes its end: the server's close waits for it until its
    # shutdown deadline. It reads until the server's end, then holds the
    # socket 3 s more.
    s, buf = ws_open("/ws")
    s.settimeout(10)
    start = time.time()
    ended = False
    try:
        while not ended and time.time() - start < 10:
            ended = not s.recv(4096)
    except OSError:
        ended = True
    if not ended:
        return "still open"
    time.sleep(3)
    s.close()
    return "the server ended its side"


def ws_silent_probe(port_file):
    # ws_silent against a fixture that wrote its port to port_file, with
    # the wall clock of the server's end printed (a probe's mode).
    if not wait_port(port_file):
        return "no port"
    r = ws_silent()
    return "%s at %d" % (r, int(time.time() * 1000))


def wait_port(port_file):
    """Reads the port a fixture server wrote to port_file (20 s at most)."""
    global port
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            text = open(port_file).read()
            if text.endswith("\n"):
                port = int(text)
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


def ws_open(path):
    """A WebSocket connection to path: (socket, bytes after the 101)."""
    s = connect()
    s.sendall(b"GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" % path.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise EOFError("no handshake")
        buf += chunk
    head, buf = buf.split(b"\r\n\r\n", 1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        raise EOFError("handshake %r" % head.split(b"\r\n")[0])
    return s, buf


def handler_trap(port_file):
    # tests/fixtures/http/handler_trap.av: each route traps its own way.
    # Each trap is answered 500 on a kept-alive connection, and the next
    # request on it is answered; a trap in a request that closes (or an
    # HTTP/1.0 one) is answered 500 and closed; a trap in a WebSocket
    # message handler closes that connection with 1011, and a new one
    # echoes. Then the server is stopped.
    if not wait_port(port_file):
        return "no port"
    out = []
    s = connect()
    s.settimeout(5)

    def ask(path):
        s.sendall(get(path))
        got = read_answers(s, 1)
        return (got[0][0], got[0][1].decode("latin1")) if got else (0, "")

    for name in ["bounds", "divide", "grow", "grow-hash", "status", "header", "route"]:
        st, _ = ask("/" + name)
        st2, body = ask("/ok")
        out.append("%s:%d,ok:%d %s" % (name, st, st2, body))
    out.append("late:%d" % ask("/late")[0])
    out.append("mutex:%d" % ask("/mutex")[0])
    out.append("mutex-again:%d" % ask("/mutex-again")[0])
    out.append("mutex-state:%d %s" % ask("/mutex-state"))
    out.append("pin:%d" % ask("/pin")[0])
    out.append("pins:%d %s" % ask("/pins"))
    s.close()
    for name, raw in [("close", get("/bounds", "Connection: close\r\n")),
                      ("1.0", b"GET /bounds HTTP/1.0\r\n\r\n")]:
        status, data, closed = exchange(raw)
        if data.startswith(b"HTTP/1.0 "):
            status = int(data.split(b" ", 2)[1])
        out.append("%s:%d %s" % (name, status, "closed" if closed else "open"))
    w, buf = ws_open("/ws")
    w.settimeout(5)
    ws_frame(w, 1, b"hi")
    op, payload, buf = ws_read_frame(w, buf)
    out.append("ws:%d %s" % (op, payload.decode("latin1")))
    ws_frame(w, 1, b"trap")
    op, payload, buf = ws_read_frame(w, buf)
    code = (payload[0] << 8 | payload[1]) if op == 8 and len(payload) >= 2 else -1
    out.append("ws-trap:%d %d" % (op, code))
    w.close()
    w, buf = ws_open("/ws")
    w.settimeout(5)
    ws_frame(w, 1, b"again")
    op, payload, buf = ws_read_frame(w, buf)
    out.append("ws-after:%d %s" % (op, payload.decode("latin1")))
    w.close()
    status, data, closed = exchange(get("/__stop"))
    out.append("stop:%d" % status)
    return ",".join(out)


def callback_trap(port_file):
    # A trap in a C callback the handler made ends the program: the
    # request is never answered.
    if not wait_port(port_file):
        return "no port"
    status, data, closed = exchange(get("/callback"))
    return "%d %s" % (status, "closed" if closed else "open")


def ws_big(size):
    # A WebSocket message larger than the socket takes at once, sent
    # from the handler to a client that reads late: the frame must
    # arrive whole (the rest of a frame already begun was refused by the
    # send queue's 1 MiB bound, leaving half a frame on the wire, whose
    # length then swallowed the next frames), and the connection must
    # still carry the next message.
    s = small_rcvbuf()
    s.sendall(b"GET /ws-big HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            return "no handshake"
        buf += chunk
    head, buf = buf.split(b"\r\n\r\n", 1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        return "handshake %r" % head.split(b"\r\n")[0]
    ws_frame(s, 1, b"big %d" % size)
    time.sleep(0.3)
    try:
        op, payload, buf = ws_read_frame(s, buf)
    except (EOFError, OSError) as e:
        return "cut:%s" % e
    want = bytes(range(97, 123)) * (size // 26 + 1)
    if op != 1 or payload != want[:size]:
        return "wrong:op %d, %d bytes" % (op, len(payload))
    ws_frame(s, 1, b"next")
    try:
        op, payload, buf = ws_read_frame(s, buf)
    except (EOFError, OSError) as e:
        return "whole,then %s" % e
    s.close()
    return "whole,then %s" % payload.decode("latin1")


def ws_cut(port_file):
    # tests/fixtures/http/ws_cut.av under the malloc_fail shim: a frame of
    # 8 MiB to a client that does not read for 0.3 s, whose rest the
    # server cannot copy. The connection must end before the frame does
    # (it stayed open, the next frame's bytes read as this one's
    # payload); a second connection then reads what the sends returned.
    if not wait_port(port_file):
        return "no port"
    s = small_rcvbuf()
    s.sendall(b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            return "no handshake"
        buf += chunk
    buf = buf.split(b"\r\n\r\n", 1)[1]
    ws_frame(s, 1, b"big 8388608")
    time.sleep(0.3)
    s.settimeout(3)
    try:
        op, payload, buf = ws_read_frame(s, buf)
        first = "a whole frame of %d bytes" % len(payload)
    except EOFError:
        first = "closed mid-frame"
    except OSError as e:
        first = "still open mid-frame (%s)" % e
    s.close()
    time.sleep(0.2)
    t, buf = ws_open("/ws")
    ws_frame(t, 1, b"said")
    op, payload, buf = ws_read_frame(t, buf)
    t.close()
    return "%s; %s" % (first, payload.decode("latin1"))


def until_closed(s, limit):
    """Reads until the server ends the connection; returns what came and
    the seconds that took (None when it was still open after `limit`)."""
    s.settimeout(0.05)
    start = time.time()
    data = b""
    while time.time() - start < limit:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return data, time.time() - start
        if not chunk:
            return data, time.time() - start
        data += chunk
    return data, None


def trickle_head():
    # A request head sent a byte every 0.1 s, never finished. The idle
    # timeout started again at every byte, so such a client held the
    # connection for good; the test's server gives a head 1 s: it is
    # answered 408 and closed then.
    s = connect()
    head = b"GET /ping HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 200
    s.settimeout(0.1)
    start = time.time()
    data = b""
    for i in range(len(head)):
        try:
            s.send(head[i:i + 1])
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        data += chunk
        break
    rest, _ = until_closed(s, 5)
    took = time.time() - start
    data += rest
    s.close()
    if not data.startswith(b"HTTP/1.1 408 "):
        return "no 408 after %.1f s: %r" % (took, data[:40])
    return "408 in time" if 0.9 <= took < 3.0 else "408 after %.1f s" % took


def body_at(rate_per_tenth, total):
    """POSTs a body of `total` bytes at `rate_per_tenth` bytes every
    0.1 s; returns (status, seconds) of the answer, status 0 if none."""
    s = connect()
    s.sendall(b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % total)
    start = time.time()
    sent = 0
    try:
        while sent < total:
            n = min(rate_per_tenth, total - sent)
            s.sendall(b"a" * n)
            sent += n
            # An answer before the body is complete ends the sending.
            if select.select([s], [], [], 0.1)[0]:
                break
    except OSError:
        pass
    got = read_answers(s, 1)
    took = time.time() - start
    s.close()
    return (got[0][0] if got else 0), took, (got[0][1] if got else b"")


def slow_body():
    # A body sent at 200 bytes a second: the test's server wants 1000
    # a second once 0.5 s has passed, so it is answered 408 soon after.
    status, took, _ = body_at(20, 100000)
    if status != 408:
        return "answered %d after %.1f s" % (status, took)
    return "408 in time" if took < 3.0 else "408 after %.1f s" % took


def fast_body():
    # A body at 40000 bytes a second takes a second, past the grace,
    # and is answered whole.
    status, took, body = body_at(4000, 40000)
    return "answered %d: %s" % (status, body.decode("latin1"))


def slow_read():
    # A 32 MiB answer (more than the kernel's buffers hold, so the
    # server queues a tail) the client does not read for 2 s: the tail
    # leaves under the test's rate (1000 bytes a second after 0.5 s), so
    # the server closes the connection; what reaches the client is cut.
    s = small_rcvbuf()
    s.sendall(get("/huge"))
    time.sleep(2)
    data, took = until_closed(s, 10)
    s.close()
    if took is None:
        return "still open"
    return "cut" if len(data) < 4 * BIG else "whole"


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
    elif mode == "hosts":
        result = hosts()
    elif mode == "header_split":
        result = header_split(arg)
    elif mode == "trickle_head":
        result = trickle_head()
    elif mode == "slow_body":
        result = slow_body()
    elif mode == "fast_body":
        result = fast_body()
    elif mode == "slow_read":
        result = slow_read()
    elif mode == "budget":
        result = budget()
    elif mode == "ceiling":
        result = ceiling(arg)
    elif mode == "conn_limit":
        result = conn_limit()
    elif mode == "ws_silent":
        result = ws_silent()
    elif mode == "ws_silent_probe":
        result = ws_silent_probe(arg)
    elif mode == "handler_trap":
        result = handler_trap(arg)
    elif mode == "callback_trap":
        result = callback_trap(arg)
    elif mode == "ws_cut":
        result = ws_cut(arg)
    elif mode == "ws_big":
        result = ws_big(int(arg))
    else:
        result = "unknown mode"
except Exception as e:  # a failure is a result the test prints
    result = "BAD:%s:%s" % (type(e).__name__, e)
print("%s=%s" % (mode, result))
