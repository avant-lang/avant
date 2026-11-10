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
 * indices in generated code stay the language layout.
 *
 * Collector is GenImmix (copying nursery, Immix mature) and the copying
 * nursery is the product default (AVANT_NURSERY=0 turns it off: every
 * collection is then a full one). From-space is discarded after a young
 * copy. Remembered-set members are roots for the young generation.
 * AVANT_FLAG_PINNED is the C-escape seam: pinned objects are not moved
 * and are young-GC roots. The collector's environment knobs are listed
 * in runtime/avant_gc.h.
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
/* C-layout field pins (Stage 28 W16c, D266). A store of a reference into
 * a C-layout field (`h.name = s`) calls avant_pin_field(&h->name, s): s is
 * pinned (counted, as avant_pin) and the pin is recorded under the field's
 * address, beside every earlier value Avant stored there. Overwriting the
 * field does not unpin the old value, so C may keep or copy any of them:
 * a value stored into C memory stays alive and in place until the
 * program releases the field. avant_release_range(lo, n), what the
 * `release(h.name)` (n = the field's size) and `release(h)` (n = the
 * struct's size) statements call, gives back every recorded pin of a
 * field whose address is in [lo, lo + n) and drops the records; a field
 * that still holds a value Avant stored there is set to NULL. A field
 * never stored by Avant, or holding a value C stored itself, is left as
 * it is, and releasing twice does nothing. Both take the world lock like
 * avant_pin and never collect. After a release, C must not use the
 * released values (they may move or be freed by the next collection);
 * release a struct before C frees it, since a later store at the same
 * address joins the old record. */
void avant_pin_field(void *field, void *payload);
void avant_release_range(void *lo, uint64_t n);
int avant_is_heap(void *payload);
/* Bumped by every collection. A length cache tagged with the generation
 * cannot outlive a move or a free, because both happen only inside a
 * collection. */
extern _Atomic uint32_t g_avant_str_gen;
static inline uint32_t avant_str_generation(void) {
  return atomic_load_explicit(&g_avant_str_gen, memory_order_acquire);
}
int32_t avant_is_young(void *payload);
void avant_gc_heap_goal_collect(void);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);
void avant_world_lock(void);
void avant_world_unlock(void);
void avant_alloc_in_mature(int32_t on);

void avant_gc_enter(void);
void avant_gc_leave(void);
/* Quiet regions: between avant_gc_quiet_enter and avant_gc_quiet_leave
 * this thread registers no root slots (avant_gc_root_begin counts none,
 * avant_gc_root returns), for calls known not to collect. The compiler
 * does not emit them; they stay as part of the runtime ABI. */
void avant_gc_quiet_enter(void);
void avant_gc_quiet_leave(void);
void avant_gc_root(void *slot);
/* Batched root registration (Collector II Wave 0): generated code calls
 * this once at frame entry with the frame's root count, stores each
 * slot address into the returned array at its fixed index, and the
 * collector scans the first count entries. avant_gc_root (one slot per
 * call) and avant_gc_reload (a slot read after a collecting call) are
 * the old myc back end's ABI: the current compiler emits neither, and
 * they stay as deprecated entries until a release note retires them
 * (gc-design.md section 1.4, Q10). */
void *avant_gc_root_begin(uint32_t count);
void *avant_gc_reload(void *slot);
/* Frames in the caller's stack (Stage 28 W18): generated code keeps
 * each frame's record in its own stack frame, laid out as
 * { prev: pointer, slots: pointer, n: u32, cap: u32, scope: u32 } (32
 * bytes with padding; the slot array follows it), with slots, n = cap
 * (the frame's root words), scope = 0 and the slot addresses stored by
 * the caller. It links the record itself (prev = avant_gc_top,
 * avant_gc_top = record) when avant_gc_top is not null, else calls
 * avant_gc_frame_enter, which also registers the thread and initializes
 * the heap; it unlinks it itself (avant_gc_top = prev) when the record is
 * avant_gc_top and prev is not null, else calls avant_gc_frame_leave.
 * User C keeps using avant_gc_enter, avant_gc_root_begin and
 * avant_gc_leave. */
struct AvantGcFrame;
extern __thread struct AvantGcFrame *avant_gc_top;
void avant_gc_frame_enter(struct AvantGcFrame *frame);
void avant_gc_frame_leave(struct AvantGcFrame *frame);
/* A C variable that lives as long as its thread (a __thread variable)
 * and holds a heap reference is registered once, by that thread, before
 * it first holds one: from then on the collections on that thread mark
 * what it names and rewrite it when the object moves (Stage 28 W14b2).
 * At most 8 per thread. */
void avant_gc_add_root(void **slot);

/* Inline nursery fast path (Collector II Wave 1). Generated code may
 * allocate without a call only while avant_gc_fast_ok is set: it bumps
 * avant_gc_bump by the aligned total, writes the header (HEAP|YOUNG),
 * zeroes the payload, sets the block start bit through
 * avant_gc_starts/avant_gc_base, and, when avant_gc_fast_ok is 1, adds
 * the total to avant_g_stats.allocated_bytes, avant_since_gc, and
 * avant_nursery_held. avant_gc_limit is the block limit capped by the
 * young threshold, and the runtime alone publishes or clears this
 * state. avant_gc_fast_ok is 1 while the program has one thread (the
 * path counts; code of an unshared module also requires g_avant_shared
 * to be 0) and 2 in a program with threads (Stage 28 W17b): the thread
 * bumps in a hole of its own young block that the runtime has charged
 * to the young budget, and the path counts nothing. AvantGcStats field
 * order is part of this ABI (allocated_bytes at offset 24). */
extern __thread uint8_t *avant_gc_bump;
extern __thread uint8_t *avant_gc_limit;
extern __thread uint8_t *avant_gc_base;
extern __thread uint64_t *avant_gc_starts;
/* The current nursery block's record in the collector's side table;
 * generated code also stores the new cursor into its bump field at
 * offset 32 (the record's layout is the runtime's, not shipped), so the
 * C cursor and the block never disagree. */
extern __thread void *avant_gc_block;
extern __thread uint32_t avant_gc_fast_ok;
extern uint64_t avant_nursery_held;
extern uint64_t avant_young_bytes;
extern uint64_t avant_since_gc;
extern uint64_t avant_inline_allocs;
void avant_type_map(uint32_t type_id, uint64_t word_bits);
/* Stage 28 W1b: one 64-word chunk (word_base a multiple of 64) of a
 * type map of any length, with an element stride in words (0: one
 * record; > 0: a buffer of repeated elements), the collector's TypeMap
 * (a runtime-private layout, not shipped). */
void avant_type_map_at(uint32_t type_id, uint32_t stride_words, uint32_t word_base,
                       uint64_t bits);
void avant_barrier(void *obj);
/* Barrier for a store through an address that may lie inside a heap object. */
void avant_barrier_at(void *addr);
/* The barrier with the slot stored into (Stage 28 W16t, card marking):
 * a large pointer buffer marks the slot's card; any other object is
 * avant_barrier(obj). Never collects. */
void avant_barrier_slot(void *obj, void *slot);

typedef struct AvantGcStats {
  uint64_t collections;
  uint64_t young_collections;
  uint64_t full_collections;
  uint64_t allocated_bytes;
  uint64_t copied_bytes;
  uint64_t live_bytes;
  uint64_t peak_heap;
} AvantGcStats;

extern AvantGcStats avant_g_stats;
void avant_gc_stats(AvantGcStats *out);
void avant_gc_collect(void);
/* A thread that runs Avant code is registered (explicitly, or by its first
 * call into Avant); one that exits still registered is unregistered by its
 * exit (Stage 28 W17c), so a C thread that calls Avant callbacks needs no
 * unregistration of its own. A thread that exits inside a callback
 * (pthread_exit from C that Avant called) is unregistered and its frames
 * are freed too (Stage 28 W18C). */
void avant_gc_register_thread(void);
void avant_gc_unregister_thread(void);
/* Threads and the stop-the-world handshake (Stage 28 W17a, gc-design.md
 * section 4.4). A collection stops every other registered thread at a
 * safepoint (a collecting call, or avant_gc_poll) or finds it native.
 * avant_gc_requested is nonzero while a collection is requested or
 * running; generated code in a program that spawns or uses .parallel
 * tests it at every loop back-edge of a frame function and calls
 * avant_gc_poll when it is set (the thread stops there until the
 * collection is done). Around a C call that may block or run long
 * without touching the Avant heap, a thread is native:
 * avant_gc_native_begin before it, avant_gc_native_end after (they nest;
 * the end waits while a collection is in progress). Between them the
 * thread must not read or write a heap object it has not pinned. A C call
 * that blocks without them holds every thread's collection until it
 * returns (and deadlocks if it waits for another Avant thread); under
 * AVANT_GC_VERIFY=1 a collection that has waited AVANT_GC_VERIFY_STALL_MS
 * (10000 by default) reports the thread and its innermost Avant frame,
 * and keeps waiting (Stage 28 W18C). */
extern atomic_int avant_gc_requested;
void avant_gc_poll(void);
void avant_gc_native_begin(void);
void avant_gc_native_end(void);

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
char *avant_buf_take_str(void *buf);
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
/* The Buf's storage, for C to read or write up to its capacity. The
 * pointer is valid until the Buf next grows (an append past its capacity,
 * avant_buf_ensure, any Avant code that may push to it): a growth moves
 * the bytes and frees the old storage, so call avant_buf_bytes again after
 * anything that may grow the Buf. Under AVANT_GC_VERIFY=1 the storage a
 * growth replaced stays PROT_NONE until the next collection, so a use of
 * a pointer kept across the growth faults and is reported (Stage 28
 * W18C). */
uint8_t *avant_buf_bytes(void *buf);
int32_t avant_buf_starts(void *buf, const char *s);

char *avant_base64_encode(const char *s);
char *avant_base64_decode(const char *s);
int32_t avant_base64_encode_into(const char *s, void *buf);
int32_t avant_base64_decode_into(const char *s, void *buf);
int32_t avant_base64_take_status(void);
int32_t avant_crc32_i32(void *arr);
int32_t avant_sha256_word0(void *arr);
char *avant_zlib_compress(const char *s);
char *avant_zlib_uncompress(const char *s);
int32_t avant_zlib_compress_into(const char *s, void *buf);
int32_t avant_zlib_uncompress_into(const char *s, void *buf);
int32_t avant_zlib_take_status(void);
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
const char *avant_hash_get_str_slice(void *h, const char *s, int32_t start, int32_t stop, int32_t *found);
int32_t avant_hash_get_concat(void *h, const char *a, const char *b, int32_t *found);
int32_t avant_hash_has_str(void *h, const char *key);
int32_t avant_hash_has_i32k(void *h, int32_t key);
int32_t avant_hash_inc_str(void *h, const char *key, int32_t by);
int32_t avant_hash_inc_i32k(void *h, int32_t key, int32_t by);
int32_t *avant_hash_upsert_i32k(void *h, int32_t key, int32_t init, int32_t *found);
int32_t *avant_hash_upsert_i32(void *h, const char *key, int32_t init, int32_t *found);
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
void avant_trap_shift(int64_t amount, int32_t kind);
void avant_trap_to_i(double x);
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

int32_t avant_chosen_threads(void);
void avant_set_flag_threads(long n);
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
int32_t avant_process_run_split(const char *path, void *args, const char *out_path, const char *err_path);
int32_t avant_tool_run(const char *path, void *args, const char *out_path, const char *err_path);
int32_t avant_fs_unlink(const char *path);
int32_t avant_fs_remove_tree(const char *path);
int32_t avant_fs_chmod_x(const char *path);
int32_t avant_fs_mkdir_p(const char *path);
char *avant_fs_cwd(void);
char *avant_file_identity(const char *path);
int32_t avant_fs_copy(const char *src, const char *dst);
void avant_env_set(const char *name, const char *value);
char *avant_env_get(const char *name, int32_t *ok);
int32_t avant_file_exists(const char *path);
void *avant_dir_list(const char *path);
char *avant_tmp_dir(void);
char *avant_tmp_new_dir(void);
void avant_tmp_keep(void);
int32_t avant_job_start(const char *path, void *args, const char *out_path);
int32_t avant_job_wait(void);
int32_t avant_job_status(void);

void avant_cov_init(const char *map_path);
void avant_cov_hit(int32_t slot);

void *avant_json_parse(const char *s);
int32_t avant_json_take_status(void);
char *avant_json_take_pool(void);
void *avant_json_root(void *doc);
int32_t avant_json_kind(void *node);
void *avant_json_get(void *node, const char *key);
void *avant_json_at(void *node, int32_t i);
void *avant_json_first(void *node);
void *avant_json_next(void *node);
void *avant_json_f64s(void *node, const char *key);
int32_t avant_json_len(void *node);
int32_t avant_json_as_int(void *node);
int32_t avant_json_obj_int(void *node, const char *key);
double avant_json_as_f64(void *node);
double avant_json_obj_f64(void *node, const char *key);
int32_t avant_json_as_bool(void *node);
int32_t avant_json_obj_bool(void *node, const char *key);
char *avant_json_as_str(void *node);
char *avant_json_obj_str(void *node, const char *key);
char *avant_json_key_at(void *node, int32_t i);
char *avant_json_text(void *node);

int32_t avant_http_take_status(void);
int32_t avant_http_header_count(const char *data);
int32_t avant_http_header_has(const char *data, const char *name);
char *avant_http_header_at(const char *data, const char *name, int32_t index);
char *avant_http_header_name_at(const char *data, int32_t index);
char *avant_http_header_value_at(const char *data, int32_t index);
char *avant_http_req_method(void *req);
char *avant_http_req_target(void *req);
char *avant_http_req_body(void *req);
char *avant_http_req_headers(void *req);
void avant_http_req_respond(void *req, int32_t status, const char *head,
                            const char *body);
void *avant_http_listen(const char *host, int32_t port, int32_t max_body, int32_t loops, int32_t max_message);
int32_t avant_http_port(void *srv);
int32_t avant_http_serve(void *srv, void *state, void *cb, void *ws_cb);
int32_t avant_http_close(void *srv);
int32_t avant_http_req_is_ws(void *req);
int32_t avant_http_req_ws_upgrade(void *req);
int32_t avant_http_req_ws_id(void *req);
int32_t avant_ws_event_kind(void *ev);
int32_t avant_ws_event_conn_id(void *ev);
void *avant_ws_event_server(void *ev);
char *avant_ws_event_path(void *ev);
int32_t avant_ws_event_is_text(void *ev);
char *avant_ws_event_data(void *ev);
int32_t avant_ws_event_code(void *ev);
char *avant_ws_event_reason(void *ev);
void *avant_http_req_ws_server(void *req);
void *avant_http_req_ws_conn(void *req);
void *avant_ws_event_conn(void *ev);
int32_t avant_ws_send_text_direct(void *handle, const char *data);
int32_t avant_ws_send_text_bytes_direct(void *handle, const char *data);
int32_t avant_ws_send_bytes_direct(void *handle, const char *data);
int32_t avant_ws_close_direct(void *handle, int32_t code, const char *reason);
int32_t avant_ws_close_conn(void *srv, int32_t id, int32_t code, const char *reason);
int32_t avant_ws_send_text_by_id(int32_t id, const char *data);
int32_t avant_ws_send_text_bytes_by_id(int32_t id, const char *data);
int32_t avant_ws_send_bytes_by_id(int32_t id, const char *data);
int32_t avant_ws_close_by_id(int32_t id, int32_t code, const char *reason);
void *avant_ws_client_connect(const char *host, int32_t port, const char *target, const char *key_b64, int32_t timeout_ms);
char *avant_ws_client_recv(void *handle, int32_t timeout_ms);
int32_t avant_ws_client_last_is_text(void);
int32_t avant_ws_client_close_code(void *handle);
int32_t avant_ws_client_send_text(void *handle, const char *data);
int32_t avant_ws_client_send_bytes(void *handle, const char *data);
int32_t avant_ws_client_close(void *handle);
char *avant_ws_client_key(void);
char *avant_http_fetch(const char *host, int32_t port, const char *request,
                       int32_t timeout_ms);
int32_t avant_http_resp_status(const char *raw);
char *avant_http_resp_headers(const char *raw);
char *avant_http_resp_body(const char *raw);

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
