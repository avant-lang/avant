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
 * The collector's state is one GcHeap, gc_heap, and one GcThread per
 * thread, gc_thread (Stage 28 W14b2, gc-design.md §1.1); no unit keeps
 * a variable of its own, and the only free-standing variables are the
 * ones generated code or the rest of the runtime name (avant_rt.h, the
 * inline allocation path's thread-locals and counters, mir.md §5.5;
 * g_avant_shared, avant_gc.h). Functions a unit shares with another
 * carry the gc_ prefix. A few shared functions without the prefix
 * (work_push, fwd_get, copy_from_slot, ...) were global before W12 and
 * are part of avant_rt.o's surface, so they keep their names while that
 * surface is frozen. Each block of declarations below names the unit
 * that defines it. */

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
 * young copy, unpinned from-space is discarded. Objects that escape to C
 * should be pinned.
 *
 * Stage 6 spawn uses 1:1 OS threads (D35). Frames are thread-local;
 * allocation and collection take a world lock.
 */

#define ALIGN 8
#define LINE_SHIFT 8
#define BLOCK_SHIFT 15
#define CHUNK_SHIFT 22
#define LINE_SIZE (1u << LINE_SHIFT)
#define LINES_PER_BLOCK 128
#define BLOCK_SIZE (LINE_SIZE * LINES_PER_BLOCK)
#define CHUNK_SIZE (1u << CHUNK_SHIFT)
#define BLOCKS_PER_CHUNK (CHUNK_SIZE / BLOCK_SIZE)
#define START_WORDS 64
#define PAGE_SHIFT 12
#define PAGE_SIZE (1u << PAGE_SHIFT)
#define PAGES_PER_CHUNK (CHUNK_SIZE / PAGE_SIZE)
#define PAGE_WORDS (PAGES_PER_CHUNK / 64)
#define CHUNK_START_WORDS (BLOCKS_PER_CHUNK * START_WORDS)
#define HEAP_RESERVE_GB_DEFAULT 64
#define HEAP_RESERVE_GB_MIN 1
#define LOS_LIMIT 8192
#define SCALAR_LOS_MIN 6144
#define YOUNG_BYTES_DEFAULT (16u * 1024u * 1024u)
#define FULL_BYTES (32u * 1024u * 1024u)
#define MAX_TYPES 65535
#define CACHE_BLOCKS 2

/* SCALAR_LOS_MIN: numeric (type-0) buffers at or above this size are
 * young large objects. They do not move, so a parallel loop can keep
 * the address in a register, and a young collection still frees them
 * when unmarked. 900 doubles is 7200 bytes: under LOS_LIMIT, but large
 * enough that copying the row is the slow path. */

/* The spaces a block belongs to. SPACE_TO: survivors of a young
 * collection are copied here, then the block is retagged as nursery.
 * Mature space is for objects that have already survived. */

#define SPACE_MATURE 0
#define SPACE_NURSERY 1
#define SPACE_FROM 2
#define SPACE_TO 3

/* The heap (Stage 28 W14a1, gc-design.md §2.1-2.3). One reserved
 * address range (gc_heap_reserve, avant_gc_heap.c) of 4 MiB chunks,
 * each 128 blocks of 32 KiB, each 128 lines of 256 bytes. Every
 * per-block and per-line datum lives in a contiguous side table indexed
 * by the block's number (offset from gc_heap.base >> BLOCK_SHIFT): the
 * block records (gc_heap.blocks, one Block of 64 bytes each), the line marks
 * (gc_heap.lines, one byte per line) and the object-start bits (gc_heap.starts, one
 * bit per 8-byte granule, 512 bytes per block). The chunk map
 * (gc_heap.chunks) has one GcChunk per chunk. Everything is reserved
 * PROT_NONE and committed chunk by chunk, data and the side tables'
 * slices together, so [gc_heap.base, gc_heap.base + gc_heap.committed) is
 * exactly the committed chunks and one compare says whether an address
 * can be in a block. No block descriptor is allocated, no block hash
 * exists, and a lookup is address arithmetic.
 *
 * A block record is ABI by one field: the inline allocation path in
 * generated code stores its new cursor at offset 32 of the record
 * avant_gc_block points to (mir.md §5.5), so bump stays at offset 32.
 * line_mark and start point at the block's slices of gc_heap.lines and
 * gc_heap.starts (set when its chunk is committed; never changed), so code
 * indexes them as before. in_use is 1 while the block belongs to a
 * space; a free or cached block is not a heap block for any lookup. */
typedef struct Block {
  struct Block *next;  /* 0: the space's list, or the free list */
  uint8_t *base;       /* 8: the block's first byte (fixed) */
  uint8_t *line_mark;  /* 16: LINES_PER_BLOCK bytes in gc_heap.lines */
  uint8_t space;       /* 24 */
  uint8_t in_use;      /* 25 */
  uint16_t search;     /* 26: the hole search cursor, in lines */
  uint8_t clean;       /* 28: free and madvised: zero, not resident */
  uint8_t pad_[3];     /* 29 */
  uint8_t *bump;       /* 32: ABI (the inline allocation path) */
  uint8_t *limit;      /* 40 */
  uint64_t *start;     /* 48: START_WORDS words in gc_heap.starts */
  uint64_t idle;       /* 56: free: the full-collection count when freed */
} Block;

_Static_assert(offsetof(Block, bump) == 32, "Block.bump is ABI at offset 32");
_Static_assert(sizeof(Block) == 64, "a block record is 64 bytes");

/* One chunk's entry in the chunk map (gc-design.md §2.2, §2.4). kind:
 *   CHUNK_FREE       in no use (never committed past the high-water
 *                    mark; below it, given back: its data may be dirty)
 *   CHUNK_SMALL      128 blocks of small objects; used counts the blocks
 *                    taken (in a space or in the two-block cache)
 *   CHUNK_LARGE      large objects on exact runs of 4 KiB pages: alloc
 *                    is the free-page bitmap's complement (a set bit is
 *                    an allocated page), used counts allocated pages,
 *                    dirty_pages the pages that may hold non-zero bytes
 *   CHUNK_HUGE       the first chunk of one object larger than a chunk
 *                    holds; head is the run's length in chunks
 *   CHUNK_HUGE_CONT  a later chunk of that run; head is the first's index
 * dirty (FREE): the data was used and may hold non-zero bytes.
 * decommitted (FREE): the data is PROT_NONE (gc_heap_return).
 * free, nfree (SMALL): the chunk's free blocks (O53), so a chunk whose
 * blocks are all free can be decommitted.
 * idle: the full-collection count when the chunk last became free or
 * empty, or (LARGE) last freed a page; gc_heap_return returns only
 * memory idle since before the collection running it (Q12). */
#define CHUNK_FREE 0
#define CHUNK_SMALL 1
#define CHUNK_LARGE 2
#define CHUNK_HUGE 3
#define CHUNK_HUGE_CONT 4
typedef struct GcChunk {
  uint8_t kind;
  uint8_t dirty;
  uint16_t used;
  uint32_t head;
  struct Block *free;
  uint16_t nfree;
  uint8_t decommitted;
  uint8_t pad_;
  uint32_t idle;
  uint64_t alloc[PAGE_WORDS];
  uint64_t dirty_pages[PAGE_WORDS];
} GcChunk;

/* A walk over every large object (gc_large_next, avant_gc_los.c), in
 * address order. The walk may free the object it just returned. */
typedef struct LargeIter {
  uint64_t c;
  uint32_t p;
} LargeIter;

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
               gc_heap.type_set[ers_id_]) {                                               \
      const TypeMap *ers_m_ = &gc_heap.types[ers_id_];                                    \
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

#define AVANT_FLAG_REM 16u
/* Young large object reached from a heap slot. It must be promoted:
 * a mature object can hold it, and that edge is not in the remembered
 * set (a large payload has no block). A buffer reached only from the
 * stack stays young so the next collection can free it. */
#define AVANT_FLAG_LOS_KEEP 128u

/* A forwarding entry of the young copy (avant_gc_heap.c). */
typedef struct FwdEnt {
  void *from;
  void *to;
} FwdEnt;

/* The collector's counters, per family (Stage 28 W14b2, gc-design.md
 * §1.7, O41). AvantGcStats (avant_rt.h, ABI: avant_g_stats) keeps its
 * seven; every other counter is a field of one family here, written
 * where its event happens and printed by its family's routine in
 * avant_gc_stats.c (AVANT_GC_STATS=1, one line at exit). */

/* root: root bookkeeping call counts (Collector II Wave 0): every call to
 * avant_gc_root (and every slot of avant_gc_root_begin), avant_gc_reload,
 * and avant_gc_enter, so a profile can split call volume from per-call
 * cost. interiors counts the slots and root locations the young copy
 * found naming an object's interior rather than its start. */
typedef struct GcRootCounters {
  uint64_t interiors;
  uint64_t roots;
  uint64_t reloads;
  uint64_t enters;
} GcRootCounters;

/* rem: remembered-set observability (Collector II Wave 2): scans counts
 * every remembered-set scan in gc_gen_young_copy (probe and real passes
 * alike), entries/fields sum the set size and field slots visited at each
 * scan, peak is the largest frontier seen at a scan start, adds counts
 * successful rem_add inserts, cleared counts frontier evictions. */
typedef struct GcRemCounters {
  uint64_t scans;
  uint64_t entries;
  uint64_t fields;
  uint64_t peak;
  uint64_t adds;
  uint64_t cleared;
} GcRemCounters;

/* def: mature defragmentation observability (Collector II Wave 3, step
 * 1): per-block live-line histogram built from the line marks a full
 * collection already makes. Buckets by live lines in a 128-line block:
 * 0, 1-32, 33-64, 65-96, 97-127, 128. Wasted bytes count only partly
 * live blocks ((128 - live) * LINE_SIZE); empty blocks are freed whole
 * by the existing sweep and are not fragmentation waste. The evac_*,
 * freed_blocks and skipped fields count the evacuation (step 2).
 * Counters are cumulative over full collections. */
typedef struct GcDefCounters {
  uint64_t blocks;
  uint64_t live_lines;
  uint64_t wasted;
  uint64_t hist[6];
  uint64_t evac_blocks;
  uint64_t evac_bytes;
  uint64_t freed_blocks;
  uint64_t skipped;
} GcDefCounters;

/* srv, yng, pk: survival and peak-heap observability (Collector II Wave
 * 4, step 1). srv: probes counts adaptive survival probes, used/surv sum
 * the nursery bytes in use and the surviving bytes at each probe,
 * min/max are the extreme surviving-byte readings, and hist buckets the
 * surv/used ratio in percent (0, 1-10, 10-25, 25-50, 50-75, 75-100) so
 * phase oscillation is visible. */
typedef struct GcSrvCounters {
  uint64_t probes;
  uint64_t used;
  uint64_t surv;
  uint64_t min;
  uint64_t max;
  uint64_t hist[6];
} GcSrvCounters;

/* yng: min/max bound the adaptive young threshold over the run.
 * cpy_peak is the largest from-space plus to-space object bytes held
 * during one young copy. sticky counts the threshold's sticky growths
 * (young_pause). */
typedef struct GcYngCounters {
  uint64_t min;
  uint64_t max;
  uint64_t cpy_peak;
  uint64_t sticky;
} GcYngCounters;

/* pk: the heap composition (block bytes per space, large-object bytes,
 * cached block bytes) at the moment peak_heap was last set
 * (gc_note_peak). */
typedef struct GcPkCounters {
  uint64_t nursery;
  uint64_t mature;
  uint64_t from;
  uint64_t to;
  uint64_t large;
  uint64_t cache;
} GcPkCounters;

/* los: large-object observability (Collector II Wave 5, step 1): allocs
 * and alloc_bytes count every gc_los_alloc by chunk bytes (the heap
 * charge for a large object); callocs counts fresh calloc calls,
 * spare_hits counts exact-size spare reuses, spare_cached counts dead
 * young Bytes buffers kept into the single spare slot, spare_dropped
 * counts spares freed unused, and freed counts large objects returned to
 * free. scalar counts Bytes allocations that are large only through
 * SCALAR_LOS_MIN, big counts allocations large through LOS_LIMIT.
 * young/mature split the birth space. hist buckets payload bytes: under
 * 16 KiB, 16-64 KiB, 64-256 KiB, 256 KiB-1 MiB, 1-4 MiB, at least 4 MiB.
 * live samples the marked large bytes at the liveness decision of each
 * collection (gc_reclaim_young_large for young, gc_sweep_large for full):
 * sum over samples, peak the largest single sample. promoted counts young
 * large objects tenured by promote_young_large. */
typedef struct GcLosCounters {
  uint64_t allocs;
  uint64_t alloc_bytes;
  uint64_t callocs;
  uint64_t spare_hits;
  uint64_t spare_cached;
  uint64_t spare_dropped;
  uint64_t freed;
  uint64_t scalar_allocs;
  uint64_t scalar_bytes;
  uint64_t big_allocs;
  uint64_t big_bytes;
  uint64_t young_allocs;
  uint64_t young_bytes;
  uint64_t mature_allocs;
  uint64_t mature_bytes;
  uint64_t hist[6];
  uint64_t live_sum;
  uint64_t live_peak;
  uint64_t live_samples;
  uint64_t promoted;
  uint64_t promoted_bytes;
} GcLosCounters;

typedef struct GcCounters {
  GcRootCounters root;
  GcRemCounters rem;
  GcDefCounters def;
  GcSrvCounters srv;
  GcYngCounters yng;
  GcPkCounters pk;
  GcLosCounters los;
  /* The largest live byte count a collection measured. */
  uint64_t peak_live;
  /* The cumulative count of stack words still naming from-space after
   * the young copy's C-stack rewrite (sm_left, avant_gc_stack.c). */
  uint64_t stack_left;
} GcCounters;

/* The settings (Stage 28 W14b2, gc-design.md §1.7, O40): read from the
 * environment once, by gc_init_slow (gc_settings_read,
 * avant_gc_collect.c), and never changed after. The knobs, their values
 * and the parsing rule are documented in one place, avant_gc.h. */
typedef struct GcSettings {
  /* AVANT_YOUNG_MB in bytes; 0: the adaptive young threshold. */
  uint64_t young_fixed;
  /* AVANT_HEAP_MAX_MB in bytes; 0: no limit. */
  uint64_t heap_max;
  /* AVANT_HEAP_RESERVE_GB: the heap's address reservation, in GiB. */
  uint64_t reserve_gb;
  /* AVANT_NURSERY=0: no young generation, full non-moving collections
   * only. */
  int nursery_off;
  /* The young copy's C-stack pass (avant_gc_stack.c): c_stack_pass (the
   * copying nursery is on) runs count_c_stack_nursery, and
   * c_stack_rewrite (the pass, unless AVANT_GC_STACK_REWRITE=0) runs
   * rewrite_c_stack. */
  int c_stack_pass;
  int c_stack_rewrite;
  /* AVANT_GC_DEFRAG: mature evacuation in full collections
   * (avant_gc_defrag.c); off, the histogram stays on. */
  int defrag;
  /* AVANT_GC_VERIFY: the checkers of avant_gc_verify.c. */
  int verify;
  /* AVANT_GC_STATS: the statistics line at exit (avant_gc_stats.c). */
  int stats;
  /* AVANT_GC_STRESS (Stage 28 W14b1, gc-design.md §4.2 item 1, the
   * minimal step): a young collection at every outermost
   * avant_gc_defer_enter and at every avant_alloc called from runtime C
   * (avant_alloc_c), in unshared programs with the nursery on. With the
   * copying nursery a young collection moves every reached young object,
   * so a helper that holds a young reference the collector does not know
   * across one of these points reads a moved or freed object at once. */
  int stress;
} GcSettings;

/* The collector's state (Stage 28 W14b2, gc-design.md §1.1, O41): one
 * GcHeap, gc_heap (defined in avant_gc_heap.c), shared by every
 * collector unit and guarded by the world lock unless a field says
 * otherwise. The fields are grouped by the unit that owns them. What the
 * generated code reads or writes by name stays outside (the inline
 * allocation path's thread-locals and counters, avant_g_stats,
 * g_avant_str_gen; mir.md §5.5), as does g_avant_shared, which the
 * runtime's other units read. Per-thread state is GcThread. */
typedef struct GcHeap {
  /* avant_gc_heap.c. The reservation (gc-design.md §2.1): base is 4 MiB
   * aligned; reserved bytes from it are reserved, the first committed
   * of them committed (whole chunks, in order). The side tables are
   * reserved for the whole range and committed by the slice of each
   * chunk. */
  uint8_t *base;
  uint64_t committed;
  uint64_t reserved;
  Block *blocks;
  uint64_t *starts;
  uint64_t *marks;
  uint8_t *lines;
  uint32_t *pages;
  GcChunk *chunks;
  /* Chunks below the high-water mark that are CHUNK_FREE again. */
  uint64_t free_chunks;
  /* Every small chunk below it has no free block (O53: blocks are taken
   * from the lowest chunk that has one, so high chunks drain and free up
   * whole). */
  uint64_t small_hint;
  /* Bytes of free blocks that are resident (not madvised). */
  uint64_t free_resident;
  /* The spaces' block lists (SPACE_*). */
  Block *mature;
  Block *nursery;
  Block *from;
  Block *to;
  /* The mark work list. */
  void **work;
  uint32_t work_n;
  uint32_t work_cap;
  /* The type maps (avant_type_map), by type id. */
  TypeMap types[MAX_TYPES];
  uint8_t type_set[MAX_TYPES];
  /* The young copy's forwarding table (open addressing). */
  FwdEnt *fwd;
  size_t fwd_cap;
  size_t fwd_used;

  /* avant_gc_alloc.c. Freed blocks kept for reuse (at most
   * CACHE_BLOCKS), still charged to heap_bytes. */
  Block *cache;
  int cache_n;
  /* The C allocation cursor's block and limit; the cursor itself is the
   * inline path's avant_gc_bump. fast_block is the block the inline
   * fast path is published on (gc_fast_refresh). */
  Block *cur;
  uint8_t *limit;
  Block *fast_block;
  /* Per-space bump cursor. A full block used to send the next allocation
   * back through every older block (take_from_list + find_hole). Workers
   * allocate in mature space, and that walk dominated Matmul::T16.
   * alloc_fresh means the cursor is a new block: when it fills, allocate
   * another instead of rescanning full blocks. A full collection clears
   * the cursor so the next allocation reuses holes. */
  Block *alloc_cur[4];
  uint8_t alloc_fresh[4];
  /* Block bytes in use or cached, against set.heap_max. */
  uint64_t heap_bytes;
  /* Block bytes per space (SPACE_*), kept as blocks enter a space, move
   * between spaces and leave (W14a2), so gc_note_peak reads them instead
   * of walking every list. */
  uint64_t space_bytes[4];
  /* Wave 4 step 2: outside a collection the copy reserve counts as
   * spent, so fresh block growth stops early enough that the next copy
   * has its destination room under the heap limit. The charge is soft:
   * it makes allocation collect earlier, never fail, so the retry after
   * a collection (reserve_retry) and any allocation inside a collection
   * (where the reserve is what is being spent) are charged only the true
   * heap bytes. Cached blocks are already owned and are never refused by
   * this test. */
  int reserve_retry;
  /* Set while a no-GC region takes a block past the heap limit (C32). */
  int over_limit;

  /* avant_gc_los.c. One dead scalar large buffer, kept for the next
   * same-sized alloc: its pages stay allocated, its start bit is clear
   * (no lookup finds it). */
  AvantHeader *los_spare;
  /* The page-run bytes of every large object (the spare excluded): the
   * peak decomposition's pk_large and the stats line's los_now_bytes. */
  uint64_t large_bytes;
  /* Bytes and count of young scalar large objects. They do not move. A
   * young collection frees the ones the mark does not reach. */
  uint64_t young_large;
  int young_large_n;
  /* Set when a young large object survived a collection. Further
   * same-sized allocs are live rows, not a replaced buffer, so they
   * must not collect on every malloc. */
  int los_eager_hold;
  /* The large-object chunks (CHUNK_LARGE), in the order they were made:
   * the first fit looks in each, in this order. */
  uint32_t *lchunks;
  uint32_t lchunks_n;
  uint32_t lchunks_cap;

  /* avant_gc_remset.c. The remembered set (object remembering) and,
   * per entry, whether its last scan found a young field (rem_dirty). */
  void **remset;
  uint32_t rem_n;
  uint32_t rem_cap;
  uint8_t *rem_dirty;
  /* Workers call avant_barrier without the world lock (a fresh array
   * header). The remembered-set realloc is shared. */
  pthread_mutex_t rem_mu;
  /* Mature objects that still point at a young survivor after this
   * collection. */
  void **keep;
  uint32_t keep_n;
  uint32_t keep_cap;

  /* avant_gc_young.c. */
  int saw_pinned_nursery;
  /* The noted list (C36, gc-design.md §2.6): every object a young
   * collection flags MARKED (its "traced this cycle" bit), probe or copy:
   * nursery objects the probe reaches, mature, large and from-space
   * objects it scans (trace_mature), remembered objects it scans, copies
   * and pinned survivors. gc_clear_noted clears exactly those when the
   * collection ends (after a probe, also when the probe decides to pause
   * instead of copying), so no MARKED outlives the collection that set it
   * and no later young collection reads a stale one as "already scanned".
   * Full collections mark in the side bitmap and never touch MARKED.
   * Before W14a2 only the probe's nursery marks were noted; the others
   * were cleared by a walk of every object of every space after a copy
   * (clear_mark_flags), and not at all after a probe-only pause, a
   * remembered entry's frontier eviction, or a full collection. */
  void **noted;
  uint32_t noted_n;
  uint32_t noted_cap;
  uint64_t to_bytes;
  int probe;
  int force_tenure;
  int from_mature;
  uint64_t probe_nursery_bytes;
  int from_heap_slot;
  /* Set while a remembered object is being scanned: copy_from_slot then
   * records whether any direct field names a young object afterwards. */
  int rem_track;
  int rem_track_dirty;
  int block_has_pin;

  /* avant_gc_mark.c. */
  uint64_t mark_sum;

  /* avant_gc_defrag.c: the evacuated blocks a defragmenting full
   * collection frees once every reference is rewritten. */
  BlockSet def_release;

  /* avant_gc_stack.c: the mature blocks a word of the collecting
   * thread's stack names, which a defragmenting full collection does not
   * evacuate (C31, gc_note_c_stack_blocks). */
  BlockSet cstack_blocks;

  /* avant_gc_verify.c. vnoted: the objects verify_trace flagged
   * FORWARDED (its visited bit), cleared when gc_verify_young_copy ends,
   * before the young copy discards from-space (W14a2: no whole-heap
   * clearing walk follows any more). */
  void **vnoted;
  size_t vnoted_n;
  size_t vnoted_cap;
  /* A word the type map does not call a reference, in an object the
   * verifier reached, that is exactly the start of an unpinned object in
   * from-space (moved or about to be discarded): the program stored a
   * reference where the collector does not look (a wrong or missing type
   * map, audit-abi 8.1). Heap strings (STRMETA) hold text and are
   * skipped; a scalar equal to such an address by chance is the only
   * false positive. */
  uint64_t verify_untraced;
  uint64_t verify_left_seen;
  /* The remembered-set check (gc_verify_remembered): the objects R1'
   * names this cycle (MARKED) and the missing barriers reported. */
  void **rnamed;
  size_t rnamed_n;
  size_t rnamed_cap;
  uint64_t missing_barriers;
  /* C36's check (gc_verify_no_stale_marks): the stale header marks
   * reported. */
  uint64_t stale_seen;

  /* avant_gc_collect.c. */
  int inited;
  int collecting;
  /* Set while a stress point's young collection runs (it always
   * copies). */
  int stress_now;
  /* C32 (Stage 28 W14b1, gc-design.md §5.4): an allocation inside a no-GC
   * (defer) region that went past AVANT_HEAP_MAX_MB. The full collection
   * it needed runs at the first collection point after the region: the
   * next gc_maybe_collect, which the allocation fast paths (C and inline)
   * reach because they stay off while this is set. */
  int limit_pending;
  uint64_t mature_since_full;
  /* The copy reserve (Wave 4 step 2): the room the next young copy needs,
   * charged as spent by heap-limit decisions outside a collection. */
  uint64_t copy_reserve;
  /* Mature growth that triggers the next full collection. Starts at
   * FULL_BYTES and adapts after every full collection to twice the
   * live set plus slack, so a program with a small live set and heavy
   * mature churn (a parsed document per iteration) does not grow the
   * heap to 32 MB before reclaiming, while a program whose live set is
   * large keeps today's spacing. */
  uint64_t full_trigger;
  /* The live bytes the last collection measured, and the allocation
   * count at the last full collection (avant_gc_heap_goal_collect). */
  uint64_t live;
  uint64_t last_full_at;
  GcSettings set;

  /* avant_gc_roots.c. The registered threads (GcThread.reg), the world
   * lock that allocation and collection take, the depth of defer
   * regions, the pins. */
  ThreadReg *threads;
  pthread_mutex_t world;
  pthread_mutex_t threads_mu;
  int defer;
  PinEnt *pins;

  /* avant_gc_stats.c. */
  GcCounters ctr;
} GcHeap;

extern GcHeap gc_heap;

/* A mutator thread's collector state (Stage 28 W14b2, gc-design.md
 * §1.1): gc_thread, thread-local, defined in avant_gc_roots.c. */
typedef struct GcThread {
  /* The innermost registered frame, and the thread's free frames. */
  Frame *frame;
  Frame *frame_free;
  /* avant_gc_quiet_enter's depth: root registration is skipped inside. */
  int quiet;
  /* avant_alloc_in_mature(1): this thread allocates in mature space. */
  int alloc_mature;
  /* Nonzero while runtime C has entered the collector (avant_alloc_c, a
   * stress point at a defer entry): only then does a young collection run
   * the conservative C-stack rewrite (avant_gc_young.c, C37). */
  int c_entry;
  /* The thread's registration (gc_heap.threads), its frame head and
   * stack bounds. */
  ThreadReg reg;
} GcThread;

extern __thread GcThread gc_thread;

/* avant_gc_heap.c */
void gc_heap_reserve(void);
Block *gc_block_take(void);
void gc_block_give(Block *b);
uint64_t gc_chunk_take(uint64_t n);
void gc_chunk_give(uint64_t c);
void gc_return_pages(void *p, size_t len);
void gc_heap_return(uint64_t live);
int gc_type_word_is_ref(const AvantHeader *h, size_t w);
__attribute__((noreturn)) void gc_die(const char *msg);
void *gc_xrealloc(void *p, size_t n);
void work_push(void *p);
void gc_fwd_reset(void);
void fwd_put(void *from, void *to);
void *fwd_get(void *from);
int gc_block_live_lines(Block *b);
size_t obj_total(const AvantHeader *h);
int is_heap_payload(void *p, Block **ob, AvantHeader **ol);
void *gc_object_payload(void *p, Block **ob, AvantHeader **ol);
void gc_foreach_block_object(Block *b, void (*fn)(AvantHeader *, Block *));
void gc_zero_line_marks(Block *list);
void gc_clear_side_marks(void);
void mark_object_lines(void *p);

/* avant_gc_alloc.c */
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
size_t gc_large_bytes(const AvantHeader *h);
void gc_large_chunk_gone(uint64_t c);
AvantHeader *gc_large_next(LargeIter *it);
void gc_los_spare_drop(void);
void gc_release_large(AvantHeader *h);
void gc_reclaim_young_large(void);
void gc_sweep_large(void);
void *gc_los_alloc(size_t payload, uint32_t type_id);

/* avant_gc_remset.c */
void gc_keep_add(void *obj);
void gc_rem_scrub(void);
void gc_rem_commit_kept(void);
void gc_rem_full_scrub(void);

/* avant_gc_young.c */
void gc_clear_noted(void);
void *copy_nursery(void *p);
void copy_from_slot(void **slot);
void gc_gen_young_copy(void);
int gc_block_has_pinned(Block *b);
void gc_young_copy_out(int tenure);

/* avant_gc_mark.c */
void gc_mark_roots(void);
int gc_keep_if_live(Block *b);
void gc_filter_blocks(Block **list, int (*keep)(Block *));
void gc_sum_marked_one(AvantHeader *h, Block *b);

/* avant_gc_defrag.c */
void gc_block_set_add(BlockSet *s, Block *b);
void gc_block_set_seal(BlockSet *s);
int gc_block_set_has(const BlockSet *s, const Block *b);
void gc_defrag_evacuate(uint64_t live_lines_this);
void gc_defrag_release(void);

/* avant_gc_stack.c */
void gc_note_c_stack_blocks(void);
void rewrite_c_stack(void);
void count_c_stack_nursery(void);

/* avant_gc_verify.c */
void gc_verify_young_copy(void);
void gc_verify_no_stale_marks(void);
void gc_verify_remembered(void);
void gc_verify_defrag(void);

/* avant_gc_collect.c */
/* A collection (O42): GC_YOUNG copies the nursery's live objects (it
 * always moves them, so allow_move must be 1); GC_FULL marks and sweeps
 * the whole heap and, when allow_move is 1 and AVANT_GC_DEFRAG is on,
 * evacuates sparse mature blocks (defragmentation). A caller whose C
 * frames may hold references the collector cannot rewrite passes 0. */
typedef enum GcKind { GC_YOUNG, GC_FULL } GcKind;
void gc_collect(GcKind kind, int allow_move);
void gc_limit_collect_pending(void);
void gc_maybe_collect(void);
void gc_stress_point(void);
void gc_init_slow(void);

/* avant_gc_stats.c */
void gc_print_stats(void);

/* avant_gc_roots.c */
extern atomic_int g_avant_shared;

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

/* O(1) lookups (gc-design.md §2.3). An address is in the committed
 * heap iff its offset from gc_heap.base is below gc_heap.committed (an
 * address below the base wraps to a large offset); its chunk, block and
 * line are that offset shifted. block_of answers only for a block in
 * use, as the block hash it replaced did. */
static inline int heap_offset(const void *p, uintptr_t *off) {
  *off = (uintptr_t)p - (uintptr_t)gc_heap.base;
  return *off < gc_heap.committed;
}

static inline GcChunk *chunk_of(const void *p) {
  uintptr_t off;
  return heap_offset(p, &off) ? &gc_heap.chunks[off >> CHUNK_SHIFT] : NULL;
}

static inline Block *block_of(const void *p) {
  uintptr_t off;
  Block *b;
  if (!heap_offset(p, &off)) {
    return NULL;
  }
  b = &gc_heap.blocks[off >> BLOCK_SHIFT];
  return b->in_use ? b : NULL;
}

static inline uint8_t *line_mark_of(const void *p) {
  uintptr_t off;
  return heap_offset(p, &off) ? &gc_heap.lines[off >> LINE_SHIFT] : NULL;
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

/* The full collection's mark (Stage 28 W14a2, gc-design.md §2.2, C36):
 * one bit per 8-byte granule in the side table gc_heap.marks, at the granule
 * of the object's header (the same index as its start bit), cleared per
 * chunk by gc_clear_side_marks when a full collection starts. Every
 * object, large ones included, lies in the reservation. The header's
 * MARKED bit is the young collection's alone ("traced this cycle"), and
 * every MARKED a young collection sets is cleared when it ends (the
 * noted list, avant_gc_young.c). */
static inline int full_marked(const AvantHeader *h) {
  uint64_t g = (uint64_t)((uintptr_t)h - (uintptr_t)gc_heap.base) >> 3;
  return (int)((gc_heap.marks[g >> 6] >> (g & 63)) & 1ULL);
}

static inline void full_set_mark(AvantHeader *h) {
  uint64_t g = (uint64_t)((uintptr_t)h - (uintptr_t)gc_heap.base) >> 3;
  gc_heap.marks[g >> 6] |= 1ULL << (g & 63);
}

/* Start bits by address, for any chunk (large objects' headers have one
 * too, gc-design.md §2.3). */
static inline uint64_t granule_of(const void *p) {
  return (uint64_t)((uintptr_t)p - (uintptr_t)gc_heap.base) >> 3;
}

static inline int start_bit_at(const void *p) {
  uint64_t g = granule_of(p);
  return (int)((gc_heap.starts[g >> 6] >> (g & 63)) & 1ULL);
}

static inline void start_bit_set(const void *p, int on) {
  uint64_t g = granule_of(p);
  if (on) {
    gc_heap.starts[g >> 6] |= 1ULL << (g & 63);
  } else {
    gc_heap.starts[g >> 6] &= ~(1ULL << (g & 63));
  }
}

static inline uint8_t *chunk_base(uint64_t c) {
  return gc_heap.base + (c << CHUNK_SHIFT);
}

static inline void *payload_of(AvantHeader *h) {
  return (void *)(h + 1);
}

static inline void gc_large_iter_init(LargeIter *it) {
  it->c = 0;
  it->p = 0;
}

/* The collector initializes on first use. The check is inline so the
 * hot entry points (avant_gc_enter, avant_alloc) keep the one-load test
 * they had when the whole collector was one unit; gc_init_slow reads the
 * environment once (avant_gc_collect.c). */
static inline void gc_init(void) {
  if (!gc_heap.inited) {
    gc_init_slow();
  }
}

#endif
