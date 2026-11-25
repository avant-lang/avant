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
 * dropped. An accessor answers a missing key or a wrong kind in its
 * result (below); a parse reports its failure through the thread-local
 * status, in the zlib/base64 shape. */

/* 0 ok, 1 invalid JSON / not an integer, 2 allocation failure,
 * 3 nested deeper than the parse's max_depth, 4 more values than its
 * max_entries. The readers answer in their result alone (NULL for a
 * string, -1 for a Bool, NaN for a float) but the integer's, whose
 * every value is an Int: it answers AVANT_INT_MIN for "not an integer"
 * and only then sets the status, which tells a real AVANT_INT_MIN from
 * it (status 0). */
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

/* The smallest pool: a Bytes this large is a large object (the
 * collector's SCALAR_LOS_MIN), which never moves. */
#define AVANT_JSON_POOL_MIN 6144

/* The document's allocator: a bump over the
 * pool. yyjson allocates the text's copy and then the values, which it
 * grows by realloc at the end, so a reallocation of the last block grows
 * it in place and a free of the last block takes the bump back. After
 * the parse the document is the pool's first `top` bytes and the rest
 * goes back to the heap at once (avant_gc_large_trim): the pool is sized
 * for the worst case (yyjson's bound, 13 bytes per text byte), and its
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
    /* yyjson's own bound for a text of n bytes (13 bytes a text byte
     * and 256: the text's copy, then values of 16 bytes, at most one a
     * two text bytes, grown by half each time), with room for this
     * bump's two 16-byte alignments; at least a large object's size,
     * since a smaller Bytes would be a block object the young copy
     * moves, and yyjson's pointers into the pool must stay put. The
     * retry, after an allocation failure (none is expected), takes
     * 96 bytes a text byte. */
    size_t pool_n = attempt == 0 ? yyjson_read_max_memory_usage(n, 0) + 64 : n * 96 + 8192;
    if (pool_n < (size_t)AVANT_JSON_POOL_MIN) {
      pool_n = (size_t)AVANT_JSON_POOL_MIN;
    }
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
    /* Not zeroed: yyjson writes every byte of the pool it reads (its
     * text copy, its values), and a Bytes payload holds no reference the
     * collector reads (avant_alloc_bytes_unzeroed). Zeroing it was most
     * of a small document's parse. */
    pool = (char *)avant_alloc_bytes_unzeroed((uint64_t)pool_n);
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

/* The member count of an object, the element count of an array, -1 for
 * any other value: the typed decode tells the kind and reads the size
 * in one call. */
avant_int avant_json_obj_size(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !yyjson_is_obj(v)) {
    return -1;
  }
  return (avant_int)yyjson_obj_size(v);
}

avant_int avant_json_arr_size(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !yyjson_is_arr(v)) {
    return -1;
  }
  return (avant_int)yyjson_arr_size(v);
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

/* Json.int: a JSON integer (no fraction, no exponent: yyjson's int and
 * uint kinds) within Int (64-bit; there is no separate Json.int64, one
 * spelling). Anything else (a real, a non-number, an integer beyond
 * Int) is AVANT_INT_MIN with status 1; AVANT_INT_MIN itself is answered
 * with status 0, so the caller reads the status only for that value. */
static avant_int node_int(yyjson_val *v) {
  if (v && yyjson_is_uint(v)) {
    uint64_t u = yyjson_get_uint(v);
    if (u <= (uint64_t)AVANT_INT_MAX) {
      return (avant_int)u;
    }
  } else if (v && yyjson_is_sint(v)) {
    int64_t s = yyjson_get_sint(v);
    if (s == AVANT_INT_MIN) {
      set_status(0);
    }
    return (avant_int)s;
  }
  set_status(1);
  return AVANT_INT_MIN;
}

/* 1 for true, 0 for false, -1 for any other value (Json.bool is nil). */
static avant_int node_bool(yyjson_val *v) {
  if (!v || !yyjson_is_bool(v)) {
    return -1;
  }
  return yyjson_get_bool(v) ? 1 : 0;
}

/* A string's copy, NULL for any other value (Json.str is nil: String?). */
static char *node_str(yyjson_val *v) {
  if (!v || !yyjson_is_str(v)) {
    return NULL;
  }
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

/* The name of an object's member: the string of a name node
 * (avant_json_first_key, then every second avant_json_next), which is
 * always a string. */
char *avant_json_name(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  return copy_str(unsafe_yyjson_get_str(v), unsafe_yyjson_get_len(v));
}

/* The name of member i, NULL when there is none (Json.key_at: String?). */
char *avant_json_key_at(void *nodep, avant_int i) {
  yyjson_val *v = (yyjson_val *)nodep;
  yyjson_val *key;
  yyjson_obj_iter iter;
  avant_int at = 0;
  if (!v || i < 0 || !yyjson_is_obj(v)) {
    return NULL;
  }
  iter = yyjson_obj_iter_with(v);
  while ((key = yyjson_obj_iter_next(&iter)) != NULL) {
    if (at == i) {
      return copy_str(yyjson_get_str(key), yyjson_get_len(key));
    }
    at++;
  }
  return NULL;
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


/* ---- The typed encode's writers ----
 *
 * json.encode and a JsonBuilder write a string and a float straight into
 * the Buf: a string is scanned once for the bytes that need an escape
 * and its runs copied whole (the Avant loop it replaced pushed byte by
 * byte), a float's text is formed on the stack, with no String made for
 * it. */

/* The bytes a string's byte takes in JSON text: 1, 2 for ", \ and the
 * escapes \n, \r, \t, 6 for any other byte below 0x20 (\u00XX). */
static const uint8_t json_esc_len[256] = {
  6, 6, 6, 6, 6, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 6,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

/* s as a JSON string, quoted, appended to buf: " and \ escaped, \n, \r
 * and \t by their short escapes, any other byte below 0x20 as \u00XX in
 * lower case, every other byte (UTF-8 included) as it is. The escaped
 * size is counted first, so the Buf grows at most once. */
void avant_json_write_str(void *buf, const char *s) {
  static const char hex[] = "0123456789abcdef";
  size_t n;
  size_t i;
  size_t out_n = 2;
  const uint8_t *p;
  int32_t at;
  uint8_t *dst;
  uint8_t *o;
  if (!buf) {
    return;
  }
  p = (const uint8_t *)avant_str_bytes(s ? s : AVANT_STR_EMPTY, &n);
  for (i = 0; i < n; i++) {
    out_n += json_esc_len[p[i]];
  }
  at = (int32_t)avant_buf_size(buf);
  if (out_n > (size_t)INT32_MAX - (size_t)at) {
    avant_size_check((uint64_t)at + (uint64_t)out_n, AVANT_SIZE_BUF);
  }
  /* RC1/RC2: the growth may collect; the text and the Buf are handles,
   * and the text's bytes are formed again after it. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_buf_ensure(buf, (avant_int)at + (avant_int)out_n);
  p = (const uint8_t *)avant_str_bytes(s ? s : AVANT_STR_EMPTY, &n);
  dst = avant_buf_bytes(buf);
  if (!dst) {
    return;
  }
  o = dst + at;
  *o++ = '"';
  if (out_n == n + 2) {
    memcpy(o, p, n);
    o += n;
  } else {
    size_t run = 0;
    for (i = 0; i < n; i++) {
      uint8_t c = p[i];
      if (json_esc_len[c] == 1) {
        continue;
      }
      if (i > run) {
        memcpy(o, p + run, i - run);
        o += i - run;
      }
      run = i + 1;
      *o++ = '\\';
      if (c == '"' || c == '\\') {
        *o++ = c;
      } else if (c == '\n') {
        *o++ = 'n';
      } else if (c == '\r') {
        *o++ = 'r';
      } else if (c == '\t') {
        *o++ = 't';
      } else {
        *o++ = 'u';
        *o++ = '0';
        *o++ = '0';
        *o++ = (uint8_t)hex[c >> 4];
        *o++ = (uint8_t)hex[c & 15];
      }
    }
    if (n > run) {
      memcpy(o, p + run, n - run);
      o += n - run;
    }
  }
  *o++ = '"';
  avant_buf_set_size(buf, (avant_int)(o - dst));
}

/* v's text (the language's one float text) appended to buf; 1 without
 * writing anything for NaN and the infinities, which JSON has no text
 * for, else 0. */
/* gc-lint: holds nothing (tmp is C memory; the append is the last statement). */
avant_int avant_json_write_f64(void *buf, double v) {
  char tmp[48];
  int k;
  if (v - v != 0.0) {
    return 1;
  }
  k = avant_fmt_float_text(tmp, v);
  avant_buf_append(buf, tmp, (int32_t)k);
  return 0;
}

/* ---- The typed decode's ordered reads ----
 *
 * A decoded type's fields are read in their declared order, and a
 * document written from that type (or by most writers of that shape)
 * has its members in the same order: each read first tries the member
 * after the one the last read took, one name comparison, and searches
 * the object only when that member has another name. The cursor is the
 * thread's: avant_json_ord_begin sets it on an object, each read that
 * takes the member at it steps it on, a search leaves it where it is (so
 * the member at it is always the first of its name: every member before
 * it was taken by an earlier field, and fields have distinct names), and
 * a read on another object than the cursor's searches. A nested decode
 * begins its own object; the caller takes the cursor before it
 * (avant_json_ord_mark) and puts it back after (avant_json_ord_resume).
 * No pointer here is read unless the object it came from is the one
 * read: the cursor only points inside the object it was set on. */
static __thread yyjson_val *g_ord_obj;
static __thread yyjson_val *g_ord_cur;
static __thread yyjson_val *g_ord_end;

/* The member count of an object, -1 for any other value (as
 * avant_json_obj_size), the cursor set at its first member. */
avant_int avant_json_ord_begin(void *nodep) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !yyjson_is_obj(v)) {
    g_ord_obj = NULL;
    return -1;
  }
  g_ord_obj = v;
  g_ord_cur = unsafe_yyjson_get_first(v);
  g_ord_end = unsafe_yyjson_get_next(v);
  return (avant_int)yyjson_obj_size(v);
}

static yyjson_val *ord_get(void *nodep, const char *key) {
  yyjson_val *v = (yyjson_val *)nodep;
  const char *k;
  size_t n;
  if (!v || !key) {
    return NULL;
  }
  k = avant_str_bytes(key, &n);
  if (v == g_ord_obj && g_ord_cur < g_ord_end) {
    yyjson_val *cur = g_ord_cur;
    if (unsafe_yyjson_get_len(cur) == n && memcmp(unsafe_yyjson_get_str(cur), k, n) == 0) {
      yyjson_val *val = cur + 1;
      g_ord_cur = unsafe_yyjson_get_next(val);
      return val;
    }
  }
  if (!yyjson_is_obj(v)) {
    return NULL;
  }
  return yyjson_obj_getn(v, k, n);
}

void *avant_json_ord_get(void *nodep, const char *key) {
  return ord_get(nodep, key);
}

avant_int avant_json_ord_int(void *nodep, const char *key) {
  return node_int(ord_get(nodep, key));
}

double avant_json_ord_f64(void *nodep, const char *key) {
  return node_f64_or_nan(ord_get(nodep, key));
}

avant_int avant_json_ord_bool(void *nodep, const char *key) {
  return node_bool(ord_get(nodep, key));
}

char *avant_json_ord_str(void *nodep, const char *key) {
  return node_str(ord_get(nodep, key));
}

/* The cursor, when it is on this object (else NULL), and the cursor put
 * back on it after a nested decode moved it (NULL: no cursor, every read
 * searches). */
void *avant_json_ord_mark(void *nodep) {
  if (nodep && (yyjson_val *)nodep == g_ord_obj) {
    return g_ord_cur;
  }
  return NULL;
}

void avant_json_ord_resume(void *nodep, void *cur) {
  yyjson_val *v = (yyjson_val *)nodep;
  if (!v || !cur || !yyjson_is_obj(v)) {
    g_ord_obj = NULL;
    return;
  }
  g_ord_obj = v;
  g_ord_cur = (yyjson_val *)cur;
  g_ord_end = unsafe_yyjson_get_next(v);
}
