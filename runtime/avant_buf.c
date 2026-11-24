#define _GNU_SOURCE
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
 * allocated: every growth comes through this one. */
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
   * protect it too. */
  /* Large storage grows in place when the pages after it are free. */
  if (b->data && b->size > 0 && avant_gc_large_extend(b->data, (uint64_t)ncap, 1)) {
    b->cap = ncap;
    return;
  }
  uint8_t *ndata = (uint8_t *)avant_gc_verify_paged(b->data, (uint64_t)ncap, 0);
  if (!ndata) {
    /* Not zeroed: the bytes below the size are copied in now, and the
     * capacity past it is written before the size covers it (a push, or
     * a C writer that fills it and then sets the size). */
    ndata = (uint8_t *)avant_alloc_bytes_room((uint64_t)ncap);
  }
  uint8_t *odata = b->data;
  if (odata && b->size > 0) {
    memcpy(ndata, odata, (size_t)b->size);
  }
  avant_barrier(b);
  b->data = ndata;
  b->cap = ncap;
  /* The Buf no longer names its old storage: a large one goes back now
   * unless a root still names it; under
   * AVANT_GC_VERIFY=1 it stays PROT_NONE until the next collection, so a
   * C pointer from avant_buf_bytes kept across this growth faults and is
   * reported. */
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

/* A Buf of size 0 with room for CAP bytes. The room is not zeroed (a
 * large one is left as its pages were): nothing reads a Buf's bytes past
 * its size, and every way of raising the size writes the bytes it
 * covers first (a push, `Buf.new(n, byte)`, a C writer that fills the
 * capacity and then calls avant_buf_set_size). */
void *avant_buf_new(avant_int cap) {
  AvantBuf *b = (AvantBuf *)avant_alloc(sizeof(AvantBuf), AVANT_TYPE_BUF);
  if (cap > 0) {
    avant_size_check((uint64_t)cap, AVANT_SIZE_BUF);
    /* RC1: the fresh Buf is a handle across its data's allocation. */
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &b);
    uint8_t *data = (uint8_t *)avant_alloc_bytes_room((uint64_t)cap);
    avant_barrier(b);
    b->data = data;
    b->cap = (int32_t)cap;
  }
  return b;
}

/* `Buf.new(n, byte)`: a Buf of N bytes, each BYTE (the generated code
 * checks that N is not negative and BYTE is a byte, naming the call). Its
 * storage is written once: zero bytes come from a zeroed allocation
 * (pages fresh from the kernel are zero already and are not written),
 * any other byte from an unzeroed one that the fill writes whole. */
void *avant_buf_new_filled(avant_int n, avant_int byte) {
  AvantBuf *b = (AvantBuf *)avant_alloc(sizeof(AvantBuf), AVANT_TYPE_BUF);
  if (n > 0) {
    avant_size_check((uint64_t)n, AVANT_SIZE_BUF);
    /* RC1: the fresh Buf is a handle across its data's allocation. */
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &b);
    uint8_t *data;
    if (byte == 0) {
      data = (uint8_t *)avant_alloc((uint64_t)n, AVANT_TYPE_BYTES);
    } else {
      data = (uint8_t *)avant_alloc_bytes_room((uint64_t)n);
      memset(data, (int)(uint8_t)byte, (size_t)n);
    }
    avant_barrier(b);
    b->data = data;
    b->size = (int32_t)n;
    b->cap = (int32_t)n;
  }
  return b;
}

/* The slow path of the avant_buf_push_* rows and avant_buf_append (RC1,
 * avant_gc.h, it was a named safepoint). p may be derived
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
void avant_buf_push_byte(void *buf, avant_int c) {
  AvantBuf *b = (AvantBuf *)buf;
  if ((uint64_t)(int64_t)c > 255u) {
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
  s = avant_str_bytes(s, &n);
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
void avant_buf_push_slice(void *buf, const char *s, avant_int start, avant_int stop) {
  AvantBuf *b = (AvantBuf *)buf;
  size_t n;
  int32_t i;
  int32_t end;
  int32_t len;
  s = s ? avant_str_bytes(s, &n) : NULL;
  if (!s) {
    n = 0;
  }
  /* 0 <= start <= stop <= size, else a trap. */
  if (start < 0 || stop < start || (size_t)stop > n) {
    avant_trap_slice(start, stop, (avant_int)n);
  }
  if (!b || !s) {
    return;
  }
  /* Within the string, so within the ceiling. */
  i = (int32_t)start;
  end = (int32_t)stop;
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
void avant_buf_push_fmt_f(void *buf, double v, avant_int prec) {
  /* 400: the longest %.64f (a sign, 309 digits, the point, 64); 64 let
   * snprintf truncate a value of 1e47 or more and the append read past
   * the buffer. */
  char tmp[400];
  int k;
  /* 0..64 (the longest text, a sign, 309 digits, the point and 64,
   * fits); outside traps (it was clamped to 0..16). */
  if (prec < 0 || prec > 64) {
    avant_trap_precision(prec);
  }
  k = avant_fmt_fixed(tmp, v, (int)prec);
  if (k < 0) {
    k = snprintf(tmp, sizeof(tmp), "%.*f", (int)prec, v);
  }
  if (k < 0) {
    return;
  }
  avant_buf_append(buf, tmp, k);
}

/* gc-lint: holds nothing (tmp is C memory; the append is the last statement). */
void avant_buf_push_float(void *buf, double v) {
  /* The Float's one text (avant_str_from_float's), with no String made. */
  char tmp[48];
  int k = avant_fmt_float_text(tmp, v);
  avant_buf_append(buf, tmp, k);
}

/* gc-lint: holds nothing (tmp is C memory; the append is the last statement). */
void avant_buf_push_fmt_i(void *buf, avant_int n) {
  char tmp[24];
  int k = avant_fmt_int_text(tmp, n);
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

/* The string of a Buf that nothing uses again: the
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
      /* The Buf's reference to its storage is removed (RC5). */
      avant_gc_overwrite(b);
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
    /* RC2: the bytes are formed after the allocation. */
    avant_buf_append(buf, avant_str_bytes(s, &n), (int32_t)n);
  }
  return buf;
}

/* buf[start..stop], buf.slice(start, stop): a new Buf holding a copy of
 * those bytes; traps unless 0 <= start <= stop <= size. */
void *avant_buf_slice(void *buf, avant_int start, avant_int stop) {
  AvantBuf *b = (AvantBuf *)buf;
  avant_int n = b ? b->size : 0;
  if (start < 0 || stop < start || stop > n) {
    avant_trap_slice(start, stop, n);
  }
  AvantBuf *r = NULL;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &b);
  AVANT_GC_ROOT(1, &r);
  r = (AvantBuf *)avant_buf_new(stop > start ? stop - start : 16);
  if (stop > start) {
    /* b->data is read after the allocation (RC2); r has the room. */
    memcpy(r->data, b->data + start, (size_t)(stop - start));
    r->size = (int32_t)(stop - start);
  }
  return r;
}

/* buf.push_str(other): appends other's bytes (other may be buf itself:
 * its size is read first, its storage after the growth). */
void avant_buf_push_buf(void *buf, void *other) {
  AvantBuf *b = (AvantBuf *)buf;
  AvantBuf *o = (AvantBuf *)other;
  if (!b || !o || o->size <= 0) {
    return;
  }
  int32_t n = o->size;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &b);
  AVANT_GC_ROOT(1, &o);
  buf_reserve(b, n);
  memcpy(b->data + b->size, o->data, (size_t)n);
  b->size += n;
}

avant_int avant_buf_size(void *buf) {
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
void avant_bytes_ensure(void *buf, avant_int n) {
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
void avant_buf_ensure(void *buf, avant_int n) {
  buf_ensure_to(buf, n);
}

void avant_buf_set_size(void *buf, avant_int n) {
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
  b->size = (int32_t)n;
}

void avant_buf_set_ident(void *buf, avant_int n) {
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
  b->size = (int32_t)n;
}

/* The elements of an Array(Int) (avant_int), when it
 * has at least n. */
static avant_int *buf_i32s(void *arr, avant_int n) {
  typedef struct {
    avant_int *buf;
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
                            avant_int n) {
  AvantBuf *b = (AvantBuf *)buf;
  avant_int *dst;
  avant_int *src;
  avant_int *ln;
  avant_int *ex;
  avant_int i;
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
    int64_t len = ln[i];
    int64_t d = dst[i];
    int64_t s = src[i];
    int64_t need;
    uint8_t *p;
    if (len < 0 || d < 0 || s < 0) {
      return;
    }
    /* In 64 bits: d + len + 1 can pass the ceiling. */
    need = d + len + 1;
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
                           avant_int n) {
  AvantBuf *dst = (AvantBuf *)dst_buf;
  AvantBuf *src = (AvantBuf *)src_buf;
  avant_int *doff;
  avant_int *soff;
  avant_int *ln;
  avant_int i;
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
    int64_t len = ln[i];
    int64_t d = doff[i];
    int64_t s = soff[i];
    int64_t need;
    if (len < 0 || d < 0 || s < 0) {
      return;
    }
    if (len == 0) {
      continue;
    }
    need = d + len;
    if (need > dst->cap) {
      buf_ensure_to(dst_buf, need);
      dst = (AvantBuf *)dst_buf;
      src = (AvantBuf *)src_buf;
      doff = buf_i32s(dst_offs, n);
      soff = buf_i32s(src_offs, n);
      ln = buf_i32s(lens, n);
    }
    if (!dst->data || !src->data || need > dst->cap || s + len > src->cap) {
      return;
    }
    memcpy(dst->data + d, src->data + s, (size_t)len);
  }
}

/* AVANT_GC_VERIFY=1: storage in a block (a small
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
    s = AVANT_STR_EMPTY;
  }
  size_t n;
  s = avant_str_bytes(s, &n);
  if (n == 0) {
    return 1;
  }
  if (!b || !b->data || (size_t)b->size < n) {
    return 0;
  }
  return memcmp(b->data, s, n) == 0 ? 1 : 0;
}

/* The bytes of a sequence for the search methods: a Buf when KIND is 1,
 * else a String or Bytes (flat or a view). Read in place; the searches
 * below never allocate, so the address stays valid while they run. */
static const char *seq_bytes(void *p, int32_t kind, size_t *n) {
  if (kind == 1) {
    AvantBuf *b = (AvantBuf *)p;
    if (!b || !b->data || b->size <= 0) {
      *n = 0;
      return "";
    }
    *n = (size_t)b->size;
    return (const char *)b->data;
  }
  return avant_str_bytes((const char *)p, n);
}

/* The last place of the NN bytes of ND in the HN bytes of H, or -1: a
 * backward scan, memcmp at each place whose first byte matches. */
static avant_int seq_last(const char *h, size_t hn, const char *nd, size_t nn) {
  size_t i;
  if (nn > hn) {
    return -1;
  }
  i = hn - nn + 1;
  while (i > 0) {
    i--;
    if (h[i] == nd[0] && memcmp(h + i, nd, nn) == 0) {
      return (avant_int)i;
    }
  }
  return -1;
}

/* The first place of the NN bytes of ND (NN of 2 or more) in the HN
 * bytes of H, or NULL. memchr finds each place that starts with ND's
 * first byte and memcmp tests it: little setup, so a walk over many
 * matches (index_of with a start, count) pays little per match, and a
 * rare first byte is skipped at memchr's speed. After 16 places that
 * start alike but differ (a common first byte), memmem takes the rest,
 * its setup paid once. */
static const char *seq_find_from(const char *h, size_t hn, const char *nd, size_t nn) {
  const char *p = h;
  const char *last;
  int misses = 0;
  if (nn > hn) {
    return NULL;
  }
  last = h + (hn - nn);
  while (p <= last && (p = (const char *)memchr(p, (unsigned char)nd[0], (size_t)(last - p) + 1)) != NULL) {
    if (memcmp(p, nd, nn) == 0) {
      return p;
    }
    p++;
    if (++misses == 16) {
      return (const char *)memmem(p, (size_t)(h + hn - p), nd, nn);
    }
  }
  return NULL;
}

/* One search of the N bytes of ND in the HN bytes of H. MODE: 0 the
 * first place at or after FROM (-1 when there is none; FROM outside
 * 0..HN traps as the slice h[from..] does), 1 the last place, 2 whether H
 * starts with ND, 3 whether it ends with it, 4 how many times ND occurs
 * without overlapping (an empty ND traps), 5 whether H and ND are equal
 * with ASCII letters compared without their case (A-Z as a-z, every
 * other byte as it is). An empty ND is found at FROM (0) and at the end
 * (1). */
static avant_int seq_search(const char *h, size_t hn, const char *nd, size_t nn, avant_int from, int32_t mode) {
  const char *at;
  avant_int count = 0;
  size_t i = 0;
  switch (mode) {
  case 0:
    if (from < 0 || (uint64_t)from > (uint64_t)hn) {
      avant_trap_slice(from, (avant_int)hn, (avant_int)hn);
    }
    if (nn == 0) {
      return from;
    }
    if (nn > hn - (size_t)from) {
      return -1;
    }
    if (nn == 1) {
      at = (const char *)memchr(h + from, (unsigned char)nd[0], hn - (size_t)from);
    } else {
      at = seq_find_from(h + from, hn - (size_t)from, nd, nn);
    }
    return at ? (avant_int)(at - h) : -1;
  case 1:
    if (nn == 0) {
      return (avant_int)hn;
    }
    return seq_last(h, hn, nd, nn);
  case 2:
    return nn <= hn && (nn == 0 || memcmp(h, nd, nn) == 0) ? 1 : 0;
  case 3:
    return nn <= hn && (nn == 0 || memcmp(h + hn - nn, nd, nn) == 0) ? 1 : 0;
  case 5:
    if (nn != hn) {
      return 0;
    }
    for (i = 0; i < hn; i++) {
      unsigned char a = (unsigned char)h[i];
      unsigned char b = (unsigned char)nd[i];
      if (a != b && ((a | 0x20) != (b | 0x20) || (unsigned char)((a | 0x20) - 'a') >= 26u)) {
        return 0;
      }
    }
    return 1;
  default:
    if (nn == 0) {
      avant_trap_text("count of an empty pattern");
    }
    while (i + nn <= hn) {
      if (nn == 1) {
        at = (const char *)memchr(h + i, (unsigned char)nd[0], hn - i);
      } else {
        at = seq_find_from(h + i, hn - i, nd, nn);
      }
      if (!at) {
        break;
      }
      count++;
      i = (size_t)(at - h) + nn;
    }
    return count;
  }
}

/* The search methods of String, Bytes and Buf (contains, index_of,
 * last_index_of, starts_with, ends_with, count, eq_ignore_ascii_case):
 * the receiver HAY and the pattern NEEDLE, each a Buf when its kind is
 * 1 and a String or Bytes otherwise, both read in place (a view's bytes are its base's). */
avant_int avant_seq_find(void *hay, int32_t hay_kind, void *needle, int32_t needle_kind, avant_int from, int32_t mode) {
  size_t hn;
  size_t nn;
  const char *h = seq_bytes(hay, hay_kind, &hn);
  const char *nd = seq_bytes(needle, needle_kind, &nn);
  return seq_search(h, hn, nd, nn, from, mode);
}

/* The same with one byte as the pattern (0..255, else a trap). */
avant_int avant_seq_find_byte(void *hay, int32_t hay_kind, avant_int byte, avant_int from, int32_t mode) {
  size_t hn;
  char c;
  const char *h = seq_bytes(hay, hay_kind, &hn);
  if ((uint64_t)byte > 255u) {
    avant_trap_byte(byte);
  }
  c = (char)(unsigned char)byte;
  return seq_search(h, hn, &c, 1, from, mode);
}
