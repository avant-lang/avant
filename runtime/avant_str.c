#include "avant_rt.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern atomic_int g_avant_shared;

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

static char *g_byte_str[256];
static char *g_empty;

static char *empty_str(void);

/* Short tagged strings keep the length in the header. The NUL check stays
 * inside the same page, so a lookalike header cannot fault. Longer strings
 * and raw C pointers still take the block walk or strlen. */
static int str_meta_len(const char *s, size_t *out) {
  uintptr_t u;
  AvantHeader *h;
  uint32_t sz;
  uintptr_t last;
  if (!s) {
    return 0;
  }
  u = (uintptr_t)s;
  if (u & 7u) {
    return 0;
  }
  h = avant_header((void *)s);
  if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA)) != (AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA)) {
    return 0;
  }
  if (h->type_id != AVANT_TYPE_BYTES) {
    return 0;
  }
  sz = h->size;
  if (sz == 0 || sz > 128) {
    return 0;
  }
  last = u + (sz - 1);
  if (((last ^ u) & ~(uintptr_t)4095) != 0) {
    return 0;
  }
  if (s[sz - 1] != 0) {
    return 0;
  }
  *out = (size_t)sz - 1;
  return 1;
}

size_t avant_str_n(const char *s) {
  size_t meta = 0;
  if (!s) {
    return 0;
  }
  if (str_meta_len(s, &meta)) {
    return meta;
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
  avant_str_tag(p);
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
  avant_str_tag(p);
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
  avant_str_tag(p);
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
  avant_str_tag(p);
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
  /* Does not allocate. The lock only matters once another thread can
   * collect while the caller's pointers sit in registers. */
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0) {
    return exp(x);
  }
  avant_world_lock();
  double r = exp(x);
  avant_world_unlock();
  return r;
}

char *avant_str_cat_i32(const char *prefix, int32_t n) {
  char digs[16];
  int nd = 0;
  uint32_t u;
  if (n < 0) {
    u = (uint32_t)(-(n + 1)) + 1u;
  } else {
    u = (uint32_t)n;
  }
  if (u == 0) {
    digs[nd++] = '0';
  } else {
    while (u) {
      digs[nd++] = (char)('0' + (u % 10u));
      u /= 10u;
    }
  }
  if (!prefix) {
    prefix = "";
  }
  size_t np = avant_str_n(prefix);
  size_t neg = n < 0 ? 1u : 0u;
  size_t total = np + neg + (size_t)nd;
  avant_gc_defer_enter();
  char *p = (char *)avant_alloc((uint64_t)total + 1, AVANT_TYPE_BYTES);
  if (np) {
    memcpy(p, prefix, np);
  }
  size_t k = np;
  if (neg) {
    p[k++] = '-';
  }
  while (nd > 0) {
    nd--;
    p[k++] = digs[nd];
  }
  p[k] = 0;
  avant_str_tag(p);
  avant_gc_defer_leave();
  return p;
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

static int is_ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* mode 0 = both ends, 1 = left, 2 = right. */
char *avant_str_strip(const char *s, int32_t mode) {
  size_t i = 0;
  size_t len;
  if (!s) {
    s = "";
  }
  len = avant_str_n(s);
  if (mode == 0 || mode == 1) {
    while (i < len && is_ascii_space((unsigned char)s[i])) {
      i++;
    }
  }
  if (mode == 0 || mode == 2) {
    while (len > i && is_ascii_space((unsigned char)s[len - 1])) {
      len--;
    }
  }
  if (i == 0 && len == avant_str_n(s)) {
    return (char *)s;
  }
  if (len <= i) {
    return empty_str();
  }
  return dup_bytes(s + i, len - i);
}

static void *str_array_build(char **items, int32_t n) {
  avant_gc_defer_enter();
  AvantArray *a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  char **buf = NULL;
  int32_t i;
  if (n > 0) {
    buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
    for (i = 0; i < n; i++) {
      buf[i] = items[i];
    }
  }
  a->buf = buf;
  a->size = n;
  a->cap = n;
  avant_gc_defer_leave();
  return a;
}

/* Keeps empty fields. Empty separator traps. */
void *avant_str_split(const char *s, const char *sep) {
  size_t n;
  size_t m;
  size_t i;
  size_t start;
  int32_t count = 1;
  int32_t idx = 0;
  char **items;
  if (!s) {
    s = "";
  }
  if (!sep) {
    sep = "";
  }
  n = avant_str_n(s);
  m = avant_str_n(sep);
  if (m == 0) {
    fprintf(stderr, "avant runtime: split separator is empty\n");
    exit(1);
  }
  for (i = 0; i + m <= n; i++) {
    if (memcmp(s + i, sep, m) == 0) {
      count++;
      i += m - 1;
    }
  }
  items = (char **)malloc((size_t)count * sizeof(char *));
  if (!items) {
    return str_array_build(NULL, 0);
  }
  start = 0;
  i = 0;
  while (i + m <= n) {
    if (memcmp(s + i, sep, m) == 0) {
      items[idx++] = dup_bytes(s + start, i - start);
      i += m;
      start = i;
    } else {
      i++;
    }
  }
  items[idx++] = dup_bytes(s + start, n - start);
  {
    void *r = str_array_build(items, idx);
    free(items);
    return r;
  }
}

/* Splits on runs of ASCII whitespace and drops empty fields. */
void *avant_str_fields(const char *s) {
  size_t n;
  size_t i = 0;
  int32_t count = 0;
  int32_t idx = 0;
  char **items;
  if (!s) {
    s = "";
  }
  n = avant_str_n(s);
  while (i < n) {
    while (i < n && is_ascii_space((unsigned char)s[i])) {
      i++;
    }
    if (i >= n) {
      break;
    }
    count++;
    while (i < n && !is_ascii_space((unsigned char)s[i])) {
      i++;
    }
  }
  if (count == 0) {
    return str_array_build(NULL, 0);
  }
  items = (char **)malloc((size_t)count * sizeof(char *));
  if (!items) {
    return str_array_build(NULL, 0);
  }
  i = 0;
  while (i < n && idx < count) {
    size_t st;
    while (i < n && is_ascii_space((unsigned char)s[i])) {
      i++;
    }
    if (i >= n) {
      break;
    }
    st = i;
    while (i < n && !is_ascii_space((unsigned char)s[i])) {
      i++;
    }
    items[idx++] = dup_bytes(s + st, i - st);
  }
  {
    void *r = str_array_build(items, idx);
    free(items);
    return r;
  }
}

static int utf8_valid(const unsigned char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) {
      i++;
      continue;
    }
    if ((c & 0xe0) == 0xc0) {
      if (c < 0xc2) {
        return 0;
      }
      extra = 1;
      cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      extra = 2;
      cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      if (c > 0xf4) {
        return 0;
      }
      extra = 3;
      cp = c & 0x07;
    } else {
      return 0;
    }
    if (i + (size_t)extra >= n) {
      return 0;
    }
    {
      int k;
      for (k = 1; k <= extra; k++) {
        unsigned char cc = s[i + (size_t)k];
        if ((cc & 0xc0) != 0x80) {
          return 0;
        }
        cp = (cp << 6) | (cc & 0x3f);
      }
    }
    if (extra == 2) {
      if (cp < 0x800) {
        return 0;
      }
      if (cp >= 0xd800 && cp <= 0xdfff) {
        return 0;
      }
    }
    if (extra == 3) {
      if (cp < 0x10000 || cp > 0x10ffff) {
        return 0;
      }
    }
    i += (size_t)extra + 1;
  }
  return 1;
}

char *avant_str_from_utf8(const char *s, int32_t *found) {
  size_t n;
  if (!s) {
    s = "";
  }
  n = avant_str_n(s);
  if (!utf8_valid((const unsigned char *)s, n)) {
    if (found) {
      *found = 0;
    }
    return empty_str();
  }
  if (found) {
    *found = 1;
  }
  return (char *)s;
}
