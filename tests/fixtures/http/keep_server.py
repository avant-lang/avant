# A server that answers each request completely and then keeps the
# connection open for KEEP seconds (it ignores the client's
# Connection: close), for tests/cases/test_std_http_safety.av: std.http's
# client must return when the response is complete by its framing, not
# when the server closes (Stage 29 W-FIX1g, O566).
# Usage: keep_server.py PORT_FILE KEEP. Writes its port to PORT_FILE and
# serves until GET /__stop, which it answers and then exits.
import os
import socket
import sys
import threading
import time

port_file, keep = sys.argv[1], float(sys.argv[2])

# Each answer is a list of pieces sent with 0.1 s between them, so a
# client that decides too early (half a head, half a body, half a chunk
# size) is caught; "close" says whether the server closes at once.
ANSWERS = {
    "/cl": [b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"],
    "/cl-late": [b"HTTP/1.1 200 OK\r\nCont", b"ent-Length: 5\r\n\r\nhe", b"llo"],
    "/chunked": [b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nhel\r\n2\r\nlo\r\n0\r\nX-Trailer: 1\r\n\r\n"],
    "/chunked-late": [b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nhe", b"l\r\n2\r\nlo\r\n0", b"\r\n\r\n"],
    "/both": [b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 100\r\n\r\n5\r\nhello\r\n0\r\n\r\n"],
    "/204": [b"HTTP/1.1 204 No Content\r\n\r\n"],
    "/redirect": [b"HTTP/1.1 302 Found\r\nLocation: /cl\r\nContent-Length: 0\r\n\r\n"],
    "/close": [b"HTTP/1.1 200 OK\r\n\r\nuntil close"],
    "/__stop": [b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbye"],
}


def serve(c):
    try:
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = c.recv(4096)
            if not chunk:
                return
            data += chunk
        path = data.split(b"\r\n", 1)[0].decode("latin1").split(" ")[1]
        pieces = ANSWERS.get(path, [b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"])
        for i, piece in enumerate(pieces):
            if i > 0:
                time.sleep(0.1)
            c.sendall(piece)
        if path == "/__stop":
            c.close()
            os._exit(0)
        if path != "/close":
            time.sleep(keep)
    except OSError:
        pass
    finally:
        c.close()


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(64)
with open(port_file + ".tmp", "w") as f:
    f.write("%d\n" % srv.getsockname()[1])
os.rename(port_file + ".tmp", port_file)
deadline = time.time() + 120
srv.settimeout(1)
while time.time() < deadline:
    try:
        c, _ = srv.accept()
    except socket.timeout:
        continue
    threading.Thread(target=serve, args=(c,), daemon=True).start()
