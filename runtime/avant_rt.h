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

/* An Avant `Int` at the runtime's C boundary (Stage 29 W-INT1, D314):
 * every parameter, result or stored value the generated code or a `fun`
 * declaration treats as an Avant `Int` is declared avant_int, so the
 * width is said in one place. AVANT_INT_BITS is 64, the language's `Int`;
 * runtime/build.sh also builds one copy with -DAVANT_INT_BITS=32
 * (runtime/build/seed-abi/), the ABI of the v0.40.0 seed, which only B
 * links: B's code comes from the seed's 32-bit code generator (the
 * bootstrap, tools/letters.sh). That copy goes when a release with the
 * 64-bit `Int` is the seed. Values that are C's own (flags, kinds, site
 * numbers, element widths named in an entry, `found` out-parameters)
 * keep their exact C types. */
#ifndef AVANT_INT_BITS
#define AVANT_INT_BITS 64
#endif
#if AVANT_INT_BITS == 32
typedef int32_t avant_int;
#define AVANT_INT_MAX INT32_MAX
#define AVANT_INT_MIN INT32_MIN
#elif AVANT_INT_BITS == 64
typedef int64_t avant_int;
#define AVANT_INT_MAX INT64_MAX
#define AVANT_INT_MIN INT64_MIN
#else
#error "AVANT_INT_BITS is 32 or 64"
#endif

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
/* `gc.hold { }` (std.gc, Stage 29 W1c2, D305): no collection this thread
 * would start runs between the two (nested pairs are one); the outermost
 * end runs what fell due inside. Other threads still collect. */
void avant_gc_hold_begin(void);
void avant_gc_hold_end(void);
void avant_pin(void *payload);
void avant_unpin(void *payload);
/* Number of distinct pinned payloads (refcounted pins share one entry). */
avant_int avant_pin_count(void);
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
/* A young collection on request: std.gc's `gc.collect()` (deferred to the
 * end of an enclosing hold), and the collector's own tests (declared with
 * `lib` as avant_gc_collect is). It was avant_nursery_reclaim, the
 * language's `nursery_reclaim()`, until Stage 29 removed that form (D298,
 * D305). */
void avant_gc_collect_young(void);
/* A thread that runs Avant code is registered (explicitly, or by its first
 * call into Avant); one that exits still registered is unregistered by its
 * exit (Stage 28 W17c), so a C thread that calls Avant callbacks needs no
 * unregistration of its own. A thread that exits inside a callback
 * (pthread_exit from C that Avant called) is unregistered and its frames
 * are freed too (Stage 28 W18C). A thread may unregister between
 * callbacks and be registered again by its next one; unregistering drops
 * every reference the runtime held for it (Stage 29 W1b, O70). */
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
char *avant_str_cat_i32(const char *prefix, avant_int n);
char *avant_str_from_i32_bytes(void *arr);
char *avant_str_repeat_byte(avant_int b, avant_int n);
char *avant_str_slice(const char *s, avant_int start, avant_int stop);
char *avant_str_from_int(avant_int n);
char *avant_str_from_i64(int64_t n);
char *avant_str_from_u64(uint64_t n);
char *avant_str_from_float(double n);
char *avant_str_from_bool(int32_t n);
char *avant_str_from_byte(avant_int b);
char *avant_str_fmt_float(double v, avant_int prec);
int avant_fmt_float_text(char *out, double v);
void avant_puts_f64(double v);
void avant_puts_bool(int32_t v);
void avant_puts_str(const char *s);
int32_t avant_str_parse_i32(const char *s, avant_int base, int32_t *ok);
int64_t avant_str_parse_i64(const char *s, avant_int base, int32_t *ok);
uint64_t avant_str_parse_u64(const char *s, avant_int base, int32_t *ok);
double avant_str_parse_f64(const char *s);
double avant_str_parse_f64_slice(const char *s, avant_int start, avant_int stop);
int32_t avant_str_eq(const char *a, const char *b);
avant_int avant_str_size(const char *s);
avant_int avant_str_byte(const char *s, avant_int i);
int32_t avant_checksum_str(const char *s);
int32_t avant_checksum_f64(double v);
char *avant_str_strip(const char *s, int32_t mode);
void *avant_str_split(const char *s, const char *sep);
void *avant_str_fields(const char *s);
char *avant_str_from_utf8(const char *s, int32_t *found);
void *avant_bytes_to_buf(const char *s);

void *avant_buf_new(avant_int cap);
void avant_buf_push_byte(void *buf, avant_int b);
void avant_buf_push_str(void *buf, const char *s);
void avant_buf_push_slice(void *buf, const char *s, avant_int start, avant_int stop);
void avant_buf_push_fmt_f(void *buf, double v, avant_int prec);
void avant_buf_push_fmt_i(void *buf, avant_int n);
char *avant_buf_to_str(void *buf);
char *avant_buf_take_str(void *buf);
avant_int avant_buf_size(void *buf);
void avant_buf_clear(void *buf);
void avant_buf_ensure(void *buf, avant_int n);
void avant_bytes_ensure(void *buf, avant_int n);
void avant_buf_append(void *buf, const void *p, int32_t n);
void avant_buf_set_size(void *buf, avant_int n);
void avant_buf_set_ident(void *buf, avant_int n);
/* Append-only fills of `buf`: copy len bytes from src_off to dst_off, then
 * write one extra byte. Source and dest are the same buffer. */
void avant_buf_fill_appends(void *buf, void *dst_offs, void *src_offs, void *lens, void *extras, avant_int n);
/* Copy n slices from src into dst. Offsets are byte indexes. */
void avant_buf_copy_ranges(void *dst, void *src, void *dst_offs, void *src_offs, void *lens, avant_int n);
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
avant_int avant_base64_encode_into(const char *s, void *buf);
avant_int avant_base64_decode_into(const char *s, void *buf);
avant_int avant_base64_take_status(void);
int32_t avant_crc32_i32(void *arr);
int32_t avant_sha256_word0(void *arr);
char *avant_zlib_compress(const char *s);
char *avant_zlib_uncompress(const char *s);
avant_int avant_zlib_compress_into(const char *s, void *buf);
avant_int avant_zlib_uncompress_into(const char *s, void *buf);
avant_int avant_zlib_take_status(void);
char *avant_sha256(const char *s);

void *avant_re_compile(const char *pat, int32_t flags);
int32_t avant_re_find(void *re, const char *s, int32_t from);
int32_t avant_re_m0(void);
int32_t avant_re_m1(void);
int32_t avant_re_c0(void);
int32_t avant_re_c1(void);
void avant_re_set_span(int32_t m0, int32_t m1, int32_t c0, int32_t c1);
int32_t avant_re_count(const char *pat, const char *s, int32_t caseless);
avant_int avant_wide_op(avant_int op, avant_int a_lo, avant_int a_hi, avant_int b_lo, avant_int b_hi);
avant_int avant_wide_hi(void);

void *avant_hash_new(int32_t key_kind, int32_t val_kind);
avant_int avant_hash_size(void *h);
void avant_hash_clear(void *h);
void avant_hash_set_i32(void *h, const char *key, avant_int val);
void avant_hash_set_str(void *h, const char *key, const char *val);
void avant_hash_set_i32k_i32(void *h, avant_int key, avant_int val);
avant_int avant_hash_get_i32(void *h, const char *key, int32_t *found);
const char *avant_hash_get_str(void *h, const char *key, int32_t *found);
avant_int avant_hash_get_i32k_i32(void *h, avant_int key, int32_t *found);
avant_int avant_hash_take_i32(void *h, const char *key, int32_t *found);
char *avant_hash_take_str(void *h, const char *key, int32_t *found);
avant_int avant_hash_take_i32k_i32(void *h, avant_int key, int32_t *found);
void avant_hash_del(void *h, const char *key);
void avant_hash_del_i32k(void *h, avant_int key);
avant_int avant_hash_inc_slice(void *h, const char *s, avant_int start, avant_int stop);
const char *avant_hash_last_key(void);
const char *avant_hash_get_str_slice(void *h, const char *s, avant_int start, avant_int stop, int32_t *found);
avant_int avant_hash_get_concat(void *h, const char *a, const char *b, int32_t *found);
int32_t avant_hash_has_str(void *h, const char *key);
int32_t avant_hash_has_i32k(void *h, avant_int key);
avant_int avant_hash_inc_str(void *h, const char *key, avant_int by);
avant_int avant_hash_inc_i32k(void *h, avant_int key, avant_int by);
avant_int *avant_hash_upsert_i32k(void *h, avant_int key, avant_int init, int32_t *found);
avant_int *avant_hash_upsert_i32(void *h, const char *key, avant_int init, int32_t *found);
avant_int avant_hash_get_or_i32(void *h, const char *key, avant_int fallback);
const char *avant_hash_get_or_str(void *h, const char *key, const char *fallback);
avant_int avant_hash_get_or_i32k_i32(void *h, avant_int key, avant_int fallback);
void *avant_hash_keys_str(void *h);
void *avant_hash_keys_i32(void *h);
void *avant_hash_vals_str(void *h);
void *avant_hash_vals_i32(void *h);

void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id);
void avant_array_push_ptr(void *arr, void *elem, uint32_t buf_type_id);
void avant_array_set_ptr(void *arr, avant_int i, void *elem);
void *avant_array_get_ptr(void *arr, avant_int i);
void *avant_array_pop_ptr(void *arr);
void avant_array_push_i32(void *arr, int32_t v);
void avant_array_push_i64(void *arr, int64_t v);
void avant_array_clear(void *arr);
void avant_array_reserve(void *arr, avant_int n, uint64_t elem_size, uint32_t buf_type_id);
void *avant_array_pop_slot(void *arr, uint64_t elem_size);
void avant_array_sort_i32(void *arr);
void avant_array_sort_u32(void *arr);
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
avant_int avant_array_index_bool(void *arr, int32_t v);
avant_int avant_array_index_i32(void *arr, int32_t v);
avant_int avant_array_index_i64(void *arr, int64_t v);
avant_int avant_array_index_u64(void *arr, uint64_t v);
avant_int avant_array_index_f64(void *arr, double v);
avant_int avant_array_index_str(void *arr, const char *v);
avant_int avant_array_index_ptr(void *arr, void *v);
int32_t avant_array_min_i32(void *arr, int32_t *found);
uint32_t avant_array_min_u32(void *arr, int32_t *found);
uint32_t avant_array_max_u32(void *arr, int32_t *found);
uint32_t avant_array_sum_u32(void *arr);
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
void avant_array_fill_i64(void *arr, int64_t v);
void avant_array_fill_u8(void *arr, int32_t v);
/* Burrows–Wheeler of an Int buffer of bytes. Scratch: sa, rank, tmp
 * (each size >= n) and count (size > every rank, so size >= n).
 * `out` receives n bytes. Returns the original-string slot. */
avant_int avant_bwt_transform(void *input, void *sa, void *rank, void *tmp, void *count, void *out);
/* Inverse. `next` is scratch of size >= n. `out` receives n bytes. */
void avant_bwt_inverse(void *bwt, void *next, void *out, avant_int start);

/* Wave B safety floor (Q67-A/Q68-A): language traps. Each prints one
 * diagnostic to stderr and exits 1. They never return. */
__attribute__((noreturn, cold)) void avant_trap_overflow_in(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_div_zero_in(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_bounds_in(avant_int index, avant_int size, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_empty_at(int32_t what, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_bounds_at(avant_int index, avant_int size, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_div_zero_at(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_overflow_at(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_shift_at(int64_t amount, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_to_i_at(double x, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_c_range_at(int64_t value, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_convert_at(int64_t value, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_byte_at(avant_int value, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_negative_at(avant_int n, int32_t what, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_bounds(avant_int index, avant_int size);
__attribute__((noreturn, cold)) void avant_trap_div_zero(void);
__attribute__((noreturn, cold)) void avant_trap_overflow(void);
__attribute__((noreturn, cold)) void avant_trap_shift(int64_t amount, int32_t kind);
__attribute__((noreturn, cold)) void avant_trap_to_i(double x);
__attribute__((noreturn, cold)) void avant_trap_c_range(int64_t value, int32_t kind);
__attribute__((noreturn, cold)) void avant_trap_convert(int64_t value, int32_t kind);
__attribute__((noreturn, cold)) void avant_trap_byte(avant_int value);
__attribute__((noreturn, cold)) void avant_trap_precision(avant_int prec);
__attribute__((noreturn, cold)) void avant_trap_slice(avant_int start, avant_int stop, avant_int size);
__attribute__((noreturn, cold)) void avant_trap_negative(avant_int n, int32_t what);
__attribute__((noreturn, cold)) void avant_trap_exit_status_at(avant_int status, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_c_null_at(const char *name, int32_t site);

/* The size ceiling (Stage 29 W-INT1, review 6 section 1.6): a String,
 * Bytes or Buf holds at most AVANT_SIZE_MAX bytes and an Array at most
 * AVANT_SIZE_MAX elements, because their headers keep 32-bit sizes; a
 * Hash at most AVANT_HASH_SLOTS_MAX slots (a power of two). Every size or
 * capacity the runtime computes is checked before anything is allocated
 * or written, and one that would pass its ceiling traps with
 * "avant runtime: Buf would exceed 2147483647 bytes" (no wrapped size, no
 * write past a capacity). The check is one compare on a growth or
 * allocation path; the fast paths already compare against the capacity.
 * file_read of a larger file is an IoError (EFBIG). */
#define AVANT_SIZE_MAX 2147483647
#define AVANT_HASH_SLOTS_MAX 1073741824
#define AVANT_SIZE_STRING 0
#define AVANT_SIZE_BUF 1
#define AVANT_SIZE_ARRAY 2
#define AVANT_SIZE_HASH 3
__attribute__((noreturn, cold)) void avant_trap_size(int32_t kind);
__attribute__((noreturn, cold)) void avant_trap_size_at(int32_t kind, int32_t site);
/* An Avant Int the runtime passes on to a C int (an HTTP status, an errno,
 * a signal number): the value, or a trap naming the C type (Stage 29
 * W-INT1; D215, the correct value or a trap). Free in the seed-ABI copy. */
static inline int32_t avant_c_int(avant_int v) {
  if (__builtin_expect(v < INT32_MIN || v > INT32_MAX, 0)) {
    avant_trap_c_range((int64_t)v, 6);
  }
  return (int32_t)v;
}
static inline void avant_size_check(uint64_t n, int32_t kind) {
  if (__builtin_expect(n > (uint64_t)AVANT_SIZE_MAX, 0)) {
    avant_trap_size(kind);
  }
}
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

avant_int avant_chosen_threads(void);
void avant_set_flag_threads(long n);
void *avant_spawn(void *fn, void *arg);
avant_int avant_join(void *handle);
void avant_parallel_for(avant_int n, void *fn, void *ctx);
uint64_t avant_gate_new(void);
void avant_gate_lock(uint64_t gate);
void avant_gate_unlock(uint64_t gate);
void avant_gate_wait(uint64_t gate);
void avant_gate_signal(uint64_t gate);
void avant_gate_broadcast(uint64_t gate);
int32_t avant_now_ms(void);
int64_t avant_clock_ms(void);
int64_t avant_clock_us(void);
int32_t avant_now_us(void);

void avant_io_init_argv(int32_t argc, void *argv);
void *avant_argv(void);
char *avant_file_read(const char *path, int32_t *ok);
char *avant_sys_read(const char *path);
avant_int avant_sys_write(const char *path, const char *body);
void *avant_sys_list(const char *path);
avant_int avant_sys_run(const char *path, void *args, const char *out_path);
avant_int avant_sys_errno(void);
avant_int avant_sys_stage(void);
avant_int avant_sys_signal(void);
char *avant_sys_strerror(avant_int code);
char *avant_sys_strsignal(avant_int sig);
avant_int avant_fs_run(const char *path, void *args);
avant_int avant_fs_write(const char *path, const char *body);
void *avant_fs_list(const char *path);
avant_int avant_process_run_split(const char *path, void *args, const char *out_path, const char *err_path);
avant_int avant_tool_run(const char *path, void *args, const char *out_path, const char *err_path);
avant_int avant_fs_unlink(const char *path);
avant_int avant_fs_remove_tree(const char *path);
avant_int avant_fs_chmod_x(const char *path);
avant_int avant_fs_mkdir_p(const char *path);
char *avant_fs_cwd(void);
char *avant_file_identity(const char *path);
avant_int avant_fs_copy(const char *src, const char *dst);
void avant_env_set(const char *name, const char *value);
char *avant_env_get(const char *name, int32_t *ok);
int32_t avant_file_exists(const char *path);
char *avant_tmp_dir(void);
char *avant_tmp_new_dir(void);
/* A failed check (std.testing, D84): the process exits 1 if it would exit 0. */
void avant_test_fail(void);
void avant_tmp_keep(void);
avant_int avant_job_start(const char *path, void *args, const char *out_path);
avant_int avant_job_wait(void);
avant_int avant_job_status(void);

void avant_cov_init(const char *map_path);
void avant_cov_hit(int32_t slot);

void *avant_json_parse(const char *s);
avant_int avant_json_take_status(void);
char *avant_json_take_pool(void);
void *avant_json_root(void *doc);
avant_int avant_json_kind(void *node);
void *avant_json_get(void *node, const char *key);
void *avant_json_at(void *node, avant_int i);
void *avant_json_first(void *node);
void *avant_json_next(void *node);
void *avant_json_f64s(void *node, const char *key);
avant_int avant_json_len(void *node);
avant_int avant_json_as_int(void *node);
avant_int avant_json_obj_int(void *node, const char *key);
double avant_json_as_f64(void *node);
double avant_json_obj_f64(void *node, const char *key);
avant_int avant_json_as_bool(void *node);
avant_int avant_json_obj_bool(void *node, const char *key);
char *avant_json_as_str(void *node);
char *avant_json_obj_str(void *node, const char *key);
char *avant_json_key_at(void *node, avant_int i);
char *avant_json_text(void *node);

avant_int avant_http_take_status(void);
avant_int avant_http_header_count(const char *data);
avant_int avant_http_header_has(const char *data, const char *name);
char *avant_http_header_at(const char *data, const char *name, avant_int index);
char *avant_http_header_name_at(const char *data, avant_int index);
char *avant_http_header_value_at(const char *data, avant_int index);
char *avant_http_req_method(void *req);
char *avant_http_req_target(void *req);
char *avant_http_req_body(void *req);
char *avant_http_req_headers(void *req);
void avant_http_req_respond(void *req, avant_int status, const char *head,
                            const char *body);
void *avant_http_listen(const char *host, avant_int port, avant_int max_body, avant_int loops, avant_int max_message);
avant_int avant_http_port(void *srv);
avant_int avant_http_serve(void *srv, void *state, void *cb, void *ws_cb);
avant_int avant_http_close(void *srv);
avant_int avant_http_req_is_ws(void *req);
avant_int avant_http_req_ws_upgrade(void *req);
avant_int avant_http_req_ws_id(void *req);
avant_int avant_ws_event_kind(void *ev);
avant_int avant_ws_event_conn_id(void *ev);
void *avant_ws_event_server(void *ev);
char *avant_ws_event_path(void *ev);
avant_int avant_ws_event_is_text(void *ev);
char *avant_ws_event_data(void *ev);
avant_int avant_ws_event_code(void *ev);
char *avant_ws_event_reason(void *ev);
void *avant_http_req_ws_server(void *req);
void *avant_http_req_ws_conn(void *req);
void *avant_ws_event_conn(void *ev);
avant_int avant_ws_send_text_direct(void *handle, const char *data);
avant_int avant_ws_send_text_bytes_direct(void *handle, const char *data);
avant_int avant_ws_send_bytes_direct(void *handle, const char *data);
avant_int avant_ws_close_direct(void *handle, avant_int code, const char *reason);
int32_t avant_ws_close_conn(void *srv, avant_int id, avant_int code, const char *reason);
avant_int avant_ws_send_text_by_id(avant_int id, const char *data);
avant_int avant_ws_send_text_bytes_by_id(avant_int id, const char *data);
avant_int avant_ws_send_bytes_by_id(avant_int id, const char *data);
avant_int avant_ws_close_by_id(avant_int id, avant_int code, const char *reason);
void *avant_ws_client_connect(const char *host, avant_int port, const char *target, const char *key_b64, avant_int timeout_ms);
char *avant_ws_client_recv(void *handle, avant_int timeout_ms);
avant_int avant_ws_client_last_is_text(void);
avant_int avant_ws_client_close_code(void *handle);
avant_int avant_ws_client_send_text(void *handle, const char *data);
avant_int avant_ws_client_send_bytes(void *handle, const char *data);
avant_int avant_ws_client_close(void *handle);
char *avant_ws_client_key(void);
char *avant_http_fetch(const char *host, avant_int port, const char *request,
                       avant_int timeout_ms);
avant_int avant_http_resp_status(const char *raw);
char *avant_http_resp_headers(const char *raw);
char *avant_http_resp_body(const char *raw);

#define AVANT_HASH_KEY_I32 0
#define AVANT_HASH_KEY_STR 1
#define AVANT_HASH_VAL_I32 0
#define AVANT_HASH_VAL_STR 1

static inline AvantHeader *avant_header(void *payload) {
  return ((AvantHeader *)payload) - 1;
}

/* A fresh heap string of N bytes plus its NUL (not tagged, not filled):
 * N past the ceiling traps before the allocation. */
static inline char *avant_str_alloc(uint64_t n) {
  avant_size_check(n, AVANT_SIZE_STRING);
  return (char *)avant_alloc(n + 1, AVANT_TYPE_BYTES);
}

static inline void avant_str_tag(void *payload) {
  if (payload) {
    avant_header(payload)->flags =
        (uint16_t)(avant_header(payload)->flags | AVANT_FLAG_STRMETA);
  }
}

#endif
