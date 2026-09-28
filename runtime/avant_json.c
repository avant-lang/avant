#include "avant_rt.h"
#include "yyjson.h"

#include <math.h>
#include <stdio.h>

#define AVANT_RNG_IM 139968
#define AVANT_RNG_IA 3877
#define AVANT_RNG_IC 29573

void *avant_json_parse(const char *s) {
  if (!s) {
    s = "";
  }
  return yyjson_read(s, avant_str_n(s), 0);
}

void avant_json_free(void *doc) {
  if (doc) {
    yyjson_doc_free((yyjson_doc *)doc);
  }
}

static int32_t rng_step(int32_t *last) {
  *last = (*last * AVANT_RNG_IA + AVANT_RNG_IC) % AVANT_RNG_IM;
  return *last;
}

static double rng_float(int32_t *last) {
  rng_step(last);
  return (double)(*last) / (double)AVANT_RNG_IM;
}

static double round8(double x) {
  return floor(x * 100000000.0 + 0.5) / 100000000.0;
}

static const char JSON_HDR[] = "{\"coordinates\":[";
static const char JSON_FTR[] = "],\"info\":\"some info\"}";

static void json_write_body(void *buf, int32_t n) {
  int32_t last = 42;
  int32_t i;
  int32_t need = 32 + (n > 0 ? n * 64 : 0);
  avant_buf_clear(buf);
  avant_buf_ensure(buf, need);
  avant_buf_append(buf, JSON_HDR, (int32_t)(sizeof(JSON_HDR) - 1));
  for (i = 0; i < n; i++) {
    char tmp[80];
    int k;
    double x = round8(rng_float(&last));
    double y = round8(rng_float(&last));
    double z = round8(rng_float(&last));
    rng_float(&last);
    rng_step(&last);
    if (i > 0) {
      avant_buf_append(buf, ",", 1);
    }
    k = snprintf(tmp, sizeof(tmp), "{\"x\":%.8f,\"y\":%.8f,\"z\":%.8f}", x, y, z);
    if (k > 0) {
      avant_buf_append(buf, tmp, k);
    }
  }
  avant_buf_append(buf, JSON_FTR, (int32_t)(sizeof(JSON_FTR) - 1));
}

void avant_json_gen_into(void *buf, int32_t n) {
  if (n < 0) {
    n = 0;
  }
  json_write_body(buf, n);
}

char *avant_json_gen_body(int32_t n) {
  if (n < 0) {
    n = 0;
  }
  avant_gc_defer_enter();
  void *buf = avant_buf_new(n > 0 ? n * 48 : 32);
  json_write_body(buf, n);
  char *out = avant_buf_to_str(buf);
  avant_gc_defer_leave();
  return out;
}

int32_t avant_json_get_int(void *docp, const char *key, int32_t *found) {
  yyjson_doc *doc = (yyjson_doc *)docp;
  if (found) {
    *found = 0;
  }
  if (!doc || !key) {
    return 0;
  }
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!root || !yyjson_is_obj(root)) {
    return 0;
  }
  yyjson_val *v = yyjson_obj_get(root, key);
  if (!v || !yyjson_is_int(v)) {
    return 0;
  }
  if (found) {
    *found = 1;
  }
  return (int32_t)yyjson_get_int(v);
}

const char *avant_json_get_str(void *docp, const char *key, int32_t *found) {
  yyjson_doc *doc = (yyjson_doc *)docp;
  if (found) {
    *found = 0;
  }
  if (!doc || !key) {
    return "";
  }
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!root || !yyjson_is_obj(root)) {
    return "";
  }
  yyjson_val *v = yyjson_obj_get(root, key);
  if (!v || !yyjson_is_str(v)) {
    return "";
  }
  if (found) {
    *found = 1;
  }
  return yyjson_get_str(v);
}

void *avant_json_root(void *docp) {
  yyjson_doc *doc = (yyjson_doc *)docp;
  if (!doc) {
    return NULL;
  }
  return yyjson_doc_get_root(doc);
}

void *avant_json_obj_get(void *valp, const char *key) {
  yyjson_val *v = (yyjson_val *)valp;
  if (!v || !key || !yyjson_is_obj(v)) {
    return NULL;
  }
  return yyjson_obj_get(v, key);
}

int32_t avant_json_arr_len(void *valp) {
  yyjson_val *v = (yyjson_val *)valp;
  if (!v || !yyjson_is_arr(v)) {
    return 0;
  }
  return (int32_t)yyjson_arr_size(v);
}

void *avant_json_arr_get(void *valp, int32_t i) {
  yyjson_val *v = (yyjson_val *)valp;
  if (!v || !yyjson_is_arr(v) || i < 0) {
    return NULL;
  }
  return yyjson_arr_get(v, (size_t)i);
}

double avant_json_as_f64(void *valp, int32_t *found) {
  yyjson_val *v = (yyjson_val *)valp;
  if (found) {
    *found = 0;
  }
  if (!v || !yyjson_is_num(v)) {
    return 0.0;
  }
  if (found) {
    *found = 1;
  }
  return yyjson_get_num(v);
}

double avant_json_obj_f64(void *valp, const char *key, int32_t *found) {
  return avant_json_as_f64(avant_json_obj_get(valp, key), found);
}

double avant_json_arr_sum_f64(void *arrp, const char *key) {
  yyjson_val *arr = (yyjson_val *)arrp;
  yyjson_val *el;
  double sum = 0.0;
  size_t idx;
  size_t max;
  if (!arr || !key || !yyjson_is_arr(arr)) {
    return 0.0;
  }
  yyjson_arr_foreach(arr, idx, max, el) {
    yyjson_val *v;
    if (!el || !yyjson_is_obj(el)) {
      continue;
    }
    v = yyjson_obj_get(el, key);
    if (v && yyjson_is_num(v)) {
      sum += yyjson_get_num(v);
    }
  }
  return sum;
}
