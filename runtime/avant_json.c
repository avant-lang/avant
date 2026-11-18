#include "avant_rt.h"
#include "avant_gc.h"
#include "yyjson.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* std.json backing (yyjson). A parsed document lives in one pool buffer
 * allocated in collector space (a bump allocator over it, below), a large object,
 * which never moves: the Avant Json value roots that buffer, interior
 * yyjson pointers stay valid,
 * and the collector reclaims the whole document when the last Json is
 * dropped. Accessors report a missing key or a wrong kind through the
 * thread-local status, in the zlib/base64 shape. */

/* 0 ok, 1 invalid JSON / missing or wrong kind, 2 allocation failure,
 * 3 nested deeper than the parse's max_depth, 4 more values than its
 * max_entries. */
static __thread int32_t g_json_status;
/* Why the last parse on this thread failed:
 * yyjson's message (a static string) and the byte offset it names. */
static __thread const char *g_json_err_msg;
static __thread int64_t g_json_err_pos;
static __thread char *g_json_pool;
/* The last parse refused for a limit (status 3 to 5): what it measured
 * (values, bytes) and the limit. */
static __thread int64_t g_json_count;
static __thread int64_t g_json_limit;

/* yyjson.c's Avant patch: the deepest nesting a read takes (0: none). */
extern __thread size_t yyjson_avant_max_depth;
static __thread int g_json_pool_rooted;

static void set_status(int32_t code) {
  g_json_status = code;
}

avant_int avant_json_take_status(void) {
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

/* The document's allocator: a bump over the
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

/* RC1: the text is a handle across the pool's allocation (it once had
 * no protection: a young copy relied on the C-stack rewrite);
 * yyjson reads it after the allocation and allocates
 * only from its pool, never from the collector, so the parse itself
 * needs no no-GC region. */
/* The limits (std.json's Json.parse; -1 for none, else 1 or more):
 * max_depth, the deepest nesting of arrays and objects (the root's is 1),
 * checked by the reader as it goes (an increment and a compare per
 * container: yyjson.c's Avant patch); max_bytes, the text's size, before
 * anything is allocated (status 5); max_entries, the most values (each
 * member's name counted as one, as yyjson counts them), against the
 * parsed document's count (status 4). */
void *avant_json_parse(const char *s, avant_int max_depth, avant_int max_bytes, avant_int max_entries) {
  size_t n = avant_str_n(s);
  int attempt;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  set_status(0);
  if (max_bytes >= 0 && (int64_t)n > (int64_t)max_bytes) {
    g_json_count = (int64_t)n;
    g_json_limit = (int64_t)max_bytes;
    set_status(5);
    return NULL;
  }
  /* The pool waits in g_json_pool until avant_json_take_pool hands it to
   * the Json value: a runtime root of this thread meanwhile (without
   * it, a full collection between the two calls could free it). */
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
     * dropped. Before, it was allocated mature
     * (avant_alloc_in_mature), so a dropped document waited for a full
     * collection, and with collections at the helpers' own allocations
     * (RC1) the full trigger fired inside a helper that allocated an
     * array from the document while the new document was still live (Json::ParseMapping 34 -> 90 MB). The
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
    yyjson_avant_max_depth = max_depth > 0 ? (size_t)max_depth : 0;
    doc = yyjson_read_opts((char *)(s ? s : ""), n, 0, &alc, &err);
    yyjson_avant_max_depth = 0;
    if (doc && max_entries >= 0 && (int64_t)yyjson_doc_get_val_count(doc) > (int64_t)max_entries) {
      /* The pool is young and not yet rooted (g_json_pool): it dies
       * with the document. */
      g_json_count = (int64_t)yyjson_doc_get_val_count(doc);
      g_json_limit = (int64_t)max_entries;
      set_status(4);
      return NULL;
    }
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
      g_json_limit = (int64_t)max_depth;
      set_status(err.code == YYJSON_READ_ERROR_AVANT_DEPTH ? 3 : 1);
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
avant_int avant_json_kind(void *nodep) {
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

/* The member named key, by the key's bytes and size (avant_str_bytes):
 * no strlen at each lookup, and a name holding a zero byte (\u0000 in
 * JSON) is found whole. */
void *avant_json_get(void *nodep, const char *key) {
  yyjson_val *v = (yyjson_val *)nodep;
  const char *k;
  size_t n;
  if (!v || !key || !yyjson_is_obj(v)) {
    return NULL;
  }
  k = avant_str_bytes(key, &n);
  return yyjson_obj_getn(v, k, n);
}

/* The value of an object's member i, in the document's order (the key
 * avant_json_key_at gives for i), or NULL past the last member. */
static yyjson_val *obj_value_at(yyjson_val *v, avant_int i) {
  yyjson_obj_iter iter = yyjson_obj_iter_with(v);
  yyjson_val *key;
  avant_int at = 0;
  while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
    if (at == i) {
      return yyjson_obj_iter_get_val(key);
    }
    at++;
  }
  return NULL;
}

/* Element i of an array, or the value of member i of an object. */
void *avant_json_at(void *nodep, avant_int i) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || i < 0) {
    return NULL;
  }
  if (yyjson_is_arr(v)) {
    return yyjson_arr_get(v, (size_t)i);
  }
  if (yyjson_is_obj(v)) {
    return obj_value_at(v, i);
  }
  return NULL;
}

/* The first element of an array, or the first member's value of an
 * object. */
void *avant_json_first(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v) {
    return NULL;
  }
  if (yyjson_is_arr(v)) {
    return yyjson_arr_get_first(v);
  }
  if (yyjson_is_obj(v)) {
    return obj_value_at(v, 0);
  }
  return NULL;
}

/* The first member's name of an object (a string node; the node after it
 * is that member's value, and the node after the value the next name),
 * or NULL for an empty object and any other value: the typed decode
 * walks a Hash's members and an object's unknown names with it, in one
 * pass, where at(i) and key_at(i) each walk from the start. */
void *avant_json_first_key(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !yyjson_is_obj(v) || yyjson_obj_size(v) == 0) {
    return NULL;
  }
  return unsafe_yyjson_get_first(v);
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

avant_int avant_json_len(void *nodep) {
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
 * within [lo, hi], else status 1 (Json.int is
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

/* Json.int: an integer within Int (64-bit; there is no separate
 * Json.int64, one spelling). */
static avant_int node_int(yyjson_val *v) {
  return (avant_int)node_int_in(v, AVANT_INT_MIN, AVANT_INT_MAX);
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

avant_int avant_json_as_int(void *nodep) {
  return node_int((yyjson_val *)nodep);
}

avant_int avant_json_obj_int(void *nodep, const char *key) {
  return node_int((yyjson_val *)avant_json_get(nodep, key));
}

/* A number's value, or NaN when the node is not a number: the reader
 * refuses NaN and the infinities (flags 0), so a JSON number is never
 * NaN, and Json.f64 reads the answer of one call. */
static double node_f64_or_nan(yyjson_val *v) {
  if (!v || !yyjson_is_num(v)) {
    return __builtin_nan("");
  }
  return yyjson_get_num(v);
}

double avant_json_as_f64(void *nodep) {
  return node_f64_or_nan((yyjson_val *)nodep);
}

double avant_json_obj_f64(void *nodep, const char *key) {
  return node_f64_or_nan((yyjson_val *)avant_json_get(nodep, key));
}

avant_int avant_json_as_bool(void *nodep) {
  return node_bool((yyjson_val *)nodep);
}

avant_int avant_json_obj_bool(void *nodep, const char *key) {
  return node_bool((yyjson_val *)avant_json_get(nodep, key));
}

char *avant_json_as_str(void *nodep) {
  return node_str((yyjson_val *)nodep);
}

char *avant_json_obj_str(void *nodep, const char *key) {
  return node_str((yyjson_val *)avant_json_get(nodep, key));
}

char *avant_json_key_at(void *nodep, avant_int i) {
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

/* What the last parse refused for a limit measured (values or bytes),
 * and that limit. */
int64_t avant_json_error_count(void) {
  return g_json_count;
}

int64_t avant_json_error_limit(void) {
  return g_json_limit;
}

/* A JsonBuilder used against its contract: what says how. A trap: inside
 * a task or an HTTP handler it fails that task or request alone; anywhere
 * else stdout is flushed first, then the message, status 1. */
void avant_json_misuse(const char *what) {
  /* The program's String as it is (this call never collects): its bytes
   * by length, terminated in a buffer as long as a trap's text, then the
   * trap path. */
  char text[1024];
  size_t n;
  const char *d = avant_str_bytes(what, &n);
  snprintf(text, sizeof text, "%.*s", (int)(n < sizeof text ? n : sizeof text - 1), d);
  avant_trap_text(text);
}
