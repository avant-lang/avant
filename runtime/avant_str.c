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

/* One-entry length cache. A hit needs the same pointer, a generation
 * with no collection since the entry was stored (so the object cannot
 * have moved or been freed: both happen only inside a collection),
 * the identical 8 header bytes, and the terminator still in place.
 * Strings are immutable, so that object still has the stored length.
 * Entries are stored only after avant_is_heap or str_meta_len has
 * validated the pointer, so a raw C pointer is never promoted into a
 * heap reading: it reaches the cache only by occupying the exact
 * address of a validated heap string with its header bytes untouched
 * and no collection in between, in which case the uncached heap path
 * would read the same header and return the same length. */
typedef struct {
  const char *ptr;
  size_t n;
  uint64_t hdr;
  uint32_t gen;
} LenSlot;

static __thread LenSlot g_len_slots[4];
static __thread unsigned g_len_victim;

static void len_cache_store(const char *s, size_t n) {
  LenSlot *slot = &g_len_slots[g_len_victim & 3u];
  uint64_t hdr = 0;
  g_len_victim++;
  memcpy(&hdr, avant_header((void *)s), sizeof(hdr));
  slot->ptr = s;
  slot->n = n;
  slot->hdr = hdr;
  slot->gen = avant_str_generation();
}

static size_t str_n_long(const char *s) __attribute__((noinline));
static size_t str_n_long(const char *s) {
  uint32_t gen = avant_str_generation();
  int i;
  for (i = 0; i < 4; i++) {
    LenSlot *slot = &g_len_slots[i];
    uint64_t hdr = 0;
    if (s != slot->ptr || gen != slot->gen) {
      continue;
    }
    memcpy(&hdr, avant_header((void *)s), sizeof(hdr));
    if (hdr == slot->hdr && s[slot->n] == 0) {
      return slot->n;
    }
  }
  if (((uintptr_t)s % 8) == 0) {
    AvantHeader *h = avant_header((void *)s);
    if ((h->flags & AVANT_FLAG_HEAP) && h->type_id == AVANT_TYPE_BYTES && h->size > 0
        && avant_is_heap((void *)s)) {
      size_t n = (size_t)h->size - 1;
      len_cache_store(s, n);
      return n;
    }
  }
  return strlen(s);
}

size_t avant_str_n(const char *s) {
  size_t meta = 0;
  if (!s) {
    return 0;
  }
  if (str_meta_len(s, &meta)) {
    return meta;
  }
  return str_n_long(s);
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

/* Exact fast float parse. Accepts only [-]digits[.digits][(e|E)[-]digits]
 * (either digit run may be empty, not both) whose mantissa fits in 53 bits
 * with at most 15 significant digits and whose decimal exponent is within
 * 22. The true value is then mantissa * 10^e with both operands exactly
 * representable, so one IEEE multiply or divide rounds it exactly as the
 * correctly rounded strtod does. Anything else returns 0 and the caller
 * falls back to strtod on the same bytes. */
static int fast_f64(const char *p, size_t len, double *out) {
  static const double pow10_exact[23] = {
      1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  size_t i = 0;
  int neg = 0;
  uint64_t mant = 0;
  int sig = 0;
  int started = 0;
  long exp10 = 0;
  size_t ndig = 0;
  if (i < len && (p[i] == '-' || p[i] == '+')) {
    neg = p[i] == '-';
    i++;
  }
  while (i < len && p[i] >= '0' && p[i] <= '9') {
    int d = p[i] - '0';
    if (d != 0 || started) {
      started = 1;
      sig++;
      if (sig > 15) {
        return 0;
      }
      mant = mant * 10u + (uint64_t)d;
    }
    ndig++;
    i++;
  }
  if (i < len && p[i] == '.') {
    i++;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
      int d = p[i] - '0';
      exp10--;
      if (d != 0 || started) {
        started = 1;
        sig++;
        if (sig > 15) {
          return 0;
        }
        mant = mant * 10u + (uint64_t)d;
      }
      ndig++;
      i++;
    }
  }
  if (ndig == 0) {
    return 0;
  }
  if (i < len && (p[i] == 'e' || p[i] == 'E')) {
    long e = 0;
    int eneg = 0;
    size_t j = i + 1;
    if (j < len && (p[j] == '-' || p[j] == '+')) {
      eneg = p[j] == '-';
      j++;
    }
    if (j >= len) {
      return 0;
    }
    for (; j < len; j++) {
      if (p[j] < '0' || p[j] > '9') {
        return 0;
      }
      e = e * 10 + (p[j] - '0');
      if (e > 1000) {
        return 0;
      }
    }
    exp10 += eneg ? -e : e;
    i = len;
  }
  if (i != len || mant >= (1ull << 53)) {
    return 0;
  }
  if (exp10 > 22 || exp10 < -22) {
    return 0;
  }
  {
    double v = (double)mant;
    if (exp10 > 0) {
      v = v * pow10_exact[exp10];
    } else if (exp10 < 0) {
      v = v / pow10_exact[-exp10];
    }
    *out = neg ? -v : v;
    return 1;
  }
}

double avant_str_to_f(const char *s) {
  if (!s) {
    return 0.0;
  }
  double v = 0.0;
  if (fast_f64(s, avant_str_n(s), &v)) {
    return v;
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
  {
    double fv = 0.0;
    if (fast_f64(s + i, len, &fv)) {
      return fv;
    }
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

static size_t utf8_char_len(const unsigned char *s, size_t n, size_t i, int *valid) {
  unsigned char c = s[i];
  uint32_t cp;
  int extra;
  int k;
  *valid = 1;
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xe0) == 0xc0 && c >= 0xc2) {
    extra = 1;
    cp = c & 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    extra = 2;
    cp = c & 0x0f;
  } else if ((c & 0xf8) == 0xf0 && c <= 0xf4) {
    extra = 3;
    cp = c & 0x07;
  } else {
    *valid = 0;
    return 1;
  }
  if (i + (size_t)extra >= n) {
    *valid = 0;
    return 1;
  }
  for (k = 1; k <= extra; k++) {
    unsigned char cc = s[i + (size_t)k];
    if ((cc & 0xc0) != 0x80) {
      *valid = 0;
      return 1;
    }
    cp = (cp << 6) | (uint32_t)(cc & 0x3f);
  }
  if (extra == 1 && cp < 0x80) {
    *valid = 0;
    return 1;
  }
  if (extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) {
    *valid = 0;
    return 1;
  }
  if (extra == 3 && (cp < 0x10000 || cp > 0x10ffff)) {
    *valid = 0;
    return 1;
  }
  return (size_t)extra + 1;
}

static void *chars_build(const char *s, int strict) {
  size_t n;
  size_t i = 0;
  int32_t count = 0;
  int32_t idx = 0;
  char **items;
  if (!s) {
    s = "";
  }
  n = avant_str_n(s);
  if (strict && !utf8_valid((const unsigned char *)s, n)) {
    fprintf(stderr, "avant runtime: Bytes.chars needs valid UTF-8\n");
    exit(1);
  }
  while (i < n) {
    int valid = 0;
    size_t len = utf8_char_len((const unsigned char *)s, n, i, &valid);
    (void)valid;
    count++;
    i += len;
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
    int valid = 0;
    size_t len = utf8_char_len((const unsigned char *)s, n, i, &valid);
    if (valid) {
      items[idx++] = dup_bytes(s + i, len);
    } else {
      items[idx++] = dup_bytes("\xef\xbf\xbd", 3);
    }
    i += len;
  }
  {
    void *r = str_array_build(items, idx);
    free(items);
    return r;
  }
}

void *avant_str_chars(const char *s) {
  return chars_build(s, 0);
}

void *avant_bytes_chars(const char *s) {
  return chars_build(s, 1);
}
