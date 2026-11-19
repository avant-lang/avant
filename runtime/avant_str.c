/* fwrite_unlocked (puts: a line under one hold of the stream's lock). */
#define _GNU_SOURCE

#include "avant_rt.h"
#include "avant_gc.h"
#include "avant_fmt.h"
#include "third_party/yyjson/f64_to_dec.h"

#include <math.h>
#include <stdatomic.h>
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

/* memcpy of N bytes for the short strings most copies are: up to 16
 * bytes in two overlapping loads and stores of each width, inline, where
 * the libc call cost more than the copy (a tokenizer's names, a key's
 * prefix and digits); a longer one is memcpy's. */
static inline __attribute__((always_inline)) void copy_short(char *d, const char *s, size_t n) {
  if (n <= 16) {
    if (n >= 8) {
      uint64_t a, b;
      memcpy(&a, s, 8);
      memcpy(&b, s + n - 8, 8);
      memcpy(d, &a, 8);
      memcpy(d + n - 8, &b, 8);
    } else if (n >= 4) {
      uint32_t a, b;
      memcpy(&a, s, 4);
      memcpy(&b, s + n - 4, 4);
      memcpy(d, &a, 4);
      memcpy(d + n - 4, &b, 4);
    } else if (n > 0) {
      char a = s[0], b = s[n >> 1], c = s[n - 1];
      d[0] = a;
      d[n >> 1] = b;
      d[n - 1] = c;
    }
    return;
  }
  memcpy(d, s, n);
}

/* A string's length is its header's: every String and Bytes value a
 * program holds is a string object (a heap one, or a literal the
 * compiler emitted with the same header), whose header size is the bytes
 * and the terminator, or a view (AvantStrView) holding its length. A C
 * function's result, a C field's string and a C callback's string
 * argument are copied into the heap on the way in (avant_c_str), so no
 * bare C pointer is ever a String. */
size_t avant_str_n(const char *s) {
  const AvantHeader *h;
  if (!s) {
    return 0;
  }
  h = avant_header((void *)s);
  if (__builtin_expect(h->type_id == AVANT_TYPE_STRVIEW, 0)) {
    return ((const AvantStrView *)(const void *)s)->len;
  }
  return (size_t)h->size - 1;
}

char *avant_str_empty(void) {
  return empty_str();
}

/* A copy of the N bytes at S + OFF (RC1/RC2, avant_gc.h), S a C buffer
 * or an address inside a heap object: S is a handle across the
 * allocation (its slot leaves C memory alone and follows the object an
 * inside address lies in) and the source is formed from it after the
 * allocation. */
static char *dup_raw_at(const char *s, size_t off, size_t n) {
  char *p;
  /* The inline allocation when it serves (it cannot collect), else
   * the one that may, with s a handle across it. */
  p = avant_str_alloc_try((uint64_t)(n));
  if (!p) {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &s);
    p = avant_str_alloc((uint64_t)(n));
  }
  if (n) {
    copy_short(p, s + off, n);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

static char *dup_bytes(const char *s, size_t n) {
  return dup_raw_at(s, 0, n);
}

/* A copy of the N bytes of the String S from its byte OFF on: S (flat or
 * a view) is a handle across the allocation and its bytes are formed
 * again after it. */
static char *dup_bytes_at(const char *s, size_t off, size_t n) {
  size_t sn;
  char *p;
  /* The inline allocation when it serves (it cannot collect), else
   * the one that may, with s a handle across it. */
  p = avant_str_alloc_try((uint64_t)(n));
  if (!p) {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &s);
    p = avant_str_alloc((uint64_t)(n));
  }
  if (n) {
    copy_short(p, avant_str_bytes(s, &sn) + off, n);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* The bytes of S from OFF, N of them, as a String: a view when it is
 * worth an object and cannot hold a much larger parent alive (avant_rt.h,
 * AvantStrView), else a copy. S is the whole string N and OFF lie in;
 * a view of a view names the first one's base. */
static char *str_part(const char *s, size_t off, size_t n) {
  size_t sn;
  char *base;
  uint32_t boff;
  size_t bn;
  AvantStrView *v;
  if (n == 0) {
    return empty_str();
  }
  avant_str_bytes(s, &sn);
  if (off == 0 && n == sn) {
    return (char *)s;
  }
  if (n < AVANT_VIEW_MIN) {
    return dup_bytes_at(s, off, n);
  }
  if (avant_str_is_view(s)) {
    base = ((const AvantStrView *)(const void *)s)->base;
    boff = ((const AvantStrView *)(const void *)s)->off;
  } else {
    base = (char *)s;
    boff = 0;
  }
  bn = (size_t)avant_header(base)->size - 1;
  if (n * AVANT_VIEW_SHARE < bn) {
    return dup_bytes_at(s, off, n);
  }
  {
    /* RC1: the base is a handle across the view's allocation. */
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &base);
    v = (AvantStrView *)avant_alloc(sizeof(AvantStrView), AVANT_TYPE_STRVIEW);
    /* The barrier before the store (RC3): the view may be born mature. */
    avant_barrier(v);
    v->base = base;
    v->off = boff + (uint32_t)off;
    v->len = (uint32_t)n;
    return (char *)v;
  }
}

/* The call form of the generated code's inline read of a string's bytes
 * (compiler/llvm/emit_gc.av, ll_str_data). */
const char *avant_str_data(const char *s, int32_t is_view) {
  size_t n;
  (void)is_view;
  return avant_str_bytes(s, &n);
}

/* The call form of the generated code's inline view test
 * (compiler/llvm/emit_gc.av, ll_str_view_test). */
int32_t avant_str_view_test(const char *s) {
  return avant_str_is_view(s) ? 1 : 0;
}

char *avant_str_flat(const char *s) {
  size_t n;
  if (!avant_str_is_view(s)) {
    return (char *)s;
  }
  avant_str_bytes(s, &n);
  return dup_bytes_at(s, 0, n);
}

char *avant_str_c_arg(const char *s) {
  const AvantStrView *v;
  if (!avant_str_is_view(s)) {
    return (char *)s;
  }
  v = (const AvantStrView *)(const void *)s;
  if ((size_t)v->off + v->len == (size_t)avant_header(v->base)->size - 1) {
    return v->base + v->off;
  }
  return dup_bytes_at(s, 0, v->len);
}

/* gc-lint: holds nothing (its input is a C literal; the result is pinned before any other call). */
static char *empty_str(void) {
  if (!g_empty) {
    g_empty = dup_bytes("", 0);
    avant_pin(g_empty);
  }
  return g_empty;
}

/* A string that came from C: a C function's `String` result, a `String`
 * field of C memory or of a struct C returned, a C callback's `String`
 * argument (compiler/mir/lower_ffi.av passes each one through here).
 * Every String the program holds carries its length in its header, and
 * an address C hands back has none unless it is the exact start of a
 * heap string (one the program passed to C, or the runtime's own result:
 * a heap object of bytes whose header size ends at its terminator): that
 * one comes back as it is, and null too. Anything else (C's own
 * memory, a literal of C, the inside of a heap object, which a
 * reference-typed field may not hold either: avant_gc_internal.h) comes
 * back as a fresh heap copy of the C string there, to its terminator, so
 * C may free or change its memory afterwards. The copy allocates, so
 * this is a collecting call: S is a handle across it (dup_bytes_at: a
 * root slot keeps the object S lies in and follows it when it moves, and
 * leaves C's memory alone). */
char *avant_c_str(char *s) {
  size_t n;
  if (!s) {
    return s;
  }
  if (((uintptr_t)s & 7u) == 0 && avant_is_heap(s)) {
    AvantHeader *h = avant_header(s);
    if (h->type_id == AVANT_TYPE_BYTES && h->size > 0 && s[h->size - 1] == 0) {
      return s;
    }
  }
  n = strlen(s);
  avant_size_check(n, AVANT_SIZE_STRING);
  return dup_bytes(s, n);
}

char *avant_str_concat(const char *a, const char *b) {
  if (!a) {
    a = AVANT_STR_EMPTY;
  }
  if (!b) {
    b = AVANT_STR_EMPTY;
  }
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  if (na == 0) {
    return (char *)b;
  }
  if (nb == 0) {
    return (char *)a;
  }
  char *p = avant_str_alloc_try((uint64_t)na + (uint64_t)nb);
  if (!p) {
    AVANT_GC_SCOPE(2);
    AVANT_GC_ROOT(0, &a);
    AVANT_GC_ROOT(1, &b);
    p = avant_str_alloc((uint64_t)na + (uint64_t)nb);
  }
  /* RC2: the bytes are formed after the allocation (a view's too). */
  copy_short(p, avant_str_bytes(a, &na), na);
  copy_short(p + na, avant_str_bytes(b, &nb), nb);
  p[na + nb] = 0;
  avant_str_tag(p);
  return p;
}

/* bytes_to_str: the bytes are an Array(Int)'s elements (avant_int). */
char *avant_str_from_int_bytes(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t n = 0;
  const avant_int *src = NULL;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &a);
  if (a && a->buf && a->size > 0) {
    n = a->size;
  }
  char *p = avant_str_alloc((uint64_t)n);
  if (n) {
    src = (const avant_int *)a->buf;
  }
  int32_t i;
  uint64_t any = 0;
  for (i = 0; i < n; i++) {
    any |= (uint64_t)(int64_t)src[i];
    p[i] = (char)(unsigned char)src[i];
  }
  if (any > 255u) {
    /* A byte outside 0..255: the first one is named. */
    for (i = 0; i < n; i++) {
      if ((uint64_t)(int64_t)src[i] > 255u) {
        avant_trap_byte(src[i]);
      }
    }
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* gc-lint: holds nothing (its inputs are integers). */
char *avant_str_repeat_byte(avant_int b, avant_int n) {
  if ((uint64_t)(int64_t)b > 255u) {
    avant_trap_byte(b);
  }
  if (n < 0) {
    avant_trap_negative(n, 1);
  }
  char *p = avant_str_alloc((uint64_t)n);
  memset(p, (unsigned char)b, (size_t)n);
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* A slice needs 0 <= start <= stop <= size, else it traps like an index
 * (slices used to clamp). */
static void check_range(size_t n, avant_int start, avant_int stop, size_t *out_i, size_t *out_len) {
  if (start < 0 || stop < start || (size_t)stop > n) {
    avant_trap_slice(start, stop, (avant_int)n);
  }
  *out_i = (size_t)start;
  *out_len = (size_t)(stop - start);
}

/* RC2: the source is passed as the string and an offset (str_part),
 * never as s + i formed before the allocation. A long slice is a view
 * (str_part's rule), a short one a copy. */
/* gc-lint: holds nothing (str_part roots what it holds; nothing is read after it). */
char *avant_str_slice(const char *s, avant_int start, avant_int stop) {
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  size_t i = 0;
  size_t len = 0;
  check_range(avant_str_n(s), start, stop, &i, &len);
  return str_part(s, i, len);
}

/* An integer's text, its digits two at a time (avant_fmt.h's
 * avant_fmt_u64), at the end of buf: printf's parsing of "%lld" cost more
 * than the digits. Returns the start; the text runs to end. */
static char *int_text(char *end, int64_t n) {
  uint64_t u = n < 0 ? 0u - (uint64_t)n : (uint64_t)n;
  char *o = avant_fmt_u64(end, u);
  if (n < 0) {
    *--o = '-';
  }
  return o;
}

int avant_fmt_int_text(char *out, int64_t n) {
  char buf[24];
  char *o = int_text(buf + sizeof(buf), n);
  int k = (int)(buf + sizeof(buf) - o);
  memcpy(out, o, (size_t)k);
  return k;
}

char *avant_str_from_int(avant_int n) {
  char buf[24];
  char *o = int_text(buf + sizeof(buf), n);
  return dup_bytes(o, (size_t)(buf + sizeof(buf) - o));
}

char *avant_str_from_u64(uint64_t n) {
  char buf[24];
  char *o = avant_fmt_u64(buf + sizeof(buf), n);
  return dup_bytes(o, (size_t)(buf + sizeof(buf) - o));
}

/* v (below 10^8) as exactly eight digits at p. */
static void put8_digits(char *p, uint32_t v) {
  uint32_t a = v / 10000;
  uint32_t b = v % 10000;
  memcpy(p, avant_fmt_d2 + 2 * (a / 100), 2);
  memcpy(p + 2, avant_fmt_d2 + 2 * (a % 100), 2);
  memcpy(p + 4, avant_fmt_d2 + 2 * (b / 100), 2);
  memcpy(p + 6, avant_fmt_d2 + 2 * (b % 100), 2);
}

/* The one text of a Float64: the shortest
 * decimal that reads back as the same number, laid out as ECMAScript's
 * Number::toString with a decimal point kept on whole values and no '+'
 * in the exponent (the layout yyjson's writer gives Json.text, so a float
 * reads the same in puts, interpolation and JSON): `0.1`, `2.0`, `-0.0`,
 * `100000000000000000000.0`, `1e21`, `1.5e-7`, `5e-324`; NaN is `nan`, the
 * infinities `inf` and `-inf`. The digits are Schubfach's (yyjson's
 * f64_bin_to_dec, third_party/yyjson/f64_to_dec.h): the shortest decimal
 * inside the number's rounding interval, the closest one when several
 * are as short, as a search of %.15g, %.16g and %.17g with a read back
 * finds them, without printing or reading anything.
 * out holds at least 40 bytes; returns the length (out is NUL-terminated). */
int avant_fmt_float_text(char *out, double v) {
  char *o = out;
  if (v != v) {
    memcpy(out, "nan", 4);
    return 3;
  }
  if (signbit(v)) {
    *o++ = '-';
    v = -v;
  }
  if (isinf(v)) {
    memcpy(o, "inf", 4);
    return (int)(o - out) + 3;
  }
  if (v == 0.0) {
    memcpy(o, "0.0", 4);
    return (int)(o - out) + 3;
  }
  uint64_t raw;
  memcpy(&raw, &v, sizeof raw);
  uint64_t sig_raw = raw & (((uint64_t)1 << 52) - 1);
  uint32_t exp_raw = (uint32_t)(raw >> 52) & 0x7ff;
  uint64_t sig_bin = sig_raw;
  int32_t exp_bin = 1 - 1023 - 52;
  if (exp_raw != 0) {
    sig_bin = sig_raw | ((uint64_t)1 << 52);
    exp_bin = (int32_t)exp_raw - 1023 - 52;
  }
  uint64_t sig_dec = 0;
  int32_t exp_dec = 0;
  f64_bin_to_dec(sig_raw, exp_raw, sig_bin, exp_bin, &sig_dec, &exp_dec);
  /* Its digits (at most 17), two at a time from a table of pairs in
   * two halves of eight (the pairs of avant_fmt.h), as yyjson's writer
   * writes them: a digit a time was a chain of 17 dependent divisions. */
  char all[17];
  uint64_t top = sig_dec / 100000000;
  all[0] = (char)('0' + (uint32_t)(top / 100000000));
  put8_digits(all + 1, (uint32_t)(top % 100000000));
  put8_digits(all + 9, (uint32_t)(sig_dec % 100000000));
  int lead = 0;
  while (lead < 16 && all[lead] == '0') {
    lead++;
  }
  int n = 17 - lead;
  char dig[24];
  memcpy(dig, all + lead, (size_t)n);
  int e10 = exp_dec + n - 1;
  while (n > 1 && dig[n - 1] == '0') {
    n--;
  }
  dig[n] = 0;
  int dot = e10 + 1; /* the decimal point after this many digits */
  if (dot > -6 && dot <= 21) {
    if (dot <= 0) {
      *o++ = '0';
      *o++ = '.';
      for (int i = 0; i < -dot; i++) {
        *o++ = '0';
      }
      memcpy(o, dig, (size_t)n);
      o += n;
    } else {
      for (int i = 0; i < dot; i++) {
        *o++ = i < n ? dig[i] : '0';
      }
      *o++ = '.';
      if (n > dot) {
        memcpy(o, dig + dot, (size_t)(n - dot));
        o += n - dot;
      } else {
        *o++ = '0';
      }
    }
  } else {
    *o++ = dig[0];
    if (n > 1) {
      *o++ = '.';
      memcpy(o, dig + 1, (size_t)(n - 1));
      o += n - 1;
    }
    o += sprintf(o, "e%d", dot - 1);
  }
  *o = 0;
  return (int)(o - out);
}

char *avant_str_from_float(double n) {
  char buf[48];
  int k = avant_fmt_float_text(buf, n);
  return dup_bytes(buf, (size_t)k);
}

/* puts of a Float64 and of a Bool: the float's one
 * text and the words true and false, through stdout's buffer as puts's
 * printf. A failed write stops the program (avant_out_check). */
void avant_puts_f64(double v) {
  char buf[48];
  int k = avant_fmt_float_text(buf, v);
  buf[k] = '\n';
  fwrite(buf, 1, (size_t)k + 1, stdout);
  avant_out_check();
}

/* puts of a String: its bytes by length, so a NUL byte inside prints
 * (printf's %s stopped at it). */
void avant_puts_str(const char *s) {
  size_t n;
  s = avant_str_bytes(s, &n);
  /* The text and its newline under one hold of stdout's lock: another
   * thread's flush between them (a task's trap flushes stdout) would
   * write the line without its end, and the trap's line after it. */
  flockfile(stdout);
  if (n) {
    fwrite_unlocked(s, 1, n, stdout);
  }
  putc_unlocked('\n', stdout);
  funlockfile(stdout);
  avant_out_check();
}

/* puts of an integer: its decimal digits and a
 * newline through stdout's buffer, checked after the write as every
 * puts (avant_out_check); before, the generated code called
 * printf, whose failed write was found only at the next check. Int32
 * and UInt32 arrive widened by their sign or by zero; puts_u passes the
 * bits as an unsigned number at their own width. */
void avant_puts_u64(uint64_t v) {
  char buf[24];
  buf[sizeof(buf) - 1] = '\n';
  char *o = avant_fmt_u64(buf + sizeof(buf) - 1, v);
  fwrite(o, 1, (size_t)(buf + sizeof(buf) - o), stdout);
  avant_out_check();
}

void avant_puts_i64(int64_t v) {
  char buf[24];
  buf[sizeof(buf) - 1] = '\n';
  char *o = int_text(buf + sizeof(buf) - 1, v);
  fwrite(o, 1, (size_t)(buf + sizeof(buf) - o), stdout);
  avant_out_check();
}

void avant_puts_bool(int32_t v) {
  fputs(v ? "true\n" : "false\n", stdout);
  avant_out_check();
}

char *avant_str_from_bool(int32_t n) {
  return dup_bytes(n ? "true" : "false", n ? 4 : 5);
}

/* gc-lint: holds nothing (its input is a C byte; the result is pinned before any other call). */
char *avant_str_from_byte(avant_int b) {
  if ((uint64_t)(int64_t)b > 255u) {
    avant_trap_byte(b);
  }
  unsigned idx = (unsigned)b;
  if (g_byte_str[idx]) {
    return g_byte_str[idx];
  }
  char c = (char)idx;
  char *p = dup_bytes(&c, 1);
  avant_pin(p);
  g_byte_str[idx] = p;
  return p;
}

char *avant_str_cat_int(const char *prefix, avant_int n) {
  char buf[24];
  char *p;
  char *o = int_text(buf + sizeof(buf), n);
  size_t nd = (size_t)(buf + sizeof(buf) - o);
  if (!prefix) {
    prefix = AVANT_STR_EMPTY;
  }
  size_t np = avant_str_n(prefix);
  /* RC1: the prefix is a handle across the allocation (buf is C
   * memory). */
  /* The inline allocation when it serves (it cannot collect), else
   * the one that may, with prefix a handle across it. */
  p = avant_str_alloc_try((uint64_t)(np + nd));
  if (!p) {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &prefix);
    p = avant_str_alloc((uint64_t)(np + nd));
  }
  if (np) {
    copy_short(p, avant_str_bytes(prefix, &np), np);
  }
  copy_short(p + np, o, nd);
  p[np + nd] = 0;
  avant_str_tag(p);
  return p;
}

/* Exact fast float parse of the strict shape [+-]digits[.digits][(e|E)
 * [+-]digits] (both digit runs of the mantissa non-empty
 * when present): 1 with the value when the mantissa fits in 53 bits with
 * at most 15 significant digits and the decimal exponent is within 22 (the
 * true value is then mantissa * 10^e with both operands exactly
 * representable, so one IEEE multiply or divide rounds it exactly as the
 * correctly rounded strtod does); -1 when the text is not of the shape; 0
 * when it may be but needs strtod. */
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
  if (i >= len || p[i] < '0' || p[i] > '9') {
    return -1;
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
    if (i >= len || p[i] < '0' || p[i] > '9') {
      return -1;
    }
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
      return -1;
    }
    for (; j < len; j++) {
      if (p[j] < '0' || p[j] > '9') {
        return -1;
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

/* Strict number parsing: the whole text
 * is the number or the result is "absent" (*ok = 0). An integer is an
 * optional sign and digits of its base (2..36, letters either case; no
 * prefix, no space, no underscore), within the target type's range; a
 * UInt64 takes no '-'. A float is an optional sign, decimal digits, an
 * optional fraction (a point and digits) and an optional exponent (e or
 * E, an optional sign, digits) -- Avant's literal with a sign -- whose
 * value is finite. */
static void trap_base(avant_int base) {
  char msg[64];
  snprintf(msg, sizeof msg, "base %lld out of range (2..36)", (long long)base);
  avant_trap_text(msg);
}

/* The magnitude of p[0..n) in base, at most limit; 0 when it is not one. */
static int parse_mag(const char *p, size_t n, int32_t base, uint64_t limit, uint64_t *out) {
  uint64_t v = 0;
  size_t i;
  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'z') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      d = c - 'A' + 10;
    } else {
      return 0;
    }
    if (d >= base) {
      return 0;
    }
    if (v > (limit - (uint64_t)d) / (uint64_t)base) {
      return 0;
    }
    v = v * (uint64_t)base + (uint64_t)d;
  }
  *out = v;
  return 1;
}

/* A signed integer of p[0..n) within [-(max + 1), max]. */
static int64_t parse_signed(const char *p, size_t n, avant_int base, uint64_t max, int32_t *ok) {
  int neg = 0;
  uint64_t m = 0;
  if (base < 2 || base > 36) {
    trap_base(base);
  }
  *ok = 0;
  if (n > 0 && (p[0] == '-' || p[0] == '+')) {
    neg = p[0] == '-';
    p++;
    n--;
  }
  if (!parse_mag(p, n, (int32_t)base, neg ? max + 1 : max, &m)) {
    return 0;
  }
  *ok = 1;
  return neg ? (int64_t)(0 - m) : (int64_t)m;
}

int32_t avant_str_parse_i32(const char *s, avant_int base, int32_t *ok) {
  size_t n;
  const char *p = avant_str_bytes(s, &n);
  return (int32_t)parse_signed(p, n, base, 2147483647ull, ok);
}

int64_t avant_str_parse_i64(const char *s, avant_int base, int32_t *ok) {
  size_t n;
  const char *p = avant_str_bytes(s, &n);
  return parse_signed(p, n, base, 9223372036854775807ull, ok);
}

uint64_t avant_str_parse_u64(const char *s, avant_int base, int32_t *ok) {
  size_t n;
  const char *p = avant_str_bytes(s, &n);
  uint64_t m = 0;
  if (base < 2 || base > 36) {
    trap_base(base);
  }
  *ok = 0;
  if (n > 0 && p[0] == '+') {
    p++;
    n--;
  }
  if (!parse_mag(p, n, (int32_t)base, UINT64_MAX, &m)) {
    return 0;
  }
  *ok = 1;
  return m;
}

static int digits_at(const char *p, size_t n, size_t *i) {
  size_t start = *i;
  while (*i < n && p[*i] >= '0' && p[*i] <= '9') {
    (*i)++;
  }
  return *i > start;
}

static int float_syntax(const char *p, size_t n) {
  size_t i = 0;
  if (i < n && (p[i] == '-' || p[i] == '+')) {
    i++;
  }
  if (!digits_at(p, n, &i)) {
    return 0;
  }
  if (i < n && p[i] == '.') {
    i++;
    if (!digits_at(p, n, &i)) {
      return 0;
    }
  }
  if (i < n && (p[i] == 'e' || p[i] == 'E')) {
    i++;
    if (i < n && (p[i] == '-' || p[i] == '+')) {
      i++;
    }
    if (!digits_at(p, n, &i)) {
      return 0;
    }
  }
  return i == n;
}

static double parse_float(const char *p, size_t n, int32_t *ok) {
  double v = 0.0;
  *ok = 0;
  int fast = fast_f64(p, n, &v);
  if (fast < 0) {
    return 0.0;
  }
  if (fast == 0) {
    if (!float_syntax(p, n)) {
      return 0.0;
    }
    char buf[128];
    char *tmp = buf;
    if (n >= sizeof(buf)) {
      tmp = (char *)malloc(n + 1);
      if (!tmp) {
        return 0.0;
      }
    }
    memcpy(tmp, p, n);
    tmp[n] = 0;
    v = strtod(tmp, NULL);
    if (tmp != buf) {
      free(tmp);
    }
  }
  if (isinf(v)) {
    return 0.0;
  }
  *ok = 1;
  return v;
}

/* A parsed float is never NaN (the text "nan" is not a number here), so
 * the two float parsers return NaN for "absent": the caller tests v == v,
 * with no flag through memory. */
double avant_str_parse_f64(const char *s) {
  int32_t ok = 0;
  size_t n;
  const char *p = avant_str_bytes(s, &n);
  double v = parse_float(p, n, &ok);
  return ok ? v : NAN;
}

double avant_str_parse_f64_slice(const char *s, avant_int start, avant_int stop) {
  size_t i = 0;
  size_t len = 0;
  int32_t ok = 0;
  size_t n;
  const char *p = avant_str_bytes(s, &n);
  check_range(n, start, stop, &i, &len);
  double v = parse_float(p + i, len, &ok);
  return ok ? v : NAN;
}

char *avant_str_fmt_float(double v, avant_int prec) {
  /* 400: the longest %.64f (see avant_buf_push_fmt_f). */
  char buf[400];
  /* 0..64 (the longest text, a sign, 309 digits, the point and 64,
   * fits); outside traps (it was clamped to 0..16). */
  if (prec < 0 || prec > 64) {
    avant_trap_precision(prec);
  }
  int k = avant_fmt_fixed(buf, v, (int)prec);
  if (k < 0) {
    k = snprintf(buf, sizeof(buf), "%.*f", (int)prec, v);
  }
  if (k < 0) {
    k = 0;
  }
  return dup_bytes(buf, (size_t)k);
}

int32_t avant_str_eq(const char *a, const char *b) {
  size_t na;
  size_t nb;
  a = avant_str_bytes(a, &na);
  b = avant_str_bytes(b, &nb);
  if (na != nb) {
    return 0;
  }
  return memcmp(a, b, na) == 0 ? 1 : 0;
}

/* String order: byte by byte, then the shorter
 * first, as sort orders Strings (avant_array.c str_cmp_raw); -1, 0 or 1. */
int32_t avant_str_cmp(const char *a, const char *b) {
  size_t na;
  size_t nb;
  a = avant_str_bytes(a, &na);
  b = avant_str_bytes(b, &nb);
  size_t n = na < nb ? na : nb;
  int c = n ? memcmp(a, b, n) : 0;
  if (c != 0) {
    return c < 0 ? -1 : 1;
  }
  return na < nb ? -1 : (na > nb ? 1 : 0);
}

avant_int avant_str_size(const char *s) {
  return (avant_int)avant_str_n(s);
}

avant_int avant_str_byte(const char *s, avant_int i) {
  size_t n;
  s = avant_str_bytes(s, &n);
  if (i < 0 || (size_t)i >= n) {
    return 0;
  }
  return (avant_int)(unsigned char)s[i];
}

static int is_ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* mode 0 = both ends, 1 = left, 2 = right. */
char *avant_str_strip(const char *s, int32_t mode) {
  size_t i = 0;
  size_t len;
  const char *d;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  d = avant_str_bytes(s, &len);
  if (mode == 0 || mode == 1) {
    while (i < len && is_ascii_space((unsigned char)d[i])) {
      i++;
    }
  }
  if (mode == 0 || mode == 2) {
    while (len > i && is_ascii_space((unsigned char)d[len - 1])) {
      len--;
    }
  }
  if (len <= i) {
    return empty_str();
  }
  /* As in avant_str_slice: the string and an offset (RC2); the whole
   * string comes back as it is. */
  return str_part(s, i, len - i);
}

/* An array of strings built in place (RC1, avant_gc.h):
 * str_array_new allocates the array and a buffer of N slots, size 0; the
 * builder keeps the array in a handle and str_array_put stores each fresh
 * string into it with a barrier (RC3) before the next allocation. The
 * strings used to wait in a malloc'ed items[] the collector could not
 * see, under one defer region. */
static void *str_array_new(int32_t n) {
  AvantArray *a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (n > 0) {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &a);
    char **buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
    avant_barrier(a);
    a->buf = buf;
    a->cap = n;
  }
  return a;
}

static void str_array_put(AvantArray *a, char *p) {
  char **slot = &((char **)a->buf)[a->size++];
  /* The barrier before the store (RC5). */
  avant_barrier_slot_young(a->buf, slot);
  *slot = p;
}

/* Keeps empty fields. Empty separator traps. At most LIMIT fields (the
 * last holds the rest of the text, separators and all); a LIMIT below 1
 * means no limit. RC1: the text, the separator and the array are handles
 * across each piece's allocation. */
static void *split_limit(const char *s, const char *sep, int64_t limit) {
  size_t n;
  size_t m;
  size_t i;
  size_t start;
  int32_t count = 1;
  AvantArray *a = NULL;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (!sep) {
    sep = AVANT_STR_EMPTY;
  }
  const char *d = avant_str_bytes(s, &n);
  const char *e = avant_str_bytes(sep, &m);
  if (m == 0) {
    avant_trap_text("split separator is empty");
  }
  for (i = 0; i + m <= n && (limit < 1 || count < limit); i++) {
    if (memcmp(d + i, e, m) == 0) {
      count++;
      i += m - 1;
    }
  }
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &sep);
  AVANT_GC_ROOT(2, &a);
  a = (AvantArray *)str_array_new(count);
  start = 0;
  i = 0;
  /* RC2: both texts' bytes are formed again after each allocation. */
  d = avant_str_bytes(s, &n);
  e = avant_str_bytes(sep, &m);
  while (i + m <= n && a->size + 1 < count) {
    if (memcmp(d + i, e, m) == 0) {
      char *p = str_part(s, start, i - start);
      str_array_put(a, p);
      d = avant_str_bytes(s, &n);
      e = avant_str_bytes(sep, &m);
      i += m;
      start = i;
    } else {
      i++;
    }
  }
  {
    char *p = str_part(s, start, n - start);
    str_array_put(a, p);
  }
  return a;
}

/* gc-lint: holds nothing (split_limit is its only statement). */
void *avant_str_split(const char *s, const char *sep) {
  return split_limit(s, sep, 0);
}

/* s.split(sep, limit): at most LIMIT fields, the last the rest of the
 * text; a LIMIT below 1 traps. */
/* gc-lint: holds nothing (split_limit is its last statement). */
void *avant_str_split_n(const char *s, const char *sep, avant_int limit) {
  if (limit < 1) {
    char msg[64];
    snprintf(msg, sizeof msg, "split limit %lld is less than 1", (long long)limit);
    avant_trap_text(msg);
  }
  return split_limit(s, sep, limit);
}

/* Splits on runs of ASCII whitespace and drops empty fields. */
void *avant_str_fields(const char *s) {
  size_t n;
  size_t i = 0;
  int32_t count = 0;
  AvantArray *a = NULL;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  const char *d = avant_str_bytes(s, &n);
  while (i < n) {
    while (i < n && is_ascii_space((unsigned char)d[i])) {
      i++;
    }
    if (i >= n) {
      break;
    }
    count++;
    while (i < n && !is_ascii_space((unsigned char)d[i])) {
      i++;
    }
  }
  /* RC1: the text and the array are handles (as avant_str_split). */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArray *)str_array_new(count);
  i = 0;
  while (i < n && a->size < count) {
    size_t st;
    /* RC2: the bytes are formed again after each allocation. */
    d = avant_str_bytes(s, &n);
    while (i < n && is_ascii_space((unsigned char)d[i])) {
      i++;
    }
    if (i >= n) {
      break;
    }
    st = i;
    while (i < n && !is_ascii_space((unsigned char)d[i])) {
      i++;
    }
    char *p = str_part(s, st, i - st);
    str_array_put(a, p);
  }
  return a;
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
    s = AVANT_STR_EMPTY;
  }
  const char *d = avant_str_bytes(s, &n);
  if (!utf8_valid((const unsigned char *)d, n)) {
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

/* String.chars and Bytes.chars: one String per codepoint; invalid UTF-8
 * traps on both (String.chars used to split a bad byte
 * into a U+FFFD piece while Bytes.chars trapped; one rule now). */
static void *chars_build(const char *s, const char *type) {
  size_t n;
  size_t i = 0;
  int32_t count = 0;
  AvantArray *a = NULL;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  const char *d = avant_str_bytes(s, &n);
  if (!utf8_valid((const unsigned char *)d, n)) {
    char msg[64];
    snprintf(msg, sizeof msg, "%s.chars needs valid UTF-8", type);
    avant_trap_text(msg);
  }
  while (i < n) {
    int valid = 0;
    size_t len = utf8_char_len((const unsigned char *)d, n, i, &valid);
    (void)valid;
    count++;
    i += len;
  }
  /* RC1: the text and the array are handles (as avant_str_split). */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArray *)str_array_new(count);
  i = 0;
  while (i < n && a->size < count) {
    int valid = 0;
    /* RC2: the bytes are formed again after each allocation. */
    const unsigned char *b = (const unsigned char *)avant_str_bytes(s, &n);
    size_t len = utf8_char_len(b, n, i, &valid);
    char *p = valid ? dup_bytes_at(s, i, len) : dup_bytes("\xef\xbf\xbd", 3);
    str_array_put(a, p);
    i += len;
  }
  return a;
}

void *avant_str_chars(const char *s) {
  return chars_build(s, "String");
}

void *avant_bytes_chars(const char *s) {
  return chars_build(s, "Bytes");
}

/* The code point at S[I], a valid UTF-8 sequence of LEN bytes
 * (utf8_char_len). */
static avant_int utf8_decode(const unsigned char *s, size_t i, size_t len) {
  uint32_t cp;
  size_t k;
  if (len == 1) {
    return (avant_int)s[i];
  }
  cp = (uint32_t)s[i] & (len == 2 ? 0x1fu : len == 3 ? 0x0fu : 0x07u);
  for (k = 1; k < len; k++) {
    cp = (cp << 6) | (uint32_t)(s[i + k] & 0x3fu);
  }
  return (avant_int)cp;
}

/* String.code_points and Bytes.code_points: each code point's number, in
 * order, as an Array(Int); invalid UTF-8 traps as chars does. RC1: the
 * text and the array are handles across the allocations; the text's
 * bytes are formed again after them (RC2). */
static void *code_points_build(const char *s, const char *type) {
  size_t n;
  size_t i = 0;
  int32_t count = 0;
  int32_t k = 0;
  AvantArray *a = NULL;
  avant_int *buf = NULL;
  const unsigned char *d = (const unsigned char *)avant_str_bytes(s, &n);
  if (!utf8_valid(d, n)) {
    char msg[64];
    snprintf(msg, sizeof msg, "%s.code_points needs valid UTF-8", type);
    avant_trap_text(msg);
  }
  while (i < n) {
    int valid = 0;
    i += utf8_char_len(d, n, i, &valid);
    count++;
  }
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (count > 0) {
    buf = (avant_int *)avant_alloc((uint64_t)count * sizeof(avant_int), AVANT_TYPE_BYTES);
    avant_barrier(a);
    a->buf = buf;
    a->cap = count;
  }
  d = (const unsigned char *)avant_str_bytes(s, &n);
  i = 0;
  while (i < n && k < count) {
    int valid = 0;
    size_t len = utf8_char_len(d, n, i, &valid);
    buf[k++] = utf8_decode(d, i, len);
    i += len;
  }
  a->size = k;
  return a;
}

void *avant_str_code_points(const char *s) {
  return code_points_build(s, "String");
}

void *avant_bytes_code_points(const char *s) {
  return code_points_build(s, "Bytes");
}

/* String.from_code_point(n): the UTF-8 of one code point, 0 to 0x10FFFF
 * outside the surrogates 0xD800 to 0xDFFF; anything else traps. */
/* gc-lint: holds nothing (its input is an integer). */
char *avant_str_from_code_point(avant_int cp) {
  char b[4];
  size_t n;
  if (cp < 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    char msg[96];
    snprintf(msg, sizeof msg, "String.from_code_point: %lld is not a code point", (long long)cp);
    avant_trap_text(msg);
  }
  if (cp < 0x80) {
    b[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (char)(0xc0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3f));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (char)(0xe0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    b[2] = (char)(0x80 | (cp & 0x3f));
    n = 3;
  } else {
    b[0] = (char)(0xf0 | (cp >> 18));
    b[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    b[3] = (char)(0x80 | (cp & 0x3f));
    n = 4;
  }
  return dup_bytes(b, n);
}

/* String.from_c(p, n) and Bytes.from_c(p, n): a copy of the N bytes of C
 * memory at P, NULs kept. The generated code traps before the call on a
 * negative N and on a NULL P with N above 0, so P is read only when N > 0. */
/* gc-lint: holds nothing (P is C memory, outside the collected heap). */
char *avant_str_from_c(const void *p, avant_int n) {
  char *s = avant_str_alloc((uint64_t)n);
  if (n > 0) {
    memcpy(s, p, (size_t)n);
  }
  s[n] = 0;
  avant_str_tag(s);
  return s;
}

/* s.replace(from, to): every FROM in S, left to right and without
 * overlapping, replaced by TO; an empty FROM traps (as split's empty
 * separator does). No FROM: S itself. RC1: the three strings are handles
 * across the allocation; RC2: their bytes are formed after it. */
char *avant_str_replace(const char *s, const char *from, const char *to) {
  size_t n;
  size_t m;
  size_t k;
  size_t i = 0;
  size_t o = 0;
  uint64_t count = 0;
  const char *d;
  const char *f;
  const char *t;
  const char *at;
  char *p;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (!from) {
    from = AVANT_STR_EMPTY;
  }
  if (!to) {
    to = AVANT_STR_EMPTY;
  }
  d = avant_str_bytes(s, &n);
  f = avant_str_bytes(from, &m);
  avant_str_bytes(to, &k);
  if (m == 0) {
    avant_trap_text("replace of an empty pattern");
  }
  while (i + m <= n && (at = (const char *)memmem(d + i, n - i, f, m)) != NULL) {
    count++;
    i = (size_t)(at - d) + m;
  }
  if (count == 0) {
    return (char *)s;
  }
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &from);
  AVANT_GC_ROOT(2, &to);
  p = avant_str_alloc((uint64_t)n - count * (uint64_t)m + count * (uint64_t)k);
  d = avant_str_bytes(s, &n);
  f = avant_str_bytes(from, &m);
  t = avant_str_bytes(to, &k);
  i = 0;
  while (i + m <= n && (at = (const char *)memmem(d + i, n - i, f, m)) != NULL) {
    size_t j = (size_t)(at - d);
    memcpy(p + o, d + i, j - i);
    o += j - i;
    memcpy(p + o, t, k);
    o += k;
    i = j + m;
  }
  memcpy(p + o, d + i, n - i);
  o += n - i;
  p[o] = 0;
  avant_str_tag(p);
  return p;
}

/* s.repeat(n): N copies of S one after another; a negative N traps. RC1:
 * S is a handle across the allocation. */
char *avant_str_repeat(const char *s, avant_int count) {
  size_t n;
  uint64_t total;
  size_t i;
  const char *d;
  char *p;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (count < 0) {
    avant_trap_negative(count, 2);
  }
  avant_str_bytes(s, &n);
  if (count == 0 || n == 0) {
    return empty_str();
  }
  if (count == 1) {
    return (char *)s;
  }
  if (__builtin_mul_overflow((uint64_t)n, (uint64_t)count, &total)) {
    total = UINT64_MAX;
  }
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  p = avant_str_alloc(total);
  d = avant_str_bytes(s, &n);
  for (i = 0; i < (size_t)count; i++) {
    memcpy(p + i * n, d, n);
  }
  p[total] = 0;
  avant_str_tag(p);
  return p;
}

/* s.pad_left(width, fill) (LEFT 1) and s.pad_right(width, fill) (LEFT
 * 0): S with the byte FILL added before or after it up to WIDTH bytes; S
 * itself when it has WIDTH bytes or more. A FILL outside 0..255 traps.
 * RC1: S is a handle across the allocation. */
char *avant_str_pad(const char *s, avant_int width, avant_int fill, int32_t left) {
  size_t n;
  size_t extra;
  const char *d;
  char *p;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if ((uint64_t)fill > 255u) {
    avant_trap_byte(fill);
  }
  avant_str_bytes(s, &n);
  if (width <= 0 || (uint64_t)width <= (uint64_t)n) {
    return (char *)s;
  }
  extra = (size_t)width - n;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  p = avant_str_alloc((uint64_t)width);
  d = avant_str_bytes(s, &n);
  if (left) {
    memset(p, (unsigned char)fill, extra);
    memcpy(p + extra, d, n);
  } else {
    memcpy(p, d, n);
    memset(p + n, (unsigned char)fill, extra);
  }
  p[width] = 0;
  avant_str_tag(p);
  return p;
}

/* s.to_ascii_lower() (UPPER 0) and s.to_ascii_upper() (UPPER 1): S with
 * each ASCII letter changed to the other case (A-Z to a-z, or a-z to
 * A-Z) and every other byte kept, so a UTF-8 text stays valid and keeps
 * its size; S itself when no byte changes. RC1: S is a handle across the
 * allocation. */
char *avant_str_ascii_case(const char *s, int32_t upper) {
  size_t n;
  size_t i;
  const char *d;
  char *p;
  unsigned char from = upper ? 'a' : 'A';
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  d = avant_str_bytes(s, &n);
  for (i = 0; i < n; i++) {
    if ((unsigned char)((unsigned char)d[i] - from) < 26u) {
      break;
    }
  }
  if (i == n) {
    return (char *)s;
  }
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  p = avant_str_alloc((uint64_t)n);
  d = avant_str_bytes(s, &n);
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)d[i];
    p[i] = (char)((unsigned char)(c - from) < 26u ? c ^ 0x20 : c);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* s.delete_prefix(p) (SUFFIX 0) and s.delete_suffix(p) (SUFFIX 1): S
 * without P at its start (or its end), a view or a copy as a slice is;
 * S itself when it does not start (or end) with P, or P is empty. */
/* gc-lint: holds nothing (str_part is its only allocation and its last call). */
char *avant_str_delete_affix(const char *s, const char *pat, int32_t suffix) {
  size_t n;
  size_t m;
  const char *d;
  const char *e;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (!pat) {
    pat = AVANT_STR_EMPTY;
  }
  d = avant_str_bytes(s, &n);
  e = avant_str_bytes(pat, &m);
  if (m == 0 || m > n) {
    return (char *)s;
  }
  if (suffix) {
    if (memcmp(d + n - m, e, m) != 0) {
      return (char *)s;
    }
    return str_part(s, 0, n - m);
  }
  if (memcmp(d, e, m) != 0) {
    return (char *)s;
  }
  return str_part(s, m, n - m);
}

/* s.lines(): the lines of S, each ended by "\n" or "\r\n" (the ending
 * not kept); a last line without an ending is a line, and a final ending
 * adds no empty line: "a\nb\n" and "a\r\nb" are ["a", "b"], "" is [],
 * "\n" is [""]. A "\r" alone ends no line. RC1: the text and the array
 * are handles across each line's allocation; RC2: the bytes are formed
 * again after it. */
void *avant_str_lines(const char *s) {
  size_t n;
  size_t start = 0;
  int32_t count = 0;
  const char *d;
  const char *nl;
  AvantArray *a = NULL;
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  d = avant_str_bytes(s, &n);
  while (start < n) {
    nl = (const char *)memchr(d + start, '\n', n - start);
    count++;
    if (!nl) {
      break;
    }
    start = (size_t)(nl - d) + 1;
  }
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArray *)str_array_new(count);
  start = 0;
  d = avant_str_bytes(s, &n);
  while (start < n) {
    size_t stop;
    size_t next;
    nl = (const char *)memchr(d + start, '\n', n - start);
    if (nl) {
      stop = (size_t)(nl - d);
      next = stop + 1;
      if (stop > start && d[stop - 1] == '\r') {
        stop--;
      }
    } else {
      stop = n;
      next = n;
    }
    {
      char *p = str_part(s, start, stop - start);
      str_array_put(a, p);
    }
    d = avant_str_bytes(s, &n);
    start = next;
  }
  return a;
}

/* xs.join(sep) of an Array(String): the elements in order with SEP
 * between them; an empty array is "", one element is that element. RC1:
 * the array and the separator are handles across the allocation; RC2:
 * every element's bytes are formed after it. */
char *avant_str_join(void *arr, const char *sep) {
  AvantArray *a = (AvantArray *)arr;
  size_t m;
  size_t n;
  uint64_t total = 0;
  int32_t i;
  size_t o = 0;
  const char *e;
  char *p;
  if (!sep) {
    sep = AVANT_STR_EMPTY;
  }
  if (!a || a->size <= 0) {
    return empty_str();
  }
  if (a->size == 1) {
    char *only = ((char **)a->buf)[0];
    return only ? only : empty_str();
  }
  avant_str_bytes(sep, &m);
  for (i = 0; i < a->size; i++) {
    avant_str_bytes(((char **)a->buf)[i], &n);
    total += (uint64_t)n;
  }
  total += (uint64_t)m * (uint64_t)(a->size - 1);
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &a);
  AVANT_GC_ROOT(1, &sep);
  p = avant_str_alloc(total);
  e = avant_str_bytes(sep, &m);
  for (i = 0; i < a->size; i++) {
    const char *d = avant_str_bytes(((char **)a->buf)[i], &n);
    if (i > 0) {
      memcpy(p + o, e, m);
      o += m;
    }
    memcpy(p + o, d, n);
    o += n;
  }
  p[o] = 0;
  avant_str_tag(p);
  return p;
}
