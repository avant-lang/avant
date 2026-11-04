/* Hand-written single-threaded epoll hello-world server for the
 * five-language harness (bench/http/run.sh). Keep-alive, body "hello".
 * GET /quit closes the listen socket and exits after answering. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

static const char RESP[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 5\r\n\r\nhello";
static const char RESP_BYE[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 3\r\nConnection: close\r\n\r\nbye";

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

typedef struct {
  int fd;
  char buf[16384];
  size_t len;
  int quit;
} Conn;

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 18301;
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
  if (listen(ls, 4096) != 0) { perror("listen"); return 1; }
  set_nonblock(ls);
  int ep = epoll_create1(0);
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.fd = ls;
  epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
  Conn *conns[65536];
  memset(conns, 0, sizeof(conns));
  int running = 1;
  while (running) {
    struct epoll_event evs[256];
    int n = epoll_wait(ep, evs, 256, -1);
    for (int i = 0; i < n; i++) {
      int fd = evs[i].data.fd;
      if (fd == ls) {
        for (;;) {
          int c = accept(ls, NULL, NULL);
          if (c < 0) break;
          set_nonblock(c);
          Conn *cn = (Conn *)calloc(1, sizeof(Conn));
          cn->fd = c;
          conns[c] = cn;
          memset(&ev, 0, sizeof(ev));
          ev.events = EPOLLIN;
          ev.data.fd = c;
          epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
        }
        continue;
      }
      Conn *cn = conns[fd];
      if (!cn) { close(fd); continue; }
      ssize_t r = recv(fd, cn->buf + cn->len, sizeof(cn->buf) - cn->len, 0);
      if (r <= 0) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        free(cn);
        conns[fd] = NULL;
        continue;
      }
      cn->len += (size_t)r;
      /* Answer every complete request head in the buffer (pipelining). */
      for (;;) {
        void *end = NULL;
        for (size_t k = 0; k + 3 < cn->len; k++) {
          if (memcmp(cn->buf + k, "\r\n\r\n", 4) == 0) { end = cn->buf + k + 4; break; }
        }
        if (!end) break;
        size_t head = (size_t)((char *)end - cn->buf);
        int is_quit = cn->len >= 10 && memcmp(cn->buf, "GET /quit", 9) == 0;
        const char *resp = is_quit ? RESP_BYE : RESP;
        size_t rlen = strlen(resp);
        size_t off = 0;
        while (off < rlen) {
          ssize_t w = send(fd, resp + off, rlen - off, MSG_NOSIGNAL);
          if (w <= 0) break;
          off += (size_t)w;
        }
        if (is_quit) { running = 0; break; }
        memmove(cn->buf, cn->buf + head, cn->len - head);
        cn->len -= head;
      }
    }
  }
  close(ls);
  return 0;
}
