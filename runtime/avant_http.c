#include "avant_rt.h"
#include "libusockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *body;
  char *copied;
  struct us_listen_socket_t *listen;
  int done;
} HttpJob;

static void on_wakeup(struct us_loop_t *loop) {
  (void)loop;
}

static void on_pre(struct us_loop_t *loop) {
  (void)loop;
}

static void on_post(struct us_loop_t *loop) {
  (void)loop;
}

static HttpJob *job_of(struct us_socket_t *s) {
  struct us_loop_t *loop = us_socket_context_loop(0, us_socket_context(0, s));
  return (HttpJob *)us_loop_ext(loop);
}

static struct us_socket_t *on_writable(struct us_socket_t *s) {
  return s;
}

static struct us_socket_t *on_close(struct us_socket_t *s, int code, void *reason) {
  (void)code;
  (void)reason;
  return s;
}

static struct us_socket_t *on_end(struct us_socket_t *s) {
  us_socket_shutdown(0, s);
  return us_socket_close(0, s, 0, NULL);
}

static struct us_socket_t *on_timeout(struct us_socket_t *s) {
  return s;
}

static struct us_socket_t *on_connect_error(struct us_socket_t *s, int code) {
  (void)code;
  return s;
}

static struct us_socket_t *on_server_open(struct us_socket_t *s, int is_client, char *ip, int ip_length) {
  (void)is_client;
  (void)ip;
  (void)ip_length;
  return s;
}

static struct us_socket_t *on_server_data(struct us_socket_t *s, char *data, int length) {
  (void)data;
  (void)length;
  HttpJob *job = job_of(s);
  const char *body = job->body ? job->body : "";
  size_t n = strlen(body);
  char header[128];
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", n);
  us_socket_write(0, s, header, hlen, 1);
  us_socket_write(0, s, body, (int)n, 0);
  us_socket_shutdown(0, s);
  return s;
}

static struct us_socket_t *on_client_open(struct us_socket_t *s, int is_client, char *ip, int ip_length) {
  (void)is_client;
  (void)ip;
  (void)ip_length;
  const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  us_socket_write(0, s, req, (int)strlen(req), 0);
  return s;
}

static struct us_socket_t *on_client_data(struct us_socket_t *s, char *data, int length) {
  HttpJob *job = job_of(s);
  char *sep = NULL;
  int i;
  for (i = 0; i + 3 < length; i++) {
    if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
      sep = data + i + 4;
      break;
    }
  }
  if (sep) {
    int n = length - (int)(sep - data);
    avant_gc_defer_enter();
    char *copy = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
    memcpy(copy, sep, (size_t)n);
    copy[n] = 0;
    avant_gc_defer_leave();
    job->copied = copy;
    job->done = 1;
  }
  if (job->listen) {
    us_listen_socket_close(0, job->listen);
    job->listen = NULL;
  }
  return us_socket_close(0, s, 0, NULL);
}

const char *avant_http_roundtrip(const char *body) {
  struct us_loop_t *loop = us_create_loop(0, on_wakeup, on_pre, on_post, sizeof(HttpJob));
  if (!loop) {
    return "";
  }
  HttpJob *job = (HttpJob *)us_loop_ext(loop);
  memset(job, 0, sizeof(HttpJob));
  job->body = body ? body : "";

  struct us_socket_context_options_t options;
  memset(&options, 0, sizeof(options));

  struct us_socket_context_t *server = us_create_socket_context(0, loop, 0, options);
  struct us_socket_context_t *client = us_create_socket_context(0, loop, 0, options);
  if (!server || !client) {
    return "";
  }

  us_socket_context_on_open(0, server, on_server_open);
  us_socket_context_on_data(0, server, on_server_data);
  us_socket_context_on_writable(0, server, on_writable);
  us_socket_context_on_close(0, server, on_close);
  us_socket_context_on_timeout(0, server, on_timeout);
  us_socket_context_on_end(0, server, on_end);

  us_socket_context_on_open(0, client, on_client_open);
  us_socket_context_on_data(0, client, on_client_data);
  us_socket_context_on_writable(0, client, on_writable);
  us_socket_context_on_close(0, client, on_close);
  us_socket_context_on_timeout(0, client, on_timeout);
  us_socket_context_on_end(0, client, on_end);
  us_socket_context_on_connect_error(0, client, on_connect_error);

  struct us_listen_socket_t *listen = us_socket_context_listen(0, server, "127.0.0.1", 0, 0, 0);
  if (!listen) {
    return "";
  }
  job->listen = listen;
  int port = us_socket_local_port(0, (struct us_socket_t *)listen);
  if (port <= 0) {
    us_listen_socket_close(0, listen);
    return "";
  }
  if (!us_socket_context_connect(0, client, "127.0.0.1", port, NULL, 0, 0)) {
    us_listen_socket_close(0, listen);
    return "";
  }
  us_loop_run(loop);
  return job->copied ? job->copied : "";
}
