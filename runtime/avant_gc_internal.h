#ifndef AVANT_GC_INTERNAL_H
#define AVANT_GC_INTERNAL_H

#include "avant_rt.h"
/* The collector implements what avant_gc.h declares for the rest of the
 * runtime (the frame type of RC1's scopes, the no-GC regions). */
#include "avant_gc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The collector's internal header (Stage 28 W12). Private to the
 * collector's units, one per concern:
 *   avant_gc_heap.c     the spaces' block lists and the block table,
 *                       object lookup, forwarding, the work list, type
 *                       maps, gc_die and gc_xrealloc
 *   avant_gc_alloc.c    block supply, bump cursors, the inline fast
 *                       path's state, avant_alloc
 *   avant_gc_los.c      the large-object space
 *   avant_gc_remset.c   the remembered set and the store barriers
 *   avant_gc_sticky.c   the young collection (S+: promotion in place
 *                       and the opportunistic young copy) and the
 *                       noted list
 *   avant_gc_mark.c     the full collection's mark and block sweep
 *   avant_gc_defrag.c   mature defragmentation, block sets
 *   avant_gc_verify.c   AVANT_GC_VERIFY's checkers
 *   avant_gc_collect.c  the driver, its policy and the environment knobs
 *   avant_gc_stats.c    AvantGcStats, the counters, AVANT_GC_STATS
 *   avant_gc_roots.c    threads, frames and root slots, quiet leaves,
 *                       pins, defer regions, the world lock
 * They fold into one avant_rt.o (runtime/build.sh), which keeps global
 * only the symbols avant_rt.exports lists, so nothing declared here
 * becomes part of the object's surface. Other
 * runtime units and user programs see avant_rt.h (the shipped ABI) and
 * avant_gc.h (what the rest of the runtime may call); never this file.
 * The collector's state is one GcHeap, gc_heap, and one GcThread per
 * thread, gc_thread (Stage 28 W14b2, gc-design.md §1.1); no unit keeps
 * a variable of its own, and the only free-standing variables are the
 * ones generated code or the rest of the runtime name (avant_rt.h, the
 * inline allocation path's thread-locals and counters, mir.md §5.5;
 * g_avant_shared, avant_gc.h). Functions a unit shares with another
 * carry the gc_ prefix (since W14b2 without exception: the ten that were
 * global before W12 and kept their names while avant_rt.o's surface was
 * frozen, work_push, fwd_get and the rest, are gc_ names and local to
 * the object now, O37). Each block of declarations below names the unit
 * that defines it. */

/*
 * Precise Immix-family collector.
 *
 * Geometry: 32 KiB blocks, 256-byte lines. Object-start bits identify
 * headers. Tracing is precise (type maps + a shadow stack of local slots).
 *
 * Sticky Immix with an opportunistic young copy (S+, Stage 28 W15c,
 * D233): young objects are allocated into free lines of mature blocks
 * and promoted in place by young collections, except that the survivors
 * of a young block that held no mature line are copied out in trace
 * order when every reference to them is precise (avant_gc_sticky.c).
 * Pinned objects never move. Objects that escape to C should be pinned.
 *
 * Stage 6 spawn uses 1:1 OS threads (D35). Frames are thread-local;
 * collection and the slow path of allocation take a world lock; each
 * thread bumps in its own young hole without it (Stage 28 W17b).
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
/* Card marking for large pointer buffers (Stage 28 W16t, gc-design.md
 * Q5, O64): a card is 2^CARD_SHIFT bytes of heap address space, one byte
 * of the side table gc_heap.cards. Settable at build time for a
 * measurement. */
#ifndef CARD_SHIFT
#define CARD_SHIFT 8
#endif
/* 8 (256 bytes, 32 slots, a line) by measurement (Stage 28 W16t: the four
 * heap fixtures under the full stress mode 110.0 / 120.9 / 131.9 s at
 * 8 / 9 / 10, split_held.av 41.5 / 55.8 / 60.2 s; the default mode equal).
 * At most 10: each chunk's slice of the table is whole pages. */
#if CARD_SHIFT > 10
#error "CARD_SHIFT above 10: a chunk's card slice would not be whole pages"
#endif
#define CARD_SIZE (1u << CARD_SHIFT)
#define SCALAR_LOS_MIN 6144
/* The default young budget (gc-design.md §5.2, Q3): 4 MiB, chosen in
 * Stage 28 W15d among 4, 8 and 16 MiB on the final collector (LangArena
 * prod at each: M1 total 42.29 / 42.53 / 42.67 s, average RSS 28.27 /
 * 28.21 / 29.02 MB, worst M2 memory multiplier 2.57 / 3.62 / 3.74; the
 * record states the rule). Settable at build time for a measurement. */
#ifndef YOUNG_BYTES_DEFAULT
#define YOUNG_BYTES_DEFAULT (4ull * 1024ull * 1024ull)
#endif
/* S_LARGE_AGE (Stage 28 W15c item 1, O88; W15d item 1): the young
 * collections a young large object lives through before it is promoted.
 * 2 keeps a reached one young at its first survival (SURVIVED) and
 * promotes it at its second, with the remembered set kept for its holders
 * (gc_rem_keep_young_holders); 1 promotes it at its first survival (W15b's
 * rule). A fixed AVANT_YOUNG_MB promotes at once either way. W15c measured
 * 2 without small-object aging: the array header that owns a buffer was
 * promoted in place at the buffer's first survival and, remembered, kept
 * the buffer alive through its second (Sort::Quick 20 -> 23 MB), so 1 was
 * kept. With YOUNG_AGE 2 and holder aging (splus_age_holders) the header
 * stays young and dies with its buffer: 2 by measurement (W15d record,
 * var/A1L1, A2L1, A2L2h: Sort::Quick / Merge / Self 19 / 20 / 21 ->
 * 11 / 13 / 14 MB RSS, Template::Regex / Parse 43 / 42 -> 37 / 36 MB). */
#ifndef S_LARGE_AGE
#define S_LARGE_AGE 2
#endif
/* LARGE_AGE_BYTES (Stage 28 W15e, O98): with S_LARGE_AGE 2, a reached
 * young large object is promoted at its second survival only once at
 * least this many young bytes were allocated since its birth (the young
 * clock, gc_heap.young_clock); until then it stays young. Its age no
 * longer depends on the budget: at 4 MiB the second survival came after
 * 4-8 MiB of allocation, at the old 8 MiB after 8-16, and
 * Template::Regex / Parse promoted 17.4 MB of buffers instead of 7.0
 * (42 / 41 MB RSS against 37 / 36). A young large object that survived
 * is not counted in the young budget after the collection (counted, the
 * survivors filled a 4 MiB budget, the clock barely moved between
 * collections, and Template ran 18,395 young collections). Measured
 * (W15e record, var/): 8 MiB gives Template 36 / 36 MB, Sort::Quick
 * 13 -> 11 MB, 16 MiB the same within 1 MB; 0 is W15d's rule. */
#ifndef LARGE_AGE_BYTES
#define LARGE_AGE_BYTES (8ull * 1024ull * 1024ull)
#endif
/* YOUNG_AGE (Stage 28 W15d item 1, O91, D232: E's aging, Q4, inside S+'s
 * copy path): the young collections a small object S+ copies lives
 * through before it is promoted. 2 is one aging step: a survivor S+
 * copies out of an evacuated block at its first survival goes into a
 * survivor block (SPLUS_SURV during the collection), still young
 * (YOUNG | SURVIVED), and is promoted at its next survival (copied to
 * mature space, or in place); so the header that owns a short-lived
 * buffer dies young with it. 1 promotes every survivor at its first
 * survival (W15c's S+). A survivor S+ keeps in place (a kept block, a
 * block with mature lines, a shared program) is promoted at once either
 * way (S's rule), and a fixed AVANT_YOUNG_MB tenures at once. */
#ifndef YOUNG_AGE
#define YOUNG_AGE 2
#endif
/* The full-collection trigger (gc-design.md §5.4, Stage 28 W15a1): mature
 * growth since the last full collection reaching
 * max(FULL_TRIGGER_MIN, (HEAP_FACTOR - 1) * live after it), uncapped. */
#define FULL_TRIGGER_MIN (4ull * 1024ull * 1024ull)
#define HEAP_FACTOR 2
#define MAX_TYPES 65535
#define CACHE_BLOCKS 2
/* The defragmenter's least gain (Stage 29 W-FIX1c, O202;
 * avant_gc_defrag.c): an evacuation must free, net, at least one block
 * for every DEFRAG_GAIN_DIV blocks' worth of live mature lines (and at
 * least one), because its fix-up walks every live object. */
#ifndef DEFRAG_GAIN_DIV
#define DEFRAG_GAIN_DIV 128
#endif
#define GC_THREAD_ROOTS 8

/* SCALAR_LOS_MIN: numeric (type-0) buffers at or above this size are
 * young large objects. They do not move, so a parallel loop can keep
 * the address in a register, and a young collection still frees them
 * when unmarked. 900 doubles is 7200 bytes: under LOS_LIMIT, but large
 * enough that copying the row is the slow path. */

/* The spaces (gc_heap.space). Every block belongs to mature space; young
 * objects lie in free lines of mature blocks (the YOUNG flag says which).
 * SPACE_NURSERY is the young allocator's cursor only (its list stays
 * empty). E's from-space and to-space (SPACE_FROM, SPACE_TO) went with E
 * in Stage 28 W15d. */

#define SPACE_MATURE 0
#define SPACE_NURSERY 1
#define SPACE_COUNT 2

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
  uint8_t kept;        /* 29: a young block's state in a young
                        * collection (SPLUS_*, YOUNG_EMPTY) */
  uint8_t young_lo;    /* 30: S: the lines young allocation used in this */
  uint8_t young_hi;    /* 31: block since the last collection, [lo, hi);
                        * hi 0: the block is not a young block (S) */
  uint8_t *bump;       /* 32: ABI (the inline allocation path) */
  uint8_t *limit;      /* 40 */
  uint64_t *start;     /* 48: START_WORDS words in gc_heap.starts */
  uint64_t idle;       /* 56: free: the return clock when freed; a young
                        * block in a young collection: its survivor
                        * bytes (S+) */
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

/* One space's state (gc-design.md §1.1, Stage 28 W15a1, O69), indexed
 * by SPACE_* in gc_heap.space (SPACE_COUNT of them):
 *   list   the space's blocks (avant_gc_heap.c owns the lists)
 *   cur    the space's bump cursor (avant_gc_alloc.c). A full block used
 *          to send the next allocation back through every older block
 *          (take_from_list + find_hole); workers allocate in mature
 *          space, and that walk dominated Matmul::T16.
 *   fresh  the cursor is a new block: when it fills, allocate another
 *          instead of rescanning full blocks. A full collection clears
 *          the cursor so the next allocation reuses holes.
 *   bytes  the block bytes of the space, kept as blocks enter a space,
 *          move between spaces and leave (W14a2), so gc_note_peak reads
 *          them instead of walking every list. */
typedef struct GcSpace {
  struct Block *list;
  struct Block *cur;
  uint8_t fresh;
  uint64_t bytes;
} GcSpace;

/* A walk over every large object (gc_large_next, avant_gc_los.c), in
 * address order. The walk may free the object it just returned. */
typedef struct LargeIter {
  uint64_t c;
  uint32_t p;
} LargeIter;

/* A frame on a thread's chain (avant_gc_top): generated code's frames
 * (avant_gc_enter) and runtime C's handle scopes (AVANT_GC_SCOPE, RC1)
 * alike; the type is avant_gc.h's AvantGcFrame. */
typedef AvantGcFrame Frame;

/* A registered thread's collector state as other threads see it (Stage
 * 28 W17a, gc-design.md section 4.4). state is GC_RUNNING, GC_NATIVE or
 * GC_SAFEPOINT (avant_gc_safepoint.c); owner is the thread's GcThread,
 * whose runtime-held slots every collection visits. */
enum { GC_RUNNING = 0, GC_NATIVE = 1, GC_SAFEPOINT = 2 };
struct GcThread;
typedef struct ThreadReg {
  struct ThreadReg *next;
  Frame **head;
  void *stack_lo;
  void *stack_hi;
  atomic_int state;
  struct GcThread *owner;
  /* The kernel's thread id, for the verifier's stalled-handshake report
   * (Stage 28 W18C, O125). */
  int tid;
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

/* A sorted set of blocks: a defragmenting full collection's evacuated
 * blocks about to be freed. (Until Stage 28 W16b also the mature blocks
 * the collecting thread's C stack named, C31's exclusion.) */
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
/* Card marking (Stage 28 W16t, Q5, O64), runtime-internal like REM. A
 * carded object is a large pointer buffer (LARGE and AVANT_TYPE_PTRS,
 * gc_carded): a store into it marks the card of its slot
 * (avant_barrier_slot) and a young collection scans only the slots of
 * its marked cards. The other objects keep object remembering (REM),
 * the hybrid of D232.
 * REMC: a carded object is in the remembered set. It is not REM, so the
 * generated code's inline barrier test (HEAP and YOUNG or REM, mir.md
 * §5.6) never skips a store into a mature carded object: every such
 * store reaches the runtime with its slot. CARD_ALL: every card counts
 * as marked (a barrier that named the holder only: avant_barrier,
 * avant_barrier_at); the next young collection scans the whole object
 * and then marks the cards exactly. CARD_SCAN: this young collection
 * scanned only its marked cards (cleared with MARKED, gc_clear_noted). */
#define AVANT_FLAG_REMC 1024u
#define AVANT_FLAG_CARD_ALL 2048u
#define AVANT_FLAG_CARD_SCAN 4096u
/* (128, AVANT_FLAG_LOS_KEEP, the promotion at first survival of a young
 * large object a heap field reached, is retired since Stage 28 W15a1:
 * an edge to a young large object is remembered like any young edge.) */

/* The verifier's visited bit (avant_gc_verify.c), internal to the
 * runtime like REM. FORWARDED was the verifier's until Stage 28 W15a2,
 * when it became the forwarding mark of the header (gc_set_forward). */
#define AVANT_FLAG_VSEEN 256u

/* A young collection's "promoted by this collection" bit (Stage 28 W15d,
 * S+): set by promote_in_place, cleared with MARKED by gc_clear_noted,
 * internal to the runtime like REM (128 was LOS_KEEP's, retired in
 * W15a1). S+'s holder aging reads it (splus_age_holders). */
#define AVANT_FLAG_PROMOTED_NOW 128u

/* The collector's counters, per family (Stage 28 W14b2, gc-design.md
 * §1.7, O41). AvantGcStats (avant_rt.h, ABI: avant_g_stats) keeps its
 * seven; every other counter is a field of one family here, written
 * where its event happens and printed by its family's routine in
 * avant_gc_stats.c (AVANT_GC_STATS=1, one line at exit). */

/* root: root bookkeeping call counts (Collector II Wave 0): every call to
 * avant_gc_root (and every slot of avant_gc_root_begin), avant_gc_reload,
 * and avant_gc_enter, so a profile can split call volume from per-call
 * cost; avant_gc_frame_enter counts its frame and the frame's slots too
 * (Stage 28 W18: the frames generated code links inline count nothing,
 * so in the inline forms these are the outermost frames). interiors counts the slots and root locations the young copy
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
 * phase oscillation is visible. The probe is retired since Stage 28
 * W15a1 (gc-design.md §5.2): the family stays in the statistics line,
 * always 0. */
typedef struct GcSrvCounters {
  uint64_t probes;
  uint64_t used;
  uint64_t surv;
  uint64_t min;
  uint64_t max;
  uint64_t hist[6];
} GcSrvCounters;

/* yng: min/max bound the adaptive young threshold over the run.
 * sticky counted the threshold's sticky growths (young_pause, retired
 * since Stage 28 W15a1: always 0; ctr.size counts the adaptations).
 * (cpy_peak, E's from-space plus to-space bytes during one young copy,
 * went with E in Stage 28 W15d; the statistics line prints 0.) */
typedef struct GcYngCounters {
  uint64_t min;
  uint64_t max;
  uint64_t sticky;
} GcYngCounters;

/* pk: the heap composition (block bytes per space, large-object bytes,
 * cached block bytes) at the moment peak_heap was last set
 * (gc_note_peak). (pk_from and pk_to, E's spaces, print 0 since Stage 28
 * W15d.) */
typedef struct GcPkCounters {
  uint64_t nursery;
  uint64_t mature;
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
 * large objects tenured by promote_young_large. trims and trim_bytes count
 * the large objects avant_gc_large_trim shrank in place and the run bytes
 * that went back (Stage 28 W18M, O52); drops and drop_bytes the large
 * buffers a growth freed at once through avant_gc_large_drop (O109), also
 * counted in freed. */
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
  uint64_t trims;
  uint64_t trim_bytes;
  uint64_t drops;
  uint64_t drop_bytes;
} GcLosCounters;

/* pause, why, frag, prom (Stage 28 W15a1, gc-design.md §3.4 M5 and
 * M6). pause: the wall time of every collection (CLOCK_MONOTONIC around
 * gc_collect's body), young and full summed in nanoseconds, and the
 * longest single pause. */
typedef struct GcPauseCounters {
  uint64_t young_ns;
  uint64_t full_ns;
  uint64_t max_ns;
  /* The handshake (Stage 28 W17a): collections that waited for at least
   * one other registered thread, the time spent waiting for the others to
   * reach a safepoint or native state (summed), and the longest wait. */
  uint64_t hs_n;
  uint64_t hs_ns;
  uint64_t hs_max_ns;
} GcPauseCounters;

/* why: full collections by cause (GcWhy): mature space grew by the
 * trigger mostly through promotion (bytes the young collection moved or
 * promoted into mature space since the last full collection) or mostly
 * through mature allocation (allocation in mature space: worker threads,
 * the JSON pool, the nursery off); the heap limit
 * (AVANT_HEAP_MAX_MB, a refused block or page run, or a full collection
 * C32 left pending); or an explicit request (avant_gc_collect, the HTTP
 * boundary's avant_gc_heap_goal_collect). */
typedef struct GcWhyCounters {
  uint64_t promotion;
  uint64_t mature;
  uint64_t limit;
  uint64_t heap_goal;
  uint64_t explicit_;
} GcWhyCounters;

/* frag: M6, at each full collection after the mark (before the sweep):
 * the free lines of the partly used blocks (some line live, some free)
 * of every space, and every block's lines. free_lines and lines are
 * summed over the full collections; weighted is §3.4 rule 5's aggregate,
 * the free-line fraction weighted by the heap bytes (gc_heap.heap_bytes)
 * at each full collection, kept as the two sums (num in line-fraction
 * times bytes, den in bytes) so the line prints num / den in parts per
 * million; max_ppm is the largest single collection's fraction. */
typedef struct GcFragCounters {
  uint64_t fulls;
  uint64_t free_lines;
  uint64_t lines;
  double num;
  double den;
  uint64_t max_ppm;
} GcFragCounters;

/* prom: bytes promoted into mature space by young collections: copied
 * (a survivor copied into a mature block, S+) and large (a young large
 * object whose YOUNG is cleared); in place, sticky.inplace. (prom_pinned,
 * E's pinned nursery objects, D205, printed 0 from W15d and left the line
 * in W16b: S+ promotes them in place like any survivor of a kept block.) */
typedef struct GcPromCounters {
  uint64_t copied;
  uint64_t large;
} GcPromCounters;

/* size: the young budget's adaptations (gc-design.md §5.2, young_size in
 * avant_gc_collect.c): doublings and halvings. */
typedef struct GcSizeCounters {
  uint64_t grows;
  uint64_t shrinks;
} GcSizeCounters;

/* (keep, E's kept-in-place nursery objects and blocks, Stage 28 W15a2,
 * went with E in Stage 28 W15d: the statistics line prints its three
 * fields as 0. S+ counts the blocks it keeps in place in
 * sticky.kept_blocks.) */

/* sticky (Stage 28 W15b, candidate S, gc-design.md §3.1): inplace is the
 * bytes S's young collections promoted in place (block objects; young
 * large objects count in prom.large as for E); blocks the young blocks
 * swept, holes the holes the young allocator took (a fresh block is one
 * hole), freed_lines the lines of the young blocks' used ranges the
 * young sweeps found without a survivor, overflow the medium objects
 * placed in the overflow block because the current hole could not hold
 * them, returned the empty young blocks the young sweeps gave back to
 * their chunks (Stage 28 W15c); under S+ (GC_STICKY_COPY) evac_blocks the
 * young blocks evacuated, kept_blocks the young blocks kept in place
 * because a young object in them was named imprecisely, copy_blocks the
 * fresh blocks the copies went to (the bytes copied are prom.copied and
 * AvantGcStats.copied_bytes); aged the bytes copied into survivor blocks
 * still young (YOUNG_AGE 2, Stage 28 W15d; in copied_bytes, not in
 * prom.copied), surv_blocks the survivor blocks they went to,
 * aged_holders the holders of young large objects aged out of blocks S+
 * does not evacuate (splus_age_holders); aged_inplace_blocks the survivor
 * blocks whose dense aged survivors were promoted in place at their next
 * collection (O97, Stage 28 W15e). */
typedef struct GcStickyCounters {
  uint64_t inplace;
  uint64_t blocks;
  uint64_t holes;
  uint64_t freed_lines;
  uint64_t overflow;
  uint64_t returned;
  uint64_t evac_blocks;
  uint64_t kept_blocks;
  uint64_t copy_blocks;
  uint64_t aged;
  uint64_t surv_blocks;
  uint64_t aged_holders;
  uint64_t aged_inplace_blocks;
} GcStickyCounters;

typedef struct GcCounters {
  GcRootCounters root;
  GcRemCounters rem;
  GcDefCounters def;
  GcSrvCounters srv;
  GcYngCounters yng;
  GcPkCounters pk;
  GcLosCounters los;
  GcPauseCounters pause;
  GcWhyCounters why;
  GcFragCounters frag;
  GcPromCounters prom;
  GcSizeCounters size;
  GcStickyCounters sticky;
  /* The largest live byte count a collection measured. */
  uint64_t peak_live;
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
  /* AVANT_GC_DEFRAG: mature evacuation in full collections
   * (avant_gc_defrag.c); off, the histogram stays on. */
  int defrag;
  /* AVANT_GC_VERIFY: the checkers of avant_gc_verify.c. */
  int verify;
  /* AVANT_GC_STATS: the statistics line at exit (avant_gc_stats.c). */
  int stats;
  /* AVANT_GC_STRESS (Stage 28 W14b1, gc-design.md §4.2 item 1, the
   * minimal step): a young collection at every outermost
   * avant_gc_defer_enter and at every avant_alloc (runtime C's, and
   * since Stage 29 W-FIX1c generated code's: the inline allocation path
   * is never published under it), with the nursery on. A young
   * collection frees every unreached young object and S+ moves the
   * survivors of the blocks it evacuates, so a helper that holds a young
   * reference the collector does not know across one of these points
   * reads a moved or freed object at once. */
  int stress;
  /* AVANT_GC_VERIFY_STALL_MS (Stage 28 W18C, O125): under the verifier, a
   * handshake that has waited this long for a thread to reach a safepoint
   * reports it and keeps waiting; 10000 by default, 0 never. */
  uint64_t stall_ms;
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
  /* The line marks' epoch (gc-design.md §2.2, Stage 28 W15b): a line is
   * live when its mark equals it (line_live). A full collection advances
   * it instead of zeroing every block's marks (gc_line_epoch_advance);
   * young collections mark with it and never advance it, so marks
   * persist across them. 0 is never an epoch: a block entering use has
   * its marks zeroed. */
  uint8_t line_epoch;
  uint32_t *pages;
  /* The card table (Stage 28 W16t): one byte per CARD_SIZE bytes of the
   * range, nonzero when a store into a carded object marked it; reserved
   * for the whole range, committed with each chunk's slice. */
  uint8_t *cards;
  GcChunk *chunks;
  /* Chunks below the high-water mark that are CHUNK_FREE again. */
  uint64_t free_chunks;
  /* Every small chunk below it has no free block (O53: blocks are taken
   * from the lowest chunk that has one, so high chunks drain and free up
   * whole). */
  uint64_t small_hint;
  /* Bytes of free blocks that are resident (not madvised). */
  uint64_t free_resident;
  /* The clock of gc_heap_return's hysteresis (Q12): a free block or
   * chunk is stamped with it when freed (idle) and returned only by a
   * later tick. It ticks at every full collection, and under candidate
   * S also at every young collection that gives empty young blocks back
   * (Stage 28 W15c, O89), so a program that never runs a full collection
   * can return memory; under E it equals the full-collection count. */
  uint64_t return_clock;
  /* The spaces (SPACE_*): each one's block list, bump cursor, fresh
   * bit and bytes (GcSpace). */
  GcSpace space[SPACE_COUNT];
  /* The mark work list. */
  void **work;
  uint32_t work_n;
  uint32_t work_cap;
  /* The type maps (avant_type_map), by type id. */
  TypeMap types[MAX_TYPES];
  uint8_t type_set[MAX_TYPES];

  /* avant_gc_alloc.c. Freed blocks kept for reuse (at most
   * CACHE_BLOCKS), still charged to heap_bytes. */
  Block *cache;
  int cache_n;
  /* (The C allocation cursor, its limit and the inline path's block are
   * per thread since Stage 28 W17b: GcThread.cur, .limit, .fast_block.) */
  /* Block bytes in use or cached, against set.heap_max. (E's copy
   * reserve, charged against it outside a collection, went with E in
   * Stage 28 W15d: S+'s copy is optional and stops at the limit.) */
  uint64_t heap_bytes;
  /* Set while a no-GC region takes a block past the heap limit (C32). */
  int over_limit;

  /* avant_gc_los.c. One dead scalar large buffer, kept for the next
   * same-sized alloc: its pages stay allocated, its start bit is clear
   * (no lookup finds it). */
  AvantHeader *los_spare;
  /* The page-run bytes of every large object (the spare excluded): the
   * peak decomposition's pk_large and the stats line's los_now_bytes. */
  uint64_t large_bytes;
  /* Bytes and count of young large objects. They do not move. A young
   * collection frees the ones the mark does not reach. Their bytes are
   * also counted in avant_nursery_held, the one young budget (Stage 28
   * W15a1, gc-design.md §5.1). */
  uint64_t young_large;
  int young_large_n;
  /* The young-allocation clock (Stage 28 W15e, O98): young bytes
   * allocated before the last collection (young_clock), and the young
   * bytes that collection left counted (young_carried, aged survivors and
   * young large objects that stay young), so the clock now is young_clock
   * + avant_nursery_held - young_carried (gc_young_clock_now). A young
   * large object's birth time is stamped in the 8 bytes before its header
   * (the run's padding, avant_gc_los.c). */
  uint64_t young_clock;
  uint64_t young_carried;
  /* The large-object bytes the running young collection marked (the
   * los family's live sample without a walk, O65). */
  uint64_t young_large_live;
  /* The large-object chunks (CHUNK_LARGE), in the order they were made:
   * the first fit looks in each, in this order. */
  uint32_t *lchunks;
  uint32_t lchunks_n;
  uint32_t lchunks_cap;

  /* avant_gc_remset.c. The remembered set (object remembering). (E's
   * per-entry rem_dirty and its frontier policy went with E in Stage 28
   * W15d: S+ empties the set at every young collection.) */
  void **remset;
  uint32_t rem_n;
  uint32_t rem_cap;
  /* (rem_mu, the lock barriers of a shared program took, went in Stage
   * 28 W17b: each thread remembers into its own buffer, GcThread.rembuf,
   * and only a collection, with every other thread stopped, or an
   * unregistering thread under the world lock, writes the set.) */
  /* The keep list: the holders a young collection found naming a young
   * object that stays young across it (gc_rem_keep_young_holders). */
  void **keep;
  uint32_t keep_n;
  uint32_t keep_cap;

  /* avant_gc_sticky.c. The noted list (C36, gc-design.md §2.6): every
   * object a young collection flags MARKED (its "traced this cycle" bit):
   * the young objects it reaches, the mature and large objects it scans
   * (R1'), the remembered objects. gc_clear_noted clears exactly those
   * when the collection ends, so no MARKED outlives the collection that
   * set it and no later young collection reads a stale one as "already
   * scanned". Full collections mark in the side bitmap and never touch
   * MARKED. Before W14a2 the marks were cleared by a walk of every object
   * of every space (clear_mark_flags). */
  void **noted;
  uint32_t noted_n;
  uint32_t noted_cap;
  /* A young collection tenures every reached young large object (a fixed
   * AVANT_YOUNG_MB, or S_LARGE_AGE 1; gc_reclaim_young_large). */
  int force_tenure;

  /* avant_gc_sticky.c and the young allocator (candidate S, Stage 28
   * W15b). young_blocks: the young blocks, every block
   * young allocation took a hole in since the last collection (each has
   * young_hi set). part_blocks: blocks of the mature list young
   * allocation may take next, ones with free lines among live ones (the
   * young sweep and a full collection's sweep leave them; an entry may
   * have been used up by mature allocation since, or be the mature
   * cursor's block, and is then skipped). Wholly free blocks go back to
   * their chunks (Stage 28 W15c; before, a free_blocks list kept them in
   * mature space until a full collection).
   * Each thread's young cursor, its overflow block (the young block
   * medium objects go to when the current hole cannot hold them, Immix's
   * overflow allocation) and cur_whole (the young cursor's hole is a
   * whole block, S_INLINE_HOLES) are the thread's own since Stage 28
   * W17b (GcThread.ycur, .overflow, .cur_whole): a young block belongs to
   * the one thread whose cursor took it until the next collection. */
  Block **young_blocks;
  uint32_t young_blocks_n;
  uint32_t young_blocks_cap;
  Block **part_blocks;
  uint32_t part_blocks_n;
  uint32_t part_blocks_cap;
  /* AVANT_GC_VERIFY (Stage 28 W16b): the blocks the last young sweep
   * left with free lines, held back from part_blocks until the next
   * collection begins (the lost-reference check, avant_gc_verify.c). */
  Block **verify_holes;
  uint32_t verify_holes_n;
  uint32_t verify_holes_cap;
  /* S+ (GC_STICKY_COPY): whether the running young collection may copy;
   * the block copies go to; copied_bytes when it began. splus_age: the
   * collection copies first survivors into survivor blocks (YOUNG_AGE 2,
   * not a fixed AVANT_YOUNG_MB); splus_sdst the survivor block they go
   * to, surv_blocks the survivor blocks this collection took (young
   * blocks from its end on), aged_now the bytes it aged. */
  int splus_on;
  Block *splus_dst;
  uint64_t splus_copied0;
  int splus_age;
  Block *splus_sdst;
  Block **surv_blocks;
  uint32_t surv_blocks_n;
  uint32_t surv_blocks_cap;
  uint64_t aged_now;
  /* One byte per block of the committed heap, 1 for a block the running
   * young collection moved objects out of (SPLUS_EVAC, SPLUS_PART):
   * splus_fix's test of every field it rereads, instead of two block
   * records (Stage 28 W15d, O93). Set before the fix-up, cleared by
   * splus_finish. */
  uint8_t *evac_map;
  uint64_t evac_map_n;

  /* avant_gc_defrag.c: the evacuated blocks a defragmenting full
   * collection frees once every reference is rewritten. */
  BlockSet def_release;
  /* The destination block the last evacuation filled last, or NULL
   * (Stage 29 W-FIX1c, O202): not a candidate of the next evacuation.
   * Cleared when the block is freed (gc_cache_or_free_block). */
  Block *def_last;


  /* avant_gc_verify.c. vnoted: the objects verify_trace flagged VSEEN
   * (its visited bit), cleared when each check ends (W14a2: no
   * whole-heap clearing walk follows any more). */
  void **vnoted;
  size_t vnoted_n;
  size_t vnoted_cap;
  /* The reports of gc_verify_sticky (S+'s checks after a young
   * collection's trace and copy). */
  uint64_t verify_untraced;
  /* The remembered-set check (gc_verify_remembered): the objects R1'
   * names this cycle (MARKED) and the missing barriers reported. */
  void **rnamed;
  size_t rnamed_n;
  size_t rnamed_cap;
  uint64_t missing_barriers;
  /* C36's check (gc_verify_no_stale_marks): the stale header marks
   * reported. */
  uint64_t stale_seen;
  /* The lost-reference check (Stage 28 W16b): its reports, precise and
   * on use, and the freed blocks it holds PROT_NONE until the next
   * collection (linked by next). */
  uint64_t verify_lost;
  /* The incremental verifier (Stage 28 W18M item 7, O119; avant_gc_verify.c,
   * gc_verify_begin): sd_mode is 1 when soft-dirty tracking works (probed
   * once at start-up, gc_verify_init), else 0 and every check is whole as
   * before; v_whole and v_incr count the whole and the incremental
   * verifications, v_pages_checked and v_pages_skipped the heap pages an
   * incremental one read and the clean pages it proved untouched. */
  int sd_mode;
  uint64_t v_whole;
  uint64_t v_incr;
  uint64_t v_pages_checked;
  uint64_t v_pages_skipped;
  /* The incremental reachability checks (Stage 28 W18V2): the young
   * collections whose M and T were both incremental, and the incremental
   * checks that found a candidate and ran the whole check. */
  uint64_t v_tincr;
  uint64_t v_fallback;
  Block *quarantine;
  uint32_t quarantine_n;
  /* The freed large runs it holds (header, protected range, state, the
   * collection that freed it or the last one before a growth dropped it,
   * and whether a growth dropped it: 1 a Buf's storage, 2 another
   * runtime buffer; Stage 28 W18C, O145). */
  struct GcHeldRun {
    AvantHeader *h;
    uint8_t *lo;
    uint8_t *hi;
    int state;
    int drop;
    uint64_t coll;
  } *held_runs;
  uint32_t held_runs_n;
  uint32_t held_runs_cap;
  /* No cap on what it holds (Stage 28 W18C, O120; until then 4,096 blocks
   * and 256 runs, past which freed memory was reused unchecked): the most
   * blocks and runs held at once, and the freed blocks and runs it could
   * not protect (a refused mprotect, the mapping-count limit) or not hold
   * (no memory for the record), which the stats line reports. */
  uint32_t v_held_blocks_max;
  uint32_t v_held_runs_max;
  uint64_t v_unprotected;

  /* avant_gc_collect.c. (collecting, the running collection's flag, is
   * the collecting thread's own since Stage 28 W17b: GcThread.collecting;
   * every other thread is stopped or native meanwhile.) */
  int inited;
  /* Set while a stress point's young collection runs (it makes no
   * sizing decision). */
  int stress_now;
  /* C32 (Stage 28 W14b1, gc-design.md §5.4): an allocation inside a no-GC
   * (defer) region that went past AVANT_HEAP_MAX_MB. The full collection
   * it needed runs at the first collection point after the region: the
   * next gc_maybe_collect, which the allocation fast paths (C and inline)
   * reach because they stay off while this is set. */
  int limit_pending;
  /* C39 (Stage 28 W15a2): consecutive heap-limit full collections after
   * less than one block of allocation, and the allocation count at the
   * last one (limit_progress_check, avant_gc_collect.c). */
  int limit_strikes;
  uint64_t limit_full_at;
  uint64_t mature_since_full;
  /* The part of mature_since_full that young collections promoted (for
   * the full collection's cause, ctr.why). */
  uint64_t promoted_since_full;
  /* The last young collection's survival exceeded half its young bytes
   * (young_size, gc-design.md §5.2; Stage 28 W15e). */
  uint8_t young_high_prev;
  /* Mature growth that triggers the next full collection (gc-design.md
   * §5.4, Stage 28 W15a1): FULL_TRIGGER_MIN before the first one, then
   * max(FULL_TRIGGER_MIN, (HEAP_FACTOR - 1) * the live bytes it left).
   * Before, twice the live set plus 4 MiB capped at 32 MiB: the cap made
   * a program with a large live set collect too often for its size. */
  uint64_t full_trigger;
  /* (The bytes allocated inside a thread's outermost no-GC region and
   * its entry's address are the thread's own since Stage 28 W17b:
   * GcThread.region_bytes, region_site.) */
  /* avant_gc_collect was called inside a no-GC region (RC4, Stage 28
   * W16a): the first collection point after it runs the full collection. */
  uint8_t explicit_pending;
  /* AVANT_GC_STRESS=1 (Stage 28 W16a): the line marks every young block
   * had when the young collection began (LINES_PER_BLOCK bytes per entry
   * of young_blocks), for the mixed blocks the stress mode evacuates. */
  uint8_t *stress_lines;
  uint8_t *stress_mixed;
  uint32_t stress_lines_cap;
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
  /* avant_gc_safepoint.c. The handshake (Stage 28 W17a, gc-design.md
   * section 4.4): its own lock, used only by the collector, the safepoint
   * and native transitions and thread registration (rule 1: never held
   * with a runtime lock a stopped thread could wait for); stopped_cv
   * wakes the collector when a thread stops or turns native, resume_cv
   * wakes the stopped threads when the collection is done. The request
   * flag itself is avant_gc_requested (generated code polls it). */
  pthread_mutex_t hs_mu;
  pthread_cond_t hs_stopped_cv;
  pthread_cond_t hs_resume_cv;
  /* (defer, the process-wide no-GC depth, went in Stage 28 W17b: each
   * thread's own, GcThread.nogc, is the region, O125.) */
  PinEnt *pins;
  /* The task roots (avant_gc_task_root_add, Stage 28 W16b): spawned
   * tasks' capture records, under the world lock. */
  AvantGcTaskRoot *task_roots;

  /* avant_gc_stats.c. */
  GcCounters ctr;
} GcHeap;

extern GcHeap gc_heap;

/* A mutator thread's collector state (Stage 28 W14b2, gc-design.md
 * §1.1): gc_thread, thread-local, defined in avant_gc_roots.c. */
typedef struct GcThread {
  /* The thread's free frames (its innermost frame is avant_gc_top,
   * avant_gc.h, since Stage 28 W16a: runtime C's scopes push onto it
   * inline). */
  Frame *frame_free;
  /* Every frame avant_gc_enter allocated on this thread, free or on its
   * chain (CFrame.all), so a thread that exits inside a callback frees the
   * ones still on its chain too (Stage 28 W18C, O129): the chain itself is
   * not walked at exit, since the exit unwound the stacks its generated
   * records lived in. */
  struct CFrame *frame_all;
  /* avant_gc_quiet_enter's depth: root registration is skipped inside. */
  int quiet;
  /* avant_alloc_in_mature(1): this thread allocates in mature space. */
  int alloc_mature;
  /* The thread's registration (gc_heap.threads), its frame head and
   * stack bounds. */
  ThreadReg reg;
  /* Runtime-held references (gc-design.md §1.4): thread-local slots of
   * the runtime's other units that hold a heap reference, registered by
   * avant_gc_add_root (once each, by the thread that owns them). A
   * collection reads and rewrites them like root slots. */
  void **roots[GC_THREAD_ROOTS];
  uint32_t nroots;
  /* The handshake (Stage 28 W17a, avant_gc_safepoint.c). native: the
   * depth of avant_gc_native_begin/end; native_on: the outermost begin
   * changed the state (it does not inside a no-GC region or on an
   * unregistered thread); nogc: this thread's own no-GC region depth (a
   * thread never stops inside one); collector: this thread holds the
   * request (it is collecting); foreign: registered by its first
   * avant_gc_enter and not the first thread (a C library's thread: native
   * between callbacks, rule 4); base_native: such a thread left its
   * outermost frame and is native until its next avant_gc_enter. */
  int native;
  int native_on;
  int nogc;
  int collector;
  int foreign;
  int base_native;
  /* The thread's allocation cursors (Stage 28 W17b, gc-design.md section
   * 4.4 "What it enables"; they were gc_heap's, one per process, so only
   * one thread could allocate young). cur and limit: the C cursor's block
   * and its hole's limit (the cursor itself is the thread's
   * avant_gc_bump); fast_block: the block the inline path is published
   * on; ycur: the young cursor's block (a young block, owned by this
   * thread until the next collection); overflow: its young overflow
   * block; cur_whole: ycur's hole is a whole block. A collection clears
   * every thread's (gc_alloc_stop_all, avant_gc_alloc.c).
   *
   * Hole accounting (a shared program, avant_gc_fast_ok == 2): the slow
   * path charges the rest of the young hole it hands the thread,
   * [tl_start, tl_end), to the one young budget (avant_nursery_held) and
   * the thread then bumps in it without the world lock (the inline path
   * and avant_alloc's fast path); the counters are made exact (the bytes
   * used counted, the rest given back) at the thread's next slow
   * allocation, at every collection, and when it unregisters
   * (gc_tl_settle). tl_end NULL: nothing is charged. bump_p, limit_p and
   * fast_ok_p are the addresses of the thread's avant_gc_bump,
   * avant_gc_limit and avant_gc_fast_ok, so a collector on another thread
   * can clear them while this one is stopped. */
  Block *cur;
  uint8_t *limit;
  Block *fast_block;
  Block *ycur;
  Block *overflow;
  int cur_whole;
  uint8_t *tl_start;
  uint8_t *tl_end;
  uint8_t **bump_p;
  uint8_t **limit_p;
  uint32_t *fast_ok_p;
  /* The thread's remembered-set buffer (Stage 28 W17b, MMTk's modbuf):
   * in a shared program a barrier appends the object it remembers here,
   * without a lock (the header's REM or REMC bit, set by an atomic or,
   * says it is remembered once); every collection merges every thread's
   * buffer into gc_heap.remset after the handshake (gc_rem_merge_all),
   * and an unregistering thread merges its own. rem_adds counts its adds
   * for ctr.rem.adds. */
  void **rembuf;
  uint32_t rembuf_n;
  uint32_t rembuf_cap;
  uint64_t rem_adds;
  /* Per-thread collector state (Stage 28 W17b, item 3; they were
   * gc_heap's): collecting, set while this thread runs a collection
   * (barriers and allocation read it; no other thread runs meanwhile);
   * the bytes allocated inside this thread's outermost no-GC region and
   * the address its helper entered it from (for "heap limit in a no-GC
   * region", gc-design.md section 5.4). The region's depth is nogc,
   * above. */
  int collecting;
  uint64_t region_bytes;
  void *region_site;
  /* `gc.hold { }` (std.gc, Stage 29 W1c2, D305): hold, the depth of this
   * thread's holds (nested holds are one); while it is above zero no
   * collection this thread would start runs (gc_maybe_collect returns,
   * a young collection `gc.collect()` asks for is noted in hold_young),
   * and the heap limit is the no-GC region's (C32: the hold's own
   * allocations, counted from hold_base, past the limit stop the program
   * with "heap limit in a no-GC region"). Unlike nogc it is no region:
   * the thread still stops at safepoints and turns native at blocking
   * waits, so other threads' collections run as before (they may move
   * what it holds; generated code roots everything). The outermost
   * end (avant_gc_hold_end) is a collection point that runs what fell
   * due inside. hold_site: the outermost entry's caller, for the
   * message. Inactive under AVANT_GC_STRESS=1. */
  int hold;
  int hold_young;
  uint64_t hold_base;
  void *hold_site;
} GcThread;

extern __thread GcThread gc_thread;

/* Every task root's slot (all threads': the list is global, under the
 * world lock the collector holds). Since Stage 28 W17a its object may
 * move like any rooted object (S+'s copy and defragmentation rewrite the
 * slot); until then it was PINNED while rooted, because the worker read
 * it while other threads collected (D191). */
#define EACH_TASK_ROOT(SLOT, BODY)                                       \
  do {                                                                   \
    AvantGcTaskRoot *etr_r_;                                             \
    for (etr_r_ = gc_heap.task_roots; etr_r_; etr_r_ = etr_r_->next) {   \
      void **SLOT = &etr_r_->obj;                                        \
      BODY;                                                              \
    }                                                                    \
  } while (0)

/* Every runtime-held reference slot of every registered thread (Stage 28
 * W14b2: replaces the callback into avant_hash.c, avant_hash_walk_tls;
 * every thread's since W17a, when the handshake stops the others: before,
 * only the collecting thread's were visited, part of D191's window), and
 * the collecting thread's own when it is not registered. */
static inline void gc_each_runtime_root(void (*fn)(void **slot)) {
  ThreadReg *t;
  uint32_t i;
  for (t = gc_heap.threads; t; t = t->next) {
    if (t->owner) {
      for (i = 0; i < t->owner->nroots; i++) {
        fn(t->owner->roots[i]);
      }
    }
  }
  if (!gc_thread.reg.head) {
    for (i = 0; i < gc_thread.nroots; i++) {
      fn(gc_thread.roots[i]);
    }
  }
}

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
void gc_work_push(void *p);
int gc_block_live_lines(Block *b);
int gc_sweep_block_lines(Block *b, int lo, int hi, uint64_t *freed);
size_t gc_obj_total(const AvantHeader *h);
int gc_is_heap_payload(void *p, Block **ob, AvantHeader **ol);
void *gc_object_payload(void *p, Block **ob, AvantHeader **ol);
void gc_foreach_block_object(Block *b, void (*fn)(AvantHeader *, Block *));
void gc_zero_line_marks(Block *list);
void gc_clear_side_marks(void);
void gc_line_epoch_advance(void);

/* avant_gc_alloc.c */
void gc_reset_block_bump(Block *b);
void gc_cache_or_free_block(Block *b);
void gc_free_block_now(Block *b);
void gc_note_peak(void);
Block *gc_new_block(uint8_t space);
void gc_use_block(Block *b);
void gc_fast_clear(void);
void gc_clear_alloc_cur(uint8_t space);
void gc_alloc_stop_all(void);
void gc_tl_settle(GcThread *t);
void gc_alloc_thread_init(void);
void gc_alloc_thread_leave(void);
void gc_alloc_shared_now(void);

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
void gc_rem_full_scrub(void);
void gc_rem_keep_young_holders(void);
void gc_cards_settle(void);
void gc_rem_merge_all(void);
void gc_rem_thread_leave(void);
void gc_hdr_or(AvantHeader *h, uint16_t bits);
void gc_hdr_and(AvantHeader *h, uint16_t bits);

/* GC_STICKY_COPY (Stage 28 W15c, gc-design.md §3.1, §3.3): 1 builds
 * S+, the young generation (D233): candidate S with the opportunistic
 * young copy (avant_gc_sticky.c: survivors of young blocks are copied out
 * when every reference to them is precise); 0 builds plain S, candidate
 * S alone (the in-place young collection), kept as an internal constant
 * because it costs one compile-time condition per S+ site (Stage 28
 * W15d: E, the other candidate, and runtime/build.sh's
 * AVANT_GC_CANDIDATE knob are deleted, D232). A build-time constant, not
 * ABI. SPLUS_SPARSE_LINES: a young block that held no
 * mature line is evacuated when its survivors' bytes fill at most this
 * many of its 128 lines. Measured at 32, 64, 96, 112 and 128 (Stage 28
 * W15c record, thr-v1 to thr-v3): 128, every such block, is the one
 * setting that recovers Etc::GameOfLife's locality (its long-lived cells
 * fill their blocks densely, so a sparse-only copy moves almost nothing:
 * 1.56 s at 96, 1.43 s at 128, E 1.42 s) and gives the compiler its
 * smallest heap (__emit-ll 277 MB at 128, 290 MB at 96, S 378 MB); its
 * cost is a copy of a dense live set that a later collection would not
 * have moved (Calculator::Interpreter 27 -> 44 MB, as under E). A block
 * that holds mature lines is never evacuated: it cannot be freed. */
#ifndef GC_STICKY_COPY
#define GC_STICKY_COPY 1
#endif
#ifndef SPLUS_SPARSE_LINES
#define SPLUS_SPARSE_LINES 128
#endif
/* Block.kept under S+ during a young collection: the block holds a young
 * object named imprecisely (it stays in place), it is evacuated, or it
 * it held no mature line when the collection began (a candidate), or its
 * evacuation stopped at the heap limit (SPLUS_PART; YOUNG_EMPTY, 4, is
 * the young sweep's, avant_gc_sticky.c), or it is a survivor block. */
#define SPLUS_KEPT 1
#define SPLUS_EVAC 2
#define SPLUS_PURE 3
#define SPLUS_PART 5
/* A survivor block this collection copies aged survivors into (YOUNG_AGE
 * 2, Stage 28 W15d). */
#define SPLUS_SURV 6
/* O97 (Stage 28 W15e): a survivor block between collections
 * (SPLUS_WAS_SURV, set when it becomes a young block) and in the next
 * young collection while it is a candidate (SPLUS_PURE_AGED); it is
 * evacuated only when its survivors fill at most SPLUS_AGED_SPARSE_LINES
 * of its 128 lines, otherwise they are promoted in place: an aged object
 * that survives again is copied once, not twice. Measured at 128 (every
 * survivor block evacuated, W15d's rule), 96, 64, 32 and 0 (W15e record,
 * var/o97-*): the compiler's __emit-ll copies 308 -> 168 MB at 96 and
 * below, its young pause 787 -> 716-719 ms at 64 and 32, peak_heap 228 MB
 * at every setting; Etc::GameOfLife copies 31.6 -> 19.1 MB; Sort and
 * Words unchanged (they hardly age). 64, half the block, is the sparse
 * test. */
#define SPLUS_WAS_SURV 7
#define SPLUS_PURE_AGED 8
/* AVANT_GC_STRESS=1 (Stage 28 W16a): a young block that held mature
 * lines when the collection began; its young survivors are evacuated all
 * the same and its line marks restored from gc_heap.stress_lines. */
#define SPLUS_MIXED 9
#ifndef SPLUS_AGED_SPARSE_LINES
#define SPLUS_AGED_SPARSE_LINES 64
#endif

/* avant_gc_sticky.c: the young generation (S+; candidate S with
 * GC_STICKY_COPY 0) and the noted list, and the young allocator's block
 * lists in avant_gc_alloc.c. */
void gc_note_marked(void *payload);
void gc_clear_noted(void);
void gc_sticky_collect(int tenure);
void gc_sticky_full_begin(void);
void gc_sticky_full_end(void);
void gc_sticky_push(Block ***v, uint32_t *n, uint32_t *cap, Block *b);

/* avant_gc_mark.c */
void gc_mark_roots(void);
int gc_keep_if_live(Block *b);
void gc_filter_blocks(Block **list, int (*keep)(Block *));

/* avant_gc_defrag.c */
void gc_block_set_add(BlockSet *s, Block *b);
void gc_block_set_seal(BlockSet *s);
int gc_block_set_has(const BlockSet *s, const Block *b);
void gc_defrag_evacuate(uint64_t live_lines_this);
void gc_defrag_release(void);

/* AVANT_GC_STRESS=1: freed memory is filled with GC_POISON (Stage 28 W16a). */
#define GC_POISON 0xdb
static inline void gc_poison(void *p, size_t n) {
  if (gc_heap.set.stress) {
    memset(p, GC_POISON, n);
  }
}

/* avant_gc_verify.c */
void gc_verify_no_stale_marks(void);
void gc_verify_remembered(void);
void gc_verify_defrag(void);
void gc_verify_sticky(void);
void gc_verify_lost(void);
void gc_verify_init(void);
void gc_verify_begin(int full);
/* Set by a free of a mature object outside a collection (a growth's freed
 * buffer, a trim), under AVANT_GC_VERIFY: the incremental checks rebuild
 * their record (avant_gc_verify.c). */
extern int gc_verify_rebase;
int gc_quarantine_block(Block *b);
void gc_quarantine_release(void);
void gc_quarantine_seal(void);
int gc_quarantine_run(AvantHeader *h);
void gc_quarantine_drop_seal(AvantHeader *h, int drop);
/* Whether a root of this thread names an address inside PAYLOAD's object
 * (avant_gc_los.c; Stage 28 W18C). */
int gc_rooted(void *payload);
void gc_large_give_held(AvantHeader *h);

/* avant_gc_collect.c */
/* A collection (O42): GC_YOUNG collects the young generation (S+ may
 * move young objects, so allow_move must be 1); GC_FULL marks and sweeps
 * the whole heap and, when allow_move is 1 and AVANT_GC_DEFRAG is on,
 * evacuates sparse mature blocks (defragmentation). A caller whose C
 * frames may hold references the collector cannot rewrite passes 0. */
typedef enum GcKind { GC_YOUNG, GC_FULL } GcKind;
/* Why a collection runs (ctr.why counts the full ones): the young
 * budget, the full-collection trigger (by promotion or by mature
 * allocation, decided from promoted_since_full), the heap limit, the
 * HTTP boundary (avant_gc_heap_goal_collect), an explicit request, or a
 * stress point. */
typedef enum GcWhy {
  GC_WHY_BUDGET,
  GC_WHY_TRIGGER,
  GC_WHY_LIMIT,
  GC_WHY_HEAP_GOAL,
  GC_WHY_EXPLICIT,
  GC_WHY_STRESS
} GcWhy;
void gc_collect(GcKind kind, int allow_move, GcWhy why);
void gc_limit_collect_pending(void);
void gc_region_limit_check(uint64_t bytes);
void gc_maybe_collect(void);
void gc_stress_point(void);
void gc_init_slow(void);
void gc_verify_exit_run(void (*check)(void));

/* avant_gc_stats.c */
void gc_print_stats(void);

/* avant_gc_roots.c */
extern atomic_int g_avant_shared;

/* avant_gc_safepoint.c (Stage 28 W17a, gc-design.md section 4.4): the
 * request flag generated code polls, the stop-the-world handshake, the
 * thread states. gc_stop_world returns 1 when this thread holds the
 * request and every other registered thread is at a safepoint or native,
 * 0 when another thread was collecting and this one waited at a
 * safepoint until it was done (the caller holds no runtime lock; rule
 * 1). gc_resume_world clears the request. gc_safepoint_slow stops the
 * calling thread while a request is raised (a no-op inside its own no-GC
 * region or while it collects). gc_lock takes a lock at a collecting call
 * (native while it waits, rule 2); gc_cond_wait waits natively. */
extern atomic_int avant_gc_requested;
int gc_stop_world(void);
void gc_resume_world(void);
void gc_safepoint_slow(void);
void gc_thread_registered(int foreign);
void gc_thread_unregistering(void);
void gc_thread_exiting(void);
void gc_lock(pthread_mutex_t *m);
void gc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m);
static inline void gc_safepoint(void) {
  if (__builtin_expect(atomic_load_explicit(&avant_gc_requested, memory_order_acquire) != 0, 0)) {
    gc_safepoint_slow();
  }
}

/* Small helpers every unit inlines (static inline: one copy per unit,
 * no symbol), moved from avant_rt.c unchanged. */

static inline size_t align_up(size_t n) {
  return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

/* The program is shared (a second thread registered): barriers remember
 * into the thread's buffer and change header flags atomically, and
 * allocation uses hole accounting (Stage 28 W17b; the name is from when
 * a shared program's barriers took rem_mu). */
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

static inline int line_live(const Block *b, int i) {
  return b->line_mark[i] == gc_heap.line_epoch;
}

/* Line marks read a word (eight lines) at a time (Stage 28 W15c, O87;
 * shared since W15d, O94): the epoch replicated into every byte, the high
 * bit of every byte of MARKS equal to it (exact: no borrow crosses a
 * byte), and the count of those bits (no popcount instruction is assumed:
 * without -mpopcnt __builtin_popcountll is a library call). */
#define LINE_WORDS (LINES_PER_BLOCK / 8)
static inline uint64_t line_epoch8(void) {
  return 0x0101010101010101ULL * (uint64_t)gc_heap.line_epoch;
}

static inline uint64_t live_bytes_mask(uint64_t marks, uint64_t epoch8) {
  uint64_t x = marks ^ epoch8;
  uint64_t t = (x & 0x7f7f7f7f7f7f7f7fULL) + 0x7f7f7f7f7f7f7f7fULL;
  return ~(t | x | 0x7f7f7f7f7f7f7f7fULL);
}

static inline int live_byte_count(uint64_t mask) {
  return (int)(((mask >> 7) * 0x0101010101010101ULL) >> 56);
}

static inline void mark_lines(Block *b, size_t off, size_t total) {
  size_t start = off / LINE_SIZE;
  size_t end = (off + total - 1) / LINE_SIZE;
  size_t i;
  for (i = start; i <= end && i < LINES_PER_BLOCK; i++) {
    b->line_mark[i] = gc_heap.line_epoch;
  }
}

/* Is the object at START (its block B, or its large header OL, as
 * gc_object_payload gives them) young? A young object lies in any block,
 * among mature ones, and only its YOUNG flag says so (since Stage 28
 * W15b; under E, deleted in W15d, a block's space said so). */
static inline int gc_young_at(void *start, const Block *b, const AvantHeader *ol) {
  (void)b;
  (void)ol;
  return (avant_header(start)->flags & AVANT_FLAG_YOUNG) != 0;
}

/* S_INLINE_HOLES (Q11; Stage 28 W15b item 3): 1 (the
 * default), the inline allocation path runs in holes of recycled blocks
 * as in whole free blocks: the slow path sets avant_gc_bump, avant_gc_limit
 * and avant_gc_block to the hole (the generated code stores its cursor
 * in Block.bump and needs nothing else), and the hole's start bits are
 * clear (gc-design.md §2.3: the sweeps clear the lines they free). 0
 * publishes the inline path only in whole blocks; holes are then filled
 * through the C path (kept for Q11's measurement). */
#ifndef S_INLINE_HOLES
#define S_INLINE_HOLES 1
#endif

/* The full collection's mark (Stage 28 W14a2, gc-design.md §2.2, C36):
 * one bit per 8-byte granule in the side table gc_heap.marks, at the granule
 * of the object's header (the same index as its start bit), cleared per
 * chunk by gc_clear_side_marks when a full collection starts. Every
 * object, large ones included, lies in the reservation. The header's
 * MARKED bit is the young collection's alone ("traced this cycle"), and
 * every MARKED a young collection sets is cleared when it ends (the
 * noted list, avant_gc_sticky.c). */
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

/* gc_object_payload with its common case in line (Stage 29 W-FIX1c,
 * O203): P is the exact start of an object in a small-object block in
 * use, as nearly every traced field and root is. The tests are
 * gc_is_heap_payload's own, in its order (8-aligned, the header inside
 * the committed range, its start bit, the chunk's kind, the block in
 * use), so the answer is the same; any other address (inside an object,
 * a large object's, a static object's, outside the heap) takes the
 * function. Before, every traced reference paid two calls for this
 * (gc_object_payload, then gc_is_heap_payload inside it). */
static inline void *gc_object_payload_fast(void *p, Block **ob, AvantHeader **ol) {
  uintptr_t off = (uintptr_t)p - (uintptr_t)gc_heap.base - sizeof(AvantHeader);
  if (!p) {
    return NULL;
  }
  if (!((uintptr_t)p & (ALIGN - 1)) && off < gc_heap.committed) {
    uint64_t g = off >> 3;
    if ((gc_heap.starts[g >> 6] >> (g & 63)) & 1ULL) {
      if (gc_heap.chunks[off >> CHUNK_SHIFT].kind == CHUNK_SMALL) {
        Block *b = &gc_heap.blocks[off >> BLOCK_SHIFT];
        if (b->in_use) {
          if (ob) {
            *ob = b;
          }
          if (ol) {
            *ol = NULL;
          }
          return p;
        }
      }
    }
  }
  return gc_object_payload(p, ob, ol);
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

/* A carded object (card marking, Stage 28 W16t): a large pointer
 * buffer. */
static inline int gc_carded(const AvantHeader *h) {
  return (h->flags & AVANT_FLAG_LARGE) && h->type_id == AVANT_TYPE_PTRS;
}

static inline uint64_t gc_card_index(const void *a) {
  return (uint64_t)((uintptr_t)a - (uintptr_t)gc_heap.base) >> CARD_SHIFT;
}

/* Visit the slots of carded payload P that lie in its marked cards, as
 * `void **SLOT` (a run of eight clear cards is skipped at once). BODY
 * must not use break or continue. */
#define EACH_CARD_SLOT(P, SLOT, BODY)                                               \
  do {                                                                              \
    uint8_t *ecs_lo_ = (uint8_t *)(P);                                              \
    uint8_t *ecs_hi_ = ecs_lo_ + avant_header(ecs_lo_)->size;                       \
    uint64_t ecs_c_ = gc_card_index(ecs_lo_);                                       \
    uint64_t ecs_e_ = gc_card_index(ecs_hi_ - 1) + 1;                               \
    while (ecs_c_ < ecs_e_) {                                                       \
      if ((ecs_c_ & 7) == 0 && ecs_c_ + 8 <= ecs_e_ &&                              \
          *(const uint64_t *)(gc_heap.cards + ecs_c_) == 0) {                       \
        ecs_c_ += 8;                                                                \
        continue;                                                                   \
      }                                                                             \
      if (gc_heap.cards[ecs_c_]) {                                                  \
        uint8_t *ecs_a_ = gc_heap.base + (ecs_c_ << CARD_SHIFT);                    \
        uint8_t *ecs_b_ = ecs_a_ + CARD_SIZE;                                       \
        void **SLOT;                                                                \
        if (ecs_a_ < ecs_lo_) {                                                     \
          ecs_a_ = ecs_lo_;                                                         \
        }                                                                           \
        if (ecs_b_ > ecs_hi_) {                                                     \
          ecs_b_ = ecs_hi_;                                                         \
        }                                                                           \
        for (SLOT = (void **)ecs_a_; SLOT < (void **)ecs_b_; SLOT++) {              \
          BODY                                                                      \
        }                                                                           \
      }                                                                             \
      ecs_c_++;                                                                     \
    }                                                                               \
  } while (0)

/* Clear every card of carded payload P. */
static inline void gc_cards_clear(void *p) {
  uint64_t c0 = gc_card_index(p);
  uint64_t c1 = gc_card_index((uint8_t *)p + avant_header(p)->size - 1);
  memset(gc_heap.cards + c0, 0, (size_t)(c1 - c0 + 1));
}

static inline void *payload_of(AvantHeader *h) {
  return (void *)(h + 1);
}

/* Forwarding in the header (Stage 28 W15a2, gc-design.md §3.2): a moved
 * object's old header keeps its flags word (FORWARDED set) and holds
 * the new payload address in the 48 bits of size and type id, which the
 * old copy no longer needs (heap addresses are below 2^48,
 * gc_heap_reserve). Nothing reads an old copy's size or type after the
 * move: an exact start resolves to its forward (gc_forward_of), an
 * interior address is bounded through it (gc_obj_total_any), the
 * verifier reads the moved copy, and no walk visits a moved object
 * (from-space blocks are released whole; the blocks a young collection
 * keeps for pinned survivors keep only those survivors' start bits; a
 * defragmentation stopped mid-block clears the moved objects' bits).
 * It replaces the side forwarding table (fwd_put/fwd_get, an open
 * addressing hash cleared whole at every collection). The flags stay
 * readable, so the barrier's inline test and avant_is_young are
 * unchanged; the generated code never reads a moved object. */
static inline void gc_set_forward(AvantHeader *h, void *to) {
  uint64_t a = (uint64_t)(uintptr_t)to;
  h->size = (uint32_t)a;
  h->type_id = (uint16_t)(a >> 32);
  h->flags = (uint16_t)(h->flags | AVANT_FLAG_FORWARDED);
}

static inline void *gc_forward_of(const AvantHeader *h) {
  return (void *)(uintptr_t)((uint64_t)h->size | ((uint64_t)h->type_id << 32));
}

/* The forward of payload P when P is a moved object's start, else NULL. */
static inline void *gc_forwarded(void *p) {
  const AvantHeader *h = avant_header(p);
  return (h->flags & AVANT_FLAG_FORWARDED) ? gc_forward_of(h) : NULL;
}

/* An object's size with its header, read through the forward for a
 * moved object (whose own size field holds the address). */
static inline size_t gc_obj_total_any(const AvantHeader *h) {
  if (h->flags & AVANT_FLAG_FORWARDED) {
    return align_up(sizeof(AvantHeader) + (size_t)avant_header(gc_forward_of(h))->size);
  }
  return align_up(sizeof(AvantHeader) + (size_t)h->size);
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
