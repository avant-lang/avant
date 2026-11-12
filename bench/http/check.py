#!/usr/bin/env python3
"""Correctness of one hello-world server of the HTTP harness, untimed.

Usage: check.py ROW [--port 18301] [--out FILE.json]

The server is already listening on 127.0.0.1:PORT (`run.sh check` starts
and stops it). The checks, each on its own connection, with the request
wrk sends (`GET / HTTP/1.1` and a `Host` header, nothing else):

  single         one request: the exact response bytes, their count, the
                 header names, whether `Date` and `Server` are sent
  pipeline16     16 requests in one write: 16 complete `200 hello`
                 answers, in order, and not one byte more
  split@N        the same 16 requests in two writes cut at byte N (a
                 pause between them), for several N: inside a request
                 line, inside a header, inside the blank line that ends a
                 request, one byte from either end
  pipeline16x200 200 writes of 16 requests on one connection, each
                 answered before the next is sent (the shape of wrk with
                 pipe.lua): 3,200 answers
  keepalive1000  1,000 requests one after the other on one connection
  close_unread   16 requests in one write, then the client closes without
                 reading one answer (what a load generator does to its
                 connections when its time is up); the server must still be
                 there: a new connection gets its `200 hello`. Run last,
                 because a server that fails it is gone.

The result goes to stdout as one line per check and, with --out, to a JSON
file that tables.py reads for the response size and `Date` columns. The
exit status is 0 only when every check passed.
"""
import json
import socket
import sys
import time

TIMEOUT = 5.0


def request(port):
    return ("GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n" % port).encode()


def parse_response(buf):
    """One response at the start of buf: (bytes used, status, headers, body),
    or None while it is incomplete. Raises ValueError on a malformed one."""
    end = buf.find(b"\r\n\r\n")
    if end < 0:
        return None
    head = buf[:end].split(b"\r\n")
    parts = head[0].split(b" ", 2)
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/1."):
        raise ValueError("bad status line %r" % head[0][:60])
    status = int(parts[1])
    headers = []
    for line in head[1:]:
        name, sep, value = line.partition(b":")
        if not sep:
            raise ValueError("bad header line %r" % line[:60])
        headers.append((name.decode("latin-1"), value.strip().decode("latin-1")))
    lower = {k.lower(): v for k, v in headers}
    start = end + 4
    if lower.get("transfer-encoding", "").lower() == "chunked":
        pos = start
        body = b""
        while True:
            eol = buf.find(b"\r\n", pos)
            if eol < 0:
                return None
            size = int(buf[pos:eol].split(b";")[0], 16)
            pos = eol + 2
            if len(buf) < pos + size + 2:
                return None
            body += buf[pos:pos + size]
            pos += size + 2
            if size == 0:
                return pos, status, headers, body
    length = int(lower.get("content-length", "0"))
    if len(buf) < start + length:
        return None
    return start + length, status, headers, buf[start:start + length]


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def read_responses(self, count):
        """Reads exactly `count` responses; returns the list of
        (raw bytes, status, headers, body)."""
        out = []
        deadline = time.monotonic() + TIMEOUT
        while len(out) < count:
            got = parse_response(self.buf)
            if got is not None:
                used, status, headers, body = got
                out.append((self.buf[:used], status, headers, body))
                self.buf = self.buf[used:]
                continue
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError("%d of %d answers after %.0f s, %d bytes pending: %r" % (len(out), count, TIMEOUT, len(self.buf), self.buf[:80]))
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise TimeoutError("%d of %d answers after %.0f s, %d bytes pending: %r" % (len(out), count, TIMEOUT, len(self.buf), self.buf[:80]))
            if not chunk:
                raise ConnectionError("closed by the server after %d of %d answers, %d bytes pending: %r" % (len(out), count, len(self.buf), self.buf[:80]))
            deadline = time.monotonic() + TIMEOUT
            self.buf += chunk
        return out

    def extra_bytes(self, wait=0.3):
        """Bytes the server sends beyond what was asked for (none expected)."""
        self.sock.settimeout(wait)
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            chunk = b""
        return self.buf + chunk

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def all_hello(answers):
    for i, (_, status, _, body) in enumerate(answers):
        if status != 200 or body != b"hello":
            raise ValueError("answer %d is status %d body %r" % (i + 1, status, body[:40]))


def check_single(port, result):
    c = Conn(port)
    try:
        c.send(request(port))
        raw, status, headers, body = c.read_responses(1)[0]
        extra = c.extra_bytes()
        names = [k for k, _ in headers]
        low = [k.lower() for k in names]
        result["response_bytes"] = len(raw)
        result["response"] = raw.decode("latin-1")
        result["headers"] = names
        result["date"] = "date" in low
        result["server_header"] = "server" in low
        if status != 200 or body != b"hello":
            raise ValueError("status %d body %r" % (status, body[:40]))
        if extra:
            raise ValueError("%d bytes after the answer: %r" % (len(extra), extra[:60]))
        return "%d bytes, headers %s" % (len(raw), ", ".join(names))
    finally:
        c.close()


def check_pipeline(port, splits=None, pause=0.05):
    data = request(port) * 16
    c = Conn(port)
    try:
        if splits is None:
            c.send(data)
        else:
            c.send(data[:splits])
            time.sleep(pause)
            c.send(data[splits:])
        answers = c.read_responses(16)
        all_hello(answers)
        extra = c.extra_bytes()
        if extra:
            raise ValueError("%d bytes after the 16th answer: %r" % (len(extra), extra[:60]))
        return "16 answers, %d bytes" % sum(len(a[0]) for a in answers)
    finally:
        c.close()


def check_pipeline_rounds(port, rounds):
    data = request(port) * 16
    c = Conn(port)
    try:
        for r in range(rounds):
            c.send(data)
            try:
                all_hello(c.read_responses(16))
            except Exception as e:
                raise type(e)("round %d: %s" % (r + 1, e))
        extra = c.extra_bytes()
        if extra:
            raise ValueError("%d bytes after the last answer: %r" % (len(extra), extra[:60]))
        return "%d answers" % (rounds * 16)
    finally:
        c.close()


def check_keepalive(port, count):
    data = request(port)
    c = Conn(port)
    try:
        for i in range(count):
            c.send(data)
            try:
                all_hello(c.read_responses(1))
            except Exception as e:
                raise type(e)("request %d: %s" % (i + 1, e))
        return "%d answers on one connection" % count
    finally:
        c.close()


def check_close_unread(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    s.sendall(request(port) * 16)
    s.close()
    time.sleep(0.5)
    try:
        c = Conn(port)
    except OSError as e:
        raise ConnectionError("the server is gone after the client closed 16 unread answers: %s" % e)
    try:
        c.send(request(port))
        all_hello(c.read_responses(1))
        return "the server answers a new connection"
    finally:
        c.close()


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    row = args[0]
    port = 18301
    out = None
    i = 1
    while i < len(args):
        if args[i] == "--port":
            port = int(args[i + 1])
        elif args[i] == "--out":
            out = args[i + 1]
        else:
            sys.exit("unknown argument: %s" % args[i])
        i += 2
    one = len(request(port))
    # Cuts: one byte in; inside the 1st request line; inside the 5th
    # request's Host header; between the CR LF CR LF of the 8th request's
    # end (three places); at a request boundary; one byte from the end.
    cuts = [1, 7, 4 * one + 20, 8 * one - 3, 8 * one - 2, 8 * one - 1, 8 * one, 16 * one - 1]
    result = {"row": row, "port": port, "request_bytes": one, "checks": {}}
    checks = [("single", lambda: check_single(port, result)), ("pipeline16", lambda: check_pipeline(port))]
    for cut in cuts:
        checks.append(("split@%d" % cut, lambda cut=cut: check_pipeline(port, cut)))
    checks.append(("pipeline16x200", lambda: check_pipeline_rounds(port, 200)))
    checks.append(("keepalive1000", lambda: check_keepalive(port, 1000)))
    checks.append(("close_unread", lambda: check_close_unread(port)))
    failed = 0
    for name, fn in checks:
        try:
            detail = fn()
            ok = True
        except Exception as e:  # every failure is a finding, with its text
            detail = "%s: %s" % (type(e).__name__, e)
            ok = False
            failed += 1
        result["checks"][name] = {"ok": ok, "detail": detail}
        print("%s %s %s: %s" % (row, name, "ok" if ok else "FAIL", detail))
    result["ok"] = failed == 0
    if out:
        with open(out, "w") as f:
            json.dump(result, f, indent=1, sort_keys=True)
            f.write("\n")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
