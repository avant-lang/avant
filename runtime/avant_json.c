#include "avant_rt.h"
#include "avant_gc.h"
#include "yyjson.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* std.json backing (yyjson). A parsed document lives in one pool buffer
 * allocated in collector space (a bump allocator over it, below), a large object,
 * which never moves: the Avant Json value roots that buffer, interior
 * yyjson pointers stay valid,
 * and the collector reclaims the whole document when the last Json is
 * dropped. Accessors report a missing key or a wrong kind through the
 * thread-local status, in the zlib/base64 shape. */

/* 0 ok, 1 invalid JSON / missing or wrong kind, 2 allocation failure. */
static __thread int32_t g_json_status;
/* Why the last parse on this thread failed (Stage 29 W4b2, D295 R11):
 * yyjson's message (a static string) and the byte offset it names. */
static __thread const char *g_json_err_msg;
static __thread int64_t g_json_err_pos;
static __thread char *g_json_pool;
static __thread int g_json_pool_rooted;

static void set_status(int32_t code) {
  g_json_status = code;
}

int32_t avant_json_take_status(void) {
  int32_t code = g_json_status;
  g_json_status = 0;
  return code;
}

/* The pool buffer of the most recent successful parse, handed to the
 * Avant side once so the Json value can root it. */
char *avant_json_take_pool(void) {
  char *pool = g_json_pool;
  g_json_pool = NULL;
  return pool ? pool : avant_str_empty();
}

/* The document's allocator (Stage 28 W18M, O59/O60): a bump over the
 * pool. yyjson allocates the text's copy and then the values, which it
 * grows by realloc at the end, so a reallocation of the last block grows
 * it in place and a free of the last block takes the bump back. After
 * the parse the document is the pool's first `top` bytes and the rest
 * goes back to the heap at once (avant_gc_large_trim): the pool is sized
 * for the worst case (24 bytes per text byte, 96 on the retry), and its
 * unused tail used to stay a live young object as long as the Json did,
 * filling the young budget (Json::ParseMapping: 241 young collections). */
typedef struct {
  uint8_t *base;
  size_t cap;
  size_t top;
  size_t last;
} JsonBump;

static void *bump_malloc(void *ctx, size_t size) {
  JsonBump *b = (JsonBump *)ctx;
  size_t at = (b->top + 15) & ~(size_t)15;
  if (at > b->cap || size > b->cap - at) {
    return NULL;
  }
  b->last = at;
  b->top = at + size;
  return b->base + at;
}

static void *bump_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
  JsonBump *b = (JsonBump *)ctx;
  void *p;
  if ((uint8_t *)ptr == b->base + b->last) {
    if (size > b->cap - b->last) {
      return NULL;
    }
    b->top = b->last + size;
    return ptr;
  }
  p = bump_malloc(ctx, size);
  if (p) {
    memcpy(p, ptr, old_size < size ? old_size : size);
  }
  return p;
}

static void bump_free(void *ctx, void *ptr) {
  JsonBump *b = (JsonBump *)ctx;
  if ((uint8_t *)ptr == b->base + b->last) {
    b->top = b->last;
  }
}

/* RC1: the text is a handle across the pool's allocation (before W16a
 * nothing protected it: a young copy relied on the C-stack rewrite, audit
 * 12 shape 2 of C31); yyjson reads it after the allocation and allocates
 * only from its pool, never from the collector, so the parse itself
 * needs no no-GC region. */
void *avant_json_parse(const char *s) {
  size_t n = avant_str_n(s);
  int attempt;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  set_status(0);
  /* The pool waits in g_json_pool until avant_json_take_pool hands it to
   * the Json value: a runtime root of this thread meanwhile (Stage 28
   * W16a; before, a full collection between the two calls could free
   * it). */
  if (!g_json_pool_rooted) {
    g_json_pool_rooted = 1;
    avant_gc_add_root((void **)&g_json_pool);
  }
  g_json_pool = NULL;
  for (attempt = 0; attempt < 2; attempt++) {
    size_t pool_n = n * (attempt == 0 ? 24 : 96) + 8192;
    yyjson_alc alc;
    JsonBump bump;
    yyjson_read_err err;
    yyjson_doc *doc;
    char *pool;
    /* The pool (at least 8 KiB of Bytes) is a large object, which never
     * moves, so yyjson's interior pointers into it stay valid; it is born
     * young like every large object and dies young when its Json is
     * dropped (Stage 28 W16a). Before, it was allocated mature
     * (avant_alloc_in_mature), so a dropped document waited for a full
     * collection, and with collections at the helpers' own allocations
     * (RC1) the full trigger fired inside avant_json_f64s while the new
     * document was still live (Json::ParseMapping 34 -> 90 MB). The
     * bracket also turned a mature-allocating thread's setting off. */
    /* The previous document's pool is usually dead by now: let the due
     * young collection free it before this one exists. */
    avant_gc_young_before((uint64_t)pool_n);
    pool = (char *)avant_alloc((uint64_t)pool_n, AVANT_TYPE_BYTES);
    bump.base = (uint8_t *)pool;
    bump.cap = pool_n;
    bump.top = 0;
    bump.last = 0;
    alc.malloc = bump_malloc;
    alc.realloc = bump_realloc;
    alc.free = bump_free;
    alc.ctx = &bump;
    doc = yyjson_read_opts((char *)(s ? s : ""), n, 0, &alc, &err);
    if (doc) {
      /* The pool is rooted (g_json_pool) before the trim, whose lock
       * wait may let another thread collect; it is large and stays put. */
      g_json_pool = pool;
      (void)avant_gc_large_trim(pool, (uint64_t)bump.top);
      return doc;
    }
    if (err.code != YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
      g_json_err_msg = err.msg;
      g_json_err_pos = (int64_t)err.pos;
      set_status(1);
      return NULL;
    }
  }
  set_status(2);
  return NULL;
}

void *avant_json_root(void *docp) {
  yyjson_doc *doc = (yyjson_doc *)docp;
  if (!doc) {
    return NULL;
  }
  return yyjson_doc_get_root(doc);
}

/* Kinds: 0 no value, 1 null, 2 bool, 3 int, 4 float, 5 string, 6 array,
 * 7 object. */
int32_t avant_json_kind(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v) {
    return 0;
  }
  if (yyjson_is_null(v)) {
    return 1;
  }
  if (yyjson_is_bool(v)) {
    return 2;
  }
  if (yyjson_is_int(v) || yyjson_is_uint(v)) {
    return 3;
  }
  if (yyjson_is_num(v)) {
    return 4;
  }
  if (yyjson_is_str(v)) {
    return 5;
  }
  if (yyjson_is_arr(v)) {
    return 6;
  }
  if (yyjson_is_obj(v)) {
    return 7;
  }
  return 0;
}

void *avant_json_get(void *nodep, const char *key) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !key || !yyjson_is_obj(v)) {
    return NULL;
  }
  return yyjson_obj_get(v, key);
}

void *avant_json_at(void *nodep, int32_t i) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || i < 0 || !yyjson_is_arr(v)) {
    return NULL;
  }
  return yyjson_arr_get(v, (size_t)i);
}

void *avant_json_first(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !yyjson_is_arr(v)) {
    return NULL;
  }
  return yyjson_arr_get_first(v);
}

/* One sibling step. Only meaningful while a next sibling exists; the
 * Avant iterator tracks the element count, so it never steps past. */
void *avant_json_next(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v) {
    return NULL;
  }
  return unsafe_yyjson_get_next(v);
}

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArrayBox;

/* One sequential C traversal that projects a numeric field of every
 * element into a collector Array(Float64). NULL with status 1 when the
 * receiver is not an array or any element lacks a numeric field. */
void *avant_json_f64s(void *nodep, const char *key) {
  yyjson_val *arr = (yyjson_val *)nodep;
  yyjson_arr_iter iter;
  yyjson_val *el;
  AvantArrayBox *out;
  double *buf = NULL;
  size_t n;
  size_t i = 0;
  set_status(0);
  if (!arr || !key || !yyjson_is_arr(arr)) {
    set_status(1);
    return NULL;
  }
  n = yyjson_arr_size(arr);
  /* RC1: the node (an interior address of its document's pool), the key
   * and the fresh array are handles across the allocations. */
  out = NULL;
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &arr);
  AVANT_GC_ROOT(1, &key);
  AVANT_GC_ROOT(2, &out);
  out = (AvantArrayBox *)avant_alloc(sizeof(AvantArrayBox), AVANT_TYPE_ARRAY_OBJ);
  avant_size_check((uint64_t)n, AVANT_SIZE_ARRAY);
  if (n > 0) {
    buf = (double *)avant_alloc((uint64_t)n * sizeof(double), AVANT_TYPE_BYTES);
  }
  avant_barrier(out);
  out->buf = buf;
  out->size = (int32_t)n;
  out->cap = (int32_t)n;
  iter = yyjson_arr_iter_with(arr);
  while ((el = yyjson_arr_iter_next(&iter)) != NULL) {
    yyjson_val *field = yyjson_is_obj(el) ? yyjson_obj_get(el, key) : NULL;
    if (!field || !yyjson_is_num(field)) {
      set_status(1);
      return NULL;
    }
    buf[i] = yyjson_get_num(field);
    i++;
  }
  return out;
}

int32_t avant_json_len(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v) {
    return 0;
  }
  if (yyjson_is_arr(v)) {
    return (int32_t)yyjson_arr_size(v);
  }
  if (yyjson_is_obj(v)) {
    return (int32_t)yyjson_obj_size(v);
  }
  return 0;
}

/* RC1: P (inside a document's pool, or yyjson's malloc'ed text, which a
 * slot leaves alone) is a handle across the allocation. */
static char *copy_str(const char *p, size_t n) {
  char *out;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &p);
  out = avant_str_alloc((uint64_t)n);
  if (p && n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

/* A JSON integer (no fraction, no exponent: yyjson's int and uint kinds)
 * within [lo, hi], else status 1 (Stage 29 W4b1, D295 R10: Json.int is
 * nil for a real, for a non-number and outside the type's range). */
static int64_t node_int_in(yyjson_val *v, int64_t lo, int64_t hi) {
  if (v && yyjson_is_uint(v)) {
    uint64_t u = yyjson_get_uint(v);
    if (u <= (uint64_t)hi) {
      set_status(0);
      return (int64_t)u;
    }
  } else if (v && yyjson_is_sint(v)) {
    int64_t s = yyjson_get_sint(v);
    if (s >= lo && s <= hi) {
      set_status(0);
      return s;
    }
  }
  set_status(1);
  return 0;
}

static int32_t node_int(yyjson_val *v) {
  return (int32_t)node_int_in(v, INT32_MIN, INT32_MAX);
}

static double node_f64(yyjson_val *v) {
  if (!v || !yyjson_is_num(v)) {
    set_status(1);
    return 0.0;
  }
  set_status(0);
  return yyjson_get_num(v);
}

static int32_t node_bool(yyjson_val *v) {
  if (!v || !yyjson_is_bool(v)) {
    set_status(1);
    return 0;
  }
  set_status(0);
  return yyjson_get_bool(v) ? 1 : 0;
}

static char *node_str(yyjson_val *v) {
  if (!v || !yyjson_is_str(v)) {
    set_status(1);
    return avant_str_empty();
  }
  set_status(0);
  return copy_str(yyjson_get_str(v), yyjson_get_len(v));
}

int32_t avant_json_as_int(void *nodep) {
  return node_int((yyjson_val *)nodep);
}

int32_t avant_json_obj_int(void *nodep, const char *key) {
  return node_int((yyjson_val *)avant_json_get(nodep, key));
}

int64_t avant_json_as_int64(void *nodep) {
  return node_int_in((yyjson_val *)nodep, INT64_MIN, INT64_MAX);
}

int64_t avant_json_obj_int64(void *nodep, const char *key) {
  return node_int_in((yyjson_val *)avant_json_get(nodep, key), INT64_MIN, INT64_MAX);
}

double avant_json_as_f64(void *nodep) {
  return node_f64((yyjson_val *)nodep);
}

double avant_json_obj_f64(void *nodep, const char *key) {
  return node_f64((yyjson_val *)avant_json_get(nodep, key));
}

int32_t avant_json_as_bool(void *nodep) {
  return node_bool((yyjson_val *)nodep);
}

int32_t avant_json_obj_bool(void *nodep, const char *key) {
  return node_bool((yyjson_val *)avant_json_get(nodep, key));
}

char *avant_json_as_str(void *nodep) {
  return node_str((yyjson_val *)nodep);
}

char *avant_json_obj_str(void *nodep, const char *key) {
  return node_str((yyjson_val *)avant_json_get(nodep, key));
}

char *avant_json_key_at(void *nodep, int32_t i) {
  yyjson_val *v = (yyjson_val *)nodep;
  yyjson_val *key;
  yyjson_obj_iter iter;
  int32_t at = 0;
  set_status(0);
  if (!v || i < 0 || !yyjson_is_obj(v)) {
    set_status(1);
    return avant_str_empty();
  }
  iter = yyjson_obj_iter_with(v);
  while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
    if (at == i) {
      return copy_str(yyjson_get_str(key), yyjson_get_len(key));
    }
    at++;
  }
  set_status(1);
  return avant_str_empty();
}

/* The node's own JSON text, copied into collector memory. */
/* gc-lint: holds nothing (yyjson's text is malloc'ed C memory; the node is not read after copy_str). */
char *avant_json_text(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  size_t len = 0;
  char *raw;
  if (!v) {
    set_status(1);
    return avant_str_empty();
  }
  raw = yyjson_val_write_opts(v, 0, NULL, &len, NULL);
  if (!raw) {
    set_status(1);
    return avant_str_empty();
  }
  set_status(0);
  {
    char *out = copy_str(raw, len);
    free(raw);
    return out;
  }
}

/* yyjson's message for the last failed parse ("unexpected character"),
 * and the byte offset it names. */
/* gc-lint: holds nothing (C memory only). */
char *avant_json_error_text(void) {
  const char *m = g_json_err_msg ? g_json_err_msg : "invalid JSON document";
  return copy_str(m, strlen(m));
}

int64_t avant_json_error_offset(void) {
  return g_json_err_pos;
}

/* A JsonBuilder used against its contract (Stage 29 W4b2, D295 R11):
 * what says how. Printed as a trap (stdout first, status 1). */
void avant_json_misuse(const char *what) {
  fflush(stdout);
  fprintf(stderr, "avant runtime: %s\n", what);
  exit(1);
}
