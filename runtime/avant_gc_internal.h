#ifndef AVANT_GC_INTERNAL_H
#define AVANT_GC_INTERNAL_H

#include "avant_rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* The collector's internal header (Stage 28 W12). Private to the
 * collector's units, one per concern:
 *   avant_gc_heap.c     the spaces' block lists and the block table,
 *                       object lookup, forwarding, the work list, type
 *                       maps, gc_die and gc_xrealloc
 *   avant_gc_alloc.c    block supply, bump cursors, the inline fast
 *                       path's state, avant_alloc and avant_bump_alloc
 *   avant_gc_los.c      the large-object space
 *   avant_gc_remset.c   the remembered set and the store barriers
 *   avant_gc_young.c    the young collection (copying nursery, probe,
 *                       pinned objects promoted in place)
 *   avant_gc_mark.c     the full collection's mark and block sweep
 *   avant_gc_defrag.c   mature defragmentation, block sets
 *   avant_gc_stack.c    the conservative scans of the C stack
 *   avant_gc_verify.c   AVANT_GC_VERIFY's checkers
 *   avant_gc_collect.c  the driver, its policy and the environment knobs
 *   avant_gc_stats.c    AvantGcStats, the counters, AVANT_GC_STATS
 *   avant_gc_roots.c    threads, frames and root slots, quiet leaves,
 *                       pins, defer regions, the world lock
 * They fold into one avant_rt.o (runtime/build.sh), which keeps global
 * only the symbols avant_rt.exports lists, so nothing declared here
 * becomes part of the object's surface unless it already was. Other
 * runtime units and user programs see avant_rt.h (the shipped ABI) and
 * avant_gc.h (what the rest of the runtime may call); never this file.
 * Functions a unit shares with another carry the gc_ prefix; variables
 * keep their g_ names. A few shared functions and variables without the
 * prefix (work_push, fwd_get, copy_from_slot, g_live, ...) were global
 * before W12 and are part of avant_rt.o's surface, so they keep their
 * names while that surface is frozen. Each block of declarations below
 * names the unit that defines it. */

/*
 * Precise Immix-family collector.
 *
 * Geometry: 32 KiB blocks, 256-byte lines. Object-start bits identify
 * headers. Tracing is precise (type maps + a shadow stack of local slots).
 *
 * GenImmix: bump nursery (copying) + Immix mature. Pinned nursery objects
 * stay put (those blocks remain `SPACE_FROM` so their address is stable,
 * D34) and are promoted in place: from then on they are mature objects,
 * traced and remembered like any other (C29, promote_pinned_one). After
 * young copy, unpinned from-space is discarded. Set
 * `AVANT_NURSERY_DISCARD=0` to park all nursery blocks instead (leftover
 * pointer diagnosis). Objects that escape to C should be pinned.
 *
 * Stage 6 spawn uses 1:1 OS threads (D35). Frames are thread-local;
 * allocation and collection take a world lock.
 */

#define ALIGN 8
#define LINE_SIZE 256
#define LINES_PER_BLOCK 128
#define BLOCK_SIZE (LINE_SIZE * LINES_PER_BLOCK)
#define START_WORDS 64
#define BLOCK_HASH 4096
#define LARGE_HASH 4096
#define LOS_LIMIT 8192
#define SCALAR_LOS_MIN 6144
#define YOUNG_BYTES_DEFAULT (16u * 1024u * 1024u)
#define FULL_BYTES (32u * 1024u * 1024u)
#define MAX_TYPES 65535
#define CACHE_BLOCKS 2

/* Numeric buffers at or above this size are young large objects.
 * They do not move, so a parallel loop can keep the address in a
 * register, and a young collection still frees them when unmarked.
 * 900 doubles is 7200 bytes: under LOS_LIMIT, but large enough that
 * copying the row is the slow path. */

/* Survivors of a low-survival young collection are copied here, then the
 * block is retagged as nursery. Mature space is for objects that have
 * already survived a collection. */

#define SPACE_MATURE 0
#define SPACE_NURSERY 1
#define SPACE_FROM 2
#define SPACE_TO 3

typedef struct Block {
  struct Block *hash_next;
  struct Block *next;
  uint8_t *base;
  uint8_t space;
  uint16_t search;
  uint8_t *bump;
  uint8_t *limit;
  uint8_t line_mark[LINES_PER_BLOCK];
  uint64_t start[START_WORDS];
} Block;

typedef struct Large {
  struct Large *next;
  struct Large *hash_next;
  AvantHeader header;
} Large;

typedef struct Frame {
  struct Frame *prev;
  void ***slots;
  uint32_t n;
  uint32_t cap;
} Frame;

typedef struct ThreadReg {
  struct ThreadReg *next;
  Frame **head;
  void *stack_lo;
  void *stack_hi;
} ThreadReg;

typedef struct PinEnt {
  struct PinEnt *next;
  void *payload;
  int count;
} PinEnt;

/* Type maps (Stage 28 W1b). An entry describes which payload words of
 * an object of that type id hold references: bit i of the bitmap
 * (chunk i / 64, bit i % 64) is payload word i. The bitmap has any
 * length (nchunks 64-bit chunks). stride == 0 describes one record:
 * words past the payload size are skipped. stride > 0 describes a
 * buffer of repeated elements of stride words each (an Array element
 * buffer of a by-value struct or union): the bitmap is one element's
 * and applies to every whole element in the payload. Every walker goes
 * through EACH_REF_SLOT, so they all read the same description. */
typedef struct TypeMap {
  uint64_t *words;
  uint64_t first;
  uint32_t nchunks;
  uint32_t stride;
  uint32_t nrefs;
} TypeMap;

/* A sorted set of blocks, for the two block lists of a defragmenting
 * full collection: the mature blocks the collecting thread's C stack
 * names (C31), and the evacuated blocks about to be freed. */
typedef struct BlockSet {
  Block **v;
  size_t n;
  size_t cap;
} BlockSet;

#define g_bump avant_gc_bump

#define g_young_bytes avant_young_bytes

#define g_nursery_held avant_nursery_held

/* Visit every reference slot of payload P as `void **SLOT`. BODY must
 * not use break or continue. */
#define EACH_REF_SLOT(P, SLOT, BODY)                                                \
  do {                                                                              \
    void **ers_p_ = (void **)(P);                                                   \
    const AvantHeader *ers_h_ = avant_header(ers_p_);                               \
    uint32_t ers_id_ = ers_h_->type_id;                                             \
    size_t ers_nw_ = (size_t)ers_h_->size / sizeof(void *);                         \
    if (ers_id_ == AVANT_TYPE_PTRS) {                                               \
      size_t ers_i_;                                                                \
      for (ers_i_ = 0; ers_i_ < ers_nw_; ers_i_++) {                                \
        void **SLOT = ers_p_ + ers_i_;                                              \
        BODY                                                                        \
      }                                                                             \
    } else if (ers_id_ != AVANT_TYPE_BYTES && ers_id_ < MAX_TYPES &&                \
               g_type_set[ers_id_]) {                                               \
      const TypeMap *ers_m_ = &g_types[ers_id_];                                    \
      size_t ers_s_ = ers_m_->stride ? (size_t)ers_m_->stride : ers_nw_;            \
      size_t ers_ne_ = ers_m_->stride ? ers_nw_ / ers_s_ : 1;                       \
      size_t ers_e_;                                                                \
      for (ers_e_ = 0; ers_e_ < ers_ne_; ers_e_++) {                                \
        void **ers_b_ = ers_p_ + ers_e_ * ers_s_;                                   \
        uint32_t ers_c_;                                                            \
        for (ers_c_ = 0; ers_c_ < ers_m_->nchunks; ers_c_++) {                      \
          uint64_t ers_w_ = ers_m_->words[ers_c_];                                  \
          while (ers_w_) {                                                          \
            size_t ers_i_ = (size_t)ers_c_ * 64 + (size_t)__builtin_ctzll(ers_w_);  \
            ers_w_ &= ers_w_ - 1;                                                   \
            if (ers_i_ < ers_s_) {                                                  \
              void **SLOT = ers_b_ + ers_i_;                                        \
              BODY                                                                  \
            }                                                                       \
          }                                                                         \
        }                                                                           \
      }                                                                             \
    }                                                                               \
  } while (0)

#define g_since_gc avant_since_gc

#define g_stats avant_g_stats

#define AVANT_FLAG_REM 16u
/* Young large object reached from a heap slot. It must be promoted:
 * a mature object can hold it, and that edge is not in the remembered
 * set (a large payload has no block). A buffer reached only from the
 * stack stays young so the next collection can free it. */
#define AVANT_FLAG_LOS_KEEP 128u

/* avant_gc_heap.c */
extern Block *g_bht[BLOCK_HASH];
extern Block *g_mature;
extern Block *g_nursery;
extern Block *g_from;
extern Block *g_to;
extern void **g_work;
extern uint32_t g_work_n;
extern TypeMap g_types[MAX_TYPES];
extern uint8_t g_type_set[MAX_TYPES];
int gc_type_word_is_ref(const AvantHeader *h, size_t w);
__attribute__((noreturn)) void gc_die(const char *msg);
void *gc_xrealloc(void *p, size_t n);
void work_push(void *p);
void gc_fwd_reset(void);
void fwd_put(void *from, void *to);
void *fwd_get(void *from);
int gc_block_live_lines(Block *b);
size_t obj_total(const AvantHeader *h);
int is_heap_payload(void *p, Block **ob, Large **ol);
void *gc_object_payload(void *p, Block **ob, Large **ol);
void gc_foreach_block_object(Block *b, void (*fn)(AvantHeader *, Block *));
void gc_zero_line_marks(Block *list);
void mark_object_lines(void *p);

/* avant_gc_alloc.c */
extern Block *g_cur;
extern uint8_t *g_limit;
extern Block *g_alloc_cur[4];
extern uint8_t g_alloc_fresh[4];
extern __thread int g_alloc_mature;
extern uint64_t g_heap_bytes;
extern uint64_t g_heap_max;
void gc_reset_block_bump(Block *b);
void gc_cache_or_free_block(Block *b);
void gc_note_peak(void);
Block *gc_new_block(uint8_t space);
void gc_use_block(Block *b);
void gc_fast_clear(void);
void gc_clear_alloc_cur(uint8_t space);
void *gc_mature_alloc(size_t total);
AvantHeader *gc_to_alloc(size_t total);

/* avant_gc_los.c */
extern Large *g_large;
extern Large *g_large_ht[LARGE_HASH];
extern uint8_t *g_large_lo;
extern uint8_t *g_large_hi;
extern Large *g_los_spare;
extern uint64_t g_young_large;
extern int g_young_large_n;
extern int g_los_eager_hold;
size_t gc_large_chunk(const Large *L);
void gc_los_spare_drop(void);
void gc_free_large(Large *L);
void gc_reclaim_young_large(void);
void gc_sweep_large(void);
void *gc_los_alloc(size_t payload, uint32_t type_id);

/* avant_gc_remset.c */
extern void **g_remset;
extern uint32_t g_rem_n;
extern pthread_mutex_t g_rem_mu;
extern uint32_t g_keep_n;
extern int g_rem_mode;
extern uint8_t *g_rem_dirty;
void gc_keep_add(void *obj);
void gc_rem_scrub(void);
void gc_rem_commit_kept(void);

/* avant_gc_young.c */
extern int g_saw_pinned_nursery;
extern int g_discard_from;
extern uint64_t g_to_bytes;
extern int g_probe;
extern uint64_t g_probe_nursery_bytes;
void gc_clear_noted(void);
void *copy_nursery(void *p);
void copy_from_slot(void **slot);
void gc_gen_young_copy(void);
int gc_block_has_pinned(Block *b);
void gc_young_copy_out(int tenure);

/* avant_gc_mark.c */
extern uint64_t g_mark_sum;
void gc_clear_all_marks(void);
void gc_mark_roots(void);
int gc_keep_if_live(Block *b);
void gc_filter_blocks(Block **list, int (*keep)(Block *));
void gc_sum_marked_one(AvantHeader *h, Block *b);

/* avant_gc_defrag.c */
extern int g_defrag_mode;
extern BlockSet g_def_release;
void gc_block_set_add(BlockSet *s, Block *b);
void gc_block_set_seal(BlockSet *s);
int gc_block_set_has(const BlockSet *s, const Block *b);
void gc_defrag_evacuate(uint64_t live_lines_this);
void gc_defrag_release(void);

/* avant_gc_stack.c */
extern int g_stack_scan;
extern int g_c_stack_pass;
extern int g_c_stack_rewrite;
extern uint64_t g_c_stack_left;
extern BlockSet g_cstack_blocks;
void gc_note_c_stack_blocks(void);
void copy_from_c_stack(void);
void rewrite_c_stack(void);
void count_c_stack_nursery(void);

/* avant_gc_verify.c */
extern int g_verify;
void gc_verify_young_copy(void);
void gc_verify_defrag(void);

/* avant_gc_collect.c */
extern int g_inited;
extern int g_collecting;
extern int g_nursery_off;
extern uint64_t g_mature_since_full;
extern int g_reserve_mode;
extern uint64_t g_copy_reserve;
extern uint64_t g_live;
void gc_collect(int full);
void gc_maybe_collect(void);
void gc_init_slow(void);

/* avant_gc_stats.c */
extern uint64_t g_interior_slots;
extern uint64_t g_peak_live;
extern uint64_t g_root_calls;
extern uint64_t g_reload_calls;
extern uint64_t g_enter_calls;
extern uint64_t g_rem_scans;
extern uint64_t g_rem_entries;
extern uint64_t g_rem_fields;
extern uint64_t g_rem_peak;
extern uint64_t g_rem_adds;
extern uint64_t g_rem_cleared;
extern uint64_t g_def_blocks;
extern uint64_t g_def_live_lines;
extern uint64_t g_def_wasted;
extern uint64_t g_def_hist[6];
extern uint64_t g_def_evac_blocks;
extern uint64_t g_def_evac_bytes;
extern uint64_t g_def_freed_blocks;
extern uint64_t g_def_skipped;
extern uint64_t g_srv_probes;
extern uint64_t g_srv_used;
extern uint64_t g_srv_surv;
extern uint64_t g_srv_min;
extern uint64_t g_srv_max;
extern uint64_t g_srv_hist[6];
extern uint64_t g_yng_min;
extern uint64_t g_yng_max;
extern uint64_t g_cpy_peak;
extern uint64_t g_pk_nursery;
extern uint64_t g_pk_mature;
extern uint64_t g_pk_from;
extern uint64_t g_pk_to;
extern uint64_t g_pk_large;
extern uint64_t g_pk_cache;
extern uint64_t g_los_allocs;
extern uint64_t g_los_alloc_bytes;
extern uint64_t g_los_callocs;
extern uint64_t g_los_spare_hits;
extern uint64_t g_los_spare_cached;
extern uint64_t g_los_spare_dropped;
extern uint64_t g_los_freed;
extern uint64_t g_los_scalar_allocs;
extern uint64_t g_los_scalar_bytes;
extern uint64_t g_los_big_allocs;
extern uint64_t g_los_big_bytes;
extern uint64_t g_los_young_allocs;
extern uint64_t g_los_young_bytes;
extern uint64_t g_los_mature_allocs;
extern uint64_t g_los_mature_bytes;
extern uint64_t g_los_hist[6];
extern uint64_t g_los_live_sum;
extern uint64_t g_los_live_peak;
extern uint64_t g_los_live_samples;
extern uint64_t g_los_promoted;
extern uint64_t g_los_promoted_bytes;
extern uint64_t g_sticky_grows;
void gc_print_stats(void);

/* avant_gc_roots.c */
extern __thread ThreadReg g_self_reg;
extern ThreadReg *g_threads;
extern atomic_int g_avant_shared;
extern pthread_mutex_t g_world;
extern pthread_mutex_t g_threads_mu;
extern int g_gc_defer;
extern PinEnt *g_pins;

/* Small helpers every unit inlines (static inline: one copy per unit,
 * no symbol), moved from avant_rt.c unchanged. */

static inline size_t align_up(size_t n) {
  return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

static inline int rem_locked(void) {
  return atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0;
}

static inline size_t hash_ptr(void *p) {
  uintptr_t x = (uintptr_t)p;
  x ^= x >> 16;
  x *= (uintptr_t)0x9e3779b97f4a7c15ULL;
  return (size_t)x;
}

static inline size_t block_hash(const void *p) {
  return ((uintptr_t)p / BLOCK_SIZE) % BLOCK_HASH;
}

static inline Block *block_of(const void *p) {
  size_t k = block_hash(p);
  Block *b;
  for (b = g_bht[k]; b; b = b->hash_next) {
    if (p >= (void *)b->base && p < (void *)(b->base + BLOCK_SIZE)) {
      return b;
    }
  }
  return NULL;
}

static inline void set_start(Block *b, size_t off) {
  size_t bit = off / 8;
  b->start[bit / 64] |= 1ULL << (bit % 64);
}

static inline void clear_starts(Block *b, size_t off, size_t n) {
  size_t i;
  for (i = 0; i < n; i += 8) {
    size_t bit = (off + i) / 8;
    b->start[bit / 64] &= ~(1ULL << (bit % 64));
  }
}

static inline int has_start(Block *b, size_t off) {
  size_t bit = off / 8;
  return (int)((b->start[bit / 64] >> (bit % 64)) & 1ULL);
}

static inline void mark_lines(Block *b, size_t off, size_t total) {
  size_t start = off / LINE_SIZE;
  size_t end = (off + total - 1) / LINE_SIZE;
  size_t i;
  for (i = start; i <= end && i < LINES_PER_BLOCK; i++) {
    b->line_mark[i] = 1;
  }
}

static inline void *payload_of(AvantHeader *h) {
  return (void *)(h + 1);
}

static inline size_t large_key(void *payload) {
  return hash_ptr(payload) & (size_t)(LARGE_HASH - 1);
}

static inline Large *large_of(void *payload) {
  Large *L;
  if (!payload) {
    return NULL;
  }
  for (L = g_large_ht[large_key(payload)]; L; L = L->hash_next) {
    if (payload == payload_of(&L->header)) {
      return L;
    }
  }
  return NULL;
}

/* The collector initializes on first use. The check is inline so the
 * hot entry points (avant_gc_enter, avant_alloc) keep the one-load test
 * they had when the whole collector was one unit; gc_init_slow reads the
 * environment once (avant_gc_collect.c). */
static inline void gc_init(void) {
  if (!g_inited) {
    gc_init_slow();
  }
}

#endif
