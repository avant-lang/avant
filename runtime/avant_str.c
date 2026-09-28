#include "avant_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

static char *g_byte_str[256];
static char *g_empty;

static char *empty_str(void);

size_t avant_str_n(const char *s) {
  if (!s) {
    return 0;
  }
  if (((uintptr_t)s % 8) == 0) {
    AvantHeader *h = avant_header((void *)s);
    if ((h->flags & AVANT_FLAG_HEAP) && h->type_id == AVANT_TYPE_BYTES && h->size > 0
        && avant_is_heap((void *)s)) {
      return (size_t)h->size - 1;
    }
  }
  return strlen(s);
}

char *avant_str_empty(void) {
  return empty_str();
}

static char *dup_bytes(const char *s, size_t n) {
  avant_gc_defer_enter();
  char *p = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = 0;
  avant_gc_defer_leave();
  return p;
}

static char *empty_str(void) {
  if (!g_empty) {
    g_empty = dup_bytes("", 0);
    avant_pin(g_empty);
  }
  return g_empty;
}

char *avant_str_concat(const char *a, const char *b) {
  if (!a) {
    a = "";
  }
  if (!b) {
    b = "";
  }
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  if (na == 0) {
    return (char *)b;
  }
  if (nb == 0) {
    return (char *)a;
  }
  avant_gc_defer_enter();
  char *p = (char *)avant_alloc((uint64_t)na + (uint64_t)nb + 1, AVANT_TYPE_BYTES);
  memcpy(p, a, na);
  memcpy(p + na, b, nb);
  p[na + nb] = 0;
  avant_gc_defer_leave();
  return p;
}

char *avant_str_from_i32_bytes(void *arr) {
  avant_gc_defer_enter();
  AvantArray *a = (AvantArray *)arr;
  int32_t n = 0;
  const int32_t *src = NULL;
  if (a && a->buf && a->size > 0) {
    n = a->size;
    src = (const int32_t *)a->buf;
  }
  char *p = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  int32_t i;
  for (i = 0; i < n; i++) {
    p[i] = (char)(unsigned char)(src[i] & 255);
  }
  p[n] = 0;
  avant_gc_defer_leave();
  return p;
}

char *avant_str_repeat_byte(int32_t b, int32_t n) {
  if (n < 0) {
    n = 0;
  }
  char *p = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  memset(p, (unsigned char)(b & 255), (size_t)n);
  p[n] = 0;
  return p;
}

static void clamp_range(size_t n, int32_t start, int32_t stop, size_t *out_i, size_t *out_len) {
  int32_t i = start;
  if (i < 0) {
    i = 0;
  }
  if ((size_t)i > n) {
    i = (int32_t)n;
  }
  int32_t end = stop;
  if (end < i) {
    end = i;
  }
  if ((size_t)end > n) {
    end = (int32_t)n;
  }
  *out_i = (size_t)i;
  *out_len = (size_t)(end - i);
}

char *avant_str_slice(const char *s, int32_t start, int32_t stop) {
  if (!s) {
    s = "";
  }
  size_t i = 0;
  size_t len = 0;
  clamp_range(avant_str_n(s), start, stop, &i, &len);
  if (len == 0) {
    return empty_str();
  }
  return dup_bytes(s + i, len);
}

char *avant_str_from_int(int32_t n) {
  char buf[32];
  int k = snprintf(buf, sizeof(buf), "%d", n);
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

char *avant_str_from_i64(int64_t n) {
  char buf[32];
  int k = snprintf(buf, sizeof(buf), "%lld", (long long)n);
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

char *avant_str_from_u64(uint64_t n) {
  char buf[32];
  int k = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

char *avant_str_from_float(double n) {
  char buf[64];
  int k = snprintf(buf, sizeof(buf), "%g", n);
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

char *avant_str_from_bool(int32_t n) {
  return dup_bytes(n ? "true" : "false", n ? 4 : 5);
}

char *avant_str_from_byte(int32_t b) {
  unsigned idx = (unsigned)(b & 255);
  if (g_byte_str[idx]) {
    return g_byte_str[idx];
  }
  char c = (char)idx;
  char *p = dup_bytes(&c, 1);
  avant_pin(p);
  g_byte_str[idx] = p;
  return p;
}

double avant_exp(double x) {
  return exp(x);
}

double avant_str_to_f(const char *s) {
  if (!s) {
    return 0.0;
  }
  return strtod(s, NULL);
}

double avant_str_to_f_slice(const char *s, int32_t start, int32_t stop) {
  if (!s) {
    return 0.0;
  }
  size_t i = 0;
  size_t len = 0;
  clamp_range(avant_str_n(s), start, stop, &i, &len);
  if (len == 0) {
    return 0.0;
  }
  if (len < 128) {
    char buf[128];
    memcpy(buf, s + i, len);
    buf[len] = 0;
    return strtod(buf, NULL);
  }
  char *tmp = (char *)malloc(len + 1);
  if (!tmp) {
    return 0.0;
  }
  memcpy(tmp, s + i, len);
  tmp[len] = 0;
  double v = strtod(tmp, NULL);
  free(tmp);
  return v;
}

int32_t avant_str_to_i(const char *s) {
  if (!s) {
    return 0;
  }
  return (int32_t)strtol(s, NULL, 10);
}

char *avant_str_fmt_float(double v, int32_t prec) {
  char buf[64];
  if (prec < 0) {
    prec = 0;
  }
  if (prec > 16) {
    prec = 16;
  }
  int k = snprintf(buf, sizeof(buf), "%.*f", prec, v);
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

int32_t avant_str_eq(const char *a, const char *b) {
  if (!a) {
    a = "";
  }
  if (!b) {
    b = "";
  }
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  if (na != nb) {
    return 0;
  }
  return memcmp(a, b, na) == 0 ? 1 : 0;
}

int32_t avant_str_size(const char *s) {
  return (int32_t)avant_str_n(s);
}

int32_t avant_str_byte(const char *s, int32_t i) {
  if (!s || i < 0) {
    return 0;
  }
  size_t n = avant_str_n(s);
  if ((size_t)i >= n) {
    return 0;
  }
  return (int32_t)(unsigned char)s[i];
}

int32_t avant_checksum_str(const char *s) {
  uint32_t hash = 5381u;
  if (!s) {
    return (int32_t)hash;
  }
  size_t n = avant_str_n(s);
  size_t i;
  for (i = 0; i < n; i++) {
    hash = ((hash << 5) + hash) + (unsigned char)s[i];
  }
  return (int32_t)hash;
}

int32_t avant_checksum_f64(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.7f", v);
  return avant_checksum_str(buf);
}
