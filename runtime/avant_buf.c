#include "avant_rt.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  int32_t size;
  int32_t cap;
} AvantBuf;

static void buf_grow(AvantBuf *b, int32_t need) {
  int32_t ncap = b->cap ? b->cap : 16;
  while (ncap < need) {
    if (ncap > 1073741823) {
      ncap = need;
      break;
    }
    ncap *= 2;
  }
  uint8_t *ndata = (uint8_t *)avant_alloc((uint64_t)ncap, AVANT_TYPE_BYTES);
  if (b->data && b->size > 0) {
    memcpy(ndata, b->data, (size_t)b->size);
  }
  avant_barrier(b);
  b->data = ndata;
  b->cap = ncap;
}

static void buf_reserve(AvantBuf *b, int32_t extra) {
  int64_t need = (int64_t)b->size + (int64_t)extra;
  if (need > b->cap) {
    buf_grow(b, (int32_t)need);
  }
}

void *avant_buf_new(int32_t cap) {
  avant_gc_defer_enter();
  AvantBuf *b = (AvantBuf *)avant_alloc(sizeof(AvantBuf), AVANT_TYPE_BUF);
  if (cap > 0) {
    b->data = (uint8_t *)avant_alloc((uint64_t)cap, AVANT_TYPE_BYTES);
    b->cap = cap;
  }
  avant_gc_defer_leave();
  return b;
}

static void buf_append_slow(AvantBuf *b, const void *p, int32_t n) {
  avant_gc_defer_enter();
  buf_reserve(b, n);
  if (p && n > 0 && b->data) {
    memcpy(b->data + b->size, p, (size_t)n);
    b->size += n;
  }
  avant_gc_defer_leave();
}

void avant_buf_append(void *buf, const void *p, int32_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!b || n <= 0) {
    return;
  }
  if (b->data && (int64_t)b->size + (int64_t)n <= b->cap) {
    memcpy(b->data + b->size, p, (size_t)n);
    b->size += n;
    return;
  }
  buf_append_slow(b, p, n);
}

void avant_buf_push_byte(void *buf, int32_t c) {
  AvantBuf *b = (AvantBuf *)buf;
  uint8_t byte = (uint8_t)(c & 255);
  if (!b) {
    return;
  }
  if (b->data && b->size < b->cap) {
    b->data[b->size] = byte;
    b->size += 1;
    return;
  }
  buf_append_slow(b, &byte, 1);
}

void avant_buf_push_str(void *buf, const char *s) {
  AvantBuf *b = (AvantBuf *)buf;
  size_t n;
  if (!b) {
    return;
  }
  n = avant_str_n(s);
  if (!n) {
    return;
  }
  if (b->data && (int64_t)b->size + (int64_t)n <= b->cap) {
    memcpy(b->data + b->size, s, n);
    b->size += (int32_t)n;
    return;
  }
  buf_append_slow(b, s, (int32_t)n);
}

void avant_buf_push_slice(void *buf, const char *s, int32_t start, int32_t stop) {
  AvantBuf *b = (AvantBuf *)buf;
  size_t n;
  int32_t i;
  int32_t end;
  int32_t len;
  if (!b || !s) {
    return;
  }
  n = avant_str_n(s);
  i = start;
  if (i < 0) {
    i = 0;
  }
  if ((size_t)i > n) {
    i = (int32_t)n;
  }
  end = stop;
  if (end < i) {
    end = i;
  }
  if ((size_t)end > n) {
    end = (int32_t)n;
  }
  len = end - i;
  if (len <= 0) {
    return;
  }
  if (b->data && (int64_t)b->size + (int64_t)len <= b->cap) {
    memcpy(b->data + b->size, s + i, (size_t)len);
    b->size += len;
    return;
  }
  buf_append_slow(b, s + i, len);
}

void avant_buf_push_fmt_f(void *buf, double v, int32_t prec) {
  char tmp[64];
  int k;
  if (prec < 0) {
    prec = 0;
  }
  if (prec > 16) {
    prec = 16;
  }
  k = snprintf(tmp, sizeof(tmp), "%.*f", prec, v);
  if (k < 0) {
    return;
  }
  avant_buf_append(buf, tmp, k);
}

void avant_buf_push_fmt_i(void *buf, int32_t n) {
  char tmp[32];
  int k = snprintf(tmp, sizeof(tmp), "%d", n);
  if (k < 0) {
    return;
  }
  avant_buf_append(buf, tmp, k);
}

char *avant_buf_to_str(void *buf) {
  avant_gc_defer_enter();
  AvantBuf *b = (AvantBuf *)buf;
  int32_t n = 0;
  const uint8_t *src = NULL;
  if (b && b->data && b->size > 0) {
    n = b->size;
    src = b->data;
  }
  char *p = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n) {
    memcpy(p, src, (size_t)n);
  }
  p[n] = 0;
  avant_gc_defer_leave();
  return p;
}

int32_t avant_buf_size(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  return b ? b->size : 0;
}

void avant_buf_clear(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  if (b) {
    b->size = 0;
  }
}

void avant_buf_ensure(void *buf, int32_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!b || n <= b->cap) {
    return;
  }
  avant_gc_defer_enter();
  buf_grow(b, n);
  avant_gc_defer_leave();
}

void avant_buf_set_size(void *buf, int32_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!b) {
    return;
  }
  if (n < 0) {
    n = 0;
  }
  if (n > b->cap) {
    n = b->cap;
  }
  b->size = n;
}

uint8_t *avant_buf_bytes(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  return b ? b->data : NULL;
}

int32_t avant_buf_starts(void *buf, const char *s) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!s) {
    s = "";
  }
  size_t n = avant_str_n(s);
  if (n == 0) {
    return 1;
  }
  if (!b || !b->data || (size_t)b->size < n) {
    return 0;
  }
  return memcmp(b->data, s, n) == 0 ? 1 : 0;
}
