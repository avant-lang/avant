#include "avant_rt.h"
#include "avant_gc.h"
#include "avant_fmt.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  int32_t size;
  int32_t cap;
} AvantBuf;

/* need is checked against the ceiling here, before anything is
 * allocated (Stage 29 W-INT1): every growth comes through this one. */
static void buf_grow(AvantBuf *b, int64_t need) {
  avant_size_check((uint64_t)need, AVANT_SIZE_BUF);
  int32_t ncap = b->cap ? b->cap : 16;
  while (ncap < need) {
    if (ncap > 1073741823) {
      ncap = (int32_t)need;
      break;
    }
    ncap *= 2;
  }
  /* RC1: the allocation may collect; b is a handle, b->data is read
   * after it (RC2). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &b);
  /* AVANT_GC_VERIFY=1: storage C has seen (avant_buf_bytes moved it to
   * its own pages) keeps its own pages, so the growth after this one can
   * protect it too (Stage 28 W18C, O145). */
  uint8_t *ndata = (uint8_t *)avant_gc_verify_paged(b->data, (uint64_t)ncap, 0);
  if (!ndata) {
    ndata = (uint8_t *)avant_alloc((uint64_t)ncap, AVANT_TYPE_BYTES);
  }
  uint8_t *odata = b->data;
  if (odata && b->size > 0) {
    memcpy(ndata, odata, (size_t)b->size);
  }
  avant_barrier(b);
  b->data = ndata;
  b->cap = ncap;
  /* The Buf no longer names its old storage: a large one goes back now
   * unless a root still names it (Stage 28 W18M, O109); under
   * AVANT_GC_VERIFY=1 it stays PROT_NONE until the next collection, so a
   * C pointer from avant_buf_bytes kept across this growth faults and is
   * reported (Stage 28 W18C, O145). */
  if (odata) {
    (void)avant_gc_large_drop(odata, 1);
  }
}

/* gc-lint: holds nothing (the growth is its last statement; buf_grow roots the Buf). */
static void buf_reserve(AvantBuf *b, int64_t extra) {
  int64_t need = (int64_t)b->size + extra;
  if (need > b->cap) {
    buf_grow(b, need);
  }
}

void *avant_buf_new(int32_t cap) {
  AvantBuf *b = (AvantBuf *)avant_alloc(sizeof(AvantBuf), AVANT_TYPE_BUF);
  if (cap > 0) {
    /* RC1: the fresh Buf is a handle across its data's allocation. */
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &b);
    uint8_t *data = (uint8_t *)avant_alloc((uint64_t)cap, AVANT_TYPE_BYTES);
    avant_barrier(b);
    b->data = data;
    b->cap = cap;
  }
  return b;
}

/* The slow path of the avant_buf_push_* rows and avant_buf_append (RC1,
 * avant_gc.h; Stage 28 W16a, it was a named safepoint). p may be derived
 * from a string (push_slice: s + i, an interior address, which its slot
 * relocates with its offset) or name a caller's stack buffer (push_byte,
 * push_fmt_*: no heap address, left alone); both b and p are handles
 * across the growth's allocation. */
static void buf_append_slow(AvantBuf *b, const void *p, int64_t n) {
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &b);
  AVANT_GC_ROOT(1, &p);
  buf_reserve(b, n);
  if (p && n > 0 && b->data) {
    memcpy(b->data + b->size, p, (size_t)n);
    b->size += (int32_t)n;
  }
}

/* gc-lint: holds nothing (the slow path is the last statement and roots b and p itself). */
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

/* gc-lint: holds nothing (the slow path is the last statement and roots b and p itself). */
void avant_buf_push_byte(void *buf, int32_t c) {
  AvantBuf *b = (AvantBuf *)buf;
  if ((uint32_t)c > 255u) {
    avant_trap_byte(c);
  }
  uint8_t byte = (uint8_t)c;
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

/* gc-lint: holds nothing (the slow path is the last statement and roots b and p itself). */
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
  buf_append_slow(b, s, (int64_t)n);
}

/* gc-lint: holds nothing (the slow path is the last statement and roots b and p itself). */
void avant_buf_push_slice(void *buf, const char *s, int32_t start, int32_t stop) {
  AvantBuf *b = (AvantBuf *)buf;
  size_t n;
  int32_t i;
  int32_t end;
  int32_t len;
  n = s ? avant_str_n(s) : 0;
  /* 0 <= start <= stop <= size, else a trap (Stage 29 W4b1, D295 R4). */
  if (start < 0 || stop < start || (size_t)stop > n) {
    avant_trap_slice(start, stop, (int32_t)n);
  }
  if (!b || !s) {
    return;
  }
  i = start;
  end = stop;
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

/* gc-lint: holds nothing (tmp is C memory; the append is the last statement). */
void avant_buf_push_fmt_f(void *buf, double v, int32_t prec) {
  /* 400: the longest %.64f (a sign, 309 digits, the point, 64); 64 let
   * snprintf truncate a value of 1e47 or more and the append read past
   * the buffer (W18S). */
  char tmp[400];
  int k;
  /* 0..64 (the longest text, a sign, 309 digits, the point and 64,
   * fits); outside traps (Stage 29 W4b1; it was clamped to 0..16). */
  if (prec < 0 || prec > 64) {
    avant_trap_precision(prec);
  }
  k = avant_fmt_fixed(tmp, v, prec);
  if (k < 0) {
    k = snprintf(tmp, sizeof(tmp), "%.*f", prec, v);
  }
  if (k < 0) {
    return;
  }
  avant_buf_append(buf, tmp, k);
}

/* gc-lint: holds nothing (tmp is C memory; the append is the last statement). */
void avant_buf_push_fmt_i(void *buf, int32_t n) {
  char tmp[32];
  int k = snprintf(tmp, sizeof(tmp), "%d", n);
  if (k < 0) {
    return;
  }
  avant_buf_append(buf, tmp, k);
}

char *avant_buf_to_str(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  int32_t n = 0;
  /* RC1/RC2: the Buf is a handle; its data is read after the allocation. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &b);
  if (b && b->data && b->size > 0) {
    n = b->size;
  }
  char *p = avant_str_alloc((uint64_t)n);
  if (n) {
    memcpy(p, b->data, (size_t)n);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* The string of a Buf that nothing uses again (Stage 28 W18M, O52): the
 * generated code calls this instead of avant_buf_to_str when the Buf was
 * made in the calling function, left it only as an argument to functions
 * that keep no reference to it, and is not used after the call
 * (compiler/mir/take.av). A large storage with room for the terminator
 * becomes the string: its bytes past the terminator go back to the heap
 * (avant_gc_large_trim) and the Buf is left empty, so the string costs no
 * copy and the storage's slack is not held twice. Anything else (a small
 * storage, one with no room, a string under 6 KiB) is avant_buf_to_str's
 * copy. */
char *avant_buf_take_str(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  /* RC1: the trim takes the world lock, whose wait lets another thread
   * collect: the Buf is a handle (its storage is large and never moves). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &b);
  if (b && b->data && b->size > 0 && b->size < b->cap) {
    uint8_t *p = b->data;
    int32_t n = b->size;
    if (avant_gc_large_trim(p, (uint64_t)n + 1)) {
      p[n] = 0;
      avant_str_tag(p);
      b->data = NULL;
      b->size = 0;
      b->cap = 0;
      return (char *)p;
    }
  }
  return avant_buf_to_str(b);
}

/* RC1: the text and the new Buf are handles across the allocations. */
void *avant_bytes_to_buf(const char *s) {
  size_t n = avant_str_n(s);
  void *buf = NULL;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_size_check(n, AVANT_SIZE_BUF);
  buf = avant_buf_new(n > 0 ? (int32_t)n : 16);
  if (n > 0) {
    avant_buf_append(buf, s, (int32_t)n);
  }
  return buf;
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

/* gc-lint: holds nothing (the growth is its only statement). */
void avant_bytes_ensure(void *buf, int32_t n) {
  avant_buf_ensure(buf, n);
}

/* gc-lint: holds nothing (the growth is its last statement; buf_grow roots the Buf). */
static void buf_ensure_to(void *buf, int64_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!b || n <= b->cap) {
    return;
  }
  buf_grow(b, n);
}

/* gc-lint: holds nothing (the growth is its only statement). */
void avant_buf_ensure(void *buf, int32_t n) {
  buf_ensure_to(buf, n);
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

void avant_buf_set_ident(void *buf, int32_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  int32_t i;
  if (!b || n <= 0) {
    return;
  }
  if (b->cap < n) {
    /* RC1: the growth may collect; the Buf is read again after it. */
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &buf);
    avant_buf_ensure(buf, n);
    b = (AvantBuf *)buf;
  }
  if (!b->data || b->cap < n) {
    return;
  }
  for (i = 0; i < n; i++) {
    b->data[i] = (uint8_t)i;
  }
  b->size = n;
}

static int32_t *buf_i32s(void *arr, int32_t n) {
  typedef struct {
    int32_t *buf;
    int32_t size;
    int32_t cap;
  } I32Arr;
  I32Arr *a = (I32Arr *)arr;
  if (!a || !a->buf || n < 0 || a->size < n) {
    return NULL;
  }
  return a->buf;
}

void avant_buf_fill_appends(void *buf, void *dst_offs, void *src_offs, void *lens, void *extras,
                            int32_t n) {
  AvantBuf *b = (AvantBuf *)buf;
  int32_t *dst;
  int32_t *src;
  int32_t *ln;
  int32_t *ex;
  int32_t i;
  if (!b || n <= 0) {
    return;
  }
  dst = buf_i32s(dst_offs, n);
  src = buf_i32s(src_offs, n);
  ln = buf_i32s(lens, n);
  ex = buf_i32s(extras, n);
  if (!dst || !src || !ln || !ex) {
    return;
  }
  /* RC1/RC2: the growth may collect; the Buf and the four arrays are
   * handles, and their data pointers are formed again after it. */
  AVANT_GC_SCOPE(5);
  AVANT_GC_ROOT(0, &buf);
  AVANT_GC_ROOT(1, &dst_offs);
  AVANT_GC_ROOT(2, &src_offs);
  AVANT_GC_ROOT(3, &lens);
  AVANT_GC_ROOT(4, &extras);
  for (i = 0; i < n; i++) {
    int32_t len = ln[i];
    int32_t d = dst[i];
    int32_t s = src[i];
    int64_t need;
    uint8_t *p;
    if (len < 0 || d < 0 || s < 0) {
      return;
    }
    /* In 64 bits: d + len + 1 can pass the ceiling (Stage 29 W-INT1). */
    need = (int64_t)d + (int64_t)len + 1;
    if (need > b->cap) {
      buf_ensure_to(buf, need);
      b = (AvantBuf *)buf;
      dst = buf_i32s(dst_offs, n);
      src = buf_i32s(src_offs, n);
      ln = buf_i32s(lens, n);
      ex = buf_i32s(extras, n);
    }
    p = b->data;
    if (!p || need > b->cap) {
      return;
    }
    if (len > 0) {
      memmove(p + d, p + s, (size_t)len);
    }
    p[d + len] = (uint8_t)ex[i];
  }
}

void avant_buf_copy_ranges(void *dst_buf, void *src_buf, void *dst_offs, void *src_offs, void *lens,
                           int32_t n) {
  AvantBuf *dst = (AvantBuf *)dst_buf;
  AvantBuf *src = (AvantBuf *)src_buf;
  int32_t *doff;
  int32_t *soff;
  int32_t *ln;
  int32_t i;
  if (!dst || !src || n <= 0) {
    return;
  }
  doff = buf_i32s(dst_offs, n);
  soff = buf_i32s(src_offs, n);
  ln = buf_i32s(lens, n);
  if (!doff || !soff || !ln) {
    return;
  }
  /* RC1/RC2, as in avant_buf_fill_appends. */
  AVANT_GC_SCOPE(5);
  AVANT_GC_ROOT(0, &dst_buf);
  AVANT_GC_ROOT(1, &src_buf);
  AVANT_GC_ROOT(2, &dst_offs);
  AVANT_GC_ROOT(3, &src_offs);
  AVANT_GC_ROOT(4, &lens);
  for (i = 0; i < n; i++) {
    int32_t len = ln[i];
    int32_t d = doff[i];
    int32_t s = soff[i];
    int64_t need;
    if (len < 0 || d < 0 || s < 0) {
      return;
    }
    if (len == 0) {
      continue;
    }
    need = (int64_t)d + (int64_t)len;
    if (need > dst->cap) {
      buf_ensure_to(dst_buf, need);
      dst = (AvantBuf *)dst_buf;
      src = (AvantBuf *)src_buf;
      doff = buf_i32s(dst_offs, n);
      soff = buf_i32s(src_offs, n);
      ln = buf_i32s(lens, n);
    }
    if (!dst->data || !src->data || need > dst->cap || (int64_t)s + (int64_t)len > src->cap) {
      return;
    }
    memcpy(dst->data + d, src->data + s, (size_t)len);
  }
}

/* AVANT_GC_VERIFY=1 (Stage 28 W18C, O145): storage in a block (a small
 * buffer) moves to its own pages before C sees it, so that a growth can
 * protect the storage it replaces whole and a C read through a pointer
 * kept across the growth faults and is reported. The move never collects
 * (avant_gc_verify_paged), so avant_buf_bytes stays a non-collecting call.
 * The storage stays where it is when it is pinned, when a root of this
 * thread names it (a generated-code slot holding it across this call,
 * which a move would leave behind), in a shared program, or when the
 * heap limit is near. */
static void buf_expose(AvantBuf *b) {
  uint8_t *ndata = (uint8_t *)avant_gc_verify_paged(b->data, (uint64_t)b->cap, 1);
  if (ndata) {
    memcpy(ndata, b->data, (size_t)b->cap);
    avant_barrier(b);
    b->data = ndata;
  }
}

/* gc-lint: holds nothing (buf_expose never collects: avant_gc_verify_paged takes no safepoint and runs no policy). */
uint8_t *avant_buf_bytes(void *buf) {
  AvantBuf *b = (AvantBuf *)buf;
  if (!b) {
    return NULL;
  }
  if (b->data) {
    buf_expose(b);
  }
  return b->data;
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
