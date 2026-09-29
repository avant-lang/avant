#ifndef AVANT_RT_H
#define AVANT_RT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Avant runtime — Immix-family collector (D34).
 *
 * Heap objects are [AvantHeader][payload], 8-aligned. The pointer Avant
 * sees is the payload; the header sits immediately before it so field
 * indices in myc IR stay the language layout.
 *
 * Collector is GenImmix (copying nursery, Immix mature). Stage 17 is
 * growing myc-llvm pointer safepoints so the copying nursery can be
 * the default. Until compile-compiler stays honest with young copy
 * and from-space discarded, the nursery is off unless AVANT_NURSERY=1.
 * AVANT_NURSERY_DISCARD=1 frees from-space after young copy (pinned
 * nursery blocks stay). LLVM stack maps are walked when the nursery
 * is on (AVANT_NURSERY_STACKMAP=0 disables). Discard compile-compiler
 * needs that walk so leftover myc spills are rewritten. AVANT_NURSERY=0
 * is the Stage 7 full (non-moving) collect. AVANT_FLAG_PINNED is the C-escape seam:
 * pinned objects are not moved and are young-GC roots.
 */

#define AVANT_TYPE_BYTES 0
#define AVANT_TYPE_PTRS 65535u
#define AVANT_TYPE_BUF 1021u
#define AVANT_TYPE_ARRAY_OBJ 1022u
#define AVANT_TYPE_HASH 1023u

#define AVANT_FLAG_PINNED 1u
#define AVANT_FLAG_MARKED 2u
#define AVANT_FLAG_FORWARDED 4u
#define AVANT_FLAG_YSCAN 8u
#define AVANT_FLAG_HEAP 0x8000u

typedef struct AvantHeader {
  uint32_t size;
  uint16_t type_id;
  uint16_t flags;
} AvantHeader;

void *avant_alloc(uint64_t nbytes, uint32_t type_id);
void avant_pin(void *payload);
int avant_is_heap(void *payload);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);

void avant_gc_enter(void);
void avant_gc_leave(void);
void avant_gc_root(void *slot);
void *avant_gc_reload(void *slot);
void avant_type_map(uint32_t type_id, uint64_t word_bits);
void avant_barrier(void *obj);

typedef struct AvantGcStats {
  uint64_t collections;
  uint64_t young_collections;
  uint64_t full_collections;
  uint64_t allocated_bytes;
  uint64_t copied_bytes;
  uint64_t live_bytes;
  uint64_t peak_heap;
} AvantGcStats;

void avant_gc_stats(AvantGcStats *out);
void avant_gc_collect(void);
void avant_gc_register_thread(void);
void avant_gc_unregister_thread(void);

size_t avant_str_n(const char *s);
char *avant_str_empty(void);
char *avant_str_concat(const char *a, const char *b);
char *avant_str_from_i32_bytes(void *arr);
char *avant_str_repeat_byte(int32_t b, int32_t n);
char *avant_str_slice(const char *s, int32_t start, int32_t stop);
char *avant_str_from_int(int32_t n);
char *avant_str_from_i64(int64_t n);
char *avant_str_from_u64(uint64_t n);
char *avant_str_from_float(double n);
char *avant_str_from_bool(int32_t n);
char *avant_str_from_byte(int32_t b);
char *avant_str_fmt_float(double v, int32_t prec);
double avant_str_to_f(const char *s);
double avant_str_to_f_slice(const char *s, int32_t start, int32_t stop);
int32_t avant_str_to_i(const char *s);
double avant_exp(double x);
int32_t avant_str_eq(const char *a, const char *b);
int32_t avant_str_size(const char *s);
int32_t avant_str_byte(const char *s, int32_t i);
int32_t avant_checksum_str(const char *s);
int32_t avant_checksum_f64(double v);

void *avant_buf_new(int32_t cap);
void avant_buf_push_byte(void *buf, int32_t b);
void avant_buf_push_str(void *buf, const char *s);
void avant_buf_push_slice(void *buf, const char *s, int32_t start, int32_t stop);
void avant_buf_push_fmt_f(void *buf, double v, int32_t prec);
void avant_buf_push_fmt_i(void *buf, int32_t n);
char *avant_buf_to_str(void *buf);
int32_t avant_buf_size(void *buf);
void avant_buf_clear(void *buf);
void avant_buf_ensure(void *buf, int32_t n);
void avant_buf_append(void *buf, const void *p, int32_t n);
void avant_buf_set_size(void *buf, int32_t n);
uint8_t *avant_buf_bytes(void *buf);
int32_t avant_buf_starts(void *buf, const char *s);

char *avant_b64_encode(const char *s);
char *avant_b64_decode(const char *s);
void avant_b64_encode_buf(void *buf, const char *s);
void avant_b64_decode_buf(void *buf, const char *s);
int32_t avant_crc32_i32(void *arr);
int32_t avant_sha256_word0(void *arr);
char *avant_zlib_compress(const char *s);
char *avant_zlib_uncompress(const char *s);
char *avant_sha256(const char *s);

void *avant_re_compile(const char *pat, int32_t flags);
int32_t avant_re_find(void *re, const char *s, int32_t from);
int32_t avant_re_m0(void);
int32_t avant_re_m1(void);
int32_t avant_re_c0(void);
int32_t avant_re_c1(void);
int32_t avant_re_count(const char *pat, const char *s, int32_t caseless);
int32_t avant_wide_op(int32_t op, int32_t a_lo, int32_t a_hi, int32_t b_lo, int32_t b_hi);
int32_t avant_wide_hi(void);

void *avant_hash_new(int32_t key_kind, int32_t val_kind);
int32_t avant_hash_size(void *h);
void avant_hash_set_i32(void *h, const char *key, int32_t val);
void avant_hash_set_str(void *h, const char *key, const char *val);
void avant_hash_set_i32k_i32(void *h, int32_t key, int32_t val);
int32_t avant_hash_get_i32(void *h, const char *key, int32_t *found);
const char *avant_hash_get_str(void *h, const char *key, int32_t *found);
int32_t avant_hash_get_i32k_i32(void *h, int32_t key, int32_t *found);
void avant_hash_del(void *h, const char *key);
void avant_hash_del_i32k(void *h, int32_t key);
int32_t avant_hash_inc_slice(void *h, const char *s, int32_t start, int32_t stop);
const char *avant_hash_last_key(void);
void avant_hash_walk_tls(void (*fn)(void **slot));
const char *avant_hash_get_str_slice(void *h, const char *s, int32_t start, int32_t stop, int32_t *found);
int32_t avant_hash_get_concat(void *h, const char *a, const char *b, int32_t *found);

void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id);
void avant_array_push_ptr(void *arr, void *elem, uint32_t buf_type_id);
void avant_array_set_ptr(void *arr, int32_t i, void *elem);
void *avant_array_get_ptr(void *arr, int32_t i);
void *avant_array_pop_ptr(void *arr);
void avant_array_push_i32(void *arr, int32_t v);
void avant_array_clear(void *arr);
void avant_array_reserve(void *arr, int32_t n, uint64_t elem_size, uint32_t buf_type_id);
void *avant_array_pop_slot(void *arr, uint64_t elem_size);
void avant_array_sort_i32(void *arr);
void avant_array_fill_i32(void *arr, int32_t v);

void *avant_spawn(void *fn, void *arg);
int32_t avant_join(void *handle);
int32_t avant_now_ms(void);
int32_t avant_now_us(void);

void avant_io_init_argv(int32_t argc, void *argv);
void *avant_argv(void);
char *avant_file_read(const char *path, int32_t *ok);
int32_t avant_file_write(const char *path, const char *body);
int32_t avant_process_run(const char *path, void *args);
int32_t avant_process_run_out(const char *path, void *args, const char *out_path);
char *avant_env_get(const char *name, int32_t *ok);
int32_t avant_file_exists(const char *path);
void *avant_dir_list(const char *path);

void avant_cov_init(const char *map_path);
void avant_cov_hit(int32_t slot);

void *avant_json_parse(const char *s);
void avant_json_free(void *doc);
char *avant_json_gen_body(int32_t n);
void avant_json_gen_into(void *buf, int32_t n);
int32_t avant_json_get_int(void *doc, const char *key, int32_t *found);
const char *avant_json_get_str(void *doc, const char *key, int32_t *found);
void *avant_json_root(void *doc);
void *avant_json_obj_get(void *val, const char *key);
int32_t avant_json_arr_len(void *val);
void *avant_json_arr_get(void *val, int32_t i);
double avant_json_as_f64(void *val, int32_t *found);
double avant_json_obj_f64(void *val, const char *key, int32_t *found);
double avant_json_arr_sum_f64(void *arr, const char *key);

const char *avant_http_roundtrip(const char *body);

#define AVANT_HASH_KEY_I32 0
#define AVANT_HASH_KEY_STR 1
#define AVANT_HASH_VAL_I32 0
#define AVANT_HASH_VAL_STR 1

static inline AvantHeader *avant_header(void *payload) {
  return ((AvantHeader *)payload) - 1;
}

#endif
