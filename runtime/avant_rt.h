#ifndef AVANT_RT_H
#define AVANT_RT_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Avant runtime — Immix-family collector.
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

/* An Avant `Int` at the runtime's C boundary:
 * every parameter, result or stored value the generated code or a `fun`
 * declaration treats as an Avant `Int` (64-bit) is declared avant_int, so
 * the width is said in one place. Values that are C's own (flags, kinds,
 * site numbers, element widths named in an entry, `found` out-parameters)
 * keep their exact C types. */
typedef int64_t avant_int;
#define AVANT_INT_MAX INT64_MAX
#define AVANT_INT_MIN INT64_MIN

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
/* `gc.hold { }` (std.gc): no collection this thread
 * would start runs between the two (nested pairs are one); the outermost
 * end runs what fell due inside. Other threads still collect. */
void avant_gc_hold_begin(void);
void avant_gc_hold_end(void);
void avant_pin(void *payload);
void avant_unpin(void *payload);
/* Number of distinct pinned payloads (refcounted pins share one entry). */
avant_int avant_pin_count(void);
/* C-layout field pins. A store of a reference into
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
/* Batched root registration: generated code calls
 * this once at frame entry with the frame's root count, stores each
 * slot address into the returned array at its fixed index, and the
 * collector scans the first count entries. avant_gc_root (one slot per
 * call) and avant_gc_reload (a slot read after a collecting call) are
 * a retired back end's ABI: the compiler emits neither (its call table
 * has no row for them), and they stay as deprecated entries for user C
 * until a release note retires them. */
void *avant_gc_root_begin(uint32_t count);
void *avant_gc_reload(void *slot);
/* Frames in the caller's stack: generated code keeps
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
 * what it names and rewrite it when the object moves.
 * At most 8 per thread. */
void avant_gc_add_root(void **slot);

/* Inline nursery fast path. Generated code allocates without a call when the
 * new end of its cursor (avant_gc_bump plus the aligned total) is at or
 * below the limit the runtime publishes: avant_gc_limit for an unshared
 * module, avant_gc_limit_mt for a shared one. It stores the new cursor,
 * writes the header (HEAP|YOUNG) and zeroes the payload; a shared
 * module's code also sets the block start bit through
 * avant_gc_starts/avant_gc_base. Nothing else: the limit is null
 * whenever the path may not run (no young hole, a program with threads
 * for an unshared module, a no-GC region, a collection pending; under
 * the stress mode it is null or spent whenever a site reads it,
 * avant_alloc_slow below), the runtime counts the bytes from the cursor
 * at its next entry, and an unshared module's objects get their start
 * bits from the runtime (when a collection promotes them or a lookup
 * needs them). The
 * limit is the hole's limit capped by the young budget, and the runtime
 * alone publishes or clears it. An older form of the path also tested
 * g_avant_shared, avant_gc_fast_ok and a null cursor, stored the cursor
 * in the block's record (avant_gc_block), set the start bit in every
 * module and kept four counters. */
/* The inline path's slow call: avant_alloc, except that it may return
 * null after making room in the thread's hole for exactly the object
 * asked for and publishing the limit for it; the caller then takes the
 * inline path without testing the limit again (only under
 * AVANT_GC_STRESS=1). Generated code
 * alone calls it. */
void *avant_alloc_slow(uint64_t nbytes, uint32_t type_id);
extern __thread uint8_t *avant_gc_bump;
extern __thread uint8_t *avant_gc_limit;
extern __thread uint8_t *avant_gc_limit_mt;
extern __thread uint8_t *avant_gc_base;
extern __thread uint64_t *avant_gc_starts;
extern uint64_t avant_nursery_held;
extern uint64_t avant_young_bytes;
extern uint64_t avant_since_gc;
void avant_type_map(uint32_t type_id, uint64_t word_bits);
/* One 64-word chunk (word_base a multiple of 64) of a
 * type map of any length, with an element stride in words (0: one
 * record; > 0: a buffer of repeated elements), the collector's TypeMap
 * (a runtime-private layout, not shipped). */
void avant_type_map_at(uint32_t type_id, uint32_t stride_words, uint32_t word_base,
                       uint64_t bits);
void avant_barrier(void *obj);
/* Barrier for a store through an address that may lie inside a heap object. */
void avant_barrier_at(void *addr);
/* The barrier with the slot stored into (card marking):
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
 * language's `nursery_reclaim()`, until the language removed that form. */
void avant_gc_collect_young(void);
/* A thread that runs Avant code is registered (explicitly, or by its first
 * call into Avant); one that exits still registered is unregistered by its
 * exit, so a C thread that calls Avant callbacks needs no
 * unregistration of its own. A thread that exits inside a callback
 * (pthread_exit from C that Avant called) is unregistered and its frames
 * are freed too. A thread may unregister between
 * callbacks and be registered again by its next one; unregistering drops
 * every reference the runtime held for it. */
void avant_gc_register_thread(void);
void avant_gc_unregister_thread(void);
/* Threads and the stop-the-world handshake. A collection stops every other
 * registered thread at a safepoint (a collecting call, or avant_gc_poll) or
 * finds it native.
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
 * and keeps waiting. */
extern atomic_int avant_gc_requested;
void avant_gc_poll(void);
void avant_gc_native_begin(void);
void avant_gc_native_end(void);

size_t avant_str_n(const char *s);
char *avant_str_empty(void);
char *avant_str_concat(const char *a, const char *b);
/* A C function's `String` result, made safe to store in a field: an
 * address inside a heap object comes back as a fresh copy of the string
 * there; anything else unchanged. */
char *avant_c_str(char *s);
/* The collected heap's reserved range, its first byte and its size (zero
 * until the heap is reserved): generated code tests a C function's
 * `String` result against it and calls avant_c_str only for an address
 * inside (compiler/llvm/emit_gc.av, ll_c_str). */
extern uint64_t avant_gc_heap_lo;
extern uint64_t avant_gc_heap_len;
char *avant_str_cat_int(const char *prefix, avant_int n);
char *avant_str_from_int_bytes(void *arr);
/* The names the seed's compiler calls (avant_str.c). */
char *avant_str_cat_i32(const char *prefix, avant_int n);
char *avant_str_from_i32_bytes(void *arr);
char *avant_str_repeat_byte(avant_int b, avant_int n);
char *avant_str_slice(const char *s, avant_int start, avant_int stop);
char *avant_str_from_int(avant_int n);
char *avant_str_from_u64(uint64_t n);
char *avant_str_from_float(double n);
char *avant_str_from_bool(int32_t n);
char *avant_str_from_byte(avant_int b);
char *avant_str_fmt_float(double v, avant_int prec);
int avant_fmt_float_text(char *out, double v);
void avant_puts_f64(double v);
void avant_puts_bool(int32_t v);
void avant_puts_i64(int64_t v);
void avant_puts_u64(uint64_t v);
void avant_puts_str(const char *s);
/* print, eputs, eprint (avant_io.c): HOW bit 1 a newline, bit 2 stderr. */
void avant_out_str(const char *s, int32_t how);
void avant_out_i64(int64_t v, int32_t how);
void avant_out_u64(uint64_t v, int32_t how);
void avant_out_f64(double v, int32_t how);
void avant_out_bool(int32_t v, int32_t how);
/* exit(status), its 0..255 checked by the generated code. */
__attribute__((noreturn)) void avant_exit(int32_t status);
/* The one exit of the process (avant_rt_traps.c): returns to the first
 * caller; any later one waits for good, native. */
__attribute__((visibility("hidden"))) void avant_exit_claim(void);
void avant_out_check(void);
int32_t avant_str_parse_i32(const char *s, avant_int base, int32_t *ok);
int64_t avant_str_parse_i64(const char *s, avant_int base, int32_t *ok);
uint64_t avant_str_parse_u64(const char *s, avant_int base, int32_t *ok);
double avant_str_parse_f64(const char *s);
double avant_str_parse_f64_slice(const char *s, avant_int start, avant_int stop);
int32_t avant_str_eq(const char *a, const char *b);
int32_t avant_str_cmp(const char *a, const char *b);
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
 * a pointer kept across the growth faults and is reported. */
uint8_t *avant_buf_bytes(void *buf);
int32_t avant_buf_starts(void *buf, const char *s);

char *avant_base64_encode(const char *s);
char *avant_base64_decode(const char *s);
avant_int avant_base64_encode_into(const char *s, void *buf);
avant_int avant_base64_decode_into(const char *s, void *buf);
avant_int avant_base64_take_status(void);
int32_t avant_crc32_bytes(const char *s);
int32_t avant_sha256_word0(const char *s);
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

/* Hash(K, V) (avant_hash.c). The descriptor avant_hash_create takes: the
 * key's width in bytes (AVANT_HASH_K_WIDTH: 1, 2, 4 or 8), whether an
 * integer key is signed, whether the key is a String or a struct the
 * generated code passes by address (AVANT_HASH_K_STRUCT); the value's
 * width (shifted by AVANT_HASH_V_SHIFT) and whether it is a reference.
 * avant_hash_create_typed also takes a struct key's fields (KLAYOUT, its
 * text below avant_hash.c's header), the key's and the value's sizes in
 * bytes, the type ids of the key and value buffers and of an Array of
 * the values: for a struct key or a struct value. */
#define AVANT_HASH_K_WIDTH 0xfu
#define AVANT_HASH_K_SIGNED 0x10u
#define AVANT_HASH_K_STR 0x20u
#define AVANT_HASH_K_STRUCT 0x40u
#define AVANT_HASH_V_SHIFT 8
#define AVANT_HASH_V_REF 0x1000u
/* Set by avant_hash_create_typed: the hash is an AvantHashX. */
#define AVANT_HASH_TYPED 0x2000u
/* The values are structs holding references. */
#define AVANT_HASH_V_WORDS 0x4000u
void *avant_hash_create(uint32_t desc);
void *avant_hash_create_typed(uint32_t desc, const char *klayout, uint32_t kw, uint32_t vw, uint32_t ktid,
                              uint32_t vtid, uint32_t atid);
void *avant_hash_find_xk(void *h, const void *key, int32_t *found);
void *avant_hash_put_xk(void *h, const void *key);
void *avant_hash_upsert_xk(void *h, const void *key, int32_t *found);
void avant_hash_set_ref_xk(void *h, const void *key, void *val);
void *avant_hash_take_xk(void *h, const void *key, int32_t *found);
void avant_hash_del_xk(void *h, const void *key);
void *avant_hash_slot_ik(void *h, avant_int key);
void *avant_hash_slot_sk(void *h, const char *key);
void *avant_hash_slot_xk(void *h, const void *key);
void *avant_hash_find_ik(void *h, avant_int key, int32_t *found);
void *avant_hash_find_sk(void *h, const char *key, int32_t *found);
void *avant_hash_put_ik(void *h, avant_int key);
void *avant_hash_put_sk(void *h, const char *key);
void *avant_hash_upsert_ik(void *h, avant_int key, int32_t *found);
void *avant_hash_upsert_sk(void *h, const char *key, int32_t *found);
void avant_hash_set_ref_ik(void *h, avant_int key, void *val);
void avant_hash_set_ref_sk(void *h, const char *key, void *val);
void *avant_hash_take_ik(void *h, avant_int key, int32_t *found);
void *avant_hash_take_sk(void *h, const char *key, int32_t *found);
void *avant_hash_keys(void *h);
void *avant_hash_values(void *h);
avant_int avant_hash_size(void *h);
void avant_hash_clear(void *h);
void avant_hash_del_sk(void *h, const char *key);
void avant_hash_del_ik(void *h, avant_int key);
avant_int avant_hash_inc_slice(void *h, const char *s, avant_int start, avant_int stop);
const char *avant_hash_last_key(void);
const char *avant_hash_get_str_slice(void *h, const char *s, avant_int start, avant_int stop, int32_t *found);
avant_int avant_hash_get_concat(void *h, const char *a, const char *b, int32_t *found);
/* The names the seed's compiler calls (avant_hash.c, the last section). */
void avant_hash_del(void *h, const char *key);
void avant_hash_del_i32k(void *h, avant_int key);

void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id);
void avant_array_push_ptr(void *arr, void *elem, uint32_t buf_type_id);
void avant_array_set_ptr(void *arr, avant_int i, void *elem);
void *avant_array_get_ptr(void *arr, avant_int i);
void *avant_array_pop_ptr(void *arr);
void avant_array_push_i32(void *arr, int32_t v);
void avant_array_push_i64(void *arr, int64_t v);
void avant_array_clear(void *arr);
void avant_array_zero_range(void *arr, avant_int lo, avant_int hi, uint64_t elem_size);
void avant_array_reserve(void *arr, avant_int n, uint64_t elem_size, uint32_t buf_type_id);
void *avant_array_pop_slot(void *arr, uint64_t elem_size);
/* The freeze: an Array a running task
 * captured cannot change until every such task has ended. A spawn and a
 * .parallel freeze each captured Array (and, depth levels down, the
 * arrays an array of arrays holds) before the task starts and thaw it
 * when the task ends. The count lives in the array's cap word, whose
 * sign bit is free (a capacity is at most AVANT_SIZE_MAX): while frozen
 * the word is AVANT_ARRAY_FROZEN | count, and every operation that can
 * change the array's length, buffer or elements tests the sign first
 * and traps (avant_trap_frozen). The last thaw writes back the
 * capacity, read from the buffer's header. */
#define AVANT_ARRAY_FROZEN ((int32_t)0x80000000)
void avant_array_freeze(void *arr, int32_t depth);
void avant_array_thaw(void *arr, int32_t depth, uint64_t elem_size);
/* What a frozen array refuses: the operation avant_trap_frozen names. */
#define AVANT_FROZEN_PUSH 0
#define AVANT_FROZEN_POP 1
#define AVANT_FROZEN_CLEAR 2
#define AVANT_FROZEN_FILL 3
#define AVANT_FROZEN_SORT 4
#define AVANT_FROZEN_REVERSE 5
#define AVANT_FROZEN_RESERVE 6
#define AVANT_FROZEN_STORE 7
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

/* The safety floor: language traps. Each prints one
 * diagnostic to stderr and exits 1. They never return. */
__attribute__((noreturn, cold)) void avant_trap_overflow_in(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_div_zero_in(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_bounds_in(avant_int index, avant_int size, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_empty_at(int32_t what, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_frozen(int32_t op);
__attribute__((noreturn, cold)) void avant_trap_frozen_at(int32_t op, int32_t site);
/* `with` on a poisoned mutex. */
__attribute__((noreturn, cold)) void avant_trap_poisoned(void);
__attribute__((noreturn, cold)) void avant_trap_poisoned_at(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_bounds_at(avant_int index, avant_int size, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_div_zero_at(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_overflow_at(int32_t site);
__attribute__((noreturn, cold)) void avant_trap_shift_at(int64_t amount, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_to_i_at(double x, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_c_range_at(int64_t value, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_convert_at(int64_t value, int32_t kind, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_byte_at(avant_int value, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_negative_at(avant_int n, int32_t what, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_overflow(void);
__attribute__((noreturn, cold)) void avant_trap_c_range(int64_t value, int32_t kind);
__attribute__((noreturn, cold)) void avant_trap_byte(avant_int value);
__attribute__((noreturn, cold)) void avant_trap_precision(avant_int prec);
__attribute__((noreturn, cold)) void avant_trap_slice(avant_int start, avant_int stop, avant_int size);
__attribute__((noreturn, cold)) void avant_trap_negative(avant_int n, int32_t what);
__attribute__((noreturn, cold)) void avant_trap_exit_status_at(avant_int status, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_c_null_at(const char *name, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_enum_at(int64_t value, const char *name, int32_t site);
__attribute__((noreturn, cold)) void avant_trap_expect_at(const char *why, int32_t site);

/* The size ceiling: a String,
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
 * a signal number): the value, or a trap naming the C type (the correct
 * value or a trap). */
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
avant_int avant_usable_cpus(void);
void avant_set_flag_threads(long n);
void *avant_spawn(void *fn, void *arg);
/* A spawn whose block may trap without ending the program: fail, when not null, is the block's end on a trap (the
 * captures' thaw and the captured channels' close), called with the
 * record on the worker after the trap returned to the task's entry. */
void *avant_spawn_f(void *fn, void *arg, void *fail);
avant_int avant_join(void *handle);
/* After avant_join: the trap that ended the task (its text, without the
 * site), or null when the block returned; and the trap's site, "" when
 * none. Static C strings, valid for the run (an Avant String). */
char *avant_join_what(void *handle);
char *avant_join_site(void *handle);
/* A C callback's thunk in a program that spawns brackets its call:
 * a trap inside a callback stays a process trap. */
void avant_task_cb_enter(void);
void avant_task_cb_leave(void);
/* A `pin` block's pin and a `gc.hold` in a program that spawns: the
 * collector's avant_pin, avant_unpin, avant_gc_hold_begin and _end,
 * noted on the thread so that a trap ending a task gives them back. */
void avant_task_pin(void *payload);
void avant_task_unpin(void *payload);
void avant_task_hold_begin(void);
void avant_task_hold_end(void);
/* `m.with { }`'s lock: 1 when a task trapped while holding the
 * mutex (it is poisoned), else 0; the lock is held either way. */
int32_t avant_gate_lock_with(void *obj, uint64_t off);
void avant_gate_clear_poison(void *obj, uint64_t off);
/* Mutex.poisoned: 1 when a task trapped while holding the gate. */
int32_t avant_gate_poisoned(void *obj, uint64_t off);
/* The recovery point's two entries for avant_rt_traps.c (hidden). */
__attribute__((visibility("hidden"))) int avant_task_can_recover(void);
__attribute__((visibility("hidden"), noreturn)) void avant_task_trapped(const char *what, const char *site);
__attribute__((visibility("hidden"), noreturn)) void avant_trap_text(const char *what);
/* A recovery point around one call of an Avant function from C
 * (std.http's handlers): 0 when fn returned, 1 when it trapped. */
int avant_task_recover_call(int32_t (*fn)(void *, void *), void *a, void *b);
void avant_parallel_for(avant_int n, void *fn, void *ctx);
uint64_t avant_gate_new(void);
/* The gate functions take the object holding the gate word and the word's
 * byte offset in it (avant_task.c). */
void avant_gate_lock(void *obj, uint64_t off);
void avant_gate_unlock(void *obj, uint64_t off);
void avant_gate_wait(void *obj, uint64_t off);
void avant_gate_signal(void *obj, uint64_t off);
void avant_gate_broadcast(void *obj, uint64_t off);
int64_t avant_clock_ms(void);
int64_t avant_clock_us(void);

void avant_io_init_argv(int32_t argc, void *argv);
void *avant_argv(void);
char *avant_sys_read(const char *path);
avant_int avant_sys_write(const char *path, const char *body);
void *avant_sys_list(const char *path);
avant_int avant_sys_run(const char *path, void *args, const char *out_path);
avant_int avant_sys_errno(void);
avant_int avant_main_fail(const char *text);
avant_int avant_sys_stage(void);
avant_int avant_sys_signal(void);
char *avant_sys_strerror(avant_int code);
char *avant_sys_strsignal(avant_int sig);
avant_int avant_fs_run(const char *path, void *args);
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
/* A failed check (std.testing): the process exits 1 if it would exit 0. */
void avant_test_fail(void);
/* std.testing's report line: all of it to standard output, or the process stops. */
void avant_test_write(const char *s, avant_int n);
void avant_tmp_keep(void);
avant_int avant_job_start(const char *path, void *args, const char *out_path);
avant_int avant_job_wait(void);
avant_int avant_job_status(void);
avant_int avant_job_signal(void);

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
avant_int avant_http_req_ws_upgrade(void *req, avant_int route);
avant_int avant_http_req_ws_id(void *req);
avant_int avant_ws_event_kind(void *ev);
avant_int avant_ws_event_conn_id(void *ev);
avant_int avant_ws_event_route(void *ev);
char *avant_ws_event_path(void *ev);
avant_int avant_ws_event_is_text(void *ev);
char *avant_ws_event_data(void *ev);
avant_int avant_ws_event_code(void *ev);
char *avant_ws_event_reason(void *ev);
void *avant_http_req_ws_conn(void *req);
void *avant_ws_event_conn(void *ev);
avant_int avant_ws_send_text_direct(void *handle, avant_int id, const char *data);
avant_int avant_ws_send_text_bytes_direct(void *handle, avant_int id, const char *data);
avant_int avant_ws_send_bytes_direct(void *handle, avant_int id, const char *data);
avant_int avant_ws_close_direct(void *handle, avant_int id, avant_int code, const char *reason);
int32_t avant_ws_close_conn(void *srv, avant_int id, avant_int code, const char *reason);
avant_int avant_ws_send_text_by_id(avant_int id, const char *data);
avant_int avant_ws_send_text_bytes_by_id(avant_int id, const char *data);
avant_int avant_ws_send_bytes_by_id(avant_int id, const char *data);
avant_int avant_ws_close_by_id(avant_int id, avant_int code, const char *reason);
avant_int avant_ws_client_connect(const char *host, avant_int port, const char *target, const char *key_b64, avant_int timeout_ms);
char *avant_ws_client_recv(avant_int id, avant_int timeout_ms);
avant_int avant_ws_client_last_is_text(void);
avant_int avant_ws_client_close_code(avant_int id);
avant_int avant_ws_client_send_text(avant_int id, const char *data);
avant_int avant_ws_client_send_bytes(avant_int id, const char *data);
avant_int avant_ws_client_close(avant_int id);
char *avant_ws_client_key(void);
char *avant_http_fetch(const char *host, avant_int port, const char *request,
                       avant_int timeout_ms);
avant_int avant_http_resp_status(const char *raw);
char *avant_http_resp_headers(const char *raw);
char *avant_http_resp_body(const char *raw);

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
