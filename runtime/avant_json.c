#include "avant_rt.h"
#include "yyjson.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* std.json backing (yyjson). A parsed document lives in one pool buffer
 * allocated in mature collector space (yyjson_alc_pool_init): the Avant
 * Json value roots that buffer, interior yyjson pointers never move,
 * and the collector reclaims the whole document when the last Json is
 * dropped. Accessors report a missing key or a wrong kind through the
 * thread-local status, in the zlib/base64 shape. */

/* 0 ok, 1 invalid JSON / missing or wrong kind, 2 allocation failure. */
static __thread int32_t g_json_status;
static __thread char *g_json_pool;

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

void *avant_json_parse(const char *s) {
  size_t n = avant_str_n(s);
  int attempt;
  set_status(0);
  g_json_pool = NULL;
  for (attempt = 0; attempt < 2; attempt++) {
    size_t pool_n = n * (attempt == 0 ? 24 : 96) + 8192;
    yyjson_alc alc;
    yyjson_read_err err;
    yyjson_doc *doc;
    char *pool;
    avant_alloc_in_mature(1);
    pool = (char *)avant_alloc((uint64_t)pool_n, AVANT_TYPE_BYTES);
    avant_alloc_in_mature(0);
    if (!yyjson_alc_pool_init(&alc, pool, pool_n)) {
      set_status(2);
      return NULL;
    }
    doc = yyjson_read_opts((char *)(s ? s : ""), n, 0, &alc, &err);
    if (doc) {
      g_json_pool = pool;
      return doc;
    }
    if (err.code != YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
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

static char *copy_str(const char *p, size_t n) {
  char *out;
  avant_gc_defer_enter();
  out = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (p && n > 0) {
    memcpy(out, p, n);
  }
  out[n] = 0;
  avant_str_tag(out);
  avant_gc_defer_leave();
  return out;
}

static int32_t node_int(yyjson_val *v) {
  if (!v || !yyjson_is_num(v)) {
    set_status(1);
    return 0;
  }
  set_status(0);
  if (yyjson_is_int(v)) {
    return (int32_t)yyjson_get_sint(v);
  }
  if (yyjson_is_uint(v)) {
    return (int32_t)yyjson_get_uint(v);
  }
  return (int32_t)yyjson_get_real(v);
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
