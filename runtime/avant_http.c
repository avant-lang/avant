#include "avant_rt.h"
#include "avant_gc.h"
#include "libusockets.h"
#include "picohttpparser.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* std.http backing: a cleartext HTTP/1.1 server and a blocking fetch
 * primitive on uSockets, with picohttpparser for request and response
 * parsing. The Avant side owns routing and handler values; this file
 * owns sockets, buffers, and the wire format. No request state lives in
 * globals: a parsed request is a stack value handed to the Avant
 * dispatch callback, which copies what it keeps before returning.
 * Failure is reported through the thread-local status, in the
 * zlib/base64/json shape. */

/* 0 ok, 1 listen/connect failure, 2 timeout, 3 invalid message,
 * 4 allocation failure. */
static __thread int32_t g_http_status;

int32_t avant_http_take_status(void) {
  int32_t code = g_http_status;
  g_http_status = 0;
  return code;
}

static void set_status(int32_t code) {
  g_http_status = code;
}

#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_HEADER_BLOCK 16384

/* RC1: P (a C buffer, or an address inside a heap Bytes such as the
 * packed headers, which its slot relocates with its offset) is a handle
 * across the allocation. */
static char *make_string(const char *p, size_t n) {
  char *out;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &p);
  out = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

static char *make_bytes(const char *p, size_t n) {
  char *out;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &p);
  out = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  return out;
}

/* Packed Headers: a uint32 count, then count records of
 * (name_off, name_len, value_off, value_len), then the raw header
 * block the offsets point into. One collector Bytes per header set;
 * lookups materialize a String only for the value actually taken. */
static char *build_packed_headers(struct phr_header *headers, size_t count,
                                  const char *raw, size_t raw_len) {
  size_t total = 4 + count * 16 + raw_len;
  char *out;
  uint32_t *words;
  size_t i;
  /* RC1/RC2: RAW (a heap string or a C buffer) is a handle across the
   * allocation; the header offsets are taken against its address before
   * the allocation (RAW0, never dereferenced), which the parsed headers
   * name. */
  const char *raw0 = raw;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &raw);
  out = (char *)avant_alloc((uint64_t)total + 1, AVANT_TYPE_BYTES);
  words = (uint32_t *)out;
  words[0] = (uint32_t)count;
  for (i = 0; i < count; i++) {
    words[1 + i * 4 + 0] = (uint32_t)(headers[i].name - raw0);
    words[1 + i * 4 + 1] = (uint32_t)headers[i].name_len;
    words[1 + i * 4 + 2] = (uint32_t)(headers[i].value - raw0);
    words[1 + i * 4 + 3] = (uint32_t)headers[i].value_len;
  }
  if (raw_len > 0) {
    memcpy(out + 4 + count * 16, raw, raw_len);
  }
  out[total] = 0;
  return out;
}

static uint32_t packed_count(const char *data, size_t n) {
  if (!data || n < 4) {
    return 0;
  }
  return ((uint32_t *)(void *)data)[0];
}

static const char *packed_raw(const char *data, uint32_t count) {
  return data + 4 + (size_t)count * 16;
}

static int ascii_ieq(const char *a, size_t an, const char *b, size_t bn) {
  size_t i;
  if (an != bn) {
    return 0;
  }
  for (i = 0; i < an; i++) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = (char)(x + 32);
    }
    if (y >= 'A' && y <= 'Z') {
      y = (char)(y + 32);
    }
    if (x != y) {
      return 0;
    }
  }
  return 1;
}

int32_t avant_http_header_count(const char *data) {
  return (int32_t)packed_count(data, avant_str_n(data));
}

int32_t avant_http_header_has(const char *data, const char *name) {
  size_t n = avant_str_n(data);
  size_t name_len = avant_str_n(name);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  uint32_t i;
  if (!data || count == 0) {
    return 0;
  }
  raw = packed_raw(data, count);
  for (i = 0; i < count; i++) {
    uint32_t off = words[1 + i * 4];
    uint32_t len = words[1 + i * 4 + 1];
    if (ascii_ieq(raw + off, len, name, name_len)) {
      return 1;
    }
  }
  return 0;
}

/* The index-th value for `name`, or status 1 when there is none. */
char *avant_http_header_at(const char *data, const char *name, int32_t index) {
  size_t n = avant_str_n(data);
  size_t name_len = avant_str_n(name);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  uint32_t i;
  int32_t seen = 0;
  set_status(0);
  if (!data || count == 0) {
    set_status(1);
    return avant_str_empty();
  }
  raw = packed_raw(data, count);
  for (i = 0; i < count; i++) {
    uint32_t noff = words[1 + i * 4];
    uint32_t nlen = words[1 + i * 4 + 1];
    if (ascii_ieq(raw + noff, nlen, name, name_len)) {
      if (seen == index) {
        uint32_t voff = words[1 + i * 4 + 2];
        uint32_t vlen = words[1 + i * 4 + 3];
        return make_string(raw + voff, vlen);
      }
      seen++;
    }
  }
  set_status(1);
  return avant_str_empty();
}

char *avant_http_header_name_at(const char *data, int32_t index) {
  size_t n = avant_str_n(data);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  set_status(0);
  if (!data || index < 0 || (uint32_t)index >= count) {
    set_status(1);
    return avant_str_empty();
  }
  raw = packed_raw(data, count);
  return make_string(raw + words[1 + (uint32_t)index * 4],
                     words[1 + (uint32_t)index * 4 + 1]);
}

char *avant_http_header_value_at(const char *data, int32_t index) {
  size_t n = avant_str_n(data);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  set_status(0);
  if (!data || index < 0 || (uint32_t)index >= count) {
    set_status(1);
    return avant_str_empty();
  }
  raw = packed_raw(data, count);
  return make_string(raw + words[1 + (uint32_t)index * 4 + 2],
                     words[1 + (uint32_t)index * 4 + 3]);
}

/* ---- server ---- */

typedef int32_t (*AvantHttpDispatch)(void *state, void *req);

typedef struct AvantHttpServer AvantHttpServer;
typedef struct HttpLoop HttpLoop;

/* One queued outbound frame remainder or cross-thread frame. Allocated
 * only when the kernel did not take a whole direct write, or when a
 * send arrives from a non-loop thread; the loop-thread hot path writes
 * with us_socket_write2 and allocates nothing here. */
typedef struct WsOut {
  char *data;
  size_t len;
  size_t off;
  struct WsOut *next;
} WsOut;

/* A live WebSocket connection. Owned by its Conn; registered in the
 * server's list so an Avant WsConn value (server pointer plus id, two
 * scalars that may cross a spawn boundary) can find it again from any
 * thread without holding a C pointer that could dangle. */
typedef struct AvantWsConn {
  int32_t id;
  AvantHttpServer *srv;
  struct HttpLoop *loop;
  struct us_socket_t *sock;
  struct Conn *conn;
  pthread_t owner_tid;
  char *path;
  char *frag;
  size_t frag_len;
  size_t frag_cap;
  int frag_opcode;
  WsOut *out_head;
  WsOut *out_tail;
  size_t out_bytes;
  pthread_mutex_t qmu;
  int close_sent;
  int close_received;
  int received_code;
  int dead;
  int event_dispatched;
  int pending_close_code;
  int shutdown_after_flush;
  struct AvantWsConn *next_all;
  struct AvantWsConn *next_global;
} AvantWsConn;

typedef struct Conn {
  AvantHttpServer *srv;
  char *buf;
  size_t len;
  size_t cap;
  int answered;
  int sent_continue;
  AvantWsConn *ws;
} Conn;

/* One event loop per loop thread, all binding the same port through
 * SO_REUSEPORT (the vendored default). Route state is shared and
 * read-only while serving; each loop owns its connection list, so
 * loop threads never touch each other's lists. */
struct HttpLoop {
  AvantHttpServer *srv;
  struct us_loop_t *loop;
  struct us_socket_context_t *ctx;
  struct us_listen_socket_t *listen;
  struct ConnNode *conns_head;
  pthread_t thread;
  int spawned;
};

struct AvantHttpServer {
  HttpLoop *loops;
  int nloops;
  void *state;
  AvantHttpDispatch dispatch;
  AvantHttpDispatch ws_dispatch;
  volatile int closing;
  int running;
  int port;
  int32_t max_body;
  int32_t max_message;
  AvantWsConn *ws_all;
  AvantWsConn *ws_dead;
};

typedef struct ConnNode {
  Conn conn;
  struct us_socket_t *sock;
  struct ConnNode *next;
} ConnNode;

static HttpLoop *loop_of_socket(struct us_socket_t *s) {
  struct us_socket_context_t *ctx = us_socket_context(0, s);
  return *(HttpLoop **)us_socket_context_ext(0, ctx);
}

static const char *reason_phrase(int32_t status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    default: return "Status";
  }
}

static size_t put_u32(char *dst, uint32_t v) {
  char tmp[10];
  size_t i = 0;
  size_t n = 0;
  if (v == 0) {
    dst[0] = '0';
    return 1;
  }
  while (v > 0) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (i > 0) {
    dst[n++] = tmp[--i];
  }
  return n;
}

static size_t put_str(char *dst, const char *src, size_t len) {
  memcpy(dst, src, len);
  return len;
}

/* conn_header is "close", "keep-alive", or NULL to omit the header.
 * The prefix is assembled by hand: a hello-world response costs two
 * snprintf calls per request otherwise, which profiling put at about
 * a seventh of the loop thread's user time. */
static void write_response(struct us_socket_t *s, int32_t status,
                           const char *head, const char *body,
                           const char *conn_header) {
  size_t body_len = avant_str_n(body);
  size_t head_len = head ? strlen(head) : 0;
  char prefix[4096];
  size_t n = 0;
  const char *reason = reason_phrase(status);
  n += put_str(prefix + n, "HTTP/1.1 ", 9);
  n += put_u32(prefix + n, (uint32_t)status);
  prefix[n++] = ' ';
  n += put_str(prefix + n, reason, strlen(reason));
  n += put_str(prefix + n, "\r\n", 2);
  if (head_len > 0) {
    if (n + head_len + 64 >= sizeof(prefix)) {
      /* Oversized head block: fall back to separate writes below. */
      n = sizeof(prefix);
    } else {
      n += put_str(prefix + n, head, head_len);
    }
  }
  if (n < sizeof(prefix)) {
    n += put_str(prefix + n, "Content-Length: ", 16);
    n += put_u32(prefix + n, (uint32_t)body_len);
    n += put_str(prefix + n, "\r\n", 2);
    if (conn_header) {
      n += put_str(prefix + n, "Connection: ", 12);
      n += put_str(prefix + n, conn_header, strlen(conn_header));
      n += put_str(prefix + n, "\r\n", 2);
    }
    n += put_str(prefix + n, "\r\n", 2);
  }
  if (n < 0) {
    return;
  }
  if ((size_t)n >= sizeof(prefix)) {
    /* Head alone is bounded by the header caps the Avant side builds;
     * fall back to separate writes for an oversized head block. */
    char line[256];
    int m = snprintf(line, sizeof(line), "HTTP/1.1 %d %s\r\n", (int)status,
                     reason_phrase(status));
    us_socket_write(0, s, line, m, 1);
    if (head_len > 0) {
      us_socket_write(0, s, head, (int)head_len, 1);
    }
    if (conn_header) {
      m = snprintf(line, sizeof(line),
                   "Content-Length: %zu\r\nConnection: %s\r\n\r\n", body_len,
                   conn_header);
    } else {
      m = snprintf(line, sizeof(line), "Content-Length: %zu\r\n\r\n", body_len);
    }
    us_socket_write(0, s, line, m, 1);
    if (body_len > 0) {
      us_socket_write(0, s, body, (int)body_len, 0);
    }
    return;
  }
  us_socket_write2(0, s, prefix, n, body, (int)body_len);
}

static void send_error(Conn *conn, struct us_socket_t *s, int32_t status) {
  const char *phrase = reason_phrase(status);
  write_response(s, status, "Content-Type: text/plain\r\n", phrase, "close");
  conn->answered = 1;
  us_socket_shutdown(0, s);
}

/* A parsed request, valid only for the dispatch call. Pointers borrow
 * the connection buffer; the Avant side copies what it keeps. */
typedef struct AvantHttpReq {
  AvantHttpServer *srv;
  Conn *conn;
  struct us_socket_t *sock;
  const char *method;
  size_t method_len;
  const char *target;
  size_t target_len;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t num_headers;
  const char *body;
  size_t body_len;
  int keep_alive;
  int minor_version;
  int is_ws;
  int upgraded;
  char ws_key[128];
  AvantWsConn *ws_conn;
} AvantHttpReq;

char *avant_http_req_method(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  return make_string(req->method, req->method_len);
}

char *avant_http_req_target(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  /* First getter of every dispatch: the heap-goal boundary. This
   * frame holds no Avant heap pointers yet, so the collection it may
   * run strands nothing; without it the request path, which
   * allocates only under defer, never collects. */
  avant_gc_heap_goal_collect();
  return make_string(req->target, req->target_len);
}

char *avant_http_req_body(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  if (req->body_len == 0) {
    return avant_str_empty();
  }
  return make_bytes(req->body, req->body_len);
}

char *avant_http_req_headers(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  /* The raw block is the connection buffer from its start through the
   * header terminator; offsets recorded by the parser stay valid. */
  size_t raw_len = 0;
  if (req->num_headers > 0) {
    struct phr_header *last = &req->headers[req->num_headers - 1];
    raw_len = (size_t)((last->value + last->value_len) - req->conn->buf);
  }
  return build_packed_headers(req->headers, req->num_headers, req->conn->buf,
                              raw_len);
}

void avant_http_req_respond(void *reqp, int32_t status, const char *head,
                            const char *body) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  const char *conn_header = NULL;
  /* The head and body pointers name Avant heap objects held only in
   * C registers here; nothing below is a safepoint (no allocation, no
   * native wait), so no collection runs on any thread while the response
   * is written (Stage 28 W17a's handshake; until then a no-GC region kept
   * another loop thread's collection off, D191, O111). */
  if (!req->keep_alive) {
    conn_header = "close";
  } else if (req->minor_version == 0) {
    conn_header = "keep-alive";
  }
  write_response(req->sock, status, head, body, conn_header);
}

static int ascii_ieq_contains(const char *p, size_t n, const char *needle) {
  size_t needle_len = strlen(needle);
  size_t i;
  if (needle_len == 0 || n < needle_len) {
    return 0;
  }
  for (i = 0; i + needle_len <= n; i++) {
    if (ascii_ieq(p + i, needle_len, needle, needle_len)) {
      return 1;
    }
  }
  return 0;
}

static int find_header(struct phr_header *headers, size_t count,
                       const char *name, const char **value,
                       size_t *value_len) {
  size_t name_len = strlen(name);
  size_t i;
  for (i = 0; i < count; i++) {
    if (headers[i].name &&
        ascii_ieq(headers[i].name, headers[i].name_len, name, name_len)) {
      *value = headers[i].value;
      *value_len = headers[i].value_len;
      return 1;
    }
  }
  return 0;
}

static int parse_content_length(const char *p, size_t n, long *out) {
  long v = 0;
  size_t i;
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
    v = v * 10 + (p[i] - '0');
    if (v > 2147483647L) {
      return 0;
    }
  }
  *out = v;
  return 1;
}

/* ---- WebSocket helpers (RFC 6455) ---- */

/* SHA-1 for Sec-WebSocket-Accept only: handshake naming, not a security
 * use. Written from the RFC 3174 specification; the RFC 6455 example
 * key/accept pair is the test oracle. */
typedef struct WsSha1 {
  uint32_t h[5];
  uint64_t total;
  unsigned char block[64];
  size_t used;
} WsSha1;

static uint32_t ws_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void ws_sha1_block(WsSha1 *s, const unsigned char *p) {
  uint32_t w[80];
  uint32_t a, b, c, d, e;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (i = 16; i < 80; i++) {
    w[i] = ws_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
  for (i = 0; i < 80; i++) {
    uint32_t f, k, tmp;
    if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
    else { f = b ^ c ^ d; k = 0xca62c1d6u; }
    tmp = ws_rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = ws_rol(b, 30); b = a; a = tmp;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void ws_sha1(const unsigned char *data, size_t len, unsigned char out[20]) {
  WsSha1 s;
  size_t i;
  s.h[0] = 0x67452301u; s.h[1] = 0xefcdab89u; s.h[2] = 0x98badcfeu;
  s.h[3] = 0x10325476u; s.h[4] = 0xc3d2e1f0u;
  s.total = len; s.used = 0;
  while (len >= 64) { ws_sha1_block(&s, data); data += 64; len -= 64; }
  for (i = 0; i < len; i++) { s.block[s.used++] = data[i]; }
  s.block[s.used++] = 0x80;
  if (s.used > 56) {
    while (s.used < 64) { s.block[s.used++] = 0; }
    ws_sha1_block(&s, s.block);
    s.used = 0;
  }
  while (s.used < 56) { s.block[s.used++] = 0; }
  for (i = 0; i < 8; i++) {
    s.block[56 + i] = (unsigned char)((s.total * 8) >> (56 - i * 8));
  }
  ws_sha1_block(&s, s.block);
  for (i = 0; i < 5; i++) {
    out[i * 4] = (unsigned char)(s.h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(s.h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(s.h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(s.h[i]);
  }
}

static const char ws_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void ws_b64_encode20(const unsigned char in[20], char *out) {
  int i, o = 0;
  for (i = 0; i + 3 <= 20; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = ws_b64[(v >> 18) & 63];
    out[o++] = ws_b64[(v >> 12) & 63];
    out[o++] = ws_b64[(v >> 6) & 63];
    out[o++] = ws_b64[v & 63];
  }
  {
    uint32_t v = ((uint32_t)in[18] << 16) | ((uint32_t)in[19] << 8);
    out[o++] = ws_b64[(v >> 18) & 63];
    out[o++] = ws_b64[(v >> 12) & 63];
    out[o++] = ws_b64[(v >> 6) & 63];
    out[o++] = '=';
  }
  out[o] = 0;
}

static int ws_b64_val(char ch) {
  if (ch >= 'A' && ch <= 'Z') { return ch - 'A'; }
  if (ch >= 'a' && ch <= 'z') { return ch - 'a' + 26; }
  if (ch >= '0' && ch <= '9') { return ch - '0' + 52; }
  if (ch == '+') { return 62; }
  if (ch == '/') { return 63; }
  return -1;
}

/* A client key is valid only as base64 for exactly 16 bytes. */
static int ws_key_valid(const char *p, size_t n) {
  unsigned char out[20];
  size_t o = 0;
  size_t i;
  int pad = 0;
  if (n == 0 || (n % 4) != 0) { return 0; }
  for (i = 0; i < n; i += 4) {
    uint32_t v = 0;
    int j;
    for (j = 0; j < 4; j++) {
      char ch = p[i + j];
      if (ch == '=') {
        pad++;
        v <<= 6;
      } else {
        int d = ws_b64_val(ch);
        if (d < 0 || pad > 0) { return 0; }
        v = (v << 6) | (uint32_t)d;
      }
    }
    if (pad > 2) { return 0; }
    if (o < sizeof(out)) { out[o] = (unsigned char)(v >> 16); }
    o++;
    if (pad < 2) { if (o < sizeof(out)) { out[o] = (unsigned char)(v >> 8); } o++; }
    if (pad < 1) { if (o < sizeof(out)) { out[o] = (unsigned char)v; } o++; }
  }
  return o == 16;
}

static void ws_accept_key(const char *key, size_t key_len, char out[29]) {
  static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[20];
  unsigned char joined[256];
  size_t n = 0;
  size_t i;
  if (key_len > 200) { key_len = 200; }
  for (i = 0; i < key_len; i++) { joined[n++] = (unsigned char)key[i]; }
  for (i = 0; guid[i]; i++) { joined[n++] = (unsigned char)guid[i]; }
  ws_sha1(joined, n, digest);
  ws_b64_encode20(digest, out);
}

/* Strict UTF-8: rejects overlong forms, surrogates, and > U+10FFFF. */
static int ws_utf8_valid(const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    uint32_t cp;
    size_t extra, j;
    if (c < 0x80) { i++; continue; }
    if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; extra = 1; }
    else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; extra = 2; }
    else if (c >= 0xf0 && c <= 0xf4) { cp = c & 0x07; extra = 3; }
    else { return 0; }
    if (i + 1 + extra > n) { return 0; }
    for (j = 1; j <= extra; j++) {
      if ((p[i + j] & 0xc0) != 0x80) { return 0; }
      cp = (cp << 6) | (uint32_t)(p[i + j] & 0x3f);
    }
    if (extra == 2 && cp < 0x800) { return 0; }
    if (extra == 3 && cp < 0x10000) { return 0; }
    if (cp >= 0xd800 && cp <= 0xdfff) { return 0; }
    if (cp > 0x10ffff) { return 0; }
    i += 1 + extra;
  }
  return 1;
}

#define WS_QUEUE_MAX_BYTES 1048576

/* Every live WebSocket connection in the process, so a sender that
 * crossed a spawn boundary as a bare id can find its connection again.
 * This one mutex serializes registry edits and sends; sends hold it
 * only across a non-blocking write or a bounded enqueue. */
static pthread_mutex_t g_ws_mu = PTHREAD_MUTEX_INITIALIZER;
static AvantWsConn *g_ws_all;
static int32_t g_next_ws_id;

static size_t ws_frame_header(char *dst, int opcode, size_t len) {
  size_t n = 0;
  dst[n++] = (char)(0x80 | (opcode & 0x0f));
  if (len < 126) {
    dst[n++] = (char)len;
  } else if (len <= 65535) {
    dst[n++] = 126;
    dst[n++] = (char)(len >> 8);
    dst[n++] = (char)len;
  } else {
    int i;
    dst[n++] = 127;
    for (i = 7; i >= 0; i--) {
      dst[n++] = (char)(len >> (i * 8));
    }
  }
  return n;
}

/* Loop thread only. Writes as much of the queue as the kernel takes. */
static void ws_flush(AvantWsConn *ws) {
  pthread_mutex_lock(&ws->qmu);
  while (ws->out_head) {
    WsOut *node = ws->out_head;
    size_t remaining = node->len - node->off;
    int written = us_socket_write(0, ws->sock, node->data + node->off,
                                  (int)remaining, 0);
    if (written < 0) { written = 0; }
    node->off += (size_t)written;
    ws->out_bytes -= (size_t)written;
    if (node->off < node->len) {
      break;
    }
    ws->out_head = node->next;
    if (!ws->out_head) { ws->out_tail = NULL; }
    free(node->data);
    free(node);
  }
  {
    int drained = ws->out_head == NULL;
    int want_shutdown = ws->shutdown_after_flush && drained;
    pthread_mutex_unlock(&ws->qmu);
    if (want_shutdown && !us_socket_is_closed(0, ws->sock)) {
      us_socket_shutdown(0, ws->sock);
    }
  }
}

/* Enqueue one already-framed buffer (takes ownership). Caller holds no
 * locks; returns 0 when the byte bound would be exceeded. */
static int ws_enqueue(AvantWsConn *ws, char *framed, size_t framed_len) {
  WsOut *node;
  pthread_mutex_lock(&ws->qmu);
  if (ws->out_bytes + framed_len > WS_QUEUE_MAX_BYTES) {
    pthread_mutex_unlock(&ws->qmu);
    free(framed);
    return 0;
  }
  node = (WsOut *)malloc(sizeof(WsOut));
  if (!node) {
    pthread_mutex_unlock(&ws->qmu);
    free(framed);
    return 0;
  }
  node->data = framed;
  node->len = framed_len;
  node->off = 0;
  node->next = NULL;
  if (ws->out_tail) { ws->out_tail->next = node; } else { ws->out_head = node; }
  ws->out_tail = node;
  ws->out_bytes += framed_len;
  pthread_mutex_unlock(&ws->qmu);
  return 1;
}

/* Send one frame. On the loop thread with an empty queue the frame is
 * written vectored with no allocation; only the untaken remainder (or a
 * cross-thread frame, framed into one malloc) ever queues. */
static int ws_send_frame(AvantWsConn *ws, int opcode, const char *payload,
                         size_t len) {
  char header[10];
  size_t header_len = ws_frame_header(header, opcode, len);
  if (pthread_equal(pthread_self(), ws->owner_tid)) {
    int queued;
    pthread_mutex_lock(&ws->qmu);
    queued = ws->out_head != NULL;
    pthread_mutex_unlock(&ws->qmu);
    if (!queued) {
      int total = (int)(header_len + len);
      int written = us_socket_write2(0, ws->sock, header, (int)header_len,
                                     payload, (int)len);
      if (written >= total) {
        return 1;
      }
      if (written < 0) { written = 0; }
      if ((size_t)written < header_len) {
        size_t rem_h = header_len - (size_t)written;
        size_t framed_len = rem_h + len;
        char *framed = (char *)malloc(framed_len ? framed_len : 1);
        if (!framed) { return 0; }
        memcpy(framed, header + written, rem_h);
        if (len > 0) { memcpy(framed + rem_h, payload, len); }
        return ws_enqueue(ws, framed, framed_len);
      }
      {
        size_t rem = len - ((size_t)written - header_len);
        const char *src = payload + ((size_t)written - header_len);
        char *framed = (char *)malloc(rem ? rem : 1);
        if (!framed) { return 0; }
        if (rem > 0) { memcpy(framed, src, rem); }
        return ws_enqueue(ws, framed, rem);
      }
    }
  }
  {
    size_t framed_len = header_len + len;
    char *framed = (char *)malloc(framed_len ? framed_len : 1);
    if (!framed) { return 0; }
    memcpy(framed, header, header_len);
    if (len > 0) { memcpy(framed + header_len, payload, len); }
    if (!pthread_equal(pthread_self(), ws->owner_tid)) {
      int ok = ws_enqueue(ws, framed, framed_len);
      if (ok && ws->loop && ws->loop->loop) {
        us_wakeup_loop(ws->loop->loop);
      }
      return ok;
    }
    return ws_enqueue(ws, framed, framed_len);
  }
}

static AvantWsConn *ws_find_global(int32_t id) {
  AvantWsConn *ws;
  for (ws = g_ws_all; ws; ws = ws->next_global) {
    if (ws->id == id) { return ws; }
  }
  return NULL;
}

static AvantWsConn *ws_find(AvantHttpServer *srv, int32_t id) {
  AvantWsConn *ws;
  if (!srv) { return NULL; }
  for (ws = srv->ws_all; ws; ws = ws->next_all) {
    if (ws->id == id) { return ws; }
  }
  return NULL;
}

/* kind: 0 open, 1 message, 2 close. Payload and reason borrow C memory;
 * the Avant getters copy what the handler keeps. */
typedef struct AvantWsEvent {
  AvantHttpServer *srv;
  AvantWsConn *ws;
  int32_t kind;
  int32_t is_text;
  const char *data;
  size_t len;
  int32_t code;
  const char *reason;
  size_t reason_len;
} AvantWsEvent;

/* Rule 3 of gc-design.md section 4.4 (Stage 28 W17a): a thread runs the
 * uSockets loop native (epoll_wait and the C callbacks touch no heap
 * object), and each dispatch into Avant returns to running at its entry
 * (a safepoint while a collection is in progress) and to native when it
 * returns (enter_avant / leave_avant). g_loop_native is the depth of
 * loop_run_native on this thread, so a dispatch reached while running
 * (none today) makes no transition. */
static __thread int g_loop_native;

static void loop_run_native(struct us_loop_t *loop) {
  g_loop_native++;
  avant_gc_native_begin();
  us_loop_run(loop);
  avant_gc_native_end();
  g_loop_native--;
}

static void enter_avant(void) {
  if (g_loop_native) {
    avant_gc_native_end();
  }
}

static void leave_avant(void) {
  if (g_loop_native) {
    avant_gc_native_begin();
  }
}

static void ws_dispatch_event(AvantWsConn *ws, int32_t kind, int32_t is_text,
                              const char *data, size_t len, int32_t code,
                              const char *reason, size_t reason_len) {
  AvantWsEvent ev;
  if (!ws->srv->ws_dispatch || !ws->srv->state) { return; }
  memset(&ev, 0, sizeof(ev));
  ev.srv = ws->srv;
  ev.ws = ws;
  ev.kind = kind;
  ev.is_text = is_text;
  ev.data = data;
  ev.len = len;
  ev.code = code;
  ev.reason = reason;
  ev.reason_len = reason_len;
  enter_avant();
  ws->srv->ws_dispatch(ws->srv->state, &ev);
  leave_avant();
}

int32_t avant_ws_event_kind(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  /* First getter of every WS dispatch: the heap-goal boundary, in the
   * avant_http_req_target position. This frame holds no Avant heap
   * pointers yet, so the collection it may run strands nothing. */
  avant_gc_heap_goal_collect();
  return ev->kind;
}

int32_t avant_ws_event_conn_id(void *evp) {
  return ((AvantWsEvent *)evp)->ws->id;
}

void *avant_ws_event_server(void *evp) {
  return ((AvantWsEvent *)evp)->srv;
}

char *avant_ws_event_path(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  return make_string(ev->ws->path, strlen(ev->ws->path));
}

int32_t avant_ws_event_is_text(void *evp) {
  return ((AvantWsEvent *)evp)->is_text;
}

char *avant_ws_event_data(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  if (ev->len == 0) { return avant_str_empty(); }
  return make_bytes(ev->data, ev->len);
}

int32_t avant_ws_event_code(void *evp) {
  return ((AvantWsEvent *)evp)->code;
}

char *avant_ws_event_reason(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  if (ev->reason_len == 0) { return avant_str_empty(); }
  return make_string(ev->reason, ev->reason_len);
}

static int32_t ws_send_common(void *srvp, int32_t id, int32_t is_text,
                              const char *data) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  AvantWsConn *ws;
  size_t len;
  int ok;
  /* The data pointer names an Avant heap object held only in a C
   * register; nothing here is a safepoint (no allocation, no native wait:
   * g_ws_mu is a brief plain lock whose holders never stop), so no
   * collection runs on any thread meanwhile (Stage 28 W17a's handshake;
   * until then this and the send and close entries below held a no-GC
   * region, D191). */
  len = avant_str_n(data);
  pthread_mutex_lock(&g_ws_mu);
  ws = srv ? ws_find(srv, id) : ws_find_global(id);
  if (!ws || ws->dead || ws->close_sent) {
    pthread_mutex_unlock(&g_ws_mu);
    return 0;
  }
  ok = ws_send_frame(ws, is_text ? 1 : 2, data, len);
  pthread_mutex_unlock(&g_ws_mu);
  return ok;
}

int32_t avant_ws_send_text_by_id(int32_t id, const char *data) {
  return ws_send_common(NULL, id, 1, data);
}

int32_t avant_ws_send_text_bytes_by_id(int32_t id, const char *data) {
  return ws_send_common(NULL, id, 1, data);
}

int32_t avant_ws_send_bytes_by_id(int32_t id, const char *data) {
  return ws_send_common(NULL, id, 0, data);
}

int32_t avant_ws_close_conn(void *srvp, int32_t id, int32_t code,
                            const char *reason);

void *avant_http_req_ws_server(void *reqp) {
  return ((AvantHttpReq *)reqp)->srv;
}

int32_t avant_ws_close_conn(void *srvp, int32_t id, int32_t code,
                            const char *reason) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  AvantWsConn *ws;
  unsigned char payload[125];
  size_t reason_len;
  size_t payload_len;
  int ok;
  reason_len = avant_str_n(reason);
  if (reason_len > 123) { reason_len = 123; }
  payload[0] = (unsigned char)(code >> 8);
  payload[1] = (unsigned char)code;
  if (reason_len > 0) { memcpy(payload + 2, reason, reason_len); }
  payload_len = 2 + reason_len;
  pthread_mutex_lock(&g_ws_mu);
  ws = srv ? ws_find(srv, id) : ws_find_global(id);
  if (!ws || ws->dead || ws->close_sent) {
    pthread_mutex_unlock(&g_ws_mu);
    return 0;
  }
  ws->close_sent = 1;
  ws->shutdown_after_flush = 1;
  ok = ws_send_frame(ws, 8, (const char *)payload, payload_len);
  if (ok && pthread_equal(pthread_self(), ws->owner_tid)) {
    ws_flush(ws);
  }
  pthread_mutex_unlock(&g_ws_mu);
  return ok;
}

int32_t avant_ws_close_by_id(int32_t id, int32_t code, const char *reason) {
  return avant_ws_close_conn(NULL, id, code, reason);
}

void *avant_http_req_ws_conn(void *reqp) {
  return ((AvantHttpReq *)reqp)->ws_conn;
}

void *avant_ws_event_conn(void *evp) {
  return ((AvantWsEvent *)evp)->ws;
}

/* Loop-thread send through the connection pointer itself: no registry
 * lock, because the owner thread is the only socket writer and the
 * only thread that frees queue state. A call from any other thread
 * falls back to the locked by-id path. The pointer stays valid after
 * close because a closed connection's shell is retained, marked dead,
 * until server teardown. */
static int32_t ws_send_direct(void *handle, int32_t is_text,
                              const char *data) {
  AvantWsConn *ws = (AvantWsConn *)handle;
  size_t len;
  int ok;
  if (!ws) { return 0; }
  if (!pthread_equal(pthread_self(), ws->owner_tid)) {
    return ws_send_common(NULL, ws->id, is_text, data);
  }
  if (ws->dead || ws->close_sent) { return 0; }
  len = avant_str_n(data);
  ok = ws_send_frame(ws, is_text ? 1 : 2, data, len);
  return ok;
}

int32_t avant_ws_send_text_direct(void *handle, const char *data) {
  return ws_send_direct(handle, 1, data);
}

int32_t avant_ws_send_text_bytes_direct(void *handle, const char *data) {
  return ws_send_direct(handle, 1, data);
}

int32_t avant_ws_send_bytes_direct(void *handle, const char *data) {
  return ws_send_direct(handle, 0, data);
}

int32_t avant_ws_close_direct(void *handle, int32_t code,
                              const char *reason) {
  AvantWsConn *ws = (AvantWsConn *)handle;
  unsigned char payload[125];
  size_t reason_len;
  int ok;
  if (!ws) { return 0; }
  if (!pthread_equal(pthread_self(), ws->owner_tid)) {
    return avant_ws_close_conn(NULL, ws->id, code, reason);
  }
  if (ws->dead || ws->close_sent) { return 0; }
  reason_len = avant_str_n(reason);
  if (reason_len > 123) { reason_len = 123; }
  payload[0] = (unsigned char)(code >> 8);
  payload[1] = (unsigned char)code;
  if (reason_len > 0) { memcpy(payload + 2, reason, reason_len); }
  ws->close_sent = 1;
  ws->shutdown_after_flush = 1;
  ok = ws_send_frame(ws, 8, (const char *)payload, 2 + reason_len);
  ws_flush(ws);
  return ok;
}

static void ws_fail(AvantWsConn *ws, int code) {
  unsigned char payload[2];
  if (ws->dead) { return; }
  ws->dead = 1;
  ws->pending_close_code = code;
  if (!ws->close_sent) {
    ws->close_sent = 1;
    ws->shutdown_after_flush = 1;
    payload[0] = (unsigned char)(code >> 8);
    payload[1] = (unsigned char)code;
    ws_send_frame(ws, 8, (const char *)payload, 2);
    ws_flush(ws);
  }
}

static int ws_close_code_valid(int code) {
  if (code < 1000 || code >= 5000) { return 0; }
  if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
    return 0;
  }
  if (code >= 1016 && code <= 1099) { return 0; }
  if (code >= 1000 && code <= 1014) { return 1; }
  if (code >= 3000) { return 1; }
  return 0;
}

/* Frame parser over the connection buffer. Unfragmented payloads are
 * unmasked in place and dispatched borrowing the buffer, exactly as
 * HTTP requests borrow it; only a fragmented message allocates (its
 * reassembly buffer), and it is freed after its dispatch. */
static void ws_process(Conn *conn, struct us_socket_t *s) {
  AvantWsConn *ws = conn->ws;
  AvantHttpServer *srv = conn->srv;
  if (!ws) { return; }
  (void)s;
  while (!ws->dead) {
    unsigned char *buf = (unsigned char *)conn->buf;
    size_t len = conn->len;
    int fin, opcode;
    size_t payload_len, header_len, frame_len, i;
    unsigned char mask[4];
    int masked;
    unsigned char *payload;
    if (len < 2) { return; }
    fin = (buf[0] & 0x80) != 0;
    if (buf[0] & 0x70) { ws_fail(ws, 1002); return; }
    opcode = buf[0] & 0x0f;
    masked = (buf[1] & 0x80) != 0;
    payload_len = buf[1] & 0x7f;
    header_len = 2;
    if (payload_len == 126) {
      if (len < 4) { return; }
      payload_len = ((size_t)buf[2] << 8) | buf[3];
      header_len = 4;
    } else if (payload_len == 127) {
      uint64_t v = 0;
      int j;
      if (len < 10) { return; }
      for (j = 0; j < 8; j++) { v = (v << 8) | buf[2 + j]; }
      if (v > (uint64_t)1 << 62) { ws_fail(ws, 1002); return; }
      payload_len = (size_t)v;
      header_len = 10;
    }
    if (masked) {
      header_len += 4;
    }
    if (opcode >= 8) {
      if (!fin || payload_len > 125) { ws_fail(ws, 1002); return; }
      if (opcode != 8 && opcode != 9 && opcode != 10) {
        ws_fail(ws, 1002);
        return;
      }
    } else {
      if (opcode != 0 && opcode != 1 && opcode != 2) {
        ws_fail(ws, 1002);
        return;
      }
      if (opcode == 0 && ws->frag_opcode < 0) { ws_fail(ws, 1002); return; }
      if (opcode != 0 && ws->frag_opcode >= 0) { ws_fail(ws, 1002); return; }
      {
        size_t projected = ws->frag_len + payload_len;
        if (payload_len > (size_t)srv->max_message ||
            projected > (size_t)srv->max_message) {
          ws_fail(ws, 1009);
          return;
        }
      }
    }
    if (!masked) { ws_fail(ws, 1002); return; }
    if (len < header_len) { return; }
    frame_len = header_len + payload_len;
    if (len < frame_len) { return; }
    memcpy(mask, buf + header_len - 4, 4);
    payload = buf + header_len;
    for (i = 0; i < payload_len; i++) {
      payload[i] = (unsigned char)(payload[i] ^ mask[i & 3]);
    }
    if (opcode == 9) {
      ws_send_frame(ws, 10, (const char *)payload, payload_len);
      ws_flush(ws);
    } else if (opcode == 10) {
      /* pong: no action */
    } else if (opcode == 8) {
      int code = 1005;
      const char *reason = "";
      size_t reason_len = 0;
      if (payload_len == 1) { ws_fail(ws, 1002); return; }
      if (payload_len >= 2) {
        code = ((int)payload[0] << 8) | payload[1];
        if (!ws_close_code_valid(code)) { ws_fail(ws, 1002); return; }
        reason = (const char *)payload + 2;
        reason_len = payload_len - 2;
        if (!ws_utf8_valid((const unsigned char *)reason, reason_len)) {
          ws_fail(ws, 1007);
          return;
        }
      }
      ws->close_received = 1;
      ws->received_code = code;
      ws->dead = 1;
      if (!ws->event_dispatched) {
        ws->event_dispatched = 1;
        ws_dispatch_event(ws, 2, 0, NULL, 0, code, reason, reason_len);
      }
      if (!ws->close_sent) {
        ws->close_sent = 1;
        ws->shutdown_after_flush = 1;
        ws_send_frame(ws, 8, (const char *)payload, payload_len);
        ws_flush(ws);
      }
      if (conn->len > frame_len) {
        memmove(conn->buf, conn->buf + frame_len, conn->len - frame_len);
      }
      conn->len -= frame_len;
      return;
    } else if (opcode == 0 || fin) {
      const char *msg = (const char *)payload;
      size_t msg_len = payload_len;
      int msg_opcode = opcode;
      if (opcode == 0) {
        if (ws->frag_len + payload_len > ws->frag_cap) {
          size_t want = ws->frag_cap ? ws->frag_cap * 2 : 1024;
          char *next;
          while (want < ws->frag_len + payload_len) { want *= 2; }
          next = (char *)realloc(ws->frag, want);
          if (!next) { ws_fail(ws, 1009); return; }
          ws->frag = next;
          ws->frag_cap = want;
        }
        if (payload_len > 0) {
          memcpy(ws->frag + ws->frag_len, payload, payload_len);
        }
        ws->frag_len += payload_len;
        msg = ws->frag;
        msg_len = ws->frag_len;
        msg_opcode = ws->frag_opcode;
      }
      if (!fin) {
        /* Non-final continuation: appended above, nothing to dispatch. */
      } else if (msg_opcode == 1 &&
                 !ws_utf8_valid((const unsigned char *)msg, msg_len)) {
        ws_fail(ws, 1007);
        return;
      } else {
        ws_dispatch_event(ws, 1, msg_opcode == 1, msg, msg_len, 0, NULL, 0);
      }
      if (opcode == 0 && fin) {
        free(ws->frag);
        ws->frag = NULL;
        ws->frag_len = 0;
        ws->frag_cap = 0;
        ws->frag_opcode = -1;
      }
    } else {
      /* First fragment of a fragmented message. */
      if (payload_len > 0) {
        if (payload_len > ws->frag_cap) {
          char *next = (char *)malloc(payload_len);
          if (!next) { ws_fail(ws, 1009); return; }
          free(ws->frag);
          ws->frag = next;
          ws->frag_cap = payload_len;
        }
        memcpy(ws->frag, payload, payload_len);
      }
      ws->frag_len = payload_len;
      ws->frag_opcode = opcode;
    }
    if (conn->len > frame_len) {
      memmove(conn->buf, conn->buf + frame_len, conn->len - frame_len);
    }
    conn->len -= frame_len;
    if (conn->len == 0) { return; }
  }
}

int32_t avant_http_req_is_ws(void *reqp) {
  return ((AvantHttpReq *)reqp)->is_ws;
}

int32_t avant_http_req_ws_id(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  return req->ws_conn ? req->ws_conn->id : 0;
}

int32_t avant_http_req_ws_upgrade(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  AvantHttpServer *srv = req->srv;
  AvantWsConn *ws;
  char accept[29];
  char response[256];
  int response_len;
  size_t path_len = 0;
  if (!req->is_ws || req->upgraded) { return 0; }
  ws = (AvantWsConn *)calloc(1, sizeof(AvantWsConn));
  if (!ws) { return 0; }
  while (path_len < req->target_len && req->target[path_len] != '?') {
    path_len++;
  }
  ws->path = (char *)malloc(path_len + 1);
  if (!ws->path) {
    free(ws);
    return 0;
  }
  memcpy(ws->path, req->target, path_len);
  ws->path[path_len] = 0;
  ws_accept_key(req->ws_key, strlen(req->ws_key), accept);
  response_len = snprintf(response, sizeof(response),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
  if (response_len < 0 ||
      us_socket_write(0, req->sock, response, response_len, 0) != response_len) {
    free(ws->path);
    free(ws);
    return 0;
  }
  ws->srv = srv;
  ws->loop = loop_of_socket(req->sock);
  ws->sock = req->sock;
  ws->conn = req->conn;
  ws->owner_tid = pthread_self();
  ws->frag_opcode = -1;
  pthread_mutex_init(&ws->qmu, NULL);
  pthread_mutex_lock(&g_ws_mu);
  ws->id = ++g_next_ws_id;
  ws->next_all = srv->ws_all;
  srv->ws_all = ws;
  ws->next_global = g_ws_all;
  g_ws_all = ws;
  pthread_mutex_unlock(&g_ws_mu);
  req->conn->ws = ws;
  req->ws_conn = ws;
  req->upgraded = 1;
  us_socket_timeout(0, req->sock, 0);
  return 1;
}

static void conn_process(Conn *conn, struct us_socket_t *s) {
  AvantHttpServer *srv = conn->srv;

  if (conn->answered) {
    return;
  }
  for (;;) {
    const char *method = NULL;
    const char *target = NULL;
    size_t method_len = 0;
    size_t target_len = 0;
    int minor_version = 0;
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t num_headers = HTTP_MAX_HEADERS;
    int pret;
    const char *value = NULL;
    size_t value_len = 0;
    long content_length = 0;
    int has_cl = 0;
    int keep_alive;
    size_t total;
    AvantHttpReq req;

    pret = phr_parse_request(conn->buf, conn->len, &method, &method_len,
                             &target, &target_len, &minor_version, headers,
                             &num_headers, 0);
    if (pret == -2) {
      if (conn->len > HTTP_MAX_HEADER_BLOCK) {
        send_error(conn, s, 400);
      }
      return;
    }
    if (pret < 0) {
      send_error(conn, s, 400);
      return;
    }
    if (find_header(headers, num_headers, "Transfer-Encoding", &value,
                    &value_len)) {
      send_error(conn, s, 411);
      return;
    }
    if (find_header(headers, num_headers, "Content-Length", &value,
                    &value_len)) {
      if (!parse_content_length(value, value_len, &content_length)) {
        send_error(conn, s, 400);
        return;
      }
      has_cl = 1;
    }
    if (has_cl && content_length > srv->max_body) {
      send_error(conn, s, 413);
      return;
    }
    total = (size_t)pret + (size_t)(has_cl ? content_length : 0);
    if (conn->len < total) {
      if (find_header(headers, num_headers, "Expect", &value, &value_len) &&
          ascii_ieq(value, value_len, "100-continue", 12) &&
          !conn->sent_continue) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        us_socket_write(0, s, cont, (int)strlen(cont), 0);
        conn->sent_continue = 1;
      }
      return;
    }
    keep_alive = minor_version == 1;
    if (find_header(headers, num_headers, "Connection", &value, &value_len)) {
      if (ascii_ieq_contains(value, value_len, "close")) {
        keep_alive = 0;
      } else if (ascii_ieq_contains(value, value_len, "keep-alive")) {
        keep_alive = 1;
      }
    }
    /* WebSocket upgrade (Q135 A): a request carrying an Upgrade header
     * must be a well-formed RFC 6455 handshake or it is answered 400
     * here; a well-formed one is flagged for the Avant dispatch, which
     * decides from its route table whether to upgrade. */
    {
      const char *up_value = NULL;
      size_t up_len = 0;
      if (find_header(headers, num_headers, "Upgrade", &up_value, &up_len) &&
          ascii_ieq_contains(up_value, up_len, "websocket")) {
        const char *hv = NULL;
        size_t hv_len = 0;
        int ok = 1;
        if (method_len != 3 || memcmp(method, "GET", 3) != 0) { ok = 0; }
        if (minor_version != 1) { ok = 0; }
        if (!find_header(headers, num_headers, "Connection", &hv, &hv_len) ||
            !ascii_ieq_contains(hv, hv_len, "upgrade")) {
          ok = 0;
        }
        if (!find_header(headers, num_headers, "Sec-WebSocket-Version",
                         &hv, &hv_len) ||
            !ascii_ieq(hv, hv_len, "13", 2)) {
          ok = 0;
        }
        if (!find_header(headers, num_headers, "Sec-WebSocket-Key", &hv,
                         &hv_len) ||
            !ws_key_valid(hv, hv_len)) {
          ok = 0;
        }
        if (!ok) {
          send_error(conn, s, 400);
          return;
        }
        /* Stash the key for the upgrade primitive (req is memset next). */
        {
          size_t klen = hv_len < sizeof(req.ws_key) - 1 ? hv_len
                                                        : sizeof(req.ws_key) - 1;
          memset(&req, 0, sizeof(req));
          memcpy(req.ws_key, hv, klen);
          req.ws_key[klen] = 0;
          req.is_ws = 1;
          goto req_filled;
        }
      }
    }
    memset(&req, 0, sizeof(req));
  req_filled:
    req.srv = srv;
    req.conn = conn;
    req.sock = s;
    req.method = method;
    req.method_len = method_len;
    req.target = target;
    req.target_len = target_len;
    memcpy(req.headers, headers, sizeof(struct phr_header) * num_headers);
    req.num_headers = num_headers;
    req.body = conn->buf + pret;
    req.body_len = (size_t)(has_cl ? content_length : 0);
    req.keep_alive = keep_alive;
    req.minor_version = minor_version;
    enter_avant();
    srv->dispatch(srv->state, &req);
    leave_avant();
    if (req.upgraded) {
      /* The 101 is written and the connection is a WebSocket now. Any
       * bytes past the handshake are frames already in the buffer. */
      if (conn->len > (size_t)pret) {
        memmove(conn->buf, conn->buf + pret, conn->len - (size_t)pret);
      }
      conn->len -= (size_t)pret;
      conn->sent_continue = 0;
      if (conn->len > 0) {
        ws_process(conn, s);
      }
      return;
    }
    if (!keep_alive) {
      conn->answered = 1;
      us_socket_shutdown(0, s);
      return;
    }
    /* The dispatch copied what it kept; consume this request and look
     * for a pipelined one already in the buffer. */
    if (conn->len > total) {
      memmove(conn->buf, conn->buf + total, conn->len - total);
    }
    conn->len -= total;
    conn->sent_continue = 0;
    if (conn->len == 0) {
      return;
    }
  }
}

static struct us_socket_t *srv_on_open(struct us_socket_t *s, int is_client,
                                       char *ip, int ip_length) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  HttpLoop *hl = loop_of_socket(s);
  (void)is_client;
  (void)ip;
  (void)ip_length;
  memset(node, 0, sizeof(ConnNode));
  node->conn.srv = hl->srv;
  node->sock = s;
  node->next = hl->conns_head;
  hl->conns_head = node;
  us_socket_timeout(0, s, 30);
  return s;
}

static struct us_socket_t *srv_on_data(struct us_socket_t *s, char *data,
                                       int length) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  Conn *conn = &node->conn;
  if (conn->answered) {
    return s;
  }
  if (conn->len + (size_t)length > conn->cap) {
    size_t want = conn->cap ? conn->cap : 8192;
    char *next;
    while (want < conn->len + (size_t)length) {
      want *= 2;
    }
    next = (char *)realloc(conn->buf, want);
    if (!next) {
      send_error(conn, s, 500);
      return s;
    }
    conn->buf = next;
    conn->cap = want;
  }
  memcpy(conn->buf + conn->len, data, (size_t)length);
  conn->len += (size_t)length;
  if (conn->ws) {
    ws_process(conn, s);
  } else {
    conn_process(conn, s);
  }
  return s;
}

static void ws_free(AvantWsConn *ws) {
  WsOut *node = ws->out_head;
  while (node) {
    WsOut *next = node->next;
    free(node->data);
    free(node);
    node = next;
  }
  free(ws->frag);
  free(ws->path);
  pthread_mutex_destroy(&ws->qmu);
  free(ws);
}

/* Close-time release: buffers and queue go back, but the shell stays
 * allocated, marked dead, on the server's dead list until teardown,
 * so a WsConn value an Avant program still holds can be sent on and
 * answered 0 rather than dereferencing freed memory. */
static void ws_release_shell(AvantWsConn *ws) {
  WsOut *node = ws->out_head;
  while (node) {
    WsOut *next = node->next;
    free(node->data);
    free(node);
    node = next;
  }
  ws->out_head = NULL;
  ws->out_tail = NULL;
  ws->out_bytes = 0;
  free(ws->frag);
  ws->frag = NULL;
  ws->frag_len = 0;
  ws->frag_cap = 0;
  free(ws->path);
  ws->path = NULL;
  ws->dead = 1;
  ws->sock = NULL;
  ws->next_all = ws->srv->ws_dead;
  ws->srv->ws_dead = ws;
}

static struct us_socket_t *srv_on_close(struct us_socket_t *s, int code,
                                        void *reason) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  HttpLoop *hl = loop_of_socket(s);
  ConnNode **link;
  (void)code;
  (void)reason;
  if (node->conn.ws) {
    AvantWsConn *ws = node->conn.ws;
    AvantHttpServer *srv = ws->srv;
    node->conn.ws = NULL;
    if (!ws->event_dispatched) {
      int close_code = ws->pending_close_code ? ws->pending_close_code
                       : ws->close_received  ? ws->received_code
                                             : 1006;
      ws->event_dispatched = 1;
      ws->dead = 1;
      ws_dispatch_event(ws, 2, 0, NULL, 0, close_code, NULL, 0);
    }
    pthread_mutex_lock(&g_ws_mu);
    {
      AvantWsConn **link2 = &srv->ws_all;
      while (*link2 && *link2 != ws) {
        link2 = &(*link2)->next_all;
      }
      if (*link2 == ws) {
        *link2 = ws->next_all;
      }
      link2 = &g_ws_all;
      while (*link2 && *link2 != ws) {
        link2 = &(*link2)->next_global;
      }
      if (*link2 == ws) {
        *link2 = ws->next_global;
      }
    }
    pthread_mutex_unlock(&g_ws_mu);
    ws_release_shell(ws);
  }
  free(node->conn.buf);
  node->conn.buf = NULL;
  if (hl) {
    link = &hl->conns_head;
    while (*link && *link != node) {
      link = &(*link)->next;
    }
    if (*link == node) {
      *link = node->next;
    }
  }
  return s;
}

static struct us_socket_t *srv_on_end(struct us_socket_t *s) {
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *srv_on_timeout(struct us_socket_t *s) {
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *srv_on_writable(struct us_socket_t *s) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  if (node->conn.ws) {
    ws_flush(node->conn.ws);
  }
  return s;
}

static void srv_on_wakeup(struct us_loop_t *loop) {
  HttpLoop *hl = *(HttpLoop **)us_loop_ext(loop);
  if (!hl) {
    return;
  }
  if (!hl->srv->closing) {
    /* A cross-thread WebSocket send queued frames and woke the loop. */
    ConnNode *node = hl->conns_head;
    while (node) {
      if (node->conn.ws) {
        ws_flush(node->conn.ws);
      }
      node = node->next;
    }
    return;
  }
  if (hl->listen) {
    us_listen_socket_close(0, hl->listen);
    hl->listen = NULL;
  }
  {
    /* Answered connections are already shut down with their response
     * queued ahead of the FIN; leave them to close from the peer side
     * so the last response is not discarded. Idle connections are
     * closed here so the loop can drain. Open WebSocket connections
     * first get a 1001 close frame (Q133 A) and the same leave-to-peer
     * treatment. */
    ConnNode *node = hl->conns_head;
    while (node) {
      ConnNode *next = node->next;
      if (node->conn.ws && !node->conn.ws->close_sent) {
        unsigned char payload[2];
        node->conn.ws->close_sent = 1;
        node->conn.ws->shutdown_after_flush = 1;
        payload[0] = (unsigned char)(1001 >> 8);
        payload[1] = (unsigned char)1001;
        ws_send_frame(node->conn.ws, 8, (const char *)payload, 2);
        ws_flush(node->conn.ws);
        node->conn.answered = 1;
      } else if (node->conn.ws) {
        node->conn.answered = 1;
      }
      if (!node->conn.answered) {
        us_socket_close(0, node->sock, 0, NULL);
      }
      node = next;
    }
  }
}

static void srv_pre(struct us_loop_t *loop) {
  (void)loop;
}

static void srv_post(struct us_loop_t *loop) {
  (void)loop;
}

/* Bound (host, port) pairs owned by live servers in this process.
 * SO_REUSEPORT lets every loop of one server share its port, but it
 * would also let a second server bind the same port; the wave-5
 * contract is that a second listen fails. The registry keeps the
 * sharing intra-server. */
typedef struct ListenReg {
  char host[64];
  int32_t port;
  AvantHttpServer *srv;
} ListenReg;

static pthread_mutex_t g_listen_mu = PTHREAD_MUTEX_INITIALIZER;
static ListenReg *g_listen_regs;
static int g_listen_n;
static int g_listen_cap;

static int listen_port_taken(const char *host, int32_t port,
                             AvantHttpServer *self) {
  int i;
  int taken = 0;
  pthread_mutex_lock(&g_listen_mu);
  for (i = 0; i < g_listen_n; i++) {
    if (g_listen_regs[i].srv != self &&
        g_listen_regs[i].port == port &&
        strcmp(g_listen_regs[i].host, host) == 0) {
      taken = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_listen_mu);
  return taken;
}

static void listen_reg_add(const char *host, int32_t port,
                           AvantHttpServer *srv) {
  pthread_mutex_lock(&g_listen_mu);
  if (g_listen_n == g_listen_cap) {
    int cap = g_listen_cap ? g_listen_cap * 2 : 8;
    ListenReg *next = (ListenReg *)realloc(g_listen_regs,
                                           (size_t)cap * sizeof(ListenReg));
    if (!next) {
      pthread_mutex_unlock(&g_listen_mu);
      return;
    }
    g_listen_regs = next;
    g_listen_cap = cap;
  }
  snprintf(g_listen_regs[g_listen_n].host,
           sizeof(g_listen_regs[g_listen_n].host), "%s", host);
  g_listen_regs[g_listen_n].port = port;
  g_listen_regs[g_listen_n].srv = srv;
  g_listen_n++;
  pthread_mutex_unlock(&g_listen_mu);
}

static void listen_reg_remove(AvantHttpServer *srv) {
  int i;
  pthread_mutex_lock(&g_listen_mu);
  for (i = 0; i < g_listen_n; i++) {
    if (g_listen_regs[i].srv == srv) {
      g_listen_n--;
      g_listen_regs[i] = g_listen_regs[g_listen_n];
      break;
    }
  }
  pthread_mutex_unlock(&g_listen_mu);
}

static void loop_teardown(HttpLoop *hl) {
  if (hl->listen) {
    us_listen_socket_close(0, hl->listen);
    hl->listen = NULL;
  }
  if (hl->ctx) {
    us_socket_context_free(0, hl->ctx);
    hl->ctx = NULL;
  }
  if (hl->loop) {
    us_loop_free(hl->loop);
    hl->loop = NULL;
  }
}

static void srv_teardown(AvantHttpServer *srv) {
  int i;
  listen_reg_remove(srv);
  /* Live connections free their own WsConn in srv_on_close; anything
   * registered here at teardown belongs to a socket already gone. */
  pthread_mutex_lock(&g_ws_mu);
  while (srv->ws_all) {
    AvantWsConn *ws = srv->ws_all;
    AvantWsConn **link2 = &g_ws_all;
    srv->ws_all = ws->next_all;
    while (*link2 && *link2 != ws) {
      link2 = &(*link2)->next_global;
    }
    if (*link2 == ws) {
      *link2 = ws->next_global;
    }
    ws_free(ws);
  }
  pthread_mutex_unlock(&g_ws_mu);
  while (srv->ws_dead) {
    AvantWsConn *ws = srv->ws_dead;
    srv->ws_dead = ws->next_all;
    ws_free(ws);
  }
  for (i = 0; i < srv->nloops; i++) {
    loop_teardown(&srv->loops[i]);
  }
  free(srv->loops);
  free(srv);
}

/* Creates one loop; binds it to `port` (loop 0 may pass 0 and learn
 * the ephemeral port, later loops reuse srv->port). Returns 0 ok. */
static int loop_create(AvantHttpServer *srv, HttpLoop *hl, const char *host,
                       int32_t port) {
  struct us_socket_context_options_t options;
  hl->srv = srv;
  hl->loop = us_create_loop(0, srv_on_wakeup, srv_pre, srv_post,
                            (unsigned int)sizeof(HttpLoop *));
  if (!hl->loop) {
    return 1;
  }
  *(HttpLoop **)us_loop_ext(hl->loop) = hl;
  memset(&options, 0, sizeof(options));
  hl->ctx = us_create_socket_context(0, hl->loop, (int)sizeof(HttpLoop *),
                                     options);
  if (!hl->ctx) {
    return 1;
  }
  *(HttpLoop **)us_socket_context_ext(0, hl->ctx) = hl;
  us_socket_context_on_open(0, hl->ctx, srv_on_open);
  us_socket_context_on_data(0, hl->ctx, srv_on_data);
  us_socket_context_on_writable(0, hl->ctx, srv_on_writable);
  us_socket_context_on_close(0, hl->ctx, srv_on_close);
  us_socket_context_on_timeout(0, hl->ctx, srv_on_timeout);
  us_socket_context_on_end(0, hl->ctx, srv_on_end);
  hl->listen = us_socket_context_listen(0, hl->ctx, host, (int)port, 0,
                                        (int)sizeof(ConnNode));
  if (!hl->listen) {
    return 1;
  }
  return 0;
}

/* Picks a free loopback port for an ephemeral (0) request: the
 * vendored listen path enables SO_REUSEPORT only for a non-zero
 * port, so every loop must bind a concrete port for all of them to
 * share it. The probe socket is closed before the loops bind; the
 * race window is the usual one for port 0. */
static int32_t pick_ephemeral_port(const char *host) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  int32_t port = 0;
  if (fd < 0) {
    return 0;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0) {
    struct in_addr ia;
    if (inet_pton(AF_INET, host, &ia) == 1) {
      addr.sin_addr = ia;
    }
  }
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
      getsockname(fd, (struct sockaddr *)&addr, &len) == 0) {
    port = (int32_t)ntohs(addr.sin_port);
  }
  close(fd);
  return port;
}

/* `loops` <= 0 means the runtime's chosen thread count (Q124-A). */
void *avant_http_listen(const char *host, int32_t port, int32_t max_body,
                        int32_t loops, int32_t max_message) {
  AvantHttpServer *srv;
  int n;
  int i;
  set_status(0);
  if (port == 0) {
    port = pick_ephemeral_port(host);
    if (port == 0) {
      set_status(1);
      return NULL;
    }
  }
  n = loops > 0 ? loops : avant_chosen_threads();
  if (n < 1) {
    n = 1;
  }
  srv = (AvantHttpServer *)calloc(1, sizeof(AvantHttpServer));
  if (!srv) {
    set_status(4);
    return NULL;
  }
  srv->max_body = max_body;
  srv->max_message = max_message > 0 ? max_message : 1048576;
  srv->loops = (HttpLoop *)calloc((size_t)n, sizeof(HttpLoop));
  if (!srv->loops) {
    free(srv);
    set_status(4);
    return NULL;
  }
  srv->nloops = n;
  if (listen_port_taken(host, port, srv)) {
    free(srv->loops);
    free(srv);
    set_status(1);
    return NULL;
  }
  for (i = 0; i < n; i++) {
    int32_t p = i == 0 ? port : srv->port;
    if (loop_create(srv, &srv->loops[i], host, p) != 0) {
      srv->nloops = i + 1;
      srv_teardown(srv);
      set_status(1);
      return NULL;
    }
    if (i == 0) {
      srv->port = us_socket_local_port(
          0, (struct us_socket_t *)srv->loops[0].listen);
    }
  }
  listen_reg_add(host, srv->port, srv);
  return srv;
}

int32_t avant_http_port(void *srvp) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  return srv ? srv->port : 0;
}

/* Loop threads follow the pool's registration model: register with
 * the collector before the first dispatch, allocate in the
 * non-moving mature space while serving, unregister after the loop
 * has drained (no Avant frame is active then). */
static void *loop_thread_main(void *arg) {
  HttpLoop *hl = (HttpLoop *)arg;
  avant_gc_register_thread();
  avant_alloc_in_mature(1);
  /* The C callback trampoline batches its roots into the current
   * frame without entering (it forwards to the entering dispatch
   * function), so a loop thread needs a base frame for the whole
   * serve, the way the serving thread already has its `serve` frame.
   * Its slots name the pinned state and a C request pointer, never a
   * nursery object, so scanning it between dispatches is safe. */
  avant_gc_enter();
  loop_run_native(hl->loop);
  avant_gc_leave();
  avant_gc_unregister_thread();
  return NULL;
}

int32_t avant_http_serve(void *srvp, void *state, void *cb, void *ws_cb) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  int i;
  if (!srv || !srv->loops || srv->nloops < 1) {
    set_status(1);
    return 1;
  }
  /* srv (C memory) holds the state across every dispatch, which
   * collects; with one loop the program can stay unshared, where a young
   * collection may move it (S+). Pinned for the serve (Stage 28 W16a). */
  avant_pin(state);
  srv->state = state;
  srv->dispatch = (AvantHttpDispatch)cb;
  srv->ws_dispatch = (AvantHttpDispatch)ws_cb;
  srv->running = 1;
  for (i = 1; i < srv->nloops; i++) {
    if (pthread_create(&srv->loops[i].thread, NULL, loop_thread_main,
                       &srv->loops[i]) == 0) {
      srv->loops[i].spawned = 1;
    } else {
      /* A loop that could not start still has its listen socket
       * closed by close/teardown; serving continues on fewer loops. */
      srv->loops[i].spawned = 0;
    }
  }
  loop_run_native(srv->loops[0].loop);
  avant_gc_native_begin();
  for (i = 1; i < srv->nloops; i++) {
    if (srv->loops[i].spawned) {
      pthread_join(srv->loops[i].thread, NULL);
      srv->loops[i].spawned = 0;
    }
  }
  avant_gc_native_end();
  srv->running = 0;
  srv_teardown(srv);
  avant_unpin(state);
  return 0;
}

int32_t avant_http_close(void *srvp) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  int i;
  if (!srv) {
    return 0;
  }
  if (srv->running) {
    srv->closing = 1;
    for (i = 0; i < srv->nloops; i++) {
      if (srv->loops[i].loop) {
        us_wakeup_loop(srv->loops[i].loop);
      }
    }
    return 0;
  }
  srv_teardown(srv);
  return 0;
}

/* ---- WebSocket client ---- */

/* A blocking pull client: one C loop thread per connection, an inbox
 * queue the calling Avant thread drains with recv. The loop thread
 * never calls Avant code and never allocates Avant heap, so it needs
 * no collector registration; recv copies a queued message into one
 * collector Bytes on the caller's thread. Client frames are masked,
 * as RFC 6455 requires of clients. */
typedef struct WsClientMsg {
  int is_text;
  int is_close;
  char *data;
  size_t len;
  struct WsClientMsg *next;
} WsClientMsg;

typedef struct WsClient {
  struct us_loop_t *loop;
  struct us_socket_context_t *ctx;
  struct us_socket_t *sock;
  pthread_t thread;
  char *buf;
  size_t len;
  size_t cap;
  char *frag;
  size_t frag_len;
  size_t frag_cap;
  int frag_opcode;
  char expected_accept[29];
  int handshake_done;
  int handshake_ok;
  int failed;
  int closed;
  int close_sent;
  int close_code;
  char request[2048];
  int request_len;
  int want_close;
  WsClientMsg *in_head;
  WsClientMsg *in_tail;
  WsOut *out_head;
  WsOut *out_tail;
  pthread_mutex_t mu;
  pthread_cond_t cond;
} WsClient;

static void ws_client_inbox_push(WsClient *c, int is_text, int is_close,
                                 const char *data, size_t len) {
  WsClientMsg *m = (WsClientMsg *)calloc(1, sizeof(WsClientMsg));
  if (!m) { return; }
  m->is_text = is_text;
  m->is_close = is_close;
  if (len > 0) {
    m->data = (char *)malloc(len);
    if (!m->data) { free(m); return; }
    memcpy(m->data, data, len);
  }
  m->len = len;
  pthread_mutex_lock(&c->mu);
  if (c->in_tail) { c->in_tail->next = m; } else { c->in_head = m; }
  c->in_tail = m;
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->mu);
}

static void ws_client_flush(WsClient *c) {
  pthread_mutex_lock(&c->mu);
  while (c->out_head) {
    WsOut *node = c->out_head;
    size_t remaining = node->len - node->off;
    int written = us_socket_write(0, c->sock, node->data + node->off,
                                  (int)remaining, 0);
    if (written < 0) { written = 0; }
    node->off += (size_t)written;
    if (node->off < node->len) { break; }
    c->out_head = node->next;
    if (!c->out_head) { c->out_tail = NULL; }
    free(node->data);
    free(node);
  }
  pthread_mutex_unlock(&c->mu);
}

static void ws_client_random(unsigned char *out, size_t n) {
  FILE *f = fopen("/dev/urandom", "rb");
  size_t got = 0;
  if (f) {
    got = fread(out, 1, n, f);
    fclose(f);
  }
  while (got < n) {
    out[got] = (unsigned char)(rand() >> 7);
    got++;
  }
}

/* Queue one masked client frame. Loop thread flushes. */
static int ws_client_send_frame(WsClient *c, int opcode, const char *payload,
                                size_t len) {
  char header[14];
  size_t header_len = 0;
  unsigned char mask[4];
  char *framed;
  size_t framed_len;
  size_t i;
  WsOut *node;
  header[header_len++] = (char)(0x80 | (opcode & 0x0f));
  if (len < 126) {
    header[header_len++] = (char)(0x80 | len);
  } else if (len <= 65535) {
    header[header_len++] = (char)(0x80 | 126);
    header[header_len++] = (char)(len >> 8);
    header[header_len++] = (char)len;
  } else {
    int j;
    header[header_len++] = (char)(0x80 | 127);
    for (j = 7; j >= 0; j--) {
      header[header_len++] = (char)(len >> (j * 8));
    }
  }
  ws_client_random(mask, 4);
  memcpy(header + header_len, mask, 4);
  header_len += 4;
  framed_len = header_len + len;
  framed = (char *)malloc(framed_len ? framed_len : 1);
  if (!framed) { return 0; }
  memcpy(framed, header, header_len);
  for (i = 0; i < len; i++) {
    framed[header_len + i] = (char)(payload[i] ^ mask[i & 3]);
  }
  node = (WsOut *)calloc(1, sizeof(WsOut));
  if (!node) { free(framed); return 0; }
  node->data = framed;
  node->len = framed_len;
  pthread_mutex_lock(&c->mu);
  if (c->closed) {
    pthread_mutex_unlock(&c->mu);
    free(framed);
    free(node);
    return 0;
  }
  if (c->out_tail) { c->out_tail->next = node; } else { c->out_head = node; }
  c->out_tail = node;
  pthread_mutex_unlock(&c->mu);
  if (c->loop) { us_wakeup_loop(c->loop); }
  return 1;
}

static WsClient *ws_client_of_socket(struct us_socket_t *s) {
  struct us_loop_t *loop = us_socket_context_loop(0, us_socket_context(0, s));
  return *(WsClient **)us_loop_ext(loop);
}

static void ws_client_process(WsClient *c) {
  for (;;) {
    unsigned char *buf = (unsigned char *)c->buf;
    size_t len = c->len;
    if (!c->handshake_done) {
      char *end = NULL;
      size_t i;
      for (i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { end = (char *)buf + i; break; }
      }
      if (!end) {
        if (len > HTTP_MAX_HEADER_BLOCK) { c->failed = 1; }
        return;
      }
      {
        size_t head_len = (size_t)(end - (char *)buf) + 4;
        char head[HTTP_MAX_HEADER_BLOCK + 1];
        size_t hlen = head_len < sizeof(head) - 1 ? head_len : sizeof(head) - 1;
        memcpy(head, buf, hlen);
        head[hlen] = 0;
        if (!strstr(head, " 101 ") && !strstr(head, " 101\r")) {
          c->failed = 1;
        } else if (!strstr(head, c->expected_accept)) {
          c->failed = 1;
        }
        if (c->len > head_len) {
          memmove(c->buf, c->buf + head_len, c->len - head_len);
        }
        c->len -= head_len;
        c->handshake_done = 1;
        c->handshake_ok = c->failed == 0;
        pthread_mutex_lock(&c->mu);
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->mu);
        if (c->failed) { return; }
        continue;
      }
    }
    if (len < 2) { return; }
    {
      int fin = (buf[0] & 0x80) != 0;
      int opcode = buf[0] & 0x0f;
      size_t payload_len = buf[1] & 0x7f;
      size_t header_len = 2;
      size_t frame_len;
      unsigned char *payload;
      if (buf[1] & 0x80) { c->failed = 1; c->closed = 1; return; }
      if (payload_len == 126) {
        if (len < 4) { return; }
        payload_len = ((size_t)buf[2] << 8) | buf[3];
        header_len = 4;
      } else if (payload_len == 127) {
        uint64_t v = 0;
        int j;
        if (len < 10) { return; }
        for (j = 0; j < 8; j++) { v = (v << 8) | buf[2 + j]; }
        if (v > (uint64_t)1 << 62) { c->failed = 1; return; }
        payload_len = (size_t)v;
        header_len = 10;
      }
      frame_len = header_len + payload_len;
      if (len < frame_len) { return; }
      payload = buf + header_len;
      if (opcode == 9) {
        ws_client_send_frame(c, 10, (const char *)payload, payload_len);
      } else if (opcode == 8) {
        if (payload_len >= 2) {
          c->close_code = ((int)payload[0] << 8) | payload[1];
        }
        c->closed = 1;
        if (!c->close_sent) {
          c->close_sent = 1;
          ws_client_send_frame(c, 8, (const char *)payload, payload_len);
        }
        ws_client_inbox_push(c, 0, 1, NULL, 0);
      } else if (opcode == 1 || opcode == 2 || opcode == 0) {
        if (opcode == 0 || !fin) {
          if (c->frag_len + payload_len > c->frag_cap) {
            size_t want = c->frag_cap ? c->frag_cap * 2 : 1024;
            char *next;
            while (want < c->frag_len + payload_len) { want *= 2; }
            next = (char *)realloc(c->frag, want);
            if (!next) { c->failed = 1; return; }
            c->frag = next;
            c->frag_cap = want;
          }
          if (opcode != 0) { c->frag_opcode = opcode; }
          if (payload_len > 0) {
            memcpy(c->frag + c->frag_len, payload, payload_len);
          }
          c->frag_len += payload_len;
          if (fin) {
            ws_client_inbox_push(c, c->frag_opcode == 1, 0, c->frag,
                                 c->frag_len);
            c->frag_len = 0;
            c->frag_opcode = -1;
          }
        } else {
          ws_client_inbox_push(c, opcode == 1, 0, (const char *)payload,
                               payload_len);
        }
      }
      if (c->len > frame_len) {
        memmove(c->buf, c->buf + frame_len, c->len - frame_len);
      }
      c->len -= frame_len;
      if (c->len == 0) { return; }
    }
  }
}

static struct us_socket_t *ws_client_on_open(struct us_socket_t *s,
                                             int is_client, char *ip,
                                             int ip_length) {
  WsClient *c = ws_client_of_socket(s);
  (void)is_client;
  (void)ip;
  (void)ip_length;
  c->sock = s;
  /* The handshake goes out only now: writing on a still-connecting
   * socket corrupts the semi-socket state machine (the fetch rule). */
  if (c->request_len > 0) {
    us_socket_write(0, s, c->request, c->request_len, 0);
    c->request_len = 0;
  }
  return s;
}

static struct us_socket_t *ws_client_on_data(struct us_socket_t *s, char *data,
                                             int length) {
  WsClient *c = ws_client_of_socket(s);
  if (c->len + (size_t)length > c->cap) {
    size_t want = c->cap ? c->cap : 8192;
    char *next;
    while (want < c->len + (size_t)length) { want *= 2; }
    next = (char *)realloc(c->buf, want);
    if (!next) { c->failed = 1; return s; }
    c->buf = next;
    c->cap = want;
  }
  memcpy(c->buf + c->len, data, (size_t)length);
  c->len += (size_t)length;
  ws_client_process(c);
  return s;
}

static struct us_socket_t *ws_client_on_close(struct us_socket_t *s, int code,
                                              void *reason) {
  WsClient *c = ws_client_of_socket(s);
  (void)code;
  (void)reason;
  c->closed = 1;
  ws_client_inbox_push(c, 0, 1, NULL, 0);
  return s;
}

static struct us_socket_t *ws_client_on_end(struct us_socket_t *s) {
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *ws_client_on_timeout(struct us_socket_t *s) {
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *ws_client_on_writable(struct us_socket_t *s) {
  ws_client_flush(ws_client_of_socket(s));
  return s;
}

static struct us_socket_t *ws_client_on_connect_error(struct us_socket_t *s,
                                                      int code) {
  WsClient *c = ws_client_of_socket(s);
  (void)code;
  c->failed = 1;
  c->closed = 1;
  pthread_mutex_lock(&c->mu);
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mu);
  return us_socket_close(0, s, 0, NULL);
}

static void ws_client_wakeup(struct us_loop_t *loop) {
  WsClient *c = *(WsClient **)us_loop_ext(loop);
  if (!c) { return; }
  if (c->want_close) {
    if (c->sock && !us_socket_is_closed(0, c->sock)) {
      ws_client_flush(c);
      us_socket_close(0, c->sock, 0, NULL);
    }
    return;
  }
  if (c->sock && !us_socket_is_closed(0, c->sock)) {
    ws_client_flush(c);
  }
}

static void *ws_client_thread_main(void *arg) {
  WsClient *c = (WsClient *)arg;
  us_loop_run(c->loop);
  return NULL;
}

static void ws_client_free(WsClient *c) {
  WsClientMsg *m = c->in_head;
  while (m) {
    WsClientMsg *next = m->next;
    free(m->data);
    free(m);
    m = next;
  }
  while (c->out_head) {
    WsOut *next = c->out_head->next;
    free(c->out_head->data);
    free(c->out_head);
    c->out_head = next;
  }
  if (c->ctx) { us_socket_context_free(0, c->ctx); }
  if (c->loop) { us_loop_free(c->loop); }
  free(c->buf);
  free(c->frag);
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cond);
  free(c);
}

static int ws_client_alive(void *handle);
static void ws_client_register(WsClient *c);
static int ws_client_unregister(void *handle);

/* Status through avant_http_take_status: 1 connect failure, 2 timeout,
 * 3 invalid handshake. */
void *avant_ws_client_connect(const char *host, int32_t port,
                              const char *target, const char *key_b64,
                              int32_t timeout_ms) {
  WsClient *c;
  struct us_socket_context_options_t options;
  char request[2048];
  int request_len;
  struct timespec deadline;
  set_status(0);
  c = (WsClient *)calloc(1, sizeof(WsClient));
  if (!c) { set_status(4); return NULL; }
  c->frag_opcode = -1;
  c->close_code = 1006;
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cond, NULL);
  ws_accept_key(key_b64, strlen(key_b64), c->expected_accept);
  c->loop = us_create_loop(0, ws_client_wakeup, srv_pre, srv_post,
                           (unsigned int)sizeof(WsClient *));
  if (!c->loop) { ws_client_free(c); set_status(1); return NULL; }
  *(WsClient **)us_loop_ext(c->loop) = c;
  memset(&options, 0, sizeof(options));
  c->ctx = us_create_socket_context(0, c->loop, 0, options);
  if (!c->ctx) { ws_client_free(c); set_status(1); return NULL; }
  us_socket_context_on_open(0, c->ctx, ws_client_on_open);
  us_socket_context_on_data(0, c->ctx, ws_client_on_data);
  us_socket_context_on_writable(0, c->ctx, ws_client_on_writable);
  us_socket_context_on_close(0, c->ctx, ws_client_on_close);
  us_socket_context_on_timeout(0, c->ctx, ws_client_on_timeout);
  us_socket_context_on_end(0, c->ctx, ws_client_on_end);
  us_socket_context_on_connect_error(0, c->ctx, ws_client_on_connect_error);
  request_len = snprintf(request, sizeof(request),
                         "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
                         "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                         "Sec-WebSocket-Key: %s\r\n"
                         "Sec-WebSocket-Version: 13\r\n\r\n",
                         target, host, (int)port, key_b64);
  memcpy(c->request, request, (size_t)request_len);
  c->request_len = request_len;
  {
    struct us_socket_t *sock = us_socket_context_connect(
        0, c->ctx, host, (int)port, NULL, 0, 0);
    if (!sock) {
      ws_client_free(c);
      set_status(1);
      return NULL;
    }
    c->sock = sock;
  }
  if (pthread_create(&c->thread, NULL, ws_client_thread_main, c) != 0) {
    ws_client_free(c);
    set_status(1);
    return NULL;
  }
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms <= 0 ? 10 : timeout_ms / 1000;
  deadline.tv_nsec += (timeout_ms <= 0 ? 0 : (timeout_ms % 1000)) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  /* The handshake wait is native (Stage 28 W17a, rule 2); host, target
   * and the key are not read after it. */
  avant_gc_lock(&c->mu);
  while (!c->handshake_done && !c->failed && !c->closed) {
    if (avant_gc_cond_timedwait(&c->cond, &c->mu, &deadline) != 0) {
      break;
    }
  }
  {
    int ok = c->handshake_ok;
    int failed = c->failed;
    pthread_mutex_unlock(&c->mu);
    if (!ok) {
      c->closed = 1;
      c->want_close = 1;
      us_wakeup_loop(c->loop);
      avant_gc_native_begin();
      pthread_join(c->thread, NULL);
      avant_gc_native_end();
      ws_client_free(c);
      set_status(failed ? 1 : 2);
      return NULL;
    }
  }
  ws_client_register(c);
  return c;
}

static __thread int32_t g_ws_client_last_is_text;

/* Live client handles, so a second close on a copied WsConnection
 * value is a no-op instead of a use-after-free: membership is tested
 * by pointer value alone, without dereferencing the handle. */
typedef struct WsClientReg {
  WsClient *c;
  struct WsClientReg *next;
} WsClientReg;

static pthread_mutex_t g_ws_client_mu = PTHREAD_MUTEX_INITIALIZER;
static WsClientReg *g_ws_clients;

static int ws_client_alive(void *handle) {
  WsClientReg *r;
  int found = 0;
  pthread_mutex_lock(&g_ws_client_mu);
  for (r = g_ws_clients; r; r = r->next) {
    if (r->c == (WsClient *)handle) { found = 1; break; }
  }
  pthread_mutex_unlock(&g_ws_client_mu);
  return found;
}

static void ws_client_register(WsClient *c) {
  WsClientReg *r = (WsClientReg *)malloc(sizeof(WsClientReg));
  if (!r) { return; }
  r->c = c;
  pthread_mutex_lock(&g_ws_client_mu);
  r->next = g_ws_clients;
  g_ws_clients = r;
  pthread_mutex_unlock(&g_ws_client_mu);
}

static int ws_client_unregister(void *handle) {
  WsClientReg **link = &g_ws_clients;
  int found = 0;
  pthread_mutex_lock(&g_ws_client_mu);
  while (*link) {
    if ((*link)->c == (WsClient *)handle) {
      WsClientReg *dead = *link;
      *link = dead->next;
      free(dead);
      found = 1;
      break;
    }
    link = &(*link)->next;
  }
  pthread_mutex_unlock(&g_ws_client_mu);
  return found;
}

int32_t avant_ws_client_last_is_text(void) { return g_ws_client_last_is_text; }

int32_t avant_ws_client_close_code(void *handle) {
  WsClient *c = (WsClient *)handle;
  if (!c || !ws_client_alive(handle)) { return 1006; }
  return c->close_code;
}

/* One queued message as Bytes; status 1 when the connection is closed
 * with nothing queued, 2 on timeout. timeout_ms < 0 waits forever. */
/* gc-lint: holds nothing (the message is C memory; nothing heap is read after make_bytes). */
char *avant_ws_client_recv(void *handle, int32_t timeout_ms) {
  WsClient *c = (WsClient *)handle;
  WsClientMsg *m;
  char *out;
  set_status(0);
  if (!c || !ws_client_alive(handle)) {
    set_status(1);
    return avant_str_empty();
  }
  /* The receive wait is native (Stage 28 W17a, rule 2): the message is
   * C memory. */
  avant_gc_lock(&c->mu);
  if (timeout_ms < 0) {
    while (!c->in_head && !c->closed) {
      avant_gc_cond_wait(&c->cond, &c->mu);
    }
  } else {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    while (!c->in_head && !c->closed) {
      if (avant_gc_cond_timedwait(&c->cond, &c->mu, &deadline) != 0) {
        break;
      }
    }
  }
  m = c->in_head;
  if (m) {
    c->in_head = m->next;
    if (!c->in_head) { c->in_tail = NULL; }
  }
  pthread_mutex_unlock(&c->mu);
  if (!m) {
    set_status(c->closed ? 1 : 2);
    return avant_str_empty();
  }
  if (m->is_close) {
    free(m->data);
    free(m);
    set_status(1);
    return avant_str_empty();
  }
  g_ws_client_last_is_text = m->is_text;
  out = make_bytes(m->data ? m->data : "", m->len);
  free(m->data);
  free(m);
  return out;
}

int32_t avant_ws_client_send_text(void *handle, const char *data) {
  WsClient *c = (WsClient *)handle;
  int ok;
  if (!c || !ws_client_alive(handle)) { return 0; }
  ok = ws_client_send_frame(c, 1, data, avant_str_n(data));
  return ok;
}

int32_t avant_ws_client_send_bytes(void *handle, const char *data) {
  WsClient *c = (WsClient *)handle;
  int ok;
  if (!c || !ws_client_alive(handle)) { return 0; }
  ok = ws_client_send_frame(c, 2, data, avant_str_n(data));
  return ok;
}

/* A fresh Sec-WebSocket-Key: 16 random bytes, base64. */
char *avant_ws_client_key(void) {
  unsigned char raw[16];
  char out[25];
  int o = 0;
  int i;
  ws_client_random(raw, sizeof(raw));
  for (i = 0; i + 3 <= 16; i += 3) {
    uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8) |
                 raw[i + 2];
    out[o++] = ws_b64[(v >> 18) & 63];
    out[o++] = ws_b64[(v >> 12) & 63];
    out[o++] = ws_b64[(v >> 6) & 63];
    out[o++] = ws_b64[v & 63];
  }
  {
    uint32_t v = (uint32_t)raw[15] << 16;
    out[o++] = ws_b64[(v >> 18) & 63];
    out[o++] = ws_b64[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  }
  out[o] = 0;
  return make_string(out, (size_t)o);
}

int32_t avant_ws_client_close(void *handle) {
  WsClient *c = (WsClient *)handle;
  unsigned char payload[2] = {0x03, 0xe8};
  if (!c || !ws_client_unregister(handle)) { return 0; }
  if (!c->close_sent) {
    c->close_sent = 1;
    ws_client_send_frame(c, 8, (const char *)payload, 2);
  }
  c->closed = 1;
  c->want_close = 1;
  us_wakeup_loop(c->loop);
  avant_gc_native_begin();
  pthread_join(c->thread, NULL);
  avant_gc_native_end();
  ws_client_free(c);
  return 1;
}

/* ---- client fetch primitive ---- */

typedef struct FetchState {
  char *buf;
  size_t len;
  size_t cap;
  int failed;
  const char *request;
} FetchState;

static FetchState *fetch_of_socket(struct us_socket_t *s) {
  struct us_loop_t *loop = us_socket_context_loop(0, us_socket_context(0, s));
  return *(FetchState **)us_loop_ext(loop);
}

static struct us_socket_t *fetch_on_open(struct us_socket_t *s, int is_client,
                                         char *ip, int ip_length) {
  FetchState *st = fetch_of_socket(s);
  (void)is_client;
  (void)ip;
  (void)ip_length;
  /* The request goes out only now: writing on a still-connecting
   * socket corrupts the semi-socket state machine. */
  us_socket_write(0, s, st->request, (int)avant_str_n(st->request), 0);
  return s;
}

static struct us_socket_t *fetch_on_data(struct us_socket_t *s, char *data,
                                         int length) {
  FetchState *st = fetch_of_socket(s);
  if (st->len + (size_t)length > st->cap) {
    size_t want = st->cap ? st->cap : 8192;
    char *next;
    while (want < st->len + (size_t)length) {
      want *= 2;
    }
    next = (char *)realloc(st->buf, want);
    if (!next) {
      st->failed = 1;
      return us_socket_close(0, s, 0, NULL);
    }
    st->buf = next;
    st->cap = want;
  }
  memcpy(st->buf + st->len, data, (size_t)length);
  st->len += (size_t)length;
  return s;
}

static struct us_socket_t *fetch_on_close(struct us_socket_t *s, int code,
                                          void *reason) {
  (void)code;
  (void)reason;
  return s;
}

static struct us_socket_t *fetch_on_end(struct us_socket_t *s) {
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *fetch_on_timeout(struct us_socket_t *s) {
  FetchState *st = fetch_of_socket(s);
  st->failed = 2;
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *fetch_on_connect_error(struct us_socket_t *s,
                                                  int code) {
  FetchState *st = fetch_of_socket(s);
  (void)code;
  st->failed = 1;
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *fetch_on_writable(struct us_socket_t *s) {
  return s;
}

static void fetch_wakeup(struct us_loop_t *loop) {
  (void)loop;
}

/* gc-lint: holds nothing (the fetched body is C memory; nothing heap is read after make_bytes). */
char *avant_http_fetch(const char *host, int32_t port, const char *request,
                       int32_t timeout_ms) {
  struct us_loop_t *loop;
  struct us_socket_context_t *ctx;
  struct us_socket_context_options_t options;
  struct us_socket_t *sock;
  FetchState *st;
  char *out;
  int timeout_s;
  set_status(0);
  st = (FetchState *)calloc(1, sizeof(FetchState));
  if (!st) {
    set_status(4);
    return avant_str_empty();
  }
  loop = us_create_loop(0, fetch_wakeup, srv_pre, srv_post,
                        (unsigned int)sizeof(FetchState *));
  if (!loop) {
    free(st);
    set_status(1);
    return avant_str_empty();
  }
  *(FetchState **)us_loop_ext(loop) = st;
  memset(&options, 0, sizeof(options));
  ctx = us_create_socket_context(0, loop, 0, options);
  if (!ctx) {
    us_loop_free(loop);
    free(st);
    set_status(1);
    return avant_str_empty();
  }
  us_socket_context_on_open(0, ctx, fetch_on_open);
  us_socket_context_on_data(0, ctx, fetch_on_data);
  us_socket_context_on_writable(0, ctx, fetch_on_writable);
  us_socket_context_on_close(0, ctx, fetch_on_close);
  us_socket_context_on_timeout(0, ctx, fetch_on_timeout);
  us_socket_context_on_end(0, ctx, fetch_on_end);
  us_socket_context_on_connect_error(0, ctx, fetch_on_connect_error);
  sock = us_socket_context_connect(0, ctx, host, (int)port, NULL, 0, 0);
  if (!sock) {
    us_socket_context_free(0, ctx);
    us_loop_free(loop);
    free(st->buf);
    free(st);
    set_status(1);
    return avant_str_empty();
  }
  timeout_s = timeout_ms <= 0 ? 10 : (timeout_ms + 999) / 1000;
  us_socket_timeout(0, sock, (unsigned int)timeout_s);
  st->request = request;
  /* The loop runs native (rule 3); its callbacks read the request, which
   * is pinned for the loop (a collection on another thread may run
   * meanwhile and must not move or free it). */
  avant_pin((void *)request);
  avant_gc_native_begin();
  us_loop_run(loop);
  avant_gc_native_end();
  avant_unpin((void *)request);
  us_socket_context_free(0, ctx);
  us_loop_free(loop);
  if (st->failed == 1) {
    set_status(1);
    free(st->buf);
    free(st);
    return avant_str_empty();
  }
  if (st->failed == 2 && st->len == 0) {
    set_status(2);
    free(st->buf);
    free(st);
    return avant_str_empty();
  }
  out = make_bytes(st->buf ? st->buf : "", st->len);
  free(st->buf);
  free(st);
  return out;
}

/* ---- response parsing for the Avant client ---- */

static int parse_response_head(const char *raw, size_t n, int *status,
                               struct phr_header *headers,
                               size_t *num_headers, int *pret) {
  int minor_version = 0;
  const char *msg = NULL;
  size_t msg_len = 0;
  *num_headers = HTTP_MAX_HEADERS;
  *pret = phr_parse_response(raw, n, &minor_version, status, &msg, &msg_len,
                             headers, num_headers, 0);
  return *pret >= 0;
}

int32_t avant_http_resp_status(const char *raw) {
  int status = 0;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t num_headers = 0;
  int pret = 0;
  set_status(0);
  if (!parse_response_head(raw, avant_str_n(raw), &status, headers,
                           &num_headers, &pret)) {
    set_status(3);
    return 0;
  }
  return status;
}

char *avant_http_resp_headers(const char *raw) {
  int status = 0;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t num_headers = 0;
  int pret = 0;
  set_status(0);
  if (!parse_response_head(raw, avant_str_n(raw), &status, headers,
                           &num_headers, &pret)) {
    set_status(3);
    return make_bytes("", 0);
  }
  return build_packed_headers(headers, num_headers, raw, (size_t)pret);
}

/* gc-lint: holds nothing (make_bytes roots its source (an interior address of the response or a malloc copy); nothing heap is read after it). */
char *avant_http_resp_body(const char *raw) {
  size_t n = avant_str_n(raw);
  int status = 0;
  struct phr_header headers[HTTP_MAX_HEADERS];
  size_t num_headers = 0;
  int pret = 0;
  const char *value = NULL;
  size_t value_len = 0;
  const char *body;
  size_t body_len;
  set_status(0);
  if (!parse_response_head(raw, n, &status, headers, &num_headers, &pret)) {
    set_status(3);
    return make_bytes("", 0);
  }
  body = raw + pret;
  body_len = n - (size_t)pret;
  if (find_header(headers, num_headers, "Transfer-Encoding", &value,
                  &value_len) &&
      ascii_ieq_contains(value, value_len, "chunked")) {
    /* phr_decode_chunked rewrites the buffer in place; work on a copy. */
    char *work = (char *)malloc(body_len ? body_len : 1);
    struct phr_chunked_decoder decoder;
    size_t src_len = body_len;
    ssize_t done;
    if (!work) {
      set_status(4);
      return make_bytes("", 0);
    }
    memcpy(work, body, body_len);
    memset(&decoder, 0, sizeof(decoder));
    done = phr_decode_chunked(&decoder, work, &src_len);
    if (done < 0) {
      free(work);
      set_status(3);
      return make_bytes("", 0);
    }
    {
      char *out = make_bytes(work, src_len);
      free(work);
      return out;
    }
  }
  if (find_header(headers, num_headers, "Content-Length", &value,
                  &value_len)) {
    long want = 0;
    if (!parse_content_length(value, value_len, &want) ||
        (size_t)want > body_len) {
      set_status(3);
      return make_bytes("", 0);
    }
    return make_bytes(body, (size_t)want);
  }
  return make_bytes(body, body_len);
}
