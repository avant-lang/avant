/* pthread_condattr_setclock, as the collector's units have it. */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"
#include "libusockets.h"
/* For LIBUS_SOCKET_READABLE and LIBUS_SOCKET_WRITABLE, the events
 * us_poll_change takes: libusockets.h declares the function and keeps
 * the constants in its internal headers. */
#include "internal/internal.h"
#include "picohttpparser.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
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

avant_int avant_http_take_status(void) {
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
  out = avant_str_alloc((uint64_t)n);
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
  out = avant_str_alloc((uint64_t)n);
  if (n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

/* Packed Headers: a uint32 count, then count records of
 * (name_off, name_len, value_off, value_len), then the raw header
 * block the offsets point into. One collector Bytes per header set;
 * lookups materialize a String only for the value actually taken.
 * A received request's set also carries its peer's address (PEER, its
 * length and 16 bytes) after the raw block, PACKED_PEER set in the count
 * word: Request.peer reads it from there, so a request pays for its
 * peer neither a call nor a field. */
#define PACKED_PEER 0x80000000u
static char *build_packed_headers(struct phr_header *headers, size_t count,
                                  const char *raw, size_t raw_len,
                                  const unsigned char *peer) {
  size_t total = 4 + count * 16 + raw_len + (peer ? 17 : 0);
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
  out = avant_str_alloc((uint64_t)total);
  words = (uint32_t *)out;
  words[0] = (uint32_t)count | (peer ? PACKED_PEER : 0);
  for (i = 0; i < count; i++) {
    words[1 + i * 4 + 0] = (uint32_t)(headers[i].name - raw0);
    words[1 + i * 4 + 1] = (uint32_t)headers[i].name_len;
    words[1 + i * 4 + 2] = (uint32_t)(headers[i].value - raw0);
    words[1 + i * 4 + 3] = (uint32_t)headers[i].value_len;
  }
  if (raw_len > 0) {
    memcpy(out + 4 + count * 16, raw, raw_len);
  }
  if (peer) {
    memcpy(out + total - 17, peer, 17);
  }
  out[total] = 0;
  avant_str_tag(out);
  return out;
}

/* The packed form of a header block built in memory ("Name: value\r\n"
 * lines, each checked as Response.header checks it): a Request made by
 * Request.new, the headers of a Response built in code. */
/* gc-lint: holds nothing (build_packed_headers roots the block and takes
 * the offsets before it allocates; only C memory is read after it). */
char *avant_http_headers_pack(const char *block) {
  size_t n = avant_str_n(block);
  size_t count = 0;
  size_t i;
  struct phr_header *hs;
  char *out;
  for (i = 0; i + 1 < n; i++) {
    if (block[i] == '\r' && block[i + 1] == '\n') {
      count++;
    }
  }
  hs = (struct phr_header *)calloc(count ? count : 1, sizeof(struct phr_header));
  if (!hs) {
    avant_trap_text("out of memory building a header set");
  }
  {
    size_t at = 0;
    size_t k = 0;
    while (at < n && k < count) {
      size_t colon = at;
      size_t end;
      while (colon < n && block[colon] != ':') {
        colon++;
      }
      end = colon;
      while (end + 1 < n && !(block[end] == '\r' && block[end + 1] == '\n')) {
        end++;
      }
      hs[k].name = block + at;
      hs[k].name_len = colon - at;
      at = colon + 1;
      while (at < end && (block[at] == ' ' || block[at] == '\t')) {
        at++;
      }
      hs[k].value = block + at;
      hs[k].value_len = end > at ? end - at : 0;
      k++;
      at = end + 2;
    }
  }
  out = build_packed_headers(hs, count, block, n, NULL);
  free(hs);
  return out;
}

static int hex_val(unsigned char ch) {
  if (ch >= '0' && ch <= '9') { return ch - '0'; }
  if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
  if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
  return -1;
}

static int ws_utf8_valid(const unsigned char *p, size_t n);

/* Percent-decodes p[0..n) into out, which has room for 3 * n bytes;
 * '+' is a space when plus is set (application/x-www-form-urlencoded).
 * A '%' not followed by two hex digits is kept as it is. The decoded
 * bytes are read as UTF-8, each byte that does not begin a valid
 * sequence replaced by U+FFFD (three bytes). Returns the length. */
static size_t pct_decode(const char *p, size_t n, char *out, int plus) {
  unsigned char small[256];
  unsigned char *raw = n <= sizeof small ? small : (unsigned char *)malloc(n);
  size_t i = 0;
  size_t r = 0;
  size_t at = 0;
  size_t m = 0;
  if (!raw) {
    avant_trap_text("out of memory decoding a URL component");
  }
  while (i < n) {
    unsigned char ch = (unsigned char)p[i];
    if (ch == '%' && i + 2 < n && hex_val((unsigned char)p[i + 1]) >= 0 &&
        hex_val((unsigned char)p[i + 2]) >= 0) {
      raw[r++] = (unsigned char)(hex_val((unsigned char)p[i + 1]) * 16 +
                                 hex_val((unsigned char)p[i + 2]));
      i += 3;
    } else {
      raw[r++] = ch == '+' && plus ? ' ' : ch;
      i++;
    }
  }
  while (at < r) {
    unsigned char c0 = raw[at];
    size_t len = c0 < 0x80                  ? 1
                 : c0 >= 0xc2 && c0 <= 0xdf ? 2
                 : c0 >= 0xe0 && c0 <= 0xef ? 3
                 : c0 >= 0xf0 && c0 <= 0xf4 ? 4
                                            : 0;
    if (len == 1 || (len > 1 && at + len <= r && ws_utf8_valid(raw + at, len))) {
      memcpy(out + m, raw + at, len);
      m += len;
      at += len;
    } else {
      out[m++] = (char)0xef;
      out[m++] = (char)0xbf;
      out[m++] = (char)0xbd;
      at++;
    }
  }
  if (raw != small) {
    free(raw);
  }
  return m;
}

/* A query string's (or a form body's) parameters, decoded, in the packed
 * form of a header set: '&' separates them, the first '=' a name from its
 * value (none: an empty value), empty pieces are skipped. */
/* gc-lint: holds nothing (the decoded block is C memory; the text is not
 * read after build_packed_headers allocates). */
char *avant_http_params_pack(const char *text) {
  size_t n = avant_str_n(text);
  size_t count = 0;
  size_t i;
  size_t at;
  size_t m = 0;
  struct phr_header *hs;
  char *block;
  char *out;
  for (i = 0; i <= n; i++) {
    if (i == n || text[i] == '&') {
      count++;
    }
  }
  hs = (struct phr_header *)calloc(count, sizeof(struct phr_header));
  block = (char *)malloc(3 * n + 1);
  if (!hs || !block) {
    free(hs);
    free(block);
    avant_trap_text("out of memory parsing a query");
  }
  count = 0;
  at = 0;
  for (i = 0; i <= n; i++) {
    if (i == n || text[i] == '&') {
      size_t eq = at;
      if (i > at) {
        while (eq < i && text[eq] != '=') {
          eq++;
        }
        hs[count].name = block + m;
        hs[count].name_len = pct_decode(text + at, eq - at, block + m, 1);
        m += hs[count].name_len;
        hs[count].value = block + m;
        hs[count].value_len = eq < i ? pct_decode(text + eq + 1, i - eq - 1, block + m, 1) : 0;
        m += hs[count].value_len;
        count++;
      }
      at = i + 1;
    }
  }
  out = build_packed_headers(hs, count, block, m, NULL);
  free(hs);
  free(block);
  return out;
}

/* A URL component percent-decoded (a path segment: '+' stays '+'), read
 * as UTF-8 with U+FFFD for what is not. */
/* gc-lint: holds nothing (the decoded text is C memory; the argument is
 * not read after make_string allocates). */
char *avant_http_decode_component(const char *text) {
  size_t n = avant_str_n(text);
  char *buf = (char *)malloc(3 * n + 1);
  size_t m;
  char *out;
  if (!buf) {
    avant_trap_text("out of memory decoding a URL component");
  }
  m = pct_decode(text, n, buf, 0);
  out = make_string(buf, m);
  free(buf);
  return out;
}

static uint32_t packed_count(const char *data, size_t n) {
  if (!data || n < 4) {
    return 0;
  }
  return ((uint32_t *)(void *)data)[0] & ~PACKED_PEER;
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

avant_int avant_http_header_count(const char *data) {
  return (int32_t)packed_count(data, avant_str_n(data));
}

avant_int avant_http_header_has(const char *data, const char *name) {
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
char *avant_http_header_at(const char *data, const char *name, avant_int index) {
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

/* As avant_http_header_has and _at, with names compared exactly (a
 * query parameter's name has a case). */
avant_int avant_http_param_has(const char *data, const char *name) {
  size_t name_len = avant_str_n(name);
  uint32_t count = packed_count(data, avant_str_n(data));
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  uint32_t i;
  if (!data || count == 0) {
    return 0;
  }
  raw = packed_raw(data, count);
  for (i = 0; i < count; i++) {
    if (words[1 + i * 4 + 1] == name_len && memcmp(raw + words[1 + i * 4], name, name_len) == 0) {
      return 1;
    }
  }
  return 0;
}

char *avant_http_param_at(const char *data, const char *name, avant_int index) {
  size_t name_len = avant_str_n(name);
  uint32_t count = packed_count(data, avant_str_n(data));
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  uint32_t i;
  int64_t seen = 0;
  set_status(0);
  if (data && count > 0) {
    raw = packed_raw(data, count);
    for (i = 0; i < count; i++) {
      if (words[1 + i * 4 + 1] == name_len && memcmp(raw + words[1 + i * 4], name, name_len) == 0) {
        if (seen == index) {
          return make_string(raw + words[1 + i * 4 + 2], words[1 + i * 4 + 3]);
        }
        seen++;
      }
    }
  }
  set_status(1);
  return avant_str_empty();
}

char *avant_http_header_name_at(const char *data, avant_int index) {
  size_t n = avant_str_n(data);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  set_status(0);
  if (!data || index < 0 || (uint64_t)index >= count) {
    set_status(1);
    return avant_str_empty();
  }
  raw = packed_raw(data, count);
  return make_string(raw + words[1 + (uint32_t)index * 4],
                     words[1 + (uint32_t)index * 4 + 1]);
}

char *avant_http_header_value_at(const char *data, avant_int index) {
  size_t n = avant_str_n(data);
  uint32_t count = packed_count(data, n);
  const uint32_t *words = (const uint32_t *)(const void *)data;
  const char *raw;
  set_status(0);
  if (!data || index < 0 || (uint64_t)index >= count) {
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

/* A program's timer on the server's first loop (Server.every): its
 * period, its index among the program's timers, and its uSockets timer
 * while serving. */
typedef struct HttpTimer {
  AvantHttpServer *srv;
  uint32_t ms;
  avant_int index;
  struct us_timer_t *t;
} HttpTimer;

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
 * server's list so a WsSender (a bare id) can find it again from any
 * thread without holding a C pointer that could dangle.
 *
 * The record is type-stable: it comes from
 * ws_record_take and goes back with ws_record_release to a free list,
 * never to malloc, so the address an Avant WsConn value carries beside
 * its id can always be read. id is the connection's number while the
 * record is in use and 0 while it is free; a number is never given
 * twice. A WsConn whose id is not the record's is a value of a
 * connection that has closed: every operation through it answers
 * "closed" (ws_record_is). id and owner_tid are read by such values
 * from any thread, so both are stored and loaded atomically.
 *
 * route is the index, in the server's WebSocket message routes, of the
 * first route on the connection's path (-1 when the path has none),
 * given by std.http at the upgrade and read for every message by
 * avant_ws_event_route. Only the owner thread
 * writes it and reads it, so the loop threads share no table of
 * connections. */
typedef struct AvantWsConn {
  int64_t id;
  AvantHttpServer *srv;
  struct HttpLoop *loop;
  struct us_socket_t *sock;
  struct Conn *conn;
  pthread_t owner_tid;
  char *path;
  avant_int route;
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
  /* A frame was begun on the wire and its rest could not be queued (no
   * memory): the stream is cut mid-frame, so the connection is closed
   * by its loop's next wakeup (ws_cut). */
  int cut;
  /* Liveness (owner thread only): ping_sent, the server pinged the
   * peer after its idle timeout and nothing has arrived since;
   * recv_total and sent_total, the bytes received and the queued bytes
   * written, which the minimum rate measures (loop_sweep). */
  int ping_sent;
  uint64_t recv_total;
  uint64_t sent_total;
  struct AvantWsConn *next_all;
  struct AvantWsConn *next_global;
} AvantWsConn;

/* out is the connection's output queue: the
 * bytes of an answer the socket did not take, in C memory, sent by
 * srv_on_writable. While it holds anything the connection takes no
 * further request and is not read (conn_send), so it never holds more
 * than one answer's tail and the input buffer stops growing; a later
 * answer cannot overtake it. close_after_flush: the connection is shut
 * down when the queue drains (Connection: close, an error answer, the
 * server closing). peer_ended: the peer sent FIN while output was still
 * queued; the socket closes once everything is answered and sent. */
typedef struct Conn {
  AvantHttpServer *srv;
  char *buf;
  size_t len;
  size_t cap;
  int answered;
  int sent_continue;
  AvantWsConn *ws;
  char *out;
  size_t out_len;
  size_t out_off;
  size_t out_cap;
  int close_after_flush;
  int peer_ended;
  /* The deadlines' clocks (http_now_ms readings, 0 when not running):
   * head_at since the first byte of a request head that is not complete
   * yet; body_at since the start of the current rate window of a body
   * waiting for bytes (body_from: conn->len then); out_at likewise for
   * an answer's queued tail (out_from: out_off then). A connection with
   * one running is on its loop's watch list, which the loop's sweep
   * reads (loop_sweep). A request that arrives whole and an answer the
   * socket takes whole start none. */
  int64_t head_at;
  int64_t body_at;
  size_t body_from;
  int64_t out_at;
  size_t out_from;
  /* The bytes of the server's body budget this request's body holds
   * while it waits (its Content-Length), 0 when none. */
  int64_t budget;
  /* Pipelined answers gathered for one write (conn_process): while
   * corking is set, conn_send appends to the loop thread's batch buffer
   * (t_cork) instead of writing, and the batch goes out in one write
   * when the requests in the buffer are answered. cork_len is this
   * connection's bytes in it, 0 outside conn_process. */
  int corking;
  size_t cork_len;
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
  struct ConnNode *watch; /* the connections with a deadline running */
  struct us_timer_t *sweep;
  int64_t close_at; /* when this loop began to close (the shutdown deadline) */
  /* accept failed for want of descriptors or memory: the listening
   * socket is not polled until the next sweep (us_internal_accept_paused) */
  int accept_paused;
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
  /* The idle timeout in uSockets' clocks (idle_limit): idle_secs for its
   * 4-second ticks, or idle_mins for its minutes; both 0: none. */
  uint32_t idle_secs;
  uint32_t idle_mins;
  /* A WebSocket connection's idle timeout, in the same clocks
   * (Server.websocket_idle_timeout): when it passes with nothing
   * received the peer is pinged, and when it passes again the
   * connection is closed. */
  uint32_t ws_idle_secs;
  uint32_t ws_idle_mins;
  size_t ws_write_chunk; /* the most a WebSocket write hands the socket */
  /* The limits (Server's setters before serve; 0 is off):
   * head_ms, the time a request head may take from its first byte;
   * rate, the bytes per second a request body must arrive at and an
   * answer's queued tail must leave at, on average over each window of
   * grace_ms from when it began to wait. */
  uint32_t head_ms;
  uint32_t rate;
  uint32_t grace_ms;
  /* body_budget, the bytes all waiting request bodies may hold together
   * (their Content-Length), the rest answered 503; mem_ceiling, the
   * process's resident memory past which new requests are answered 503
   * (-1 until serve: then the container's limit's 90% or off), over_mem
   * the sweep's last reading; max_conns, the open connections, past
   * which the loop's longest idle one is closed (capped at serve by the
   * file-descriptor limit); shutdown_ms, how long close waits for the
   * connections it leaves to finish before it closes them. body_held
   * and conns are server-wide counts, changed atomically. */
  int64_t body_budget;
  int64_t mem_ceiling;
  int64_t max_conns;
  uint32_t shutdown_ms;
  int over_mem;
  int64_t body_held;
  int64_t conns;
  AvantWsConn *ws_all;
  /* The program's timers (Server.every), run on loops[0] while serving
   * through timer_dispatch. */
  AvantHttpDispatch timer_dispatch;
  HttpTimer *timers;
  int ntimers;
};

/* conn comes first: a Conn's address is its node's. peer is the peer's
 * address as accept gave it, kept for Request.peer: copied at open and
 * into each request's packed headers. */
typedef struct ConnNode {
  Conn conn;
  struct us_socket_t *sock;
  struct ConnNode *next;
  struct ConnNode *wprev;
  struct ConnNode *wnext;
  int watched;
  /* peer[0] is the address's length (4 or 16, 0 for none), then its
   * bytes (an IPv4-mapped IPv6 address kept as its IPv4 four). */
  unsigned char peer[17];
} ConnNode;

#define HTTP_HEAD_TIMEOUT_MS 10000
#define HTTP_MIN_RATE 240
#define HTTP_RATE_GRACE_MS 5000
#define HTTP_BODY_BUDGET (256LL * 1024 * 1024)
#define HTTP_MAX_CONNS 10000
#define HTTP_SHUTDOWN_MS 30000
/* A connection's buffer past this is given back once it is empty: a
 * large body left every connection that ever received one holding its
 * size for good. */
#define HTTP_BUF_KEEP 65536

/* A coarse monotonic clock in milliseconds (the vDSO's, a few ms of
 * resolution): read only when a deadline starts and by the sweep. */
static int64_t http_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + 1;
}

/* An HTTP connection is closed when nothing has happened on it for
 * idle_timeout milliseconds (Server.idle_timeout, 30000 by default): no
 * request bytes received and no queued answer bytes sent. uSockets
 * counts timeouts in ticks of 4 seconds (up to 239), and its long
 * timeouts in minutes: idle_limit gives the setting in those clocks so
 * that the close comes no earlier than the setting and less than 8 s
 * after it (or, past 952 s, less than 2 minutes after it). It used to be
 * a fixed 30 s that closed 28 to 32 s after the last activity, set only
 * by an internal variable of the suite. */
#define HTTP_IDLE_TIMEOUT_MS 30000
#define HTTP_IDLE_MAX_MS 14280000

static void idle_limit(uint32_t ms, uint32_t *secs, uint32_t *mins) {
  uint32_t ticks;
  *secs = 0;
  *mins = 0;
  if (ms == 0) {
    return;
  }
  ticks = (ms + 3999) / 4000 + 1;
  if (ticks <= 239) {
    *secs = ticks * 4;
  } else {
    *mins = (ms + 59999) / 60000 + 1;
  }
}

/* Starts a connection's idle clock again (0 and 0: none). */
static void idle_arm(struct us_socket_t *s, uint32_t secs, uint32_t mins) {
  if (__builtin_expect(mins != 0, 0)) {
    us_socket_long_timeout(0, s, mins);
  } else {
    us_socket_timeout(0, s, secs);
  }
}

/* A write hands the socket at most 1 GiB at a time (the server's
 * WebSocket messages, its HTTP answers, the client's request and the
 * WebSocket client's frames):
 * uSockets takes an int length. AVANT_HTTP_WS_WRITE_CHUNK is an internal
 * variable for the suite, not a user setting: a server created, or a
 * client call or connection begun, while it holds a number of bytes from
 * 4096 to 1073741824 hands the socket at most that much, so
 * tests/cases/test_std_http_safety.av runs the paths a length of 1 GiB
 * or more takes with one of 1 MiB. It can only shrink the piece; any
 * other value is ignored. */
#define WS_WRITE_CHUNK 1073741824
static size_t http_ws_write_chunk(void) {
  const char *v = getenv("AVANT_HTTP_WS_WRITE_CHUNK");
  if (v && v[0] >= '0' && v[0] <= '9') {
    long n = strtol(v, NULL, 10);
    if (n >= 4096 && n <= WS_WRITE_CHUNK) {
      return (size_t)n;
    }
  }
  return WS_WRITE_CHUNK;
}

static HttpLoop *loop_of_socket(struct us_socket_t *s) {
  struct us_socket_context_t *ctx = us_socket_context(0, s);
  return *(HttpLoop **)us_socket_context_ext(0, ctx);
}

/* The watch list: the connections of a loop with a deadline running
 * (Conn's head_at, body_at, out_at), which the loop's sweep reads. A
 * request that arrives whole and an answer the socket takes whole never
 * enter it, so the hot path pays only the test of the two clocks. */
static void conn_watch(Conn *conn, struct us_socket_t *s) {
  ConnNode *node = (ConnNode *)conn;
  HttpLoop *hl;
  if (node->watched) {
    return;
  }
  hl = loop_of_socket(s);
  node->watched = 1;
  node->wprev = NULL;
  node->wnext = hl->watch;
  if (hl->watch) {
    hl->watch->wprev = node;
  }
  hl->watch = node;
}

static void conn_unwatch(Conn *conn, struct us_socket_t *s) {
  ConnNode *node = (ConnNode *)conn;
  if (!node->watched) {
    return;
  }
  node->watched = 0;
  if (node->wprev) {
    node->wprev->wnext = node->wnext;
  } else {
    loop_of_socket(s)->watch = node->wnext;
  }
  if (node->wnext) {
    node->wnext->wprev = node->wprev;
  }
  node->wprev = NULL;
  node->wnext = NULL;
}

/* A request's deadlines end: it is complete, answered or refused; the
 * budget its body held is given back. */
static void conn_wait_end(Conn *conn, struct us_socket_t *s) {
  conn->head_at = 0;
  conn->body_at = 0;
  if (conn->budget) {
    __atomic_sub_fetch(&conn->srv->body_held, conn->budget, __ATOMIC_RELAXED);
    conn->budget = 0;
  }
  if (!conn->out_at) {
    conn_unwatch(conn, s);
  }
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
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
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

/* A length in decimal, all 64 bits of it: the
 * Content-Length was written through put_u32, so a body of 4 GiB or more
 * would have been announced as its length modulo 2^32; the String cap
 * (2^31 - 1 bytes) was all that kept it from happening. */
static size_t put_size(char *dst, size_t v) {
  char tmp[20];
  size_t i = 0;
  size_t n = 0;
  do {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  while (i > 0) {
    dst[n++] = tmp[--i];
  }
  return n;
}

static size_t put_str(char *dst, const char *src, size_t len) {
  memcpy(dst, src, len);
  return len;
}

static int conn_out_pending(const Conn *conn) {
  return conn->out_len > conn->out_off;
}

static void conn_poll(struct us_socket_t *s, int events) {
  us_poll_change((struct us_poll_t *)s,
                 us_socket_context_loop(0, us_socket_context(0, s)), events);
}

/* Writes p[*off .. len) to the socket, in pieces of at most `piece`
 * bytes (1 GiB, or less under AVANT_HTTP_WS_WRITE_CHUNK: the socket
 * takes an int length), and advances *off by what the socket took.
 * Returns 1 when everything has gone and 0 when the socket took
 * only a part: us_socket_write has then asked for the socket's writable
 * event, and the caller's on_writable handler calls this again with the
 * same offset. The server's output queue (conn_flush), an answer past
 * the piece (conn_send), the client's request (fetch_send) and the
 * WebSocket client's handshake are sent
 * by it. */
static int sock_write_rest(struct us_socket_t *s, const char *p, size_t len,
                           size_t *off, size_t piece) {
  while (*off < len) {
    size_t rem = len - *off;
    int chunk = rem > piece ? (int)piece : (int)rem;
    int written = us_socket_write(0, s, p + *off, chunk, 0);
    if (written > 0) {
      *off += (size_t)written;
    }
    if (written < chunk) {
      return 0;
    }
  }
  return 1;
}

/* Appends n bytes to the output queue; 0 when memory ran out. */
static int conn_out_append(Conn *conn, const char *p, size_t n) {
  if (n == 0) {
    return 1;
  }
  if (conn->out_len + n > conn->out_cap) {
    size_t want = conn->out_cap ? conn->out_cap : 4096;
    char *next;
    while (want < conn->out_len + n) {
      want *= 2;
    }
    next = (char *)realloc(conn->out, want);
    if (!next) {
      return 0;
    }
    conn->out = next;
    conn->out_cap = want;
  }
  memcpy(conn->out + conn->out_len, p, n);
  conn->out_len += n;
  return 1;
}

/* Sends the two pieces of an answer in order (b may be NULL with bn 0).
 * With nothing queued they go to the socket in one vectored write, and
 * only what the socket did not take is copied into the queue; with a
 * tail already queued they are appended behind it, so bytes leave in
 * the order they were handed in. The pieces may be Avant heap memory:
 * nothing of them is kept past this call. While the queue holds
 * anything the socket is polled for writable only: the connection is
 * not read, so a client that does not read its answers is held back by
 * TCP instead of filling the server's memory. When the queue cannot
 * grow the connection is closed: the answer cannot be completed, and a
 * cut answer followed by another is worse than a closed connection. */
static inline __attribute__((always_inline)) void
conn_send_raw(Conn *conn, struct us_socket_t *s, const char *a, size_t an,
              const char *b, size_t bn) {
  size_t written = 0;
  if (us_socket_is_closed(0, s) || us_socket_is_shut_down(0, s)) {
    return;
  }
  if (!conn_out_pending(conn)) {
    size_t piece = conn->srv->ws_write_chunk;
    if (an + bn <= piece) {
      int w = us_socket_write2(0, s, a, (int)an, b, (int)bn);
      written = w > 0 ? (size_t)w : 0;
    } else {
      /* Past the piece (1 GiB) the two parts go one after the other, a
       * piece at a time: us_socket_write2 takes
       * int lengths and adds them in an int, so an answer of 2 GiB or
       * more overflowed inside the vendored code (undefined behaviour
       * that happened to work on this build). */
      size_t off = 0;
      if (sock_write_rest(s, a, an, &off, piece)) {
        size_t boff = 0;
        (void)sock_write_rest(s, b, bn, &boff, piece);
        written = an + boff;
      } else {
        written = off;
      }
    }
    if (written >= an + bn) {
      return;
    }
  }
  if (written < an) {
    if (!conn_out_append(conn, a + written, an - written) ||
        !conn_out_append(conn, b, bn)) {
      us_socket_close(0, s, 0, NULL);
      return;
    }
  } else if (!conn_out_append(conn, b + (written - an), bn - (written - an))) {
    us_socket_close(0, s, 0, NULL);
    return;
  }
  if (!conn->out_at) {
    /* The tail must leave at the minimum rate (loop_sweep). */
    conn->out_at = http_now_ms();
    conn->out_from = conn->out_off;
    conn_watch(conn, s);
  }
  conn_poll(s, LIBUS_SOCKET_WRITABLE);
}

/* The most a batch of pipelined answers gathers before it is written:
 * a large answer goes out by itself rather than be copied. */
#define HTTP_CORK_MAX 65536

/* The batch buffer, one per loop thread: a connection's answers are
 * gathered and written within its conn_process, which no other
 * connection's processing interrupts on that thread, so the loop's
 * connections share it (one per connection held about 4 KiB each for the
 * connection's life: 3.5 MB more at 900 pipelining connections).
 * t_cork_owner is the connection whose bytes are in it. */
static __thread char *t_cork;
static __thread size_t t_cork_cap;
static __thread Conn *t_cork_owner;

/* A loop thread's batch buffer is given back when its loop ends. */
static void cork_release(void) {
  free(t_cork);
  t_cork = NULL;
  t_cork_cap = 0;
  t_cork_owner = NULL;
}


/* Writes the gathered answers, in one write (conn_send_raw queues what
 * the socket does not take). */
static __attribute__((noinline)) void conn_uncork(Conn *conn, struct us_socket_t *s) {
  size_t n = conn->cork_len;
  if (n == 0) {
    return;
  }
  conn->cork_len = 0;
  conn_send_raw(conn, s, t_cork, n, NULL, 0);
}

/* Appends n bytes to the batch; 0 when it would pass HTTP_CORK_MAX,
 * memory ran out, or another connection's batch is in the buffer (not
 * expected: the answer is then written alone). */
static int cork_append(Conn *conn, const char *p, size_t n) {
  if (conn->cork_len == 0) {
    if (t_cork_owner && t_cork_owner != conn && t_cork_owner->cork_len) {
      return 0;
    }
    t_cork_owner = conn;
  } else if (t_cork_owner != conn) {
    return 0;
  }
  if (conn->cork_len + n > t_cork_cap) {
    size_t want = t_cork_cap ? t_cork_cap : 4096;
    char *next;
    while (want < conn->cork_len + n) {
      want *= 2;
    }
    next = (char *)realloc(t_cork, want);
    if (!next) {
      return 0;
    }
    t_cork = next;
    t_cork_cap = want;
  }
  if (n > 0) {
    memcpy(t_cork + conn->cork_len, p, n);
  }
  conn->cork_len += n;
  return 1;
}

/* Sends the two pieces of an answer. Under pipelining (conn->corking,
 * set by conn_process while a later request waits in the buffer, or a
 * batch has begun) they are gathered, and the batch is written once:
 * one write per answer was one system call per request, where the
 * fastest servers make one per batch. A piece that would pass
 * HTTP_CORK_MAX first sends the batch, then goes as it would alone, so
 * order is kept. */
static __attribute__((noinline)) void conn_send_batched(Conn *conn, struct us_socket_t *s,
                                                        const char *a, size_t an,
                                                        const char *b, size_t bn) {
  if (conn->corking) {
    size_t before = conn->cork_len;
    if (conn->cork_len + an + bn <= HTTP_CORK_MAX && cork_append(conn, a, an) &&
        cork_append(conn, b, bn)) {
      return;
    }
    conn->cork_len = before;
  }
  conn_uncork(conn, s);
  conn_send_raw(conn, s, a, an, b, bn);
}

static void conn_send(Conn *conn, struct us_socket_t *s, const char *a,
                      size_t an, const char *b, size_t bn) {
  if (__builtin_expect(conn->corking | (conn->cork_len != 0), 0)) {
    conn_send_batched(conn, s, a, an, b, bn);
    return;
  }
  conn_send_raw(conn, s, a, an, b, bn);
}

/* The answer is complete and the connection ends after it: shut down
 * now, or when the queued tail has been sent. */
static void conn_finish(Conn *conn, struct us_socket_t *s) {
  conn_uncork(conn, s);
  conn->answered = 1;
  if (conn_out_pending(conn)) {
    conn->close_after_flush = 1;
  } else {
    us_socket_shutdown(0, s);
  }
}

/* conn_header is "close", "keep-alive", or NULL to omit the header.
 * The prefix is assembled by hand: a hello-world response costs two
 * snprintf calls per request otherwise, which profiling put at about
 * a seventh of the loop thread's user time. */
/* BODY is BODY_LEN bytes: the program's Bytes (its header's length) or
 * a C text of the runtime's own (a reason phrase, its strlen). CTYPE is
 * the response's one Content-Type value (NULL or empty: none), written
 * before HEAD's lines. */
static void write_response(Conn *conn, struct us_socket_t *s, int32_t status,
                           const char *ctype, size_t ctype_len,
                           const char *head, const char *body, size_t body_len,
                           const char *conn_header) {
  size_t head_len = head ? strlen(head) : 0;
  char prefix[4096];
  size_t n = 0;
  const char *reason = reason_phrase(status);
  n += put_str(prefix + n, "HTTP/1.1 ", 9);
  n += put_u32(prefix + n, (uint32_t)status);
  prefix[n++] = ' ';
  n += put_str(prefix + n, reason, strlen(reason));
  n += put_str(prefix + n, "\r\n", 2);
  if (ctype_len > 0) {
    if (n + ctype_len + head_len + 80 >= sizeof(prefix)) {
      n = sizeof(prefix);
    } else {
      n += put_str(prefix + n, "Content-Type: ", 14);
      n += put_str(prefix + n, ctype, ctype_len);
      n += put_str(prefix + n, "\r\n", 2);
    }
  }
  if (head_len > 0 && n < sizeof(prefix)) {
    if (n + head_len + 64 >= sizeof(prefix)) {
      /* Oversized head block: fall back to separate writes below. */
      n = sizeof(prefix);
    } else {
      n += put_str(prefix + n, head, head_len);
    }
  }
  if (n < sizeof(prefix)) {
    n += put_str(prefix + n, "Content-Length: ", 16);
    n += put_size(prefix + n, body_len);
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
    if (ctype_len > 0) {
      conn_send(conn, s, line, (size_t)m, "Content-Type: ", 14);
      conn_send(conn, s, ctype, ctype_len, "\r\n", 2);
      m = 0;
    }
    conn_send(conn, s, line, (size_t)m, head, head_len);
    if (conn_header) {
      m = snprintf(line, sizeof(line),
                   "Content-Length: %zu\r\nConnection: %s\r\n\r\n", body_len,
                   conn_header);
    } else {
      m = snprintf(line, sizeof(line), "Content-Length: %zu\r\n\r\n", body_len);
    }
    conn_send(conn, s, line, (size_t)m, body, body_len);
    return;
  }
  conn_send(conn, s, prefix, n, body, body_len);
}

static void send_error(Conn *conn, struct us_socket_t *s, int32_t status) {
  const char *phrase = reason_phrase(status);
  conn_wait_end(conn, s);
  write_response(conn, s, status, NULL, 0, "Content-Type: text/plain\r\n", phrase,
                 strlen(phrase), "close");
  conn_finish(conn, s);
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
  int responded;
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
                              raw_len, ((ConnNode *)req->conn)->peer);
}

/* The text of a received request's peer (the address after its packed
 * headers, PACKED_PEER): dotted IPv4, RFC 5952 IPv6, "" for none (a
 * request built in memory). */
char *avant_http_headers_peer(const char *data) {
  size_t n = avant_str_n(data);
  const unsigned char *p;
  char text[INET6_ADDRSTRLEN];
  if (!data || n < 4 + 17 || !(((const uint32_t *)(const void *)data)[0] & PACKED_PEER)) {
    return avant_str_empty();
  }
  p = (const unsigned char *)data + n - 17;
  if ((p[0] != 4 && p[0] != 16) ||
      !inet_ntop(p[0] == 4 ? AF_INET : AF_INET6, p + 1, text, sizeof text)) {
    return avant_str_empty();
  }
  return make_string(text, strlen(text));
}

/* A handler's status outside 200-599:
 * written as it came, 99999 or -1 gave a status line no client can read
 * (RFC 9110 15: a status code is three digits, its first 1 to 5), and a
 * 1xx is an interim answer (15.2), which a client reads past to wait for
 * the final one that never comes. The program's error, a trap of the
 * language before any byte of the answer is written (in a handler: the
 * request is answered 500). The WebSocket upgrade's 101 is the server's
 * own. */
__attribute__((noreturn, cold)) static void http_status_trap(avant_int status) {
  char msg[160];
  snprintf(msg, sizeof msg,
           "a response's status %lld is not 200 to 599: a handler gives the final answer, never an interim 1xx (RFC 9110)",
           (long long)status);
  avant_trap_text(msg);
}

void avant_http_req_respond(void *reqp, avant_int status, const char *ctype,
                            const char *head, const char *body) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  const char *conn_header = NULL;
  if (__builtin_expect(status < 200 || status > 599, 0)) {
    http_status_trap(status);
  }
  req->responded = 1;
  /* The head and body pointers name Avant heap objects held only in
   * C registers here; nothing below is a safepoint (no allocation, no
   * native wait), so no collection runs on any thread while the response
   * is written (the safepoint handshake; before it a no-GC region kept
   * another loop thread's collection off). */
  if (!req->keep_alive) {
    conn_header = "close";
  } else if (req->minor_version == 0) {
    conn_header = "keep-alive";
  }
  write_response(req->conn, req->sock, (int32_t)status, ctype,
                 avant_str_n(ctype), head, body, avant_str_n(body), conn_header);
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

/* A received response's Content-Length: 0
 * when it has none, 1 with its value in *out, -1 when its framing is
 * invalid: more than one Content-Length line (identical or not), a list
 * in one line, one continued on a folded line, or a value that is not a
 * number. The client read the first line only, so a response with two
 * values was read by one of them where a proxy may read the other (RFC
 * 9112 6.3: the user agent closes the connection and discards such a
 * response). The rule is the server's for requests. */
static int resp_content_length(struct phr_header *headers, size_t count,
                               long *out) {
  size_t i;
  int seen = 0;
  for (i = 0; i < count; i++) {
    long one = 0;
    if (!headers[i].name ||
        !ascii_ieq(headers[i].name, headers[i].name_len, "Content-Length", 14)) {
      continue;
    }
    if (seen || (i + 1 < count && !headers[i + 1].name) ||
        !parse_content_length(headers[i].value, headers[i].value_len, &one)) {
      return -1;
    }
    *out = one;
    seen = 1;
  }
  return seen;
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
static int64_t g_next_ws_id;
/* Records no connection uses, linked by next_all, under g_ws_mu. Its
 * length is bounded by the largest number of connections open at once. */
static AvantWsConn *g_ws_free;

/* Is this record still the connection numbered id? The one check every
 * operation through a WsConn value makes before it reads anything else
 * of the record. */
static int ws_record_is(const AvantWsConn *ws, avant_int id) {
  return id != 0 && __atomic_load_n(&ws->id, __ATOMIC_ACQUIRE) == id;
}

static int ws_record_owned(const AvantWsConn *ws) {
  return pthread_equal(pthread_self(),
                       __atomic_load_n(&ws->owner_tid, __ATOMIC_RELAXED));
}

/* A record with every field but its lock cleared and id 0, from the
 * free list or new; NULL when memory ran out. Every field of
 * AvantWsConn other than id, owner_tid and qmu is reset here. */
static AvantWsConn *ws_record_take(void) {
  AvantWsConn *ws;
  pthread_mutex_lock(&g_ws_mu);
  ws = g_ws_free;
  if (ws) { g_ws_free = ws->next_all; }
  pthread_mutex_unlock(&g_ws_mu);
  if (!ws) {
    ws = (AvantWsConn *)calloc(1, sizeof(AvantWsConn));
    if (!ws) { return NULL; }
    pthread_mutex_init(&ws->qmu, NULL);
  }
  ws->srv = NULL;
  ws->loop = NULL;
  ws->sock = NULL;
  ws->conn = NULL;
  ws->path = NULL;
  ws->route = -1;
  ws->frag = NULL;
  ws->frag_len = 0;
  ws->frag_cap = 0;
  ws->frag_opcode = -1;
  ws->out_head = NULL;
  ws->out_tail = NULL;
  ws->out_bytes = 0;
  ws->close_sent = 0;
  ws->close_received = 0;
  ws->received_code = 0;
  ws->dead = 0;
  ws->event_dispatched = 0;
  ws->pending_close_code = 0;
  ws->shutdown_after_flush = 0;
  ws->cut = 0;
  ws->ping_sent = 0;
  ws->recv_total = 0;
  ws->sent_total = 0;
  ws->next_all = NULL;
  ws->next_global = NULL;
  return ws;
}

/* The connection is over (or was never registered): its buffers and
 * queue are freed and the record goes back to the free list with id 0.
 * The caller has already taken it out of the server's and the global
 * list, so no sender by id can reach it; its owner thread is the only
 * other user of the queue and is the caller, or is gone. */
static void ws_record_release(AvantWsConn *ws) {
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
  pthread_mutex_lock(&g_ws_mu);
  __atomic_store_n(&ws->id, 0, __ATOMIC_RELEASE);
  ws->next_all = g_ws_free;
  g_ws_free = ws;
  pthread_mutex_unlock(&g_ws_mu);
}

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

/* The minimum rate's clocks of a WebSocket connection (owner thread
 * only; loop_sweep reads them). A message arriving in parts (a frame not
 * complete in the buffer, or a fragmented message not finished) must
 * come at the server's minimum rate, as a request body must: a client
 * sending a frame a byte at a time held its connection for good. The
 * clock is Conn's body_at, measured on recv_total. */
static void ws_watch_in(Conn *conn, struct us_socket_t *s) {
  AvantWsConn *ws = conn->ws;
  if (!ws || !conn->srv->rate || us_socket_is_closed(0, s)) {
    return;
  }
  if (!ws->dead && (conn->len > 0 || ws->frag_opcode >= 0)) {
    if (!conn->body_at) {
      conn->body_at = http_now_ms();
      conn->body_from = (size_t)ws->recv_total;
      conn_watch(conn, s);
    }
  } else if (conn->body_at) {
    conn->body_at = 0;
    if (!conn->out_at) {
      conn_unwatch(conn, s);
    }
  }
}

/* Likewise for the send queue: while it holds bytes they must leave at
 * the minimum rate, as an answer's queued tail must (a client that never
 * read what was queued for it held its connection, the queue's bound
 * limiting memory but not time). The clock is Conn's out_at, measured on
 * sent_total. */
static void ws_watch_out(AvantWsConn *ws, int queued) {
  Conn *conn = ws->conn;
  struct us_socket_t *s = ws->sock;
  if (!conn || !s || !conn->srv->rate || us_socket_is_closed(0, s)) {
    return;
  }
  if (queued) {
    if (!conn->out_at) {
      conn->out_at = http_now_ms();
      conn->out_from = (size_t)ws->sent_total;
      conn_watch(conn, s);
    }
  } else if (conn->out_at) {
    conn->out_at = 0;
    if (!conn->body_at) {
      conn_unwatch(conn, s);
    }
  }
}

/* Loop thread only. Writes as much of the queue as the kernel takes,
 * each piece at most the server's ws_write_chunk (a frame's rest may be
 * larger than an int). */
static void ws_flush(AvantWsConn *ws) {
  size_t chunk = ws->srv->ws_write_chunk;
  pthread_mutex_lock(&ws->qmu);
  while (ws->out_head) {
    WsOut *node = ws->out_head;
    int full = 0;
    while (node->off < node->len) {
      size_t remaining = node->len - node->off;
      int piece = remaining > chunk ? (int)chunk : (int)remaining;
      int written = us_socket_write(0, ws->sock, node->data + node->off,
                                    piece, 0);
      if (written < 0) { written = 0; }
      node->off += (size_t)written;
      ws->out_bytes -= (size_t)written;
      ws->sent_total += (uint64_t)written;
      if (written < piece) {
        full = 1;
        break;
      }
    }
    if (full) {
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
    ws_watch_out(ws, !drained);
    if (want_shutdown && !us_socket_is_closed(0, ws->sock)) {
      us_socket_shutdown(0, ws->sock);
    }
  }
}

/* Enqueue one already-framed buffer (takes ownership). Caller holds no
 * locks; returns 0 when the byte bound would be exceeded. begun says
 * that the frame's first bytes are already on the wire and this is its
 * rest: it is queued whatever the bound. Refusing it
 * left half a frame on the wire, whose stated length then swallowed the
 * next frames as payload; the bound only decides whether a new frame
 * starts, so the queue holds at most one frame's rest past it. */
static int ws_enqueue(AvantWsConn *ws, char *framed, size_t framed_len,
                      int begun) {
  WsOut *node;
  pthread_mutex_lock(&ws->qmu);
  if (!begun && ws->out_bytes + framed_len > WS_QUEUE_MAX_BYTES) {
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

/* A frame's first bytes are on the wire and its rest cannot be queued
 * (out of memory). Returning false and leaving the connection open let
 * the next frame's bytes be read as this one's payload; the connection
 * is closed instead, as the HTTP server's conn_send closes one whose
 * answer cannot be queued. Not here: a send may run under g_ws_mu, or
 * inside ws_process's loop over the connection's buffer, and the close
 * would call srv_on_close, which takes the lock and frees the buffer.
 * The record is marked dead (every later send answers false) and cut,
 * and the loop is woken: srv_on_wakeup closes it. Owner thread only (a
 * frame is begun only by a direct write). */
static int ws_cut(AvantWsConn *ws) {
  ws->dead = 1;
  ws->close_sent = 1;
  ws->cut = 1;
  if (ws->loop && ws->loop->loop) {
    us_wakeup_loop(ws->loop->loop);
  }
  return 0;
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
      /* Lengths stay size_t: the total was an
       * int, so a payload within 10 bytes of 2^31 made it negative, any
       * count the socket took passed for the whole frame, and send
       * returned true with only that part on the wire. The socket is
       * handed at most ws_write_chunk of the payload; the rest is queued
       * like any rest. */
      size_t total = header_len + len;
      size_t chunk = ws->srv->ws_write_chunk;
      size_t first = len > chunk ? chunk : len;
      int written = us_socket_write2(0, ws->sock, header, (int)header_len,
                                     payload, (int)first);
      if (written < 0) { written = 0; }
      if ((size_t)written >= total) {
        return 1;
      }
      if ((size_t)written < header_len) {
        size_t rem_h = header_len - (size_t)written;
        size_t framed_len = rem_h + len;
        char *framed = (char *)malloc(framed_len ? framed_len : 1);
        if (!framed) { return written > 0 ? ws_cut(ws) : 0; }
        memcpy(framed, header + written, rem_h);
        if (len > 0) { memcpy(framed + rem_h, payload, len); }
        if (!ws_enqueue(ws, framed, framed_len, written > 0)) {
          return written > 0 ? ws_cut(ws) : 0;
        }
        ws_watch_out(ws, 1);
        return 1;
      }
      {
        size_t rem = len - ((size_t)written - header_len);
        const char *src = payload + ((size_t)written - header_len);
        char *framed = (char *)malloc(rem ? rem : 1);
        if (!framed) { return ws_cut(ws); }
        if (rem > 0) { memcpy(framed, src, rem); }
        if (!ws_enqueue(ws, framed, rem, 1)) {
          return ws_cut(ws);
        }
        if ((size_t)written == header_len + first) {
          /* The socket took the whole piece: it has not asked for the
           * writable event, so the rest is written now, a piece at a
           * time, until the socket is full (ws_flush asks for the event
           * then). */
          ws_flush(ws);
        } else {
          ws_watch_out(ws, 1);
        }
        return 1;
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
      int ok = ws_enqueue(ws, framed, framed_len, 0);
      if (ok && ws->loop && ws->loop->loop) {
        us_wakeup_loop(ws->loop->loop);
      }
      return ok;
    }
    if (!ws_enqueue(ws, framed, framed_len, 0)) {
      return 0;
    }
    ws_watch_out(ws, 1);
    return 1;
  }
}

/* An id is an Avant Int: one no connection has finds none. */
static AvantWsConn *ws_find_global(avant_int id) {
  AvantWsConn *ws;
  for (ws = g_ws_all; ws; ws = ws->next_global) {
    if (ws->id == id) { return ws; }
  }
  return NULL;
}

static AvantWsConn *ws_find(AvantHttpServer *srv, avant_int id) {
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

/* The handshake's rule 3 (avant_gc_safepoint.c): a thread runs the
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

static void ws_fail(AvantWsConn *ws, int code);

static void ws_dispatch_event(AvantWsConn *ws, int32_t kind, int32_t is_text,
                              const char *data, size_t len, int32_t code,
                              const char *reason, size_t reason_len) {
  AvantWsEvent ev;
  int trapped;
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
  /* A trap in a message handler closes the connection with 1011 (an
   * unexpected condition, RFC 6455 7.4.1) and the server goes on; one in
   * a close handler has nothing left to close. */
  enter_avant();
  trapped = avant_task_recover_call(ws->srv->ws_dispatch, ws->srv->state, &ev);
  leave_avant();
  if (trapped && kind == 1) {
    ws_fail(ws, 1011);
  }
}

avant_int avant_ws_event_kind(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  /* First getter of every WS dispatch: the heap-goal boundary, in the
   * avant_http_req_target position. This frame holds no Avant heap
   * pointers yet, so the collection it may run strands nothing. */
  avant_gc_heap_goal_collect();
  return ev->kind;
}

avant_int avant_ws_event_conn_id(void *evp) {
  return ((AvantWsEvent *)evp)->ws->id;
}

/* The connection's first message route, set at the upgrade. */
avant_int avant_ws_event_route(void *evp) {
  return ((AvantWsEvent *)evp)->ws->route;
}

char *avant_ws_event_path(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  return make_string(ev->ws->path, strlen(ev->ws->path));
}

avant_int avant_ws_event_is_text(void *evp) {
  return ((AvantWsEvent *)evp)->is_text;
}

char *avant_ws_event_data(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  if (ev->len == 0) { return avant_str_empty(); }
  return make_bytes(ev->data, ev->len);
}

avant_int avant_ws_event_code(void *evp) {
  return ((AvantWsEvent *)evp)->code;
}

char *avant_ws_event_reason(void *evp) {
  AvantWsEvent *ev = (AvantWsEvent *)evp;
  if (ev->reason_len == 0) { return avant_str_empty(); }
  return make_string(ev->reason, ev->reason_len);
}

static int32_t ws_send_common(void *srvp, avant_int id, int32_t is_text,
                              const char *data) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  AvantWsConn *ws;
  size_t len;
  int ok;
  /* The data pointer names an Avant heap object held only in a C
   * register; nothing here is a safepoint (no allocation, no native wait:
   * g_ws_mu is a brief plain lock whose holders never stop), so no
   * collection runs on any thread meanwhile (the safepoint handshake;
   * before it this and the send and close entries below held a no-GC
   * region). */
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

avant_int avant_ws_send_text_by_id(avant_int id, const char *data) {
  return ws_send_common(NULL, id, 1, data);
}

avant_int avant_ws_send_text_bytes_by_id(avant_int id, const char *data) {
  return ws_send_common(NULL, id, 1, data);
}

avant_int avant_ws_send_bytes_by_id(avant_int id, const char *data) {
  return ws_send_common(NULL, id, 0, data);
}

int32_t avant_ws_close_conn(void *srvp, avant_int id, avant_int code,
                            const char *reason);

int32_t avant_ws_close_conn(void *srvp, avant_int id, avant_int code,
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

avant_int avant_ws_close_by_id(avant_int id, avant_int code, const char *reason) {
  return avant_ws_close_conn(NULL, id, code, reason);
}

void *avant_http_req_ws_conn(void *reqp) {
  return ((AvantHttpReq *)reqp)->ws_conn;
}

void *avant_ws_event_conn(void *evp) {
  return ((AvantWsEvent *)evp)->ws;
}

/* Loop-thread send through the connection's record itself: no registry
 * lock, because the owner thread is the only socket writer and the
 * only thread that releases the record. The value carries the record's
 * address and the connection's id (it used to carry
 * the address alone, and a WsConn kept after `serve` returned read a
 * record the server's teardown had freed). The record is type-stable
 * and the id is checked first: a value of a connection that has closed
 * answers 0 here, whether its record is free or serves a later
 * connection. On the owner thread a matching id cannot change under
 * the call (only this thread releases the record); from any other
 * thread the send goes by the value's id under the registry's lock. */
static int32_t ws_send_direct(void *handle, avant_int id, int32_t is_text,
                              const char *data) {
  AvantWsConn *ws = (AvantWsConn *)handle;
  size_t len;
  int ok;
  if (!ws || !ws_record_is(ws, id)) { return 0; }
  if (!ws_record_owned(ws)) {
    return ws_send_common(NULL, id, is_text, data);
  }
  if (ws->dead || ws->close_sent) { return 0; }
  len = avant_str_n(data);
  ok = ws_send_frame(ws, is_text ? 1 : 2, data, len);
  return ok;
}

avant_int avant_ws_send_text_direct(void *handle, avant_int id,
                                    const char *data) {
  return ws_send_direct(handle, id, 1, data);
}

avant_int avant_ws_send_text_bytes_direct(void *handle, avant_int id,
                                          const char *data) {
  return ws_send_direct(handle, id, 1, data);
}

avant_int avant_ws_send_bytes_direct(void *handle, avant_int id,
                                     const char *data) {
  return ws_send_direct(handle, id, 0, data);
}

avant_int avant_ws_close_direct(void *handle, avant_int id, avant_int code,
                                const char *reason) {
  AvantWsConn *ws = (AvantWsConn *)handle;
  unsigned char payload[125];
  size_t reason_len;
  int ok;
  if (!ws || !ws_record_is(ws, id)) { return 0; }
  if (!ws_record_owned(ws)) {
    return avant_ws_close_conn(NULL, id, code, reason);
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

avant_int avant_http_req_is_ws(void *reqp) {
  return ((AvantHttpReq *)reqp)->is_ws;
}

avant_int avant_http_req_ws_id(void *reqp) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  return req->ws_conn ? req->ws_conn->id : 0;
}

avant_int avant_http_req_ws_upgrade(void *reqp, avant_int route) {
  AvantHttpReq *req = (AvantHttpReq *)reqp;
  AvantHttpServer *srv = req->srv;
  AvantWsConn *ws;
  char accept[29];
  char response[256];
  int response_len;
  size_t path_len = 0;
  if (!req->is_ws || req->upgraded) { return 0; }
  ws = ws_record_take();
  if (!ws) { return 0; }
  while (path_len < req->target_len && req->target[path_len] != '?') {
    path_len++;
  }
  ws->path = (char *)malloc(path_len + 1);
  if (!ws->path) {
    ws_record_release(ws);
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
    ws_record_release(ws);
    return 0;
  }
  ws->srv = srv;
  ws->route = route;
  ws->loop = loop_of_socket(req->sock);
  ws->sock = req->sock;
  ws->conn = req->conn;
  __atomic_store_n(&ws->owner_tid, pthread_self(), __ATOMIC_RELAXED);
  pthread_mutex_lock(&g_ws_mu);
  __atomic_store_n(&ws->id, ++g_next_ws_id, __ATOMIC_RELEASE);
  ws->next_all = srv->ws_all;
  srv->ws_all = ws;
  ws->next_global = g_ws_all;
  g_ws_all = ws;
  pthread_mutex_unlock(&g_ws_mu);
  req->conn->ws = ws;
  req->ws_conn = ws;
  req->upgraded = 1;
  /* The HTTP idle timeout ends; the WebSocket one starts (the
   * connection had none: a peer gone without a FIN or a reset kept its
   * connection until the process ended). */
  us_socket_timeout(0, req->sock, 0);
  us_socket_long_timeout(0, req->sock, 0);
  idle_arm(req->sock, srv->ws_idle_secs, srv->ws_idle_mins);
  return 1;
}

/* RFC 3986's unreserved and sub-delims, the bytes of a reg-name (with
 * pct-encoded triplets) and of an IPv6 or IPvFuture literal's inside
 * (with ':'): one table, so the check is a load per byte. */
static const unsigned char http_host_byte[256] = {
  ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1, ['g'] = 1,
  ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1,
  ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1, ['u'] = 1,
  ['v'] = 1, ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1,
  ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1,
  ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1,
  ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1, ['U'] = 1,
  ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1,
  ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1, ['6'] = 1,
  ['7'] = 1, ['8'] = 1, ['9'] = 1,
  ['-'] = 1, ['.'] = 1, ['_'] = 1, ['~'] = 1,
  ['!'] = 1, ['$'] = 1, ['&'] = 1, ['\''] = 1, ['('] = 1, [')'] = 1, ['*'] = 1,
  ['+'] = 1, [','] = 1, [';'] = 1, ['='] = 1
};

static int http_hex(unsigned char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
         (ch >= 'A' && ch <= 'F');
}

/* Whether a Host value is uri-host [ ":" port ] (RFC 9112 3.2, RFC
 * 3986 3.2.2 and 3.2.3): a reg-name (an IPv4
 * address is one) of unreserved, sub-delims and pct-encoded bytes, or
 * an IP literal in brackets, then an optional ':' and digits. Empty is
 * valid (a target without an authority). The server checked only that
 * there was one Host line: a value with a space, an '@', a path or a
 * second ':' was accepted, and a router keyed on Host, or a proxy in
 * front, could read another host in it. */
static int http_host_valid(const char *p, size_t n) {
  size_t i = 0;
  if (n > 0 && p[0] == '[') {
    for (i = 1; i < n && p[i] != ']'; i++) {
      if (!http_host_byte[(unsigned char)p[i]] && p[i] != ':') {
        return 0;
      }
    }
    if (i == n || i == 1) {
      return 0;
    }
    i++;
  } else {
    /* The table first: a name's byte costs one load and one test (on
     * the hello path's Host, "127.0.0.1:18301", about 20 instructions
     * fewer per request than testing ':' and '%' first; the check as a
     * whole is about 120). */
    while (i < n) {
      unsigned char ch = (unsigned char)p[i];
      if (http_host_byte[ch]) {
        i++;
        continue;
      }
      if (ch == ':') {
        break;
      }
      if (ch != '%' || i + 2 >= n || !http_hex((unsigned char)p[i + 1]) ||
          !http_hex((unsigned char)p[i + 2])) {
        return 0;
      }
      i += 3;
    }
  }
  if (i == n) {
    return 1;
  }
  if (p[i] != ':') {
    return 0;
  }
  for (i++; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return 0;
    }
  }
  return 1;
}

static void conn_process_requests(Conn *conn, struct us_socket_t *s) {
  AvantHttpServer *srv = conn->srv;

  if (conn->answered) {
    return;
  }
  for (;;) {
    /* An answer's tail is still queued: the next request waits in the
     * buffer until srv_on_writable has sent it. */
    if (conn_out_pending(conn)) {
      return;
    }
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
    int trapped;
    size_t total;
    AvantHttpReq req;

    pret = phr_parse_request(conn->buf, conn->len, &method, &method_len,
                             &target, &target_len, &minor_version, headers,
                             &num_headers, 0);
    if (pret == -2) {
      if (conn->len > HTTP_MAX_HEADER_BLOCK) {
        send_error(conn, s, 400);
      } else if (!conn->head_at) {
        /* A head in parts: it has the head timeout from now (the
         * idle timeout restarted at every byte, so a client sending a
         * byte now and then held the connection for good). */
        conn->head_at = http_now_ms();
        conn_watch(conn, s);
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
    /* Every Content-Length line is read, not the first only: lines that
     * disagree, or one continued on a folded
     * line, give two readings of where the body ends, and a proxy in
     * front may take the other one (request smuggling). RFC 9112 6.3:
     * 400 and close. Every repeat is refused too, identical or not
     * (the owner's choice; nginx does the same):
     * two lines "5" and one line "5, 5" are the same list (RFC 9110 5.3),
     * and a list in one line is not a number here, so both spellings
     * are 400, where two identical lines used to be one value.
     * The same pass counts the Host lines:
     * RFC 9112 3.2 has a server answer 400 to an HTTP/1.1 request
     * without one and to any request with two (identical or not), and a
     * Host continued on a folded line is an invalid value, as is one
     * that is not uri-host [ ":" port ]; a request
     * that names two hosts is read one way here and maybe the other way
     * by a proxy or a virtual-host router in front. */
    {
      size_t hi;
      int hosts = 0;
      for (hi = 0; hi < num_headers; hi++) {
        long one = 0;
        if (!headers[hi].name) {
          continue;
        }
        if (ascii_ieq(headers[hi].name, headers[hi].name_len, "Host", 4)) {
          if (++hosts > 1 || (hi + 1 < num_headers && !headers[hi + 1].name) ||
              !http_host_valid(headers[hi].value, headers[hi].value_len)) {
            send_error(conn, s, 400);
            return;
          }
          continue;
        }
        if (!ascii_ieq(headers[hi].name, headers[hi].name_len,
                       "Content-Length", 14)) {
          continue;
        }
        if ((hi + 1 < num_headers && !headers[hi + 1].name) ||
            !parse_content_length(headers[hi].value, headers[hi].value_len,
                                  &one) ||
            has_cl) {
          send_error(conn, s, 400);
          return;
        }
        content_length = one;
        has_cl = 1;
      }
      if (hosts == 0 && minor_version >= 1) {
        send_error(conn, s, 400);
        return;
      }
    }
    if (__builtin_expect(__atomic_load_n(&srv->over_mem, __ATOMIC_RELAXED), 0)) {
      /* The process is past its memory ceiling: new requests are shed
       * until the sweep reads it under again. */
      send_error(conn, s, 503);
      return;
    }
    if (has_cl && content_length > srv->max_body) {
      send_error(conn, s, 413);
      return;
    }
    total = (size_t)pret + (size_t)(has_cl ? content_length : 0);
    if (conn->len < total) {
      /* The head is complete; the body waits for bytes, which must
       * come at the minimum rate once its grace has passed. */
      conn->head_at = 0;
      if (!conn->body_at) {
        /* The body takes its length from the server's budget while it
         * waits (a body that arrives with its head takes none). */
        if (srv->body_budget &&
            __atomic_add_fetch(&srv->body_held, content_length, __ATOMIC_RELAXED) >
                srv->body_budget) {
          __atomic_sub_fetch(&srv->body_held, content_length, __ATOMIC_RELAXED);
          send_error(conn, s, 503);
          return;
        }
        conn->budget = srv->body_budget ? content_length : 0;
        conn->body_at = http_now_ms();
        conn->body_from = conn->len;
        conn_watch(conn, s);
      }
      if (find_header(headers, num_headers, "Expect", &value, &value_len) &&
          ascii_ieq(value, value_len, "100-continue", 12) &&
          !conn->sent_continue) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        conn_send(conn, s, cont, strlen(cont), NULL, 0);
        conn->sent_continue = 1;
      }
      return;
    }
    if (conn->head_at | conn->body_at) {
      conn_wait_end(conn, s);
    }
    keep_alive = minor_version == 1;
    if (find_header(headers, num_headers, "Connection", &value, &value_len)) {
      if (ascii_ieq_contains(value, value_len, "close")) {
        keep_alive = 0;
      } else if (ascii_ieq_contains(value, value_len, "keep-alive")) {
        keep_alive = 1;
      }
    }
    /* WebSocket upgrade: a request carrying an Upgrade header
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
    if (req.is_ws) {
      /* The upgrade writes its 101 itself: the answers gathered go
       * first, and one still queued keeps the handshake in the buffer
       * until it has gone (conn_flush comes back here). */
      conn_uncork(conn, s);
      if (us_socket_is_closed(0, s) || conn_out_pending(conn)) {
        return;
      }
    } else {
      /* Gather this answer when a request waits behind it, or a batch
       * has begun: the batch is written once, when the buffer's requests
       * are answered (conn_process). */
      conn->corking = conn->len > total || conn->cork_len > 0;
    }
    /* The dispatch runs in a recovery point: a trap in the handler (or
     * in std.http's code for this request) returns here, the handler's
     * frames dropped, and the request is answered 500, with the
     * connection kept or closed as an answer's would be; the server
     * goes on. A trap after the upgrade, in a websocket_open handler,
     * closes the new WebSocket with 1011. */
    enter_avant();
    trapped = avant_task_recover_call(srv->dispatch, srv->state, &req);
    leave_avant();
    if (us_socket_is_closed(0, s)) {
      /* The answer could not be queued (conn_send closed the socket);
       * srv_on_close has freed the buffers. */
      return;
    }
    if (trapped && req.upgraded) {
      conn->len = 0;
      ws_fail(conn->ws, 1011);
      return;
    }
    if (trapped && !req.responded) {
      write_response(conn, s, 500, NULL, 0, "Content-Type: text/plain\r\n",
                     "Internal Server Error", strlen("Internal Server Error"),
                     !keep_alive ? "close"
                                 : minor_version == 0 ? "keep-alive" : NULL);
    }
    conn->corking = 0;
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
        ws_watch_in(conn, s);
      }
      return;
    }
    if (!keep_alive) {
      conn_finish(conn, s);
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
      if (__builtin_expect(conn->cap > HTTP_BUF_KEEP, 0)) {
        free(conn->buf);
        conn->buf = NULL;
        conn->cap = 0;
      }
      return;
    }
  }
}

/* The requests in a connection's buffer, answered in order; the
 * answers gathered under pipelining are written once, at the end. */
static void conn_process(Conn *conn, struct us_socket_t *s) {
  conn_process_requests(conn, s);
  conn->corking = 0;
  if (__builtin_expect(conn->cork_len != 0, 0) && !us_socket_is_closed(0, s)) {
    conn_uncork(conn, s);
  }
}

/* Loop thread only, from srv_on_writable: sends the queued tail. When
 * it has all gone the connection takes up what waited behind it: the
 * shutdown of a finished connection, the requests still in the input
 * buffer (which may queue again), the close a peer's FIN asked for. */
static void conn_flush(Conn *conn, struct us_socket_t *s) {
  if (conn_out_pending(conn)) {
    size_t before = conn->out_off;
    int done = sock_write_rest(s, conn->out, conn->out_len, &conn->out_off,
                               conn->srv->ws_write_chunk);
    if (conn->out_off > before) {
      /* The client took queued bytes: it is not stalled. */
      idle_arm(s, conn->srv->idle_secs, conn->srv->idle_mins);
    }
    if (!done) {
      /* us_socket_write polls for readable again; this connection is
       * read only when its queue is empty. */
      conn_poll(s, LIBUS_SOCKET_WRITABLE);
      return;
    }
  }
  free(conn->out);
  conn->out = NULL;
  conn->out_len = 0;
  conn->out_off = 0;
  conn->out_cap = 0;
  conn->out_at = 0;
  if (!conn->head_at && !conn->body_at) {
    conn_unwatch(conn, s);
  }
  conn_poll(s, LIBUS_SOCKET_READABLE | LIBUS_SOCKET_WRITABLE);
  if (conn->close_after_flush) {
    us_socket_shutdown(0, s);
    if (conn->peer_ended) {
      us_socket_close(0, s, 0, NULL);
    }
    return;
  }
  if (!conn->answered && !conn->ws && conn->len > 0) {
    conn_process(conn, s);
    if (us_socket_is_closed(0, s)) {
      return;
    }
  }
  if (conn->peer_ended && !conn_out_pending(conn)) {
    us_socket_shutdown(0, s);
    us_socket_close(0, s, 0, NULL);
  }
}

/* The process's resident memory in bytes (/proc/self/statm), 0 when it
 * cannot be read. */
static int64_t http_rss(void) {
  char text[128];
  long long pages = 0;
  long long size = 0;
  ssize_t n;
  int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  text[n] = 0;
  if (sscanf(text, "%lld %lld", &size, &pages) != 2) {
    return 0;
  }
  return (int64_t)pages * (int64_t)sysconf(_SC_PAGESIZE);
}

/* Whether `got` bytes in `ms` milliseconds is under `rate` bytes a second. */
static int http_too_slow(size_t got, int64_t ms, uint32_t rate) {
  return (uint64_t)got * 1000u < (uint64_t)rate * (uint64_t)ms;
}

/* A request past its deadline: 408 and close. The socket is closed at
 * the next timeout tick (at most 4 s) whatever the peer does: the
 * answer has its chance to leave, and a peer that neither reads nor
 * closes holds nothing for long. */
static void conn_request_timeout(Conn *conn, struct us_socket_t *s) {
  send_error(conn, s, 408);
  if (!us_socket_is_closed(0, s)) {
    us_socket_timeout(0, s, 1);
  }
}

/* The loop's sweep, every sweep_ms (at most a second): each watched
 * connection's deadlines. A head past head_ms is answered 408. A body
 * waiting for bytes, and an answer's queued tail, are measured over
 * windows of grace_ms: when one has passed, the bytes that came (or
 * left) in it must be the minimum rate's, else the request is answered
 * 408 (a body) or the connection closed (a client that does not read:
 * nothing more can be said to it); then a new window begins. An average
 * since the start would not do: the kernel takes megabytes of an answer
 * at once, and that burst would cover a client that then reads nothing
 * for hours. */
static void loop_sweep(struct us_timer_t *t) {
  HttpLoop *hl = *(HttpLoop **)us_timer_ext(t);
  AvantHttpServer *srv = hl->srv;
  int64_t now = http_now_ms();
  ConnNode *node;
  if (hl->accept_paused) {
    /* A connection could not be accepted for want of descriptors: try
     * again now, at most once a sweep. */
    hl->accept_paused = 0;
    if (hl->listen) {
      us_poll_change(&hl->listen->s.p, hl->loop, LIBUS_SOCKET_READABLE);
    }
  }
  if (srv->mem_ceiling > 0 && hl == &srv->loops[0]) {
    __atomic_store_n(&srv->over_mem, http_rss() >= srv->mem_ceiling, __ATOMIC_RELAXED);
  }
  if (hl->close_at && srv->shutdown_ms &&
      now - hl->close_at >= (int64_t)srv->shutdown_ms) {
    /* The shutdown deadline: what close left to finish (an answer being
     * sent, a WebSocket's closing handshake, a peer that does not close)
     * is closed now, so the loop drains and serve returns. */
    for (node = hl->conns_head; node; node = node->next) {
      if (!us_socket_is_closed(0, node->sock)) {
        us_socket_close(0, node->sock, 0, NULL);
      }
    }
    return;
  }
  node = hl->watch;
  while (node) {
    ConnNode *next = node->wnext;
    Conn *conn = &node->conn;
    struct us_socket_t *s = node->sock;
    if (conn->ws) {
      /* A WebSocket message arriving in parts, or a send queue, too slow
       * over a window (ws_watch_in, ws_watch_out): closed. */
      AvantWsConn *ws = conn->ws;
      int slow = 0;
      if (conn->body_at && srv->rate && now - conn->body_at >= (int64_t)srv->grace_ms) {
        if (http_too_slow((size_t)ws->recv_total - conn->body_from, now - conn->body_at, srv->rate)) {
          slow = 1;
        } else {
          conn->body_at = now;
          conn->body_from = (size_t)ws->recv_total;
        }
      }
      if (!slow && conn->out_at && srv->rate && now - conn->out_at >= (int64_t)srv->grace_ms) {
        if (http_too_slow((size_t)ws->sent_total - conn->out_from, now - conn->out_at, srv->rate)) {
          slow = 1;
        } else {
          conn->out_at = now;
          conn->out_from = (size_t)ws->sent_total;
        }
      }
      if (slow) {
        us_socket_close(0, s, 0, NULL);
      }
    } else if (conn->answered && !conn->out_at) {
      conn_wait_end(conn, s);
    } else if (conn->head_at && srv->head_ms &&
               now - conn->head_at >= (int64_t)srv->head_ms) {
      conn_request_timeout(conn, s);
    } else if (conn->body_at && srv->rate &&
               now - conn->body_at >= (int64_t)srv->grace_ms) {
      if (http_too_slow(conn->len - conn->body_from, now - conn->body_at,
                        srv->rate)) {
        conn_request_timeout(conn, s);
      } else {
        conn->body_at = now;
        conn->body_from = conn->len;
      }
    } else if (conn->out_at && srv->rate &&
               now - conn->out_at >= (int64_t)srv->grace_ms) {
      if (http_too_slow(conn->out_off - conn->out_from, now - conn->out_at,
                        srv->rate)) {
        us_socket_close(0, s, 0, NULL);
      } else {
        conn->out_at = now;
        conn->out_from = conn->out_off;
      }
    }
    node = next;
  }
}

/* uSockets' hook (its loop.c, an Avant patch): accept failed with
 * EMFILE, ENFILE, ENOBUFS or ENOMEM and uSockets stopped polling the
 * listening socket, which stayed readable with the connection in its
 * backlog: polled again at once, the loop spun a core for as long as the
 * shortage lasted. The loop's next sweep (at most a second away) polls it
 * again. Only a server's loop has listening sockets. */
void us_internal_accept_paused(struct us_listen_socket_t *ls) {
  HttpLoop *hl = *(HttpLoop **)us_socket_context_ext(0, us_socket_context(0, &ls->s));
  if (hl) {
    hl->accept_paused = 1;
  }
}

/* The sweep's period: a second, or a quarter of the shortest deadline
 * it watches when that is shorter, so a deadline is kept to within a
 * quarter of itself; never under 25 ms. */
static int sweep_interval(const AvantHttpServer *srv) {
  uint32_t iv = 1000;
  if (srv->head_ms && srv->head_ms / 4 < iv) {
    iv = srv->head_ms / 4;
  }
  if (srv->rate && srv->grace_ms / 4 < iv) {
    iv = srv->grace_ms / 4;
  }
  if (srv->shutdown_ms && srv->shutdown_ms / 4 < iv) {
    iv = srv->shutdown_ms / 4;
  }
  return iv < 25 ? 25 : (int)iv;
}

/* The loop's idle connection (kept alive between requests: nothing in
 * its buffer, nothing queued, no deadline running, not a WebSocket) that
 * has been idle longest, but `except`; NULL when there is none. Its idle
 * timeout was started at its last activity, so the one due soonest
 * (uSockets' ticks: 4 s each; among equals the oldest connection, the
 * list being newest first) is the longest idle. Walked only at the
 * connection limit. */
static ConnNode *loop_longest_idle(HttpLoop *hl, ConnNode *except) {
  ConnNode *best = NULL;
  unsigned best_left = 1000;
  ConnNode *node;
  for (node = hl->conns_head; node; node = node->next) {
    Conn *c = &node->conn;
    unsigned left;
    if (node == except || c->ws || c->answered || c->len || node->watched ||
        conn_out_pending(c) || us_socket_is_closed(0, node->sock)) {
      continue;
    }
    left = node->sock->timeout == 255
               ? 0
               : (node->sock->timeout + 240u - node->sock->context->timestamp) % 240u;
    if (left <= best_left) {
      best_left = left;
      best = node;
    }
  }
  return best;
}

static struct us_socket_t *srv_on_open(struct us_socket_t *s, int is_client,
                                       char *ip, int ip_length) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  HttpLoop *hl = loop_of_socket(s);
  (void)is_client;
  AvantHttpServer *srv = hl->srv;
  int64_t n;
  memset(node, 0, sizeof(ConnNode));
  node->conn.srv = srv;
  node->sock = s;
  if (ip && ip_length == 16 &&
      memcmp(ip, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0) {
    ip += 12;
    ip_length = 4;
  }
  if (ip && (ip_length == 4 || ip_length == 16)) {
    node->peer[0] = (unsigned char)ip_length;
    memcpy(node->peer + 1, ip, (size_t)ip_length);
  }
  node->next = hl->conns_head;
  hl->conns_head = node;
  idle_arm(s, srv->idle_secs, srv->idle_mins);
  n = __atomic_add_fetch(&srv->conns, 1, __ATOMIC_RELAXED);
  if (srv->max_conns && n > srv->max_conns) {
    /* At the connection limit: the loop's longest idle connection makes
     * room, or, with none idle, the new one is closed. */
    ConnNode *idle = loop_longest_idle(hl, node);
    if (!idle) {
      return us_socket_close(0, s, 0, NULL);
    }
    us_socket_close(0, idle->sock, 0, NULL);
  }
  return s;
}

static struct us_socket_t *srv_on_data(struct us_socket_t *s, char *data,
                                       int length) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  Conn *conn = &node->conn;
  if (conn->answered) {
    return s;
  }
  if (!conn->ws) {
    /* Request bytes arrived: the idle timeout starts again. */
    idle_arm(s, conn->srv->idle_secs, conn->srv->idle_mins);
  } else {
    /* The peer is alive: its idle timeout starts again, a ping sent is
     * answered (by this or anything else). */
    conn->ws->recv_total += (uint64_t)length;
    conn->ws->ping_sent = 0;
    idle_arm(s, conn->srv->ws_idle_secs, conn->srv->ws_idle_mins);
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
    if (conn->ws && (conn->len || conn->ws->frag_opcode >= 0 || conn->body_at)) {
      ws_watch_in(conn, s);
    }
  } else {
    conn_process(conn, s);
  }
  return s;
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
    ws_record_release(ws);
  }
  conn_unwatch(&node->conn, s);
  if (node->conn.budget) {
    __atomic_sub_fetch(&node->conn.srv->body_held, node->conn.budget, __ATOMIC_RELAXED);
    node->conn.budget = 0;
  }
  __atomic_sub_fetch(&node->conn.srv->conns, 1, __ATOMIC_RELAXED);
  free(node->conn.buf);
  node->conn.buf = NULL;
  node->conn.len = 0;
  node->conn.cap = 0;
  free(node->conn.out);
  node->conn.out = NULL;
  node->conn.out_len = 0;
  node->conn.out_off = 0;
  node->conn.out_cap = 0;
  node->conn.cork_len = 0;
  node->conn.corking = 0;
  if (t_cork_owner == &node->conn) {
    t_cork_owner = NULL;
  }
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
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  if (!node->conn.ws && conn_out_pending(&node->conn)) {
    /* The peer finished sending but may still be reading: what is
     * queued, and the requests behind it, are answered first
     * (conn_flush closes). */
    node->conn.peer_ended = 1;
    conn_poll(s, LIBUS_SOCKET_WRITABLE);
    return s;
  }
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

/* An HTTP connection's idle timeout closes it. A WebSocket connection
 * that has received nothing for its idle timeout is pinged first (a
 * pong, or anything else that arrives, starts the timeout again); when
 * it passes again with nothing received, or the connection is closing,
 * it is closed (1006 to its close handler). */
static struct us_socket_t *srv_on_timeout(struct us_socket_t *s) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  AvantWsConn *ws = node->conn.ws;
  if (ws && !ws->dead && !ws->close_sent && !ws->ping_sent) {
    AvantHttpServer *srv = node->conn.srv;
    ws->ping_sent = 1;
    ws_send_frame(ws, 9, "", 0);
    ws_flush(ws);
    idle_arm(s, srv->ws_idle_secs, srv->ws_idle_mins);
    return s;
  }
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *srv_on_writable(struct us_socket_t *s) {
  ConnNode *node = (ConnNode *)us_socket_ext(0, s);
  if (node->conn.ws) {
    ws_flush(node->conn.ws);
  } else {
    conn_flush(&node->conn, s);
  }
  return s;
}

static void timers_stop(AvantHttpServer *srv);

static void srv_on_wakeup(struct us_loop_t *loop) {
  HttpLoop *hl = *(HttpLoop **)us_loop_ext(loop);
  if (!hl) {
    return;
  }
  if (!hl->srv->closing) {
    /* A cross-thread WebSocket send queued frames and woke the loop, or
     * a frame was cut (ws_cut): that connection is closed. */
    ConnNode *node = hl->conns_head;
    while (node) {
      ConnNode *next = node->next;
      if (node->conn.ws && node->conn.ws->cut) {
        us_socket_close(0, node->sock, 0, NULL);
      } else if (node->conn.ws) {
        ws_flush(node->conn.ws);
      }
      node = next;
    }
    return;
  }
  if (hl->listen) {
    us_listen_socket_close(0, hl->listen);
    hl->listen = NULL;
  }
  if (hl == &hl->srv->loops[0]) {
    /* The program's timers stop with the server: disarmed here and
     * closed at teardown, after the loop has ended. Closing one inside
     * the running loop is not possible: us_poll_free counts every poll
     * out of the loop's num_polls, a fallthrough timer too, which never
     * counted in, so the loop's count went one below its sockets and
     * the loop never ended (serve hung). */
    timers_stop(hl->srv);
  }
  if (!hl->close_at) {
    hl->close_at = http_now_ms();
  }
  {
    /* Answered connections are already shut down with their response
     * queued ahead of the FIN; leave them to close from the peer side
     * so the last response is not discarded. Idle connections are
     * closed here so the loop can drain. Open WebSocket connections
     * first get a 1001 close frame (going away) and the same leave-to-peer
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
      } else if (conn_out_pending(&node->conn)) {
        /* An answer's tail is still queued: it is sent, then the
         * connection shuts down (conn_flush); requests behind it are
         * not taken. */
        node->conn.answered = 1;
        node->conn.close_after_flush = 1;
      }
      if (!node->conn.answered) {
        us_socket_close(0, node->sock, 0, NULL);
      }
      node = next;
    }
  }
}

/* A program's timer fired (Server.every): its handler runs as a
 * handler does, in a recovery point (a trap is printed and the server
 * goes on), unless the server is closing. */
static void timer_fire(struct us_timer_t *t) {
  HttpTimer *ht = *(HttpTimer **)us_timer_ext(t);
  AvantHttpServer *srv = ht->srv;
  if (srv->closing || !srv->timer_dispatch || !srv->state) {
    return;
  }
  enter_avant();
  (void)avant_task_recover_call(srv->timer_dispatch, srv->state, ht);
  leave_avant();
}

static void timers_stop(AvantHttpServer *srv) {
  int i;
  for (i = 0; i < srv->ntimers; i++) {
    if (srv->timers[i].t) {
      us_timer_set(srv->timers[i].t, timer_fire, 0, 0);
    }
  }
}

static void timers_close(AvantHttpServer *srv) {
  int i;
  for (i = 0; i < srv->ntimers; i++) {
    if (srv->timers[i].t) {
      us_timer_close(srv->timers[i].t);
      srv->timers[i].t = NULL;
    }
  }
}

avant_int avant_http_timer_index(void *evp) {
  return ((HttpTimer *)evp)->index;
}

/* Server.every: a timer of `ms` milliseconds (1 or more, checked by
 * std.http), started when serve starts; its index. */
avant_int avant_http_add_timer(void *srvp, avant_int ms) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  HttpTimer *next;
  if (!srv) {
    return -1;
  }
  next = (HttpTimer *)realloc(srv->timers, sizeof(HttpTimer) * (size_t)(srv->ntimers + 1));
  if (!next) {
    avant_trap_text("out of memory adding a timer to the server");
  }
  srv->timers = next;
  next[srv->ntimers].srv = srv;
  next[srv->ntimers].ms = ms > INT32_MAX ? INT32_MAX : (uint32_t)ms;
  next[srv->ntimers].index = srv->ntimers;
  next[srv->ntimers].t = NULL;
  return srv->ntimers++;
}

static void srv_pre(struct us_loop_t *loop) {
  (void)loop;
}

static void srv_post(struct us_loop_t *loop) {
  (void)loop;
}

/* Bound (host, port) pairs owned by live servers in this process.
 * SO_REUSEPORT lets every loop of one server share its port, but it
 * would also let a second server bind the same port; the contract is
 * that a second listen fails. The registry keeps the
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
  if (hl->sweep) {
    us_timer_close(hl->sweep);
    hl->sweep = NULL;
  }
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
  for (;;) {
    AvantWsConn *ws;
    AvantWsConn **link2 = &g_ws_all;
    pthread_mutex_lock(&g_ws_mu);
    ws = srv->ws_all;
    if (!ws) {
      pthread_mutex_unlock(&g_ws_mu);
      break;
    }
    srv->ws_all = ws->next_all;
    while (*link2 && *link2 != ws) {
      link2 = &(*link2)->next_global;
    }
    if (*link2 == ws) {
      *link2 = ws->next_global;
    }
    pthread_mutex_unlock(&g_ws_mu);
    ws_record_release(ws);
  }
  timers_close(srv);
  free(srv->timers);
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
  us_socket_context_on_long_timeout(0, hl->ctx, srv_on_timeout);
  us_socket_context_on_end(0, hl->ctx, srv_on_end);
  hl->listen = us_socket_context_listen(0, hl->ctx, host, (int)port, 0,
                                        (int)sizeof(ConnNode));
  if (!hl->listen) {
    return 1;
  }
  /* The deadlines' sweep (started by serve), a timer that does not keep
   * the loop running: the loop drains as before. */
  hl->sweep = us_create_timer(hl->loop, 1, (unsigned int)sizeof(HttpLoop *));
  if (!hl->sweep) {
    return 1;
  }
  *(HttpLoop **)us_timer_ext(hl->sweep) = hl;
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

/* The memory limit of the container the process runs in: the smallest
 * cgroup v2 memory.max from its own cgroup up (or cgroup v1's
 * memory.limit_in_bytes), 0 when there is none. */
static int64_t read_limit_file(const char *path) {
  char text[64];
  ssize_t n;
  long long v;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  n = read(fd, text, sizeof text - 1);
  close(fd);
  if (n <= 0) {
    return 0;
  }
  text[n] = 0;
  if (text[0] < '0' || text[0] > '9' || sscanf(text, "%lld", &v) != 1 || v <= 0 ||
      v >= (1LL << 60)) {
    return 0;
  }
  return (int64_t)v;
}

static int64_t http_container_memory(void) {
  char line[512];
  char path[700];
  int64_t best = 0;
  FILE *f = fopen("/proc/self/cgroup", "re");
  if (f) {
    while (fgets(line, sizeof line, f)) {
      if (strncmp(line, "0::", 3) == 0) {
        char *rel = line + 3;
        char *end = rel + strlen(rel);
        while (end > rel && (end[-1] == '\n' || end[-1] == '/')) {
          *--end = 0;
        }
        for (;;) {
          int64_t v;
          snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.max", rel);
          v = read_limit_file(path);
          if (v > 0 && (best == 0 || v < best)) {
            best = v;
          }
          end = strrchr(rel, '/');
          if (!end) {
            break;
          }
          *end = 0;
        }
      }
    }
    fclose(f);
  }
  if (best == 0) {
    best = read_limit_file("/sys/fs/cgroup/memory/memory.limit_in_bytes");
  }
  return best;
}

/* `loops` <= 0 means the runtime's chosen thread count. */
void *avant_http_listen(const char *host, avant_int port, avant_int max_body,
                        avant_int loops, avant_int max_message) {
  AvantHttpServer *srv;
  int n;
  int i;
  set_status(0);
  /* A port is 0..65535 and the limits and loop count C ints: anything else
   * fails like a port that cannot be bound. */
  if (port < 0 || port > 65535 || loops > 4096 || max_body > INT32_MAX || max_message > INT32_MAX) {
    set_status(1);
    return NULL;
  }
  if (port == 0) {
    port = pick_ephemeral_port(host);
    if (port == 0) {
      set_status(1);
      return NULL;
    }
  }
  n = loops > 0 ? (int)loops : (int)avant_pool_size_fix();
  if (n < 1) {
    n = 1;
  }
  srv = (AvantHttpServer *)calloc(1, sizeof(AvantHttpServer));
  if (!srv) {
    set_status(4);
    return NULL;
  }
  srv->max_body = (int32_t)max_body;
  srv->max_message = max_message > 0 ? (int32_t)max_message : 1048576;
  idle_limit(HTTP_IDLE_TIMEOUT_MS, &srv->idle_secs, &srv->idle_mins);
  idle_limit(HTTP_IDLE_TIMEOUT_MS, &srv->ws_idle_secs, &srv->ws_idle_mins);
  srv->ws_write_chunk = http_ws_write_chunk();
  srv->head_ms = HTTP_HEAD_TIMEOUT_MS;
  srv->rate = HTTP_MIN_RATE;
  srv->grace_ms = HTTP_RATE_GRACE_MS;
  srv->body_budget = HTTP_BODY_BUDGET;
  srv->mem_ceiling = -1;
  srv->max_conns = HTTP_MAX_CONNS;
  srv->shutdown_ms = HTTP_SHUTDOWN_MS;
  srv->loops = (HttpLoop *)calloc((size_t)n, sizeof(HttpLoop));
  if (!srv->loops) {
    free(srv);
    set_status(4);
    return NULL;
  }
  srv->nloops = n;
  if (listen_port_taken(host, (int32_t)port, srv)) {
    free(srv->loops);
    free(srv);
    set_status(1);
    return NULL;
  }
  for (i = 0; i < n; i++) {
    int32_t p = i == 0 ? (int32_t)port : srv->port;
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

avant_int avant_http_port(void *srvp) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  return srv ? srv->port : 0;
}

/* Loop threads follow the pool's registration model: register with
 * the collector before the first dispatch, allocate young in their own
 * young blocks while serving (the mature space before),
 * unregister after the loop has drained (no Avant frame is active
 * then). */
static void *loop_thread_main(void *arg) {
  HttpLoop *hl = (HttpLoop *)arg;
  avant_gc_register_thread();
  avant_stack_role(AVANT_STACK_LOOP);
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
  cork_release();
  return NULL;
}

avant_int avant_http_serve(void *srvp, void *state, void *cb, void *ws_cb,
                           void *timer_cb) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  int i;
  if (!srv || !srv->loops || srv->nloops < 1) {
    set_status(1);
    return 1;
  }
  /* srv (C memory) holds the state across every dispatch, which
   * collects; with one loop the program can stay unshared, where a young
   * collection may move it (S+). Pinned for the serve. */
  avant_pin(state);
  srv->state = state;
  srv->dispatch = (AvantHttpDispatch)cb;
  srv->ws_dispatch = (AvantHttpDispatch)ws_cb;
  srv->timer_dispatch = (AvantHttpDispatch)timer_cb;
  srv->running = 1;
  for (i = 0; i < srv->ntimers; i++) {
    HttpTimer *ht = &srv->timers[i];
    ht->t = us_create_timer(srv->loops[0].loop, 1, (unsigned int)sizeof(HttpTimer *));
    if (ht->t) {
      *(HttpTimer **)us_timer_ext(ht->t) = ht;
      us_timer_set(ht->t, timer_fire, (int)ht->ms, (int)ht->ms);
    }
  }
  if (srv->mem_ceiling < 0) {
    int64_t limit = http_container_memory();
    srv->mem_ceiling = limit > 0 ? limit / 10 * 9 : 0;
  }
  {
    /* The connection limit is capped by the file-descriptor limit: past
     * it accept fails and uSockets polls the listening socket again at
     * once, a busy loop, instead of the limit's eviction. The loops keep
     * a few descriptors each (epoll, the wakeup, two timers, the listen
     * socket), and the program keeps 64 for its files and clients. */
    struct rlimit rl;
    if (srv->max_conns && getrlimit(RLIMIT_NOFILE, &rl) == 0 &&
        rl.rlim_cur != RLIM_INFINITY) {
      int64_t cap = (int64_t)rl.rlim_cur - 64 - 8 * (int64_t)srv->nloops;
      if (cap < 1) {
        cap = 1;
      }
      if (srv->max_conns > cap) {
        srv->max_conns = cap;
      }
    }
  }
  {
    int iv = sweep_interval(srv);
    for (i = 0; i < srv->nloops; i++) {
      us_timer_set(srv->loops[i].sweep, loop_sweep, iv, iv);
    }
  }
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
  cork_release();
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

/* A route method of std.http called while its server serves: the loop
 * threads read the route tables with no lock, so the route is refused.
 * A trap of the language (from a handler, the usual place, a trap that
 * fails its request). what names the route ("route GET /x",
 * "websocket route /x"). */
__attribute__((noreturn, cold)) void avant_http_route_while_serving(const char *what) {
  char msg[400];
  snprintf(msg, sizeof msg, "%s registered while the server is serving: register every route before serve", what);
  avant_trap_text(msg);
}

/* A limit set while the server serves: the loops read the limits with
 * no lock, as they read the routes, so it is refused like a route. A
 * trap of the language. */
/* A std.http call given a value it refuses (the message names the call
 * and the rule): a trap of the language, which fails a handler's
 * request. */
__attribute__((noreturn, cold)) void avant_http_trap(const char *message) {
  avant_trap_text(message);
}

__attribute__((noreturn, cold)) void avant_http_limit_while_serving(const char *name) {
  char msg[160];
  snprintf(msg, sizeof msg, "http.Server.%s called while the server is serving: set every limit before serve", name);
  avant_trap_text(msg);
}

/* A limit of Server's setters (std.http), set before serve: which is
 * 1 head_timeout (a, ms), 2 min_rate (a bytes a second, b the grace in
 * ms), 3 body_budget (bytes), 4 memory_ceiling (bytes), 5
 * max_connections, 6 shutdown_timeout (ms). 0 turns a limit off. A
 * negative value or one past its range (a C int; 2^62 for the byte
 * limits) is the program's error: a trap of the language (in a handler,
 * a trap that fails its request). */
void avant_http_set_limit(void *srvp, const char *name, avant_int which,
                          avant_int a, avant_int b) {
  AvantHttpServer *srv = (AvantHttpServer *)srvp;
  long long top = (which == 3 || which == 4) ? (1LL << 62) : (which == 7 || which == 8) ? HTTP_IDLE_MAX_MS : INT32_MAX;
  if (a < 0 || b < 0 || a > top || b > INT32_MAX) {
    char msg[200];
    if (which == 2) {
      snprintf(msg, sizeof msg, "http.Server.%s(%lld, %lld): a limit is 0 (off) to %lld", name, (long long)a,
               (long long)b, top);
    } else {
      snprintf(msg, sizeof msg, "http.Server.%s(%lld): a limit is 0 (off) to %lld", name, (long long)a, top);
    }
    avant_trap_text(msg);
  }
  if (!srv) {
    return;
  }
  switch (which) {
    case 1:
      srv->head_ms = (uint32_t)a;
      break;
    case 2:
      srv->rate = (uint32_t)a;
      srv->grace_ms = (uint32_t)b;
      break;
    case 3:
      srv->body_budget = (int64_t)a;
      break;
    case 4:
      srv->mem_ceiling = (int64_t)a;
      break;
    case 5:
      srv->max_conns = (int64_t)a;
      break;
    case 6:
      srv->shutdown_ms = (uint32_t)a;
      break;
    case 7:
      idle_limit((uint32_t)a, &srv->idle_secs, &srv->idle_mins);
      break;
    case 8:
      idle_limit((uint32_t)a, &srv->ws_idle_secs, &srv->ws_idle_mins);
      break;
    default:
      break;
  }
}

/* At most 64 bytes of p into out (room for 4 bytes each and "..."), a
 * byte that is not printable ASCII (or is a quote or a backslash) as
 * \r, \n, \t or \xNN. */
static void escape_into(char *out, const char *p, size_t n) {
  size_t i;
  size_t k = 0;
  for (i = 0; i < n && i < 64; i++) {
    unsigned char ch = (unsigned char)p[i];
    if (ch == '\r') {
      k += (size_t)sprintf(out + k, "\\r");
    } else if (ch == '\n') {
      k += (size_t)sprintf(out + k, "\\n");
    } else if (ch == '\t') {
      k += (size_t)sprintf(out + k, "\\t");
    } else if (ch < 0x20 || ch >= 0x7f || ch == '"' || ch == '\\') {
      k += (size_t)sprintf(out + k, "\\x%02x", ch);
    } else {
      out[k++] = (char)ch;
    }
  }
  if (n > 64) {
    k += (size_t)sprintf(out + k, "...");
  }
  out[k] = 0;
}

/* RFC 9110 5.6.2: tchar, the bytes of a token (a field name). */
static int http_tchar(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || (ch != 0 && strchr("!#$%&'*+-.^_`|~", ch));
}

/* The index of the first byte of s[0..n) that is not a token character,
 * n when there is none: a token is that and not empty. */
static size_t http_token_end(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (!http_tchar((unsigned char)s[i])) {
      break;
    }
  }
  return i;
}

/* The index of the first control byte other than tab (DEL among them)
 * of s[0..n), n when there is none: what a field value may not hold. */
static size_t http_value_end(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
      break;
    }
  }
  return i;
}

/* Response.header's check. The name and the
 * value were appended to the answer's head as they came, so a value
 * holding CR LF (a redirect target or a cookie built from request data)
 * added header lines or began the body, and a second answer could
 * follow: response splitting. A name must be a token (RFC 9110 5.1) and
 * a value may hold no control character but tab (5.5: CR, LF and NUL
 * are refused, and the other controls with them); anything else is the
 * program's error, and it is a trap of the language naming the rule (in
 * a handler, a trap that fails its request: answered 500). */
void avant_http_header_check(const char *name, const char *value) {
  size_t nn = avant_str_n(name);
  size_t vn = avant_str_n(value);
  size_t i;
  char esc[300];
  char msg[600];
  if (nn == 0 || http_token_end(name, nn) < nn) {
    escape_into(esc, name, nn);
    snprintf(msg, sizeof msg,
             "http.Response.header name \"%s\" is not a token: a header name is letters, digits and !#$%%&'*+-.^_`|~ (RFC 9110)",
             esc);
    avant_trap_text(msg);
  }
  i = http_value_end(value, vn);
  if (i < vn) {
    escape_into(esc, name, nn);
    snprintf(msg, sizeof msg,
             "http.Response.header value of %s holds a control character (byte %u at %zu): a header value holds no CR, LF or other control character but tab (RFC 9110)",
             esc, (unsigned)(unsigned char)value[i], i);
    avant_trap_text(msg);
  }
}

/* The client's request checks, the same rules
 * as Response.header's: the method, the request target and each header
 * were written into the request as given, so CR LF in any of them added
 * request lines or a second request (request splitting). Each returns
 * the index of the first byte that breaks its rule, or -1: a method
 * and a header name are tokens (an empty one gives 0), a header value
 * holds no control character but tab, and a URL (the request target
 * and the authority) holds no control character and no space (RFC 3986
 * has neither; a space would end the request line's target). The
 * client returns its HttpError, as for its other refusals; the caller
 * names the part and the byte. */
avant_int avant_http_token_bad(const char *s) {
  size_t n = avant_str_n(s);
  size_t i = http_token_end(s, n);
  return (n > 0 && i == n) ? -1 : (avant_int)i;
}

avant_int avant_http_value_bad(const char *s) {
  size_t n = avant_str_n(s);
  size_t i = http_value_end(s, n);
  return i == n ? -1 : (avant_int)i;
}

avant_int avant_http_url_bad(const char *s) {
  size_t n = avant_str_n(s);
  size_t i;
  for (i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch <= 0x20 || ch == 0x7f) {
      return (avant_int)i;
    }
  }
  return -1;
}

avant_int avant_http_close(void *srvp) {
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
  char *request; /* the handshake, malloc'd to its length */
  size_t request_len;
  size_t request_sent; /* how much of it the socket has taken */
  size_t write_chunk; /* the most one write hands the socket */
  int want_close;
  WsClientMsg *in_head;
  WsClientMsg *in_tail;
  WsOut *out_head;
  WsOut *out_tail;
  pthread_mutex_t mu;
  pthread_cond_t cond;
  int users; /* operations in progress on it, under g_ws_client_mu */
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
  size_t piece = c->write_chunk;
  while (c->out_head) {
    WsOut *node = c->out_head;
    int full = 0;
    /* A piece at a time: the frame's whole rest
     * was passed as (int)remaining, negative for a frame over INT_MAX,
     * which only Linux's send clamping made work. */
    while (node->off < node->len) {
      size_t remaining = node->len - node->off;
      int want = remaining > piece ? (int)piece : (int)remaining;
      int written = us_socket_write(0, c->sock, node->data + node->off,
                                    want, 0);
      if (written < 0) { written = 0; }
      node->off += (size_t)written;
      if (written < want) {
        full = 1;
        break;
      }
    }
    if (full) { break; }
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
  /* All of it, under the call's deadline: one
   * us_socket_write ignored its count, so a handshake the socket took in
   * part was cut and the server waited for the rest; the rest is now
   * sent each time the socket is writable (ws_client_on_writable), by
   * the loop the server's queue and the HTTP client use. */
  (void)sock_write_rest(s, c->request, c->request_len, &c->request_sent,
                          c->write_chunk);
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
  WsClient *c = ws_client_of_socket(s);
  if (c->request_sent < c->request_len) {
    (void)sock_write_rest(s, c->request, c->request_len, &c->request_sent,
                          c->write_chunk);
    return s;
  }
  ws_client_flush(c);
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
  free(c->request);
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cond);
  free(c);
}

static avant_int ws_client_register(WsClient *c);

/* The connection's id, never 0; 0 with a status through
 * avant_http_take_status: 1 connect failure, 2 timeout, 3 invalid
 * handshake, 4 out of memory. */
avant_int avant_ws_client_connect(const char *host, avant_int port,
                                  const char *target, const char *key_b64,
                                  avant_int timeout_ms) {
  WsClient *c;
  avant_int id;
  struct us_socket_context_options_t options;
  int request_len;
  struct timespec deadline;
  set_status(0);
  /* A port outside 0..65535 cannot connect. */
  if (port < 0 || port > 65535) {
    set_status(1);
    return 0;
  }
  c = (WsClient *)calloc(1, sizeof(WsClient));
  if (c) {
    c->write_chunk = http_ws_write_chunk();
  }
  if (!c) { set_status(4); return 0; }
  c->frag_opcode = -1;
  c->close_code = 1006;
  pthread_mutex_init(&c->mu, NULL);
  /* The handshake's and recv's deadlines are kept on CLOCK_MONOTONIC: read
   * from the wall clock and waited for on
   * it, a step of the system clock during the wait shortened or
   * lengthened them (an hour back: the wait ended at once). The HTTP
   * client's deadline is a timer of its loop and was never affected. */
  {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->cond, &attr);
    pthread_condattr_destroy(&attr);
  }
  ws_accept_key(key_b64, strlen(key_b64), c->expected_accept);
  c->loop = us_create_loop(0, ws_client_wakeup, srv_pre, srv_post,
                           (unsigned int)sizeof(WsClient *));
  if (!c->loop) { ws_client_free(c); set_status(1); return 0; }
  *(WsClient **)us_loop_ext(c->loop) = c;
  memset(&options, 0, sizeof(options));
  c->ctx = us_create_socket_context(0, c->loop, 0, options);
  if (!c->ctx) { ws_client_free(c); set_status(1); return 0; }
  us_socket_context_on_open(0, c->ctx, ws_client_on_open);
  us_socket_context_on_data(0, c->ctx, ws_client_on_data);
  us_socket_context_on_writable(0, c->ctx, ws_client_on_writable);
  us_socket_context_on_close(0, c->ctx, ws_client_on_close);
  us_socket_context_on_timeout(0, c->ctx, ws_client_on_timeout);
  us_socket_context_on_end(0, c->ctx, ws_client_on_end);
  us_socket_context_on_connect_error(0, c->ctx, ws_client_on_connect_error);
  /* The handshake is built to its own length: it was
   * formatted into 2048 bytes on the stack and then copied by the length
   * snprintf would have written, so a target or a host longer than about
   * 1,900 bytes overran both buffers (glibc's fortified memcpy stopped
   * the program: "buffer overflow detected", SIGABRT). */
#define WS_CLIENT_HANDSHAKE "GET %s HTTP/1.1\r\nHost: %s:%d\r\n" \
                            "Upgrade: websocket\r\nConnection: Upgrade\r\n" \
                            "Sec-WebSocket-Key: %s\r\n" \
                            "Sec-WebSocket-Version: 13\r\n\r\n"
  request_len = snprintf(NULL, 0, WS_CLIENT_HANDSHAKE, target, host,
                         (int)port, key_b64);
  c->request = request_len > 0 ? (char *)malloc((size_t)request_len + 1) : NULL;
  if (!c->request) { ws_client_free(c); set_status(4); return 0; }
  snprintf(c->request, (size_t)request_len + 1, WS_CLIENT_HANDSHAKE, target,
           host, (int)port, key_b64);
#undef WS_CLIENT_HANDSHAKE
  c->request_len = (size_t)request_len;
  {
    struct us_socket_t *sock = us_socket_context_connect(
        0, c->ctx, host, (int)port, NULL, 0, 0);
    if (!sock) {
      ws_client_free(c);
      set_status(1);
      return 0;
    }
    c->sock = sock;
  }
  if (pthread_create(&c->thread, NULL, ws_client_thread_main, c) != 0) {
    ws_client_free(c);
    set_status(1);
    return 0;
  }
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += timeout_ms <= 0 ? 10 : timeout_ms / 1000;
  deadline.tv_nsec += (timeout_ms <= 0 ? 0 : (timeout_ms % 1000)) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  /* The handshake wait is native (rule 2); host, target
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
      return 0;
    }
  }
  id = ws_client_register(c);
  if (id == 0) {
    c->closed = 1;
    c->want_close = 1;
    us_wakeup_loop(c->loop);
    avant_gc_native_begin();
    pthread_join(c->thread, NULL);
    avant_gc_native_end();
    ws_client_free(c);
    set_status(4);
    return 0;
  }
  return id;
}

static __thread int32_t g_ws_client_last_is_text;

/* The open client connections, by id. An
 * Avant WsConnection value is the id alone. The registry used to hold
 * addresses and the value was the address: after close freed a
 * WsClient, the next ws.connect was given the same address, and a copy
 * of the closed value then sent on, and closed, the new connection; and
 * an operation tested membership and then used the client with no lock
 * held, so a close on another thread in between freed it under the
 * operation. An id is never given twice, so a copy of a closed value
 * finds nothing and every operation through it gives its "closed"
 * answer. An operation takes the client with ws_client_acquire, which
 * counts it as a user under the registry's lock, and gives it back with
 * ws_client_release; close takes the client out of the registry first,
 * so no new user can find it, and frees it only when its users are
 * gone. */
typedef struct WsClientReg {
  avant_int id;
  WsClient *c;
  struct WsClientReg *next;
} WsClientReg;

static pthread_mutex_t g_ws_client_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ws_client_idle = PTHREAD_COND_INITIALIZER;
static WsClientReg *g_ws_clients;
static avant_int g_ws_client_next_id;

/* The open client numbered id, counted as in use; NULL when there is
 * none (closed, or never opened). */
static WsClient *ws_client_acquire(avant_int id) {
  WsClientReg *r;
  WsClient *c = NULL;
  pthread_mutex_lock(&g_ws_client_mu);
  for (r = g_ws_clients; r; r = r->next) {
    if (r->id == id) {
      c = r->c;
      c->users++;
      break;
    }
  }
  pthread_mutex_unlock(&g_ws_client_mu);
  return c;
}

static void ws_client_release(WsClient *c) {
  pthread_mutex_lock(&g_ws_client_mu);
  c->users--;
  if (c->users == 0) {
    pthread_cond_broadcast(&g_ws_client_idle);
  }
  pthread_mutex_unlock(&g_ws_client_mu);
}

/* The new id, or 0 when memory ran out. */
static avant_int ws_client_register(WsClient *c) {
  WsClientReg *r = (WsClientReg *)malloc(sizeof(WsClientReg));
  avant_int id;
  if (!r) { return 0; }
  r->c = c;
  pthread_mutex_lock(&g_ws_client_mu);
  id = ++g_ws_client_next_id;
  r->id = id;
  r->next = g_ws_clients;
  g_ws_clients = r;
  pthread_mutex_unlock(&g_ws_client_mu);
  return id;
}

/* Takes the client numbered id out of the registry and returns it; NULL
 * when it is not there (a second close). */
static WsClient *ws_client_unregister(avant_int id) {
  WsClientReg **link = &g_ws_clients;
  WsClient *c = NULL;
  pthread_mutex_lock(&g_ws_client_mu);
  while (*link) {
    if ((*link)->id == id) {
      WsClientReg *dead = *link;
      *link = dead->next;
      c = dead->c;
      free(dead);
      break;
    }
    link = &(*link)->next;
  }
  pthread_mutex_unlock(&g_ws_client_mu);
  return c;
}

avant_int avant_ws_client_last_is_text(void) { return g_ws_client_last_is_text; }

avant_int avant_ws_client_close_code(avant_int id) {
  WsClient *c = ws_client_acquire(id);
  avant_int code;
  if (!c) { return 1006; }
  code = c->close_code;
  ws_client_release(c);
  return code;
}

/* One queued message as Bytes; status 1 when the connection is closed
 * with nothing queued, 2 on timeout. timeout_ms < 0 waits forever. */
/* gc-lint: holds nothing (the message is C memory; nothing heap is read after make_bytes). */
char *avant_ws_client_recv(avant_int id, avant_int timeout_ms) {
  WsClient *c = ws_client_acquire(id);
  WsClientMsg *m;
  char *out;
  int closed;
  set_status(0);
  if (!c) {
    set_status(1);
    return avant_str_empty();
  }
  /* The receive wait is native (rule 2): the message is
   * C memory. */
  avant_gc_lock(&c->mu);
  if (timeout_ms < 0) {
    while (!c->in_head && !c->closed) {
      avant_gc_cond_wait(&c->cond, &c->mu);
    }
  } else {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
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
  closed = c->closed;
  pthread_mutex_unlock(&c->mu);
  /* The client is not read after this: a close on another thread may
   * free it from here on (and the allocation below may collect). */
  ws_client_release(c);
  if (!m) {
    set_status(closed ? 1 : 2);
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

avant_int avant_ws_client_send_text(avant_int id, const char *data) {
  WsClient *c = ws_client_acquire(id);
  int ok;
  if (!c) { return 0; }
  ok = ws_client_send_frame(c, 1, data, avant_str_n(data));
  ws_client_release(c);
  return ok;
}

/* A text message held as Bytes (WsConnection.send_message): validated
 * as UTF-8 when it was received. */
avant_int avant_ws_client_send_text_bytes(avant_int id, const char *data) {
  return avant_ws_client_send_text(id, data);
}

avant_int avant_ws_client_send_bytes(avant_int id, const char *data) {
  WsClient *c = ws_client_acquire(id);
  int ok;
  if (!c) { return 0; }
  ok = ws_client_send_frame(c, 2, data, avant_str_n(data));
  ws_client_release(c);
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

avant_int avant_ws_client_close(avant_int id) {
  WsClient *c = ws_client_unregister(id);
  unsigned char payload[2] = {0x03, 0xe8};
  if (!c) { return 0; }
  if (!c->close_sent) {
    c->close_sent = 1;
    ws_client_send_frame(c, 8, (const char *)payload, 2);
  }
  /* A recv waiting on another thread sees the close and returns. */
  pthread_mutex_lock(&c->mu);
  c->closed = 1;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mu);
  c->want_close = 1;
  us_wakeup_loop(c->loop);
  avant_gc_native_begin();
  pthread_join(c->thread, NULL);
  /* Operations that found the client before it left the registry
   * finish first; none can start now. */
  pthread_mutex_lock(&g_ws_client_mu);
  while (c->users > 0) {
    pthread_cond_wait(&g_ws_client_idle, &g_ws_client_mu);
  }
  pthread_mutex_unlock(&g_ws_client_mu);
  avant_gc_native_end();
  ws_client_free(c);
  return 1;
}

/* ---- client fetch primitive ---- */

/* request is the whole request (the head and the body), pinned for the
 * loop; sent is how much of it the socket has taken. sock is the
 * socket until it is closed (fetch_on_close clears it), which is how
 * the deadline's timer knows whether there is still something to end. */
typedef struct FetchState {
  char *buf;
  size_t len;
  size_t cap;
  int failed;
  const char *request;
  size_t request_len;
  size_t sent;
  size_t write_chunk; /* the most one write hands the socket */
  struct us_socket_t *sock;
  /* Where the response ends (fetch_complete): 0 while its head has not
   * all arrived; then one of the FETCH_* framings, the head's length,
   * the body's length (FETCH_LENGTH), and for a chunked body the decoder
   * and how far it has read. */
  int framing;
  size_t start;
  size_t head_len;
  size_t body_want;
  size_t scanned;
  struct phr_chunked_decoder chunks;
  /* The idle limit (http.request's idle_ms, 0: none) and the last time
   * a byte arrived or left (http_now_ms), read by fetch_on_idle. */
  uint32_t idle_ms;
  int64_t progress_at;
} FetchState;

enum {
  FETCH_HEAD_PENDING = 0,
  FETCH_UNTIL_CLOSE,
  FETCH_LENGTH,
  FETCH_CHUNKED,
  FETCH_NO_BODY
};

static FetchState *fetch_of_socket(struct us_socket_t *s) {
  struct us_loop_t *loop = us_socket_context_loop(0, us_socket_context(0, s));
  return *(FetchState **)us_loop_ext(loop);
}

/* Sends what is left of the request (before this, the one
 * write of fetch_on_open ignored its count and fetch_on_writable was
 * empty, so a request the socket took only in part was cut there and
 * the server waited for the rest). Nothing is copied: the request stays
 * pinned until the loop ends, so the rest is sent from it, in order,
 * each time the socket is writable again. */
static void fetch_send(FetchState *st, struct us_socket_t *s) {
  size_t before = st->sent;
  (void)sock_write_rest(s, st->request, st->request_len, &st->sent,
                        st->write_chunk);
  if (st->idle_ms && st->sent > before) {
    st->progress_at = http_now_ms();
  }
}

static struct us_socket_t *fetch_on_open(struct us_socket_t *s, int is_client,
                                         char *ip, int ip_length) {
  FetchState *st = fetch_of_socket(s);
  (void)is_client;
  (void)ip;
  (void)ip_length;
  /* The request goes out only now: writing on a still-connecting
   * socket corrupts the semi-socket state machine. */
  fetch_send(st, s);
  return s;
}

static int parse_response_head(const char *raw, size_t n, int *status,
                               struct phr_header *headers,
                               size_t *num_headers, int *pret);

/* Whether the response in st->buf is complete by its own framing. The exchange
 * used to end only when the
 * server closed the socket, and the length was looked at afterwards
 * (avant_http_resp_body): a server that keeps the connection open after
 * a complete answer (it ignores Connection: close, or a proxy keeps the
 * connection) held the call until its deadline, which then failed it
 * as a timeout. RFC 9112 6.3, in its order: interim 1xx answers are
 * skipped and a 101 ends at the close; a response to HEAD, a 204
 * or a 304 ends with its head; a chunked body ends
 * after its last chunk and the trailer section; a Content-Length body
 * after that many bytes, and one with an invalid length (repeated, or
 * not a number) at once, to be refused; a body with neither (or
 * another Transfer-Encoding) when the server closes, as before. The
 * head is parsed again as bytes arrive until it
 * is whole, then never again; a chunked body is followed by the same
 * decoder avant_http_resp_body uses, on a copy, a piece at a time. */
static int fetch_complete(FetchState *st) {
  if (st->framing == FETCH_HEAD_PENDING) {
    int status = 0;
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t num_headers = 0;
    int pret = 0;
    const char *value = NULL;
    size_t value_len = 0;
    long want = 0;
    for (;;) {
      if (!parse_response_head(st->buf + st->start, st->len - st->start,
                               &status, headers, &num_headers, &pret)) {
        if (pret == -2) {
          return 0;
        }
        st->framing = FETCH_UNTIL_CLOSE;
        return 0;
      }
      /* An interim answer (100 Continue, 102, 103 Early Hints) is a head
       * with no body before the final answer (RFC 9110 15.2): the client
       * once returned it as the response, the final
       * answer's bytes as its body. It is skipped, and the response is
       * the answer after it (avant_http_fetch returns the bytes from
       * st->start). A 101 ends the exchange's HTTP: it stays the answer,
       * read until the close as before. */
      if (status >= 100 && status < 200 && status != 101) {
        st->start += (size_t)pret;
        continue;
      }
      break;
    }
    st->head_len = st->start + (size_t)pret;
    st->scanned = st->head_len;
    if (status == 101) {
      st->framing = FETCH_UNTIL_CLOSE;
    } else if ((st->request_len >= 5 && memcmp(st->request, "HEAD ", 5) == 0) ||
               status == 204 || status == 304) {
      st->framing = FETCH_NO_BODY;
    } else if (find_header(headers, num_headers, "Transfer-Encoding", &value,
                           &value_len)) {
      st->framing = ascii_ieq_contains(value, value_len, "chunked")
                        ? FETCH_CHUNKED
                        : FETCH_UNTIL_CLOSE;
      memset(&st->chunks, 0, sizeof(st->chunks));
      st->chunks.consume_trailer = 1;
    } else {
      int cl = resp_content_length(headers, num_headers, &want);
      if (cl < 0) {
        /* An invalid length: the exchange ends now and
         * avant_http_resp_body refuses the response. */
        st->framing = FETCH_NO_BODY;
      } else if (cl > 0) {
        st->framing = FETCH_LENGTH;
        st->body_want = (size_t)want;
      } else {
        st->framing = FETCH_UNTIL_CLOSE;
      }
    }
  }
  switch (st->framing) {
    case FETCH_NO_BODY:
      return 1;
    case FETCH_LENGTH:
      return st->len - st->head_len >= st->body_want;
    case FETCH_CHUNKED:
      while (st->scanned < st->len) {
        char piece[16384];
        size_t n = st->len - st->scanned;
        ssize_t done;
        if (n > sizeof(piece)) {
          n = sizeof(piece);
        }
        memcpy(piece, st->buf + st->scanned, n);
        st->scanned += n;
        done = phr_decode_chunked(&st->chunks, piece, &n);
        if (done != -2) {
          /* The last chunk and the trailers are here (done >= 0), or the
           * body is not valid chunked encoding (-1): nothing that
           * arrives later changes how avant_http_resp_body reads it. */
          return 1;
        }
      }
      return 0;
    default:
      return 0;
  }
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
  if (st->idle_ms) {
    st->progress_at = http_now_ms();
  }
  if (fetch_complete(st)) {
    /* The whole response is here: the exchange ends now, whether or not
     * the server closes. */
    return us_socket_close(0, s, 0, NULL);
  }
  return s;
}

static struct us_socket_t *fetch_on_close(struct us_socket_t *s, int code,
                                          void *reason) {
  (void)code;
  (void)reason;
  fetch_of_socket(s)->sock = NULL;
  return s;
}

static struct us_socket_t *fetch_on_end(struct us_socket_t *s) {
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

/* The socket's own timeout is not armed (see fetch_on_deadline); the
 * handler stays so that one armed by a later change still ends the
 * exchange as a timeout. */
static struct us_socket_t *fetch_on_timeout(struct us_socket_t *s) {
  FetchState *st = fetch_of_socket(s);
  st->failed = 2;
  return us_socket_close(0, s, 0, NULL);
}

/* timeout_ms is a deadline for the whole exchange: the connect, the
 * request and the complete response. It was
 * armed as the socket's timeout before the connect and uSockets clears
 * a socket's timeout when the connection is established, so it covered
 * the connect only: a server that accepted and never answered held the
 * call until that server closed. It is now a timer of the fetch's own
 * loop, set once and never re-armed, so data that keeps arriving does
 * not extend it. When it fires with the socket still open the exchange
 * failed as a timeout, whatever has arrived: a response that is not
 * complete is not a response. Its precision is the timer's (timerfd,
 * milliseconds), not the 4-second sweep uSockets counts socket
 * timeouts in, which made a 10 s connect timeout 12 s and a 1 s one
 * 4 s. */
static void fetch_on_deadline(struct us_timer_t *t) {
  FetchState *st = *(FetchState **)us_timer_ext(t);
  if (st->sock) {
    st->failed = 2;
    us_socket_close(0, st->sock, 0, NULL);
  }
}

/* The idle limit: a repeating timer of a quarter of it (at least
 * 10 ms) that ends the exchange as stalled when nothing has arrived or
 * left for idle_ms, the connect included. The deadline above bounds the
 * whole call; this one cuts a transfer that stopped, however long the
 * deadline, and lets a slow one that moves go on. */
static void fetch_on_idle(struct us_timer_t *t) {
  FetchState *st = *(FetchState **)us_timer_ext(t);
  if (st->sock && http_now_ms() - st->progress_at >= (int64_t)st->idle_ms) {
    st->failed = 3;
    us_socket_close(0, st->sock, 0, NULL);
  }
}

static struct us_socket_t *fetch_on_connect_error(struct us_socket_t *s,
                                                  int code) {
  FetchState *st = fetch_of_socket(s);
  (void)code;
  st->failed = 1;
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *fetch_on_writable(struct us_socket_t *s) {
  fetch_send(fetch_of_socket(s), s);
  return s;
}

static void fetch_wakeup(struct us_loop_t *loop) {
  (void)loop;
}

/* gc-lint: holds nothing (the fetched body is C memory; nothing heap is read after make_bytes). */
char *avant_http_fetch(const char *host, avant_int port, const char *request,
                       avant_int timeout_ms, avant_int idle_ms) {
  struct us_loop_t *loop;
  struct us_socket_context_t *ctx;
  struct us_socket_context_options_t options;
  struct us_socket_t *sock;
  struct us_timer_t *deadline;
  struct us_timer_t *idle = NULL;
  FetchState *st;
  char *out;
  int deadline_ms;
  set_status(0);
  if (port < 0 || port > 65535) {
    set_status(1);
    return avant_str_empty();
  }
  st = (FetchState *)calloc(1, sizeof(FetchState));
  if (!st) {
    set_status(4);
    return avant_str_empty();
  }
  st->write_chunk = http_ws_write_chunk();
  st->idle_ms = idle_ms <= 0 ? 0 : idle_ms > INT32_MAX ? INT32_MAX : (uint32_t)idle_ms;
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
  /* The timer does not keep the loop running (fallthrough): the loop
   * ends when the socket is closed, and the timer is closed after it. */
  deadline = us_create_timer(loop, 1, (unsigned int)sizeof(FetchState *));
  if (!deadline) {
    us_socket_context_free(0, ctx);
    us_loop_free(loop);
    free(st);
    set_status(4);
    return avant_str_empty();
  }
  *(FetchState **)us_timer_ext(deadline) = st;
  sock = us_socket_context_connect(0, ctx, host, (int)port, NULL, 0, 0);
  if (!sock) {
    us_timer_close(deadline);
    us_socket_context_free(0, ctx);
    us_loop_free(loop);
    free(st->buf);
    free(st);
    set_status(1);
    return avant_str_empty();
  }
  st->sock = sock;
  deadline_ms = timeout_ms <= 0 ? 10000 : (timeout_ms > INT32_MAX ? INT32_MAX : (int)timeout_ms);
  us_timer_set(deadline, fetch_on_deadline, deadline_ms, 0);
  if (st->idle_ms) {
    idle = us_create_timer(loop, 1, (unsigned int)sizeof(FetchState *));
    if (idle) {
      int period = (int)(st->idle_ms / 4 < 10 ? 10 : st->idle_ms / 4);
      *(FetchState **)us_timer_ext(idle) = st;
      st->progress_at = http_now_ms();
      us_timer_set(idle, fetch_on_idle, period, period);
    }
  }
  st->request = request;
  st->request_len = avant_str_n(request);
  /* The loop runs native (rule 3); its callbacks read the request, which
   * is pinned for the loop (a collection on another thread may run
   * meanwhile and must not move or free it). */
  avant_pin((void *)request);
  avant_gc_native_begin();
  us_loop_run(loop);
  avant_gc_native_end();
  avant_unpin((void *)request);
  us_timer_close(deadline);
  if (idle) {
    us_timer_close(idle);
  }
  us_socket_context_free(0, ctx);
  us_loop_free(loop);
  if (st->failed == 3) {
    set_status(3);
    free(st->buf);
    free(st);
    return avant_str_empty();
  }
  if (st->failed == 1) {
    set_status(1);
    free(st->buf);
    free(st);
    return avant_str_empty();
  }
  if (st->failed == 2) {
    set_status(2);
    free(st->buf);
    free(st);
    return avant_str_empty();
  }
  out = make_bytes(st->buf ? st->buf + st->start : "", st->len - st->start);
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

avant_int avant_http_resp_status(const char *raw) {
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
  return build_packed_headers(headers, num_headers, raw, (size_t)pret, NULL);
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
  /* A 204 or a 304 has no body whatever its head says (RFC 9112 6.3):
   * a 304's Content-Length is the length the full answer would have, so
   * reading it as this body's length made the response invalid. A HEAD's
   * answer is the caller's to know (http_fetch_once). */
  if (status == 204 || status == 304) {
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
  {
    long want = 0;
    int cl = resp_content_length(headers, num_headers, &want);
    if (cl < 0 || (cl > 0 && (size_t)want > body_len)) {
      set_status(3);
      return make_bytes("", 0);
    }
    if (cl > 0) {
      return make_bytes(body, (size_t)want);
    }
  }
  return make_bytes(body, body_len);
}
