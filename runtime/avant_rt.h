#ifndef AVANT_RT_H
#define AVANT_RT_H

#include <stdatomic.h>
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
/* Set on a nursery object that already survived one young collection.
 * The next young collection tenures it. A tree that dies first is not. */
#define AVANT_FLAG_SURVIVED 8u
/* Nursery allocation. avant_barrier reads this instead of hashing the block. */
#define AVANT_FLAG_YOUNG 32u
/* Large-object payload. It is not moved by a young collection. */
#define AVANT_FLAG_LARGE 64u
/* Heap string whose header size is the byte length plus the trailing NUL.
 * Readers may trust that length without walking the block table. */
#define AVANT_FLAG_STRMETA 0x4000u
#define AVANT_FLAG_HEAP 0x8000u

typedef struct AvantHeader {
  uint32_t size;
  uint16_t type_id;
  uint16_t flags;
} AvantHeader;

void *avant_alloc(uint64_t nbytes, uint32_t type_id);
/* Never collects. Returns null when a collection would be required,
 * when another thread exists, or when the nursery is off. */
void *avant_bump_alloc(uint64_t nbytes, uint32_t type_id);
/* Young collection. A dead nursery is discarded. A still-live tree
 * keeps the existing wait. */
void avant_nursery_reclaim(void);
void avant_pin(void *payload);
void avant_unpin(void *payload);
/* Number of distinct pinned payloads (refcounted pins share one entry). */
int avant_pin_count(void);
int avant_is_heap(void *payload);
/* Bumped by every collection. A length cache tagged with the generation
 * cannot outlive a move or a free, because both happen only inside a
 * collection. */
extern _Atomic uint32_t g_avant_str_gen;
static inline uint32_t avant_str_generation(void) {
  return atomic_load_explicit(&g_avant_str_gen, memory_order_acquire);
}
int32_t avant_is_young(void *payload);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);
void avant_world_lock(void);
void avant_world_unlock(void);
void avant_alloc_in_mature(int32_t on);

void avant_gc_enter(void);
void avant_gc_leave(void);
/* A quiet leaf does not collect. myc still roots pointer results; those
 * calls must not attach to the caller's frame. */
void avant_gc_quiet_enter(void);
void avant_gc_quiet_leave(void);
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
char *avant_str_cat_i32(const char *prefix, int32_t n);
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
char *avant_str_strip(const char *s, int32_t mode);
void *avant_str_split(const char *s, const char *sep);
void *avant_str_fields(const char *s);
char *avant_str_from_utf8(const char *s, int32_t *found);
void *avant_bytes_to_buf(const char *s);

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
void avant_bytes_ensure(void *buf, int32_t n);
void avant_buf_append(void *buf, const void *p, int32_t n);
void avant_buf_set_size(void *buf, int32_t n);
void avant_buf_set_ident(void *buf, int32_t n);
/* Append-only fills of `buf`: copy len bytes from src_off to dst_off, then
 * write one extra byte. Source and dest are the same buffer. */
void avant_buf_fill_appends(void *buf, void *dst_offs, void *src_offs, void *lens, void *extras, int32_t n);
/* Copy n slices from src into dst. Offsets are byte indexes. */
void avant_buf_copy_ranges(void *dst, void *src, void *dst_offs, void *src_offs, void *lens, int32_t n);
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
void avant_re_set_span(int32_t m0, int32_t m1, int32_t c0, int32_t c1);
int32_t avant_re_count(const char *pat, const char *s, int32_t caseless);
int32_t avant_wide_op(int32_t op, int32_t a_lo, int32_t a_hi, int32_t b_lo, int32_t b_hi);
int32_t avant_wide_hi(void);

void *avant_hash_new(int32_t key_kind, int32_t val_kind);
int32_t avant_hash_size(void *h);
void avant_hash_clear(void *h);
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
int32_t avant_hash_has_str(void *h, const char *key);
int32_t avant_hash_has_i32k(void *h, int32_t key);
int32_t avant_hash_inc_str(void *h, const char *key, int32_t by);
int32_t avant_hash_inc_i32k(void *h, int32_t key, int32_t by);
int32_t avant_hash_get_or_i32(void *h, const char *key, int32_t fallback);
const char *avant_hash_get_or_str(void *h, const char *key, const char *fallback);
int32_t avant_hash_get_or_i32k_i32(void *h, int32_t key, int32_t fallback);
void *avant_hash_keys_str(void *h);
void *avant_hash_keys_i32(void *h);
void *avant_hash_vals_str(void *h);
void *avant_hash_vals_i32(void *h);

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
void avant_array_sort_i64(void *arr);
void avant_array_sort_u64(void *arr);
void avant_array_sort_f64(void *arr);
void avant_array_sort_str(void *arr);
void avant_array_sort_bool(void *arr);
void avant_array_reverse(void *arr, uint64_t elem_size);
void *avant_array_copy(void *arr, uint64_t elem_size, uint32_t buf_type_id);
int32_t avant_array_contains_i32(void *arr, int32_t v);
int32_t avant_array_contains_i64(void *arr, int64_t v);
int32_t avant_array_contains_u64(void *arr, uint64_t v);
int32_t avant_array_contains_f64(void *arr, double v);
int32_t avant_array_contains_str(void *arr, const char *v);
int32_t avant_array_contains_ptr(void *arr, void *v);
int32_t avant_array_contains_bool(void *arr, int32_t v);
int32_t avant_array_index_bool(void *arr, int32_t v);
int32_t avant_array_index_i32(void *arr, int32_t v);
int32_t avant_array_index_i64(void *arr, int64_t v);
int32_t avant_array_index_u64(void *arr, uint64_t v);
int32_t avant_array_index_f64(void *arr, double v);
int32_t avant_array_index_str(void *arr, const char *v);
int32_t avant_array_index_ptr(void *arr, void *v);
int32_t avant_array_min_i32(void *arr, int32_t *found);
int64_t avant_array_min_i64(void *arr, int32_t *found);
uint64_t avant_array_min_u64(void *arr, int32_t *found);
double avant_array_min_f64(void *arr, int32_t *found);
const char *avant_array_min_str(void *arr, int32_t *found);
int32_t avant_array_max_i32(void *arr, int32_t *found);
int64_t avant_array_max_i64(void *arr, int32_t *found);
uint64_t avant_array_max_u64(void *arr, int32_t *found);
double avant_array_max_f64(void *arr, int32_t *found);
const char *avant_array_max_str(void *arr, int32_t *found);
int32_t avant_array_sum_i32(void *arr);
int64_t avant_array_sum_i64(void *arr);
uint64_t avant_array_sum_u64(void *arr);
double avant_array_sum_f64(void *arr);
void avant_array_fill_i32(void *arr, int32_t v);
void avant_array_fill_u8(void *arr, int32_t v);
/* Burrows–Wheeler of an Int buffer of bytes. Scratch: sa, rank, tmp
 * (each size >= n) and count (size > every rank, so size >= n).
 * `out` receives n bytes. Returns the original-string slot. */
int32_t avant_bwt_transform(void *input, void *sa, void *rank, void *tmp, void *count, void *out);
/* Inverse. `next` is scratch of size >= n. `out` receives n bytes. */
void avant_bwt_inverse(void *bwt, void *next, void *out, int32_t start);

/* Wave B safety floor (Q67-A/Q68-A): language traps. Each prints one
 * diagnostic to stderr and exits 1. They never return. */
void avant_trap_bounds(int32_t index, int32_t size);
void avant_trap_div_zero(void);
void avant_trap_overflow(void);
int64_t avant_checked_add_i64(int64_t a, int64_t b);
int64_t avant_checked_sub_i64(int64_t a, int64_t b);
int64_t avant_checked_mul_i64(int64_t a, int64_t b);
uint64_t avant_checked_add_u64(uint64_t a, uint64_t b);
uint64_t avant_checked_sub_u64(uint64_t a, uint64_t b);
uint64_t avant_checked_mul_u64(uint64_t a, uint64_t b);
int64_t avant_checked_div_i64(int64_t a, int64_t b);
int64_t avant_checked_rem_i64(int64_t a, int64_t b);
uint64_t avant_checked_div_u64(uint64_t a, uint64_t b);
uint64_t avant_checked_rem_u64(uint64_t a, uint64_t b);

void *avant_spawn(void *fn, void *arg);
int32_t avant_join(void *handle);
void avant_parallel_for(int32_t n, void *fn, void *ctx);
uint64_t avant_gate_new(void);
void avant_gate_lock(uint64_t gate);
void avant_gate_unlock(uint64_t gate);
void avant_gate_wait(uint64_t gate);
void avant_gate_signal(uint64_t gate);
void avant_gate_broadcast(uint64_t gate);
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

static inline void avant_str_tag(void *payload) {
  if (payload) {
    avant_header(payload)->flags =
        (uint16_t)(avant_header(payload)->flags | AVANT_FLAG_STRMETA);
  }
}

#endif
