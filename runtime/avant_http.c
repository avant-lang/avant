#include "avant_rt.h"
#include "libusockets.h"
#include "picohttpparser.h"

#include <stdint.h>
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

static char *make_string(const char *p, size_t n) {
  char *out;
  avant_gc_defer_enter();
  out = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_str_tag(out);
  avant_gc_defer_leave();
  return out;
}

static char *make_bytes(const char *p, size_t n) {
  char *out;
  avant_gc_defer_enter();
  out = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_gc_defer_leave();
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
  avant_gc_defer_enter();
  out = (char *)avant_alloc((uint64_t)total + 1, AVANT_TYPE_BYTES);
  words = (uint32_t *)out;
  words[0] = (uint32_t)count;
  for (i = 0; i < count; i++) {
    words[1 + i * 4 + 0] = (uint32_t)(headers[i].name - raw);
    words[1 + i * 4 + 1] = (uint32_t)headers[i].name_len;
    words[1 + i * 4 + 2] = (uint32_t)(headers[i].value - raw);
    words[1 + i * 4 + 3] = (uint32_t)headers[i].value_len;
  }
  if (raw_len > 0) {
    memcpy(out + 4 + count * 16, raw, raw_len);
  }
  out[total] = 0;
  avant_gc_defer_leave();
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

typedef struct Conn {
  AvantHttpServer *srv;
  char *buf;
  size_t len;
  size_t cap;
  int answered;
  int sent_continue;
} Conn;

struct AvantHttpServer {
  struct us_loop_t *loop;
  struct us_socket_context_t *ctx;
  struct us_listen_socket_t *listen;
  void *state;
  AvantHttpDispatch dispatch;
  volatile int closing;
  int running;
  int port;
  int32_t max_body;
  struct ConnNode *conns_head;
};

typedef struct ConnNode {
  Conn conn;
  struct us_socket_t *sock;
  struct ConnNode *next;
} ConnNode;

static AvantHttpServer *srv_of_socket(struct us_socket_t *s) {
  struct us_socket_context_t *ctx = us_socket_context(0, s);
  return *(AvantHttpServer **)us_socket_context_ext(0, ctx);
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
} AvantHttpReq;

char *avant_http_req_method(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  return make_string(req->method, req->method_len);
}

char *avant_http_req_target(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
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
    memset(&req, 0, sizeof(req));
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
    srv->dispatch(srv->state, &req);
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
  AvantHttpServer *srv = srv_of_socket(s);
  (void)is_client;
  (void)ip;
  (void)ip_length;
  memset(node, 0, sizeof(ConnNode));
  node->conn.srv = srv;
  node->sock = s;
  node->next = srv->conns_head;
  srv->conns_head = node;
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
  conn_process(conn, s);
  return s;
}

static struct us_socket_t *srv_on_close(struct us_socket_t *s, int code,
                                        void *reason) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  AvantHttpServer *srv = node->conn.srv;
  ConnNode **link;
  (void)code;
  (void)reason;
  free(node->conn.buf);
  node->conn.buf = NULL;
  if (srv) {
    link = &srv->conns_head;
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
  return s;
}

static void srv_on_wakeup(struct us_loop_t *loop) {
  AvantHttpServer *srv = *(AvantHttpServer **)us_loop_ext(loop);
  if (!srv || !srv->closing) {
    return;
  }
  if (srv->listen) {
    us_listen_socket_close(0, srv->listen);
    srv->listen = NULL;
  }
  {
    /* Answered connections are already shut down with their response
     * queued ahead of the FIN; leave them to close from the peer side
     * so the last response is not discarded. Idle connections are
     * closed here so the loop can drain. */
    ConnNode *node = srv->conns_head;
    while (node) {
      ConnNode *next = node->next;
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

void *avant_http_listen(const char *host, int32_t port, int32_t max_body) {
  AvantHttpServer *srv;
  struct us_socket_context_options_t options;
  set_status(0);
  srv = (AvantHttpServer *)calloc(1, sizeof(AvantHttpServer));
  if (!srv) {
    set_status(4);
    return NULL;
  }
  srv->max_body = max_body;
  srv->loop = us_create_loop(0, srv_on_wakeup, srv_pre, srv_post,
                             (unsigned int)sizeof(AvantHttpServer *));
  if (!srv->loop) {
    free(srv);
    set_status(1);
    return NULL;
  }
  *(AvantHttpServer **)us_loop_ext(srv->loop) = srv;
  memset(&options, 0, sizeof(options));
  srv->ctx = us_create_socket_context(0, srv->loop,
                                      (int)sizeof(AvantHttpServer *), options);
  if (!srv->ctx) {
    us_loop_free(srv->loop);
    free(srv);
    set_status(1);
    return NULL;
  }
  *(AvantHttpServer **)us_socket_context_ext(0, srv->ctx) = srv;
  us_socket_context_on_open(0, srv->ctx, srv_on_open);
  us_socket_context_on_data(0, srv->ctx, srv_on_data);
  us_socket_context_on_writable(0, srv->ctx, srv_on_writable);
  us_socket_context_on_close(0, srv->ctx, srv_on_close);
  us_socket_context_on_timeout(0, srv->ctx, srv_on_timeout);
  us_socket_context_on_end(0, srv->ctx, srv_on_end);
  srv->listen = us_socket_context_listen(0, srv->ctx, host, (int)port, 0,
                                         (int)sizeof(ConnNode));
  if (!srv->listen) {
    us_socket_context_free(0, srv->ctx);
    us_loop_free(srv->loop);
    free(srv);
    set_status(1);
    return NULL;
  }
  srv->port = us_socket_local_port(0, (struct us_socket_t *)srv->listen);
  return srv;
}

int32_t avant_http_port(void *srvp) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  return srv ? srv->port : 0;
}

static void srv_teardown(AvantHttpServer *srv) {
  if (srv->listen) {
    us_listen_socket_close(0, srv->listen);
    srv->listen = NULL;
  }
  if (srv->ctx) {
    us_socket_context_free(0, srv->ctx);
    srv->ctx = NULL;
  }
  if (srv->loop) {
    us_loop_free(srv->loop);
    srv->loop = NULL;
  }
  free(srv);
}

int32_t avant_http_serve(void *srvp, void *state, void *cb) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  if (!srv || !srv->loop) {
    set_status(1);
    return 1;
  }
  srv->state = state;
  srv->dispatch = (AvantHttpDispatch)cb;
  srv->running = 1;
  us_loop_run(srv->loop);
  srv->running = 0;
  srv_teardown(srv);
  return 0;
}

int32_t avant_http_close(void *srvp) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  if (!srv) {
    return 0;
  }
  if (srv->running) {
    srv->closing = 1;
    us_wakeup_loop(srv->loop);
    return 0;
  }
  srv_teardown(srv);
  return 0;
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
  us_loop_run(loop);
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
