/* Single-threaded epoll WebSocket echo server for the harness.
 * Handshake via OpenSSL SHA1; text frames are echoed. /quit is an
 * HTTP path that exits. Bench only; not a general WS stack. */
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

static int set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void b64(const unsigned char *in, size_t len, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0, o = 0;
  while (i + 3 <= len) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63]; out[o++] = T[v & 63];
    i += 3;
  }
  if (len - i == 1) {
    unsigned v = in[i] << 16;
    out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
    out[o++] = '='; out[o++] = '=';
  } else if (len - i == 2) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8);
    out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63]; out[o++] = '=';
  }
  out[o] = 0;
}

typedef struct {
  int fd;
  char buf[65536];
  size_t len;
  int ws;
} Conn;

static void ws_send(int fd, const char *payload, size_t len) {
  char head[10];
  size_t h = 0;
  head[h++] = (char)0x81;
  if (len < 126) { head[h++] = (char)len; }
  else if (len <= 65535) { head[h++] = 126; head[h++] = (char)(len >> 8); head[h++] = (char)len; }
  else { return; }
  size_t off = 0;
  while (off < h) { ssize_t w = send(fd, head + off, h - off, MSG_NOSIGNAL); if (w <= 0) return; off += (size_t)w; }
  off = 0;
  while (off < len) { ssize_t w = send(fd, payload + off, len - off, MSG_NOSIGNAL); if (w <= 0) return; off += (size_t)w; }
}

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 18311;
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
  ev.events = EPOLLIN; ev.data.fd = ls;
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
          setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          Conn *cn = (Conn *)calloc(1, sizeof(Conn));
          cn->fd = c;
          conns[c] = cn;
          memset(&ev, 0, sizeof(ev));
          ev.events = EPOLLIN; ev.data.fd = c;
          epoll_ctl(ep, EPOLL_CTL_ADD, c, &ev);
        }
        continue;
      }
      Conn *cn = conns[fd];
      if (!cn) { close(fd); continue; }
      ssize_t r = recv(fd, cn->buf + cn->len, sizeof(cn->buf) - cn->len, 0);
      if (r <= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL); close(fd); free(cn); conns[fd] = NULL; continue; }
      cn->len += (size_t)r;
      if (!cn->ws) {
        char *end = strstr(cn->buf, "\r\n\r\n");
        if (!end) continue;
        if (cn->len >= 10 && memcmp(cn->buf, "GET /quit", 9) == 0) {
          const char *bye = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbye";
          (void)!send(fd, bye, strlen(bye), MSG_NOSIGNAL);
          running = 0;
          break;
        }
        char *k = strstr(cn->buf, "Sec-WebSocket-Key: ");
        if (!k) { close(fd); continue; }
        k += 19;
        char *ke = strstr(k, "\r\n");
        char key[128];
        size_t kl = (size_t)(ke - k);
        if (kl >= sizeof(key)) { close(fd); continue; }
        memcpy(key, k, kl); key[kl] = 0;
        char merged[256];
        snprintf(merged, sizeof(merged), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        unsigned char digest[SHA_DIGEST_LENGTH];
        SHA1((unsigned char *)merged, strlen(merged), digest);
        char accept[64];
        b64(digest, SHA_DIGEST_LENGTH, accept);
        char resp[256];
        int rl = snprintf(resp, sizeof(resp),
          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        (void)!send(fd, resp, (size_t)rl, MSG_NOSIGNAL);
        cn->ws = 1;
        size_t used = (size_t)(end + 4 - cn->buf);
        memmove(cn->buf, cn->buf + used, cn->len - used);
        cn->len -= used;
      }
      /* Parse whole frames; echo text, answer ping, drop the rest. */
      for (;;) {
        if (cn->len < 2) break;
        unsigned char b0 = (unsigned char)cn->buf[0];
        unsigned char b1 = (unsigned char)cn->buf[1];
        size_t plen = b1 & 0x7f;
        size_t hlen = 2 + ((b1 & 0x80) ? 4 : 0);
        if (plen == 126) {
          if (cn->len < 4) break;
          plen = ((unsigned char)cn->buf[2] << 8) | (unsigned char)cn->buf[3];
          hlen += 2;
        } else if (plen == 127) { break; }
        if (cn->len < hlen + plen) break;
        char payload[65536];
        size_t j;
        for (j = 0; j < plen; j++) {
          char c = cn->buf[hlen + j];
          if (b1 & 0x80) c = (char)(c ^ cn->buf[hlen - 4 + (j & 3)]);
          payload[j] = c;
        }
        int opcode = b0 & 0x0f;
        if (opcode == 1 || opcode == 2) ws_send(fd, payload, plen);
        else if (opcode == 9) {
          /* ping -> pong */
          char head[2] = {(char)0x8a, 0};
          head[1] = (char)plen;
          (void)!send(fd, head, 2, MSG_NOSIGNAL);
          if (plen) (void)!send(fd, payload, plen, MSG_NOSIGNAL);
        } else if (opcode == 8) { close(fd); }
        memmove(cn->buf, cn->buf + hlen + plen, cn->len - hlen - plen);
        cn->len -= hlen + plen;
      }
    }
  }
  close(ls);
  return 0;
}
