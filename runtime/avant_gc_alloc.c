#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocation (moved from avant_rt.c): block supply and
 * recycling, the per-space bump cursors, the inline nursery fast path the
 * generated code shares (avant_gc_bump and its mirrors), the slow path
 * (place), and the allocation entry points avant_alloc and
 * avant_alloc_in_mature. Collection decisions are avant_gc_collect.c's;
 * large objects are avant_gc_los.c's. */

/* Inline nursery fast path (its bookkeeping is the runtime's): generated code
 * bumps avant_gc_bump directly while its end stays at or below the limit
 * the runtime publishes (avant_gc_limit for an unshared module,
 * avant_gc_limit_mt for a shared one: the hole's limit capped by the
 * young budget, or null when the path may not run), and writes the
 * header and the zeroed payload. That is all an unshared module's path
 * does; a shared module's also sets the object's start bit through
 * avant_gc_starts/avant_gc_base. The C cursor avant_gc_bump is the same
 * storage, so both sides share one cursor. Only the runtime publishes or
 * clears the path (gc_fast_publish, gc_fast_refresh below); its state is
 * the thread's GcThread.fast_ok; the limit it holds is avant_gc_limit_mt. */
__thread uint8_t *avant_gc_bump;
__thread uint8_t *avant_gc_limit;
__thread uint8_t *avant_gc_limit_mt;
__thread uint8_t *avant_gc_base;
__thread uint64_t *avant_gc_starts;

/* The inline path's state as code generated before the path's rework
 * reads it (a seed compiler's output: the compiler B of tools/letters.sh,
 * while its seed is a release from before the rework, as v0.42.0 is, tests
 * avant_gc_fast_ok before it bumps, stores its cursor through
 * avant_gc_block and keeps the counters itself). Never set: such
 * code always calls avant_alloc, which is correct for it and slower; the
 * new path has its own limit and no flag. To delete, with their lines in
 * avant_rt.exports, when a release with the rework is the seed. */
__thread uint32_t avant_gc_fast_ok;
__thread void *avant_gc_block;

uint64_t avant_since_gc;

/* Allocations generated code made without a call: counted only when
 * AVANT_GC_STATS (or a check mode) is set, by reading the headers of
 * each inline run when it is counted (inline_walk); 0 otherwise. Before
 * the path's rework it incremented it at every allocation. */
uint64_t avant_inline_allocs;

/* The first line of B at or after FROM that is live (LIVE 1) or free
 * (LIVE 0), or LINES_PER_BLOCK: the line marks read a word (eight lines)
 * at a time against the epoch, as gc_sweep_block_lines reads them (find_hole
 * used to compare them a byte at a time, 43% of a small program's stress
 * collection). */
static inline int next_line(const Block *b, int from, int live, uint64_t epoch8) {
  int k = from >> 3;
  int sh = 8 * (from & 7);
  while (k < LINE_WORDS) {
    uint64_t m;
    uint64_t bits;
    memcpy(&m, b->line_mark + 8 * k, sizeof m);
    bits = live_bytes_mask(m, epoch8);
    if (!live) {
      bits = ~bits & 0x8080808080808080ULL;
    }
    bits >>= sh;
    if (bits) {
      return 8 * k + (sh >> 3) + (__builtin_ctzll(bits) >> 3);
    }
    sh = 0;
    k++;
  }
  return LINES_PER_BLOCK;
}

/* AVANT_GC_VERIFY's check of find_hole: the search a line at a time, as
 * it used to be; the program stops when the two
 * disagree on whether B has a hole of NEED bytes from its search cursor
 * or on where it is ([*LO, *HI) in lines). */
static void find_hole_check(const Block *b, size_t need, int found, int lo, int hi) {
  int i = b->search;
  int ok = 0;
  int j = 0;
  while (i < LINES_PER_BLOCK && !ok) {
    if (line_live(b, i)) {
      i++;
      continue;
    }
    j = i;
    while (j < LINES_PER_BLOCK && !line_live(b, j)) {
      j++;
    }
    if ((size_t)(j - i) * LINE_SIZE >= need) {
      ok = 1;
    } else {
      i = j;
    }
  }
  if (ok != found || (ok && (i != lo || j != hi))) {
    gc_die("avant gc verify: the hole search by words and the search by lines disagree");
  }
}

/* The first hole of B at or after its search cursor that holds NEED
 * bytes: bump and limit are set to it. A hole is a run of free lines. */
static int find_hole(Block *b, size_t need) {
  uint64_t epoch8 = line_epoch8();
  int i = b->search;
  while (i < LINES_PER_BLOCK) {
    int j;
    i = next_line(b, i, 0, epoch8);
    if (i >= LINES_PER_BLOCK) {
      break;
    }
    j = next_line(b, i + 1, 1, epoch8);
    if ((size_t)(j - i) * LINE_SIZE >= need) {
      if (gc_heap.set.verify) {
        find_hole_check(b, need, 1, i, j);
      }
      b->bump = b->base + (size_t)i * LINE_SIZE;
      b->limit = b->base + (size_t)j * LINE_SIZE;
      return 1;
    }
    i = j;
  }
  if (gc_heap.set.verify) {
    find_hole_check(b, need, 0, 0, 0);
  }
  return 0;
}

void gc_reset_block_bump(Block *b) {
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base;
  if (!find_hole(b, ALIGN)) {
    b->bump = b->base + BLOCK_SIZE;
    b->limit = b->base + BLOCK_SIZE;
    b->search = LINES_PER_BLOCK;
  }
}

void gc_cache_or_free_block(Block *b) {
  /* Only a collection frees a block, and it has cleared every thread's
   * cursors first (gc_alloc_stop_all); the mature cursor is global. */
  if (gc_heap.space[SPACE_MATURE].cur == b) {
    gc_clear_alloc_cur(SPACE_MATURE);
  }
  if (gc_heap.def_last == b) {
    gc_heap.def_last = NULL;
  }
  /* Out of use: no lookup finds it, as when it left the block hash. A
   * cached block stays charged to gc_heap.heap_bytes; a released one goes back
   * to its chunk's free list (resident until a full collection's
   * gc_heap_return). */
  b->in_use = 0;
  gc_heap.space[b->space].bytes -= BLOCK_SIZE;
  /* AVANT_GC_VERIFY: held PROT_NONE until the next collection (the
   * lost-reference check's on-use half, avant_gc_verify.c). */
  if (gc_quarantine_block(b)) {
    return;
  }
  gc_free_block_now(b);
}

/* The tail of gc_cache_or_free_block: B, out of use, is cached or given
 * back to its chunk. */
void gc_free_block_now(Block *b) {
  if (gc_heap.cache_n < CACHE_BLOCKS) {
    b->next = gc_heap.cache;
    gc_heap.cache = b;
    gc_heap.cache_n++;
    return;
  }
  gc_block_give(b);
  gc_heap.heap_bytes -= BLOCK_SIZE;
}

/* When the heap high-water mark moves, snapshot what
 * the heap is made of, so a peak can be decomposed after the fact.
 * O(1): counters, not walks of the block lists. */
void gc_note_peak(void) {
  if (gc_heap.heap_bytes <= avant_g_stats.peak_heap) {
    return;
  }
  avant_g_stats.peak_heap = gc_heap.heap_bytes;
  gc_heap.ctr.pk.nursery = gc_heap.space[SPACE_NURSERY].bytes;
  gc_heap.ctr.pk.mature = gc_heap.space[SPACE_MATURE].bytes;
  gc_heap.ctr.pk.cache = (uint64_t)gc_heap.cache_n * BLOCK_SIZE;
  gc_heap.ctr.pk.large = gc_heap.large_bytes;
}

static int heap_block_fits(void) {
  if (!gc_heap.set.heap_max || gc_heap.over_limit) {
    return 1;
  }
  return gc_heap.heap_bytes + BLOCK_SIZE <= gc_heap.set.heap_max;
}

Block *gc_new_block(uint8_t space) {
  if (!heap_block_fits() && gc_heap.cache == NULL) {
    return NULL;
  }
  Block *b = NULL;
  if (gc_heap.cache) {
    b = gc_heap.cache;
    gc_heap.cache = b->next;
    gc_heap.cache_n--;
    memset(b->line_mark, 0, LINES_PER_BLOCK);
    memset(b->start, 0, START_WORDS * sizeof(uint64_t));
    b->next = NULL;
  } else {
    if (!heap_block_fits()) {
      return NULL;
    }
    b = gc_block_take();
    gc_heap.heap_bytes += BLOCK_SIZE;
    gc_note_peak();
  }
  /* Block memory is not pre-zeroed. Every allocation zeroes its own
   * payload in init_object, so a fresh or recycled block costs no
   * whole-block clearing pass ahead of the mutator's own writes. */
  b->space = space;
  gc_heap.space[space].bytes += BLOCK_SIZE;
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base + BLOCK_SIZE;
  b->in_use = 1;
  /* At the head of the space's order (GcSpace's comment: the top of the
   * back stack while the order is reversed). */
  if (gc_heap.space[space].rev) {
    b->next = gc_heap.space[space].back;
    gc_heap.space[space].back = b;
  } else {
    b->next = gc_heap.space[space].list;
    gc_heap.space[space].list = b;
  }
  return b;
}

/* The inline fast path's limit: the current block's limit, capped where
 * the young budget runs out, so a collecting allocation reaches
 * avant_alloc (which collects) when the budget is spent. Once it is spent
 * the cap is the block's end, so a collecting allocation
 * overshoots the budget by at most the rest of one block before
 * avant_alloc collects. (The cap was chosen for the bump constructor's
 * leaves, which the language no longer has.) */
static uint8_t *inline_cap(void) {
  uint64_t rem = avant_young_bytes > avant_nursery_held ? avant_young_bytes - avant_nursery_held : 0;
  uint8_t *cap = avant_gc_bump + rem;
  if (gc_thread.limit && (rem == 0 || cap > gc_thread.limit)) {
    return gc_thread.limit;
  }
  return cap;
}

/* What generated code sees of the inline path (the guards folded into
 * the limit): the thread's limit when the
 * path may run, else null, so the path's one compare of its cursor's new
 * end against the limit also answers "may it run" (a null limit fails
 * every compare, whatever the cursor holds). An unshared module's code
 * may run it only while the program has one thread (state 1: before the
 * rework it tested g_avant_shared and avant_gc_fast_ok == 1 inline); a
 * shared module's in states 1 and 2 (it tested avant_gc_fast_ok != 0).
 * In state 1 the cursor is noted (GcThread.mark): the path's allocations
 * from there on are counted by gc_alloc_sync. LIM is the hole's limit
 * capped by the young budget (inline_cap), or a thread's charged hole's
 * end. The caller has synced
 * (gc_alloc_sync) before it moved the cursor or changed the state; a C
 * allocation that moved the cursor counted itself (account_alloc). */
static inline void gc_fast_publish(uint8_t *lim) {
  uint32_t ok = gc_thread.fast_ok;
  if (ok == 1) {
    avant_gc_limit_mt = lim;
    avant_gc_limit = lim;
    gc_thread.mark = avant_gc_bump;
  } else {
    avant_gc_limit_mt = ok ? lim : NULL;
    avant_gc_limit = NULL;
    gc_thread.mark = NULL;
  }
}

/* The single mutator's published limit is brought to what the young
 * budget allows now (inline_cap), when the path is published. An
 * allocation in C leaves the limit so when it ends: bump_take for one in
 * the path's own hole, and this for one that does not bump there (a
 * large object, a medium one in the overflow block), so that
 * gc_fast_refresh need not publish at every allocation's entry a limit
 * that bump_take replaces a moment later. The values generated code
 * sees between two allocations are the ones it saw when the entry
 * published too. */
static void gc_fast_settle(void) {
  if (gc_thread.fast_ok == 1 && gc_thread.fast_block) {
    uint8_t *lim = inline_cap();
    if (avant_gc_limit_mt != lim) {
      avant_gc_limit_mt = lim;
      avant_gc_limit = lim;
    }
  }
}

void gc_use_block(Block *b) {
  /* The cursor moves: the inline allocations up to it are counted first
   * (every caller is inside a slow allocation, which synced at its
   * entry; nothing is pending here). */
  gc_alloc_sync();
  gc_thread.mark = NULL;
  /* No run of late start bits continues across a move of the cursor (a
   * run lies in one hole). */
  gc_heap.lazy_hi = NULL;
  gc_thread.cur = b;
  avant_gc_bump = b->bump;
  gc_thread.limit = b->limit;
  /* The young cursor's block is a mature-space block (a fresh one or a
   * recycled one with holes); the inline path follows it into its holes
   * (their start bits are clear) unless S_INLINE_HOLES is 0 and the hole
   * is not a whole block. Any other block clears it,
   * so generated code never writes start bits through another block's
   * base. */
  if (b == gc_thread.ycur && !gc_thread.collecting &&
      (S_INLINE_HOLES || gc_thread.cur_whole)) {
    gc_thread.fast_block = b;
    avant_gc_base = b->base;
    avant_gc_starts = b->start;
    gc_fast_publish(inline_cap());
  } else {
    gc_fast_clear();
  }
}

static inline __attribute__((always_inline)) int bump_fits(size_t need) {
  return gc_thread.cur && avant_gc_bump && avant_gc_bump + need <= gc_thread.limit;
}

static void *bump_take(size_t need) {
  Block *b = gc_thread.cur;
  size_t off = (size_t)(avant_gc_bump - b->base);
  /* A young block's holes have clear start bits (the sweeps clear the
   * lines they free), as the inline path requires. Any other allocation
   * is mature and marks its lines live now: young allocation may recycle
   * this block's holes before the next full collection, and finds them by
   * the line marks. */
  if (!b->young_hi) {
    if (!gc_heap.space[b->space].fresh) {
      clear_starts(b, off, need);
    }
    mark_lines(b, off, need);
  }
  set_start(b, off);
  void *h = avant_gc_bump;
  avant_gc_bump += need;
  b->bump = avant_gc_bump;
  if (b == gc_thread.fast_block) {
    uint8_t *lim = inline_cap();
    if (gc_thread.fast_ok == 1) {
      /* The single mutator: the limit when it moved (it does not when
       * this allocation is as long as the last one), and the mark past
       * this object, which account_alloc counts. */
      if (avant_gc_limit_mt != lim) {
        avant_gc_limit_mt = lim;
        avant_gc_limit = lim;
      }
      gc_thread.mark = avant_gc_bump;
    } else {
      gc_fast_publish(lim);
    }
  }
  return h;
}

/* The inline path is off: the limit generated code reads is null ("limit
 * zero"). The inline allocations made so far are counted first. */
void gc_fast_clear(void) {
  gc_alloc_sync();
  gc_thread.fast_ok = 0;
  gc_thread.fast_block = NULL;
  gc_fast_publish(NULL);
}

/* Late start bits. An unshared module's
 * inline path sets no start bit, so a young object it allocated has none
 * until one of these sets it: a young collection promotes the object and
 * leaves it in place (gc_clear_noted, avant_gc_sticky.c; one it copies
 * out gets its copy's); a full collection marks it (mark_object,
 * avant_gc_mark.c); a lookup by the start bits needs its block's
 * (gc_lazy_touch, avant_gc_internal.h: a root slot, a pin or the keep
 * list may name the inside of an object, and C asks avant_is_heap); or
 * the program becomes shared (every run is built: gc_lazy_build_all).
 * An object that dies young never gets one, and the young sweep has
 * none of its to clear. The traces need none: a field holds an exact
 * start (the field rule, avant_gc_internal.h).
 *
 * What makes the late bit possible is that the objects of a hole lie end
 * to end from the first one the inline path allocated in it: each
 * header gives the next one's place. gc_heap.lazy is the list of those
 * runs, [lo, hi): lo is the cursor where the thread's first sync found
 * inline allocations in the hole (GcThread.mark), hi the cursor at its
 * last sync there. A later sync in the same hole makes the run longer
 * (gc_heap.lazy_hi is set while the cursor has not left the hole, and
 * the cursor only moves forward in it), over whatever C allocated in
 * between (bump_take, which sets each such object's own bit: a run may
 * hold objects that have theirs). One run a hole, then, however inline
 * and C allocations alternate in it.
 *
 * The rule a walk relies on: a block object's size does not change in
 * place, except through avant_gc_shrink (below), which leaves the bytes
 * it frees as a filler header (not a heap object: no flag, no start bit)
 * so that the headers still follow one another. Runtime C that shortens
 * an object it has just allocated calls it (avant_zlib.c); a large
 * object is trimmed by avant_gc_large_trim and lies in no run.
 * AVANT_GC_VERIFY checks every run against the bits an eager path would
 * have set (avant_gc_verify.c, gc_verify_starts_*), which also finds a
 * size changed any other way.
 *
 * Only the single mutator has late bits: a shared module's inline path
 * sets each bit itself, and an unshared module's path is off in a
 * program with threads (gc_fast_publish), so no thread looks an address
 * up in a hole another thread is bumping in. */

/* The start bits of the objects in [LO, HI), headers end to end. A
 * filler (avant_gc_shrink) gets none, nor does an object a young
 * collection's copy has moved (no lookup may find a forwarded header:
 * splus_finish clears the ones set before the copy in the blocks that
 * stay), whose size is its copy's. */
static void lazy_walk(uint8_t *lo, uint8_t *hi) {
  uint8_t *p = lo;
  while (p < hi) {
    const AvantHeader *h = (const AvantHeader *)p;
    if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_FORWARDED)) == AVANT_FLAG_HEAP) {
      start_bit_set(p, 1);
    }
    p += gc_obj_total_any(h);
  }
  if (p != hi) {
    gc_die("avant gc: a run of allocations does not end at an object's end");
  }
}

/* A new run [LO, HI): the inline path allocated at LO, where no run
 * ended. */
static __attribute__((noinline)) void lazy_new_run(uint8_t *lo, uint8_t *hi) {
  uint64_t bi = (uint64_t)(lo - gc_heap.base) >> BLOCK_SHIFT;
  uint32_t n = gc_heap.lazy_n;
  if (n == gc_heap.lazy_cap) {
    gc_heap.lazy_cap = gc_heap.lazy_cap ? gc_heap.lazy_cap * 2 : 64;
    gc_heap.lazy = gc_xrealloc(gc_heap.lazy, (size_t)gc_heap.lazy_cap * sizeof(*gc_heap.lazy));
  }
  gc_heap.lazy[n].lo = lo;
  gc_heap.lazy[n].hi = hi;
  gc_heap.lazy_n = n + 1;
  gc_heap.lazy_hi = hi;
  if (bi >= gc_heap.lazy_idx_n) {
    uint64_t need = gc_heap.committed >> BLOCK_SHIFT;
    gc_heap.lazy_idx = gc_xrealloc(gc_heap.lazy_idx, (size_t)need * sizeof(uint32_t));
    memset(gc_heap.lazy_idx + gc_heap.lazy_idx_n, 0, (size_t)(need - gc_heap.lazy_idx_n) * sizeof(uint32_t));
    gc_heap.lazy_idx_n = need;
  }
  if (!gc_heap.lazy_idx[bi]) {
    gc_heap.lazy_idx[bi] = n + 1;
    gc_heap.lazy_open++;
  }
}

/* The start bits of block B's runs are set now (gc_lazy_touch). A built
 * run is left empty at its end and no run is open any more (lazy_hi):
 * what the cursor allocates next in the hole begins a new one. */
void gc_lazy_build_block(Block *b) {
  uint64_t bi = (uint64_t)(b - gc_heap.blocks);
  uint32_t i;
  if (bi >= gc_heap.lazy_idx_n || !gc_heap.lazy_idx[bi]) {
    return;
  }
  for (i = gc_heap.lazy_idx[bi] - 1; i < gc_heap.lazy_n; i++) {
    struct GcLazyRun *r = &gc_heap.lazy[i];
    if (r->hi <= b->base || r->hi > b->base + BLOCK_SIZE) {
      break;
    }
    lazy_walk(r->lo, r->hi);
    r->lo = r->hi;
    if (gc_heap.lazy_hi == r->hi) {
      gc_heap.lazy_hi = NULL;
    }
  }
  gc_heap.lazy_idx[bi] = 0;
  gc_heap.lazy_open--;
}

/* After a full collection's mark that set no start bit in its loop
 * (gc_mark_roots): the start bits of every run that lies in a block with
 * a live line. An object the mark promoted is in such a block (the mark made its lines
 * live) and has its bit after this; a block with no live line holds only
 * dead objects, is released by the sweep, and is not read. A run's dead
 * objects get bits too, as when a lookup builds a block (the sweep clears
 * the bits of the lines it frees; a dead object in a live line keeps its
 * bit, as everywhere). */
void gc_lazy_build_live(void) {
  uint32_t i;
  if (gc_heap.mark_bits) {
    return;
  }
  for (i = 0; i < gc_heap.lazy_n && gc_heap.lazy_open; i++) {
    const struct GcLazyRun *r = &gc_heap.lazy[i];
    Block *b;
    if (r->lo == r->hi) {
      continue;
    }
    b = &gc_heap.blocks[(uint64_t)(r->hi - 1 - gc_heap.base) >> BLOCK_SHIFT];
    if (gc_block_any_live(b)) {
      gc_lazy_build_block(b);
    }
  }
}

/* Every run's bits are set (the program becoming shared). */
void gc_lazy_build_all(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.lazy_n; i++) {
    struct GcLazyRun *r = &gc_heap.lazy[i];
    lazy_walk(r->lo, r->hi);
    r->lo = r->hi;
  }
}

/* The runs are forgotten (a young collection after its sweep, a full
 * one after its mark: every object that was young in a block is
 * promoted with its bit to come or set, copied or dead; a dead one
 * needs no bit). */
void gc_lazy_drop(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.lazy_n; i++) {
    uint64_t bi = (uint64_t)(gc_heap.lazy[i].hi - 1 - gc_heap.base) >> BLOCK_SHIFT;
    gc_heap.lazy_idx[bi] = 0;
  }
  gc_heap.lazy_n = 0;
  gc_heap.lazy_open = 0;
  gc_heap.lazy_hi = NULL;
}

/* A block object's payload is made NBYTES long in place (avant_rt.h; the
 * rule above): the header's size, and the bytes freed after it written
 * as a filler header, a size and no flag, so that a walk of the hole's
 * headers passes over them. A large object's size is set and nothing
 * else (its pages are avant_gc_large_trim's). avant_gc.h. */
__attribute__((visibility("hidden"))) void avant_gc_shrink(void *payload, uint64_t nbytes) {
  AvantHeader *h = avant_header(payload);
  size_t was = align_up(sizeof(AvantHeader) + (size_t)h->size);
  size_t now = align_up(sizeof(AvantHeader) + (size_t)nbytes);
  h->size = (uint32_t)nbytes;
  if (!(h->flags & AVANT_FLAG_LARGE) && now < was) {
    AvantHeader *f = (AvantHeader *)((uint8_t *)h + now);
    f->size = (uint32_t)(was - now - sizeof(AvantHeader));
    f->type_id = AVANT_TYPE_BYTES;
    f->flags = 0;
  }
}

/* The objects of an inline run [LO, HI) read one by one, when something
 * asks for it (gc_heap.set.walk): AVANT_GC_STATS counts them
 * (avant_inline_allocs, which the path itself counted before its rework);
 * AVANT_GC_VERIFY and AVANT_GC_STRESS check what the inline sequence
 * wrote, since nothing else looks at it before a collection does: each
 * header is a fresh young object's (HEAP | YOUNG and nothing else, a
 * size) and the last object ends at the cursor; the verifier also notes
 * each start for its eager comparison. */
/* The highest reference word of a record's map plus one (0: none). */
static size_t map_words_used(const TypeMap *m) {
  uint32_t c = m->nchunks;
  while (c-- > 0) {
    if (m->words[c]) {
      return (size_t)c * 64 + 64 - (size_t)__builtin_clzll(m->words[c]);
    }
  }
  return 0;
}

/* What a check mode asks of one header of an inline run, beyond its
 * flags and its size: its type id is one a module registered (or the
 * byte or pointer-buffer id) and a record type's reference words lie
 * inside the object's size, since every trace reads a record's fields by
 * its type's map and passes over a word past the size without a look;
 * and no payload word holds the hole's poison, which a sequence that
 * zeroed the payload short leaves there (young_note_hole). */
static void inline_check(const AvantHeader *h) {
  uint32_t id = h->type_id;
  size_t nw = (size_t)h->size / sizeof(uint64_t);
  const uint64_t *w = (const uint64_t *)payload_of((AvantHeader *)h);
  size_t i;
  if (id != AVANT_TYPE_BYTES && id != AVANT_TYPE_PTRS) {
    const TypeMap *m;
    if (id >= MAX_TYPES || !gc_heap.type_set[id]) {
      gc_die("avant gc verify: the inline allocation sequence left a header whose type id no module registered");
    }
    m = &gc_heap.types[id];
    if (!m->stride && map_words_used(m) > nw) {
      gc_die("avant gc verify: the inline allocation sequence left a header whose type has reference words past the object's size");
    }
  }
  for (i = 0; i < nw; i++) {
    if (w[i] == GC_POISON_WORD) {
      gc_die("avant gc verify: the inline allocation sequence left a payload word it did not zero");
    }
  }
}

static __attribute__((noinline)) void inline_walk(uint8_t *lo, uint8_t *hi) {
  const int check = gc_heap.set.verify | gc_heap.set.stress;
  uint8_t *p = lo;
  uint64_t n = 0;
  while (p < hi) {
    const AvantHeader *h = (const AvantHeader *)p;
    if (check && (h->flags != (AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG) || h->size == 0)) {
      gc_die("avant gc verify: the inline allocation sequence left a header that is not a fresh young object's");
    }
    if (check) {
      inline_check(h);
    }
    if (gc_heap.set.verify) {
      gc_verify_starts_note(p);
    }
    n++;
    p += align_up(sizeof(AvantHeader) + (size_t)h->size);
  }
  if (p != hi) {
    gc_die("avant gc verify: the inline allocation sequence's objects do not end at the cursor");
  }
  avant_inline_allocs += n;
}

/* Thread T (the single mutator: state 1) allocated [T->mark, CUR) by the
 * inline path since its last sync: the bytes are counted where the path
 * itself counted them before its rework (allocated bytes, the bytes
 * since the last collection, the young budget's held bytes), its
 * block's record is given the cursor, and the hole's run is made to end
 * at the cursor, or begun. The path's limit was capped by the young
 * budget when it was published (inline_cap), so the budget is passed
 * between two syncs by no more than it was when the path counted. */
static void sync_thread(GcThread *t, uint8_t *cur) {
  uint8_t *m = t->mark;
  uint64_t used = (uint64_t)(cur - m);
  avant_g_stats.allocated_bytes += used;
  avant_since_gc += used;
  avant_nursery_held += used;
  if (t->fast_block) {
    t->fast_block->bump = cur;
  }
  t->mark = cur;
  if (__builtin_expect(gc_heap.set.walk, 0)) {
    inline_walk(m, cur);
  }
  if (gc_heap.lazy_hi) {
    gc_heap.lazy[gc_heap.lazy_n - 1].hi = cur;
    gc_heap.lazy_hi = cur;
  } else {
    lazy_new_run(m, cur);
  }
}

void gc_alloc_sync_slow(void) {
  sync_thread(&gc_thread, avant_gc_bump);
}


/* Publish the inline fast path when every C fast-path condition holds
 * and the young cursor may take it (S_INLINE_HOLES, or a whole block).
 * Anything else clears it, so
 * generated code only ever checks its cursor against the published
 * limit. Called at slow allocation entries; state
 * transitions clear eagerly below. Never under AVANT_GC_STRESS=1 (here
 * and in tl_grant): the path is not published here then, so every
 * allocation of generated code reaches its slow call and a stress point;
 * avant_alloc_slow alone publishes it under
 * that mode, for the one object it was asked for. */
static void gc_fast_refresh(void) {
  Block *cur = gc_thread.ycur;
  int cur_ok = S_INLINE_HOLES || gc_thread.cur_whole;
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 && !gc_heap.set.stress &&
      !gc_thread.nogc && !gc_thread.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature &&
      !gc_heap.limit_pending && !gc_heap.explicit_pending && gc_heap.mature_since_full < gc_heap.full_trigger && cur && gc_thread.cur == cur &&
      cur_ok && avant_gc_bump) {
    /* Published already on this block (the usual case at a C
     * allocation's entry): nothing to store. The entry's sync left the
     * mark at the cursor, and the limit is settled where this allocation
     * ends (bump_take, gc_fast_settle). */
    if (gc_thread.fast_ok == 1 && gc_thread.fast_block == cur) {
      return;
    }
    gc_thread.fast_block = cur;
    avant_gc_base = cur->base;
    avant_gc_starts = cur->start;
    gc_thread.fast_ok = 1;
    gc_fast_publish(inline_cap());
  } else {
    gc_fast_clear();
  }
}

/* Hole accounting (GcThread's comment): T's charged rest
 * of its young hole is settled, the bytes it used counted as allocated
 * and the rest given back to the young budget. The caller holds the world
 * lock, or T is stopped (a collection). */
void gc_tl_settle(GcThread *t) {
  Block *y = t->ycur;
  if (t->tl_end && y) {
    /* The thread's cursor (it is stopped, or it is this thread): its
     * block's record is given it here (the inline path no longer
     * stores it there). */
    uint8_t *b = t->bump_p ? *t->bump_p : y->bump;
    y->bump = b;
    uint64_t used = (uint64_t)(b - t->tl_start);
    uint64_t unused = (uint64_t)(t->tl_end - b);
    avant_g_stats.allocated_bytes += used;
    avant_since_gc += used;
    avant_nursery_held -= unused;
  }
  t->tl_start = NULL;
  t->tl_end = NULL;
}

/* A shared program's slow path, after its allocation, under the world
 * lock: the rest of the young hole the thread's cursor is in is charged
 * to the young budget and published for the thread's lock-free bumps
 * (state 2: the inline path of a shared module and
 * avant_alloc's fast path), when the conditions of gc_fast_refresh hold
 * but the single mutator's (and the budget is not spent: the next
 * allocation then reaches the slow path, which collects). The thread's
 * previous charge was settled at the slow path's start. */
static void tl_grant(void) {
  Block *y = gc_thread.ycur;
  if (gc_heap.set.stress || !gc_thread.reg.head || gc_thread.nogc || gc_thread.collecting || gc_heap.set.nursery_off ||
      gc_thread.alloc_mature || gc_heap.limit_pending || gc_heap.explicit_pending ||
      gc_heap.mature_since_full >= gc_heap.full_trigger || avant_nursery_held >= avant_young_bytes ||
      !y || gc_thread.cur != y || !(S_INLINE_HOLES || gc_thread.cur_whole) || !avant_gc_bump ||
      gc_thread.limit <= avant_gc_bump) {
    gc_fast_clear();
    return;
  }
  gc_thread.tl_start = avant_gc_bump;
  gc_thread.tl_end = gc_thread.limit;
  avant_nursery_held += (uint64_t)(gc_thread.tl_end - gc_thread.tl_start);
  gc_thread.fast_block = y;
  avant_gc_base = y->base;
  avant_gc_starts = y->start;
  gc_thread.fast_ok = 2;
  gc_fast_publish(gc_thread.tl_end);
}

/* A shared program's lock-free bump (state 2) of TOTAL bytes
 * in the thread's own charged hole, or NULL: what the inline path of a
 * shared module does, without counters (hole accounting). */
static AvantHeader *tl_bump(size_t total) {
  uint8_t *h = avant_gc_bump;
  Block *b = gc_thread.fast_block;
  if (gc_thread.fast_ok != 2 || !h || !b || h + total > avant_gc_limit_mt) {
    return NULL;
  }
  set_start(b, (size_t)(h - b->base));
  avant_gc_bump = h + total;
  b->bump = avant_gc_bump;
  return (AvantHeader *)h;
}

/* The thread's cursors and inline state are dropped, its charge settled
 * (the caller holds the world lock, or the thread is stopped). */
static void cursors_drop(GcThread *t) {
  gc_tl_settle(t);
  t->cur = NULL;
  t->limit = NULL;
  t->fast_block = NULL;
  t->ycur = NULL;
  t->overflow = NULL;
  t->cur_whole = 0;
}

/* At the start of every collection (gc_collect, after the handshake):
 * every registered thread's cursors, and the collector's own, are
 * dropped, its hole charge settled and its inline state cleared through
 * the addresses it registered, so no thread bumps into a hole the
 * collection frees or hands to another thread. The other threads are
 * stopped or native. */
void gc_alloc_stop_all(void) {
  ThreadReg *t;
  /* The single mutator's inline allocations are counted and its block's
   * record given its cursor before the cursors go (gc_collect synced at
   * its entry; nothing is pending here). */
  gc_alloc_sync();
  for (t = gc_heap.threads; t; t = t->next) {
    GcThread *g = t->owner;
    if (!g || g == &gc_thread) {
      continue;
    }
    cursors_drop(g);
    g->fast_ok = 0;
    g->mark = NULL;
    if (g->bump_p) {
      *g->limit_p = NULL;
      *g->limit_mt_p = NULL;
      *g->bump_p = NULL;
    }
  }
  cursors_drop(&gc_thread);
  gc_fast_clear();
  avant_gc_bump = NULL;
}

/* The program became shared (gc_thread_registered, every other thread
 * stopped or native): no thread keeps the single mutator's inline state
 * (state 1, whose allocations are counted from the cursor without a
 * lock). Its inline allocations so far are counted and its block's
 * record given its cursor; its cursors stay: nothing is charged in that
 * state. And every late start bit is set: from now on any thread
 * may look up an object the first one allocated, and no run is noted
 * again (an unshared module's path stays off, a shared module's sets
 * each bit itself). */
void gc_alloc_shared_now(void) {
  ThreadReg *t;
  for (t = gc_heap.threads; t; t = t->next) {
    GcThread *g = t->owner;
    if (g && g != &gc_thread && g->bump_p) {
      if (g->mark && g->mark != *g->bump_p) {
        sync_thread(g, *g->bump_p);
      }
      g->fast_block = NULL;
      g->fast_ok = 0;
      g->mark = NULL;
      *g->limit_p = NULL;
      *g->limit_mt_p = NULL;
    }
  }
  gc_lazy_build_all();
  gc_lazy_drop();
  /* The proof's log is the single mutator's: from here nothing is
   * logged and no complete collection is shortened. The one mutator is
   * stopped at a safepoint, so it is not inside the log. */
  gc_proof_break(PROOF_THREADS);
}

/* A thread that registers notes the addresses of its inline state for
 * gc_alloc_stop_all. */
void gc_alloc_thread_init(void) {
  gc_thread.bump_p = &avant_gc_bump;
  gc_thread.limit_p = &avant_gc_limit;
  gc_thread.limit_mt_p = &avant_gc_limit_mt;
}

/* A thread that unregisters settles its charge and drops its cursors
 * (its young block stays a young block until the next collection). */
void gc_alloc_thread_leave(void) {
  gc_alloc_sync();
  gc_lock(&gc_heap.world);
  gc_rem_thread_leave();
  cursors_drop(&gc_thread);
  gc_fast_clear();
  avant_gc_bump = NULL;
  pthread_mutex_unlock(&gc_heap.world);
}

static int try_bump(Block *b, size_t total) {
  gc_use_block(b);
  if (bump_fits(total)) {
    return 1;
  }
  b->search = (uint16_t)((b->limit - b->base) / LINE_SIZE);
  if (!find_hole(b, total)) {
    return 0;
  }
  gc_use_block(b);
  return bump_fits(total);
}

void gc_clear_alloc_cur(uint8_t space) {
  gc_heap.space[space].cur = NULL;
  gc_heap.space[space].fresh = 0;
}

/* The young allocator (candidate S's, in-place young collection).
 * There is no nursery space: young objects are allocated, YOUNG flagged,
 * into free lines of mature-space blocks, and each thread's young
 * cursor (GcThread.ycur; once one per process, in
 * gc_heap.space[SPACE_NURSERY]) names a mature-space block. The block it
 * bumps in is a young block (young_hi
 * set) until the next collection: every hole handed out widens the
 * block's young line range [young_lo, young_hi), and the young sweep
 * visits only those lines (avant_gc_sticky.c). Mature allocation never
 * takes a young block, young allocation never takes the mature
 * cursor's block, and no thread's young allocation takes a block that
 * is young already, so a hole has one owner: the thread whose cursor
 * took it, which bumps in it without the world lock in a shared program
 * (hole accounting, GcThread). The order: the current
 * block's next hole; a hole of a block with free lines among live ones
 * (part_blocks); a new block from the chunks (where the sweeps return
 * the blocks they leave empty). A medium object (over a line) that the current
 * hole cannot hold goes to the overflow block (a hole of a free or
 * recycled block of its own), so it does not skip the current block's
 * small holes (Immix's overflow allocation). */
void gc_sticky_push(Block ***v, uint32_t *n, uint32_t *cap, Block *b) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *v = gc_xrealloc(*v, *cap * sizeof(Block *));
  }
  (*v)[(*n)++] = b;
}

static void young_note_hole(Block *b) {
  uint8_t lo = (uint8_t)((b->bump - b->base) >> LINE_SHIFT);
  uint8_t hi = (uint8_t)((b->limit - b->base) >> LINE_SHIFT);
  /* The check modes: the hole holds the poison until something is
   * allocated in it (gc_poison_checked's comment). */
  gc_poison_checked(b->bump, (size_t)(b->limit - b->bump));
  if (!b->young_hi) {
    gc_sticky_push(&gc_heap.young_blocks, &gc_heap.young_blocks_n, &gc_heap.young_blocks_cap, b);
    b->young_lo = lo;
    b->young_hi = hi;
  } else {
    if (lo < b->young_lo) {
      b->young_lo = lo;
    }
    if (hi > b->young_hi) {
      b->young_hi = hi;
    }
  }
  gc_heap.ctr.sticky.holes++;
}

/* A block another thread's cursor holds is a young block (young_hi set),
 * so it is never usable here. */
static int young_usable(Block *b) {
  return b->in_use && b->space == SPACE_MATURE && !b->young_hi &&
         b != gc_heap.space[SPACE_MATURE].cur && b != gc_thread.overflow;
}

static inline __attribute__((always_inline)) void young_take(Block *b) {
  young_note_hole(b);
  gc_thread.ycur = b;
  gc_thread.cur_whole = b->bump == b->base && b->limit == b->base + BLOCK_SIZE;
  gc_use_block(b);
}

/* A block of list V (N entries) with a hole of at least TOTAL bytes,
 * its bump and limit set to the hole, taken out of the list; or NULL.
 * A small object takes the last usable entry (any free line holds it).
 * A medium one looks at the last YOUNG_FIT_LOOK entries and leaves the
 * ones it does not fit in the list, so their small holes stay for small
 * objects. Unusable entries (gc_heap's comment) are dropped. */
#define YOUNG_FIT_LOOK 16
static Block *young_from(Block **v, uint32_t *n, size_t total) {
  uint32_t i = *n;
  uint32_t looked = 0;
  while (i > 0 && looked < YOUNG_FIT_LOOK) {
    Block *b = v[--i];
    if (!young_usable(b)) {
      v[i] = v[--*n];
      continue;
    }
    b->search = 0;
    if (find_hole(b, total)) {
      v[i] = v[--*n];
      return b;
    }
    looked++;
  }
  return NULL;
}

/* The young cursor moves to a hole of at least TOTAL bytes without
 * growing the heap, or 0. */
static int young_hole(size_t total) {
  Block *b = gc_thread.ycur;
  if (b) {
    b->search = (uint16_t)((b->limit - b->base) / LINE_SIZE);
    if (find_hole(b, total)) {
      young_note_hole(b);
      gc_thread.cur_whole = 0;
      gc_use_block(b);
      return 1;
    }
  }
  b = young_from(gc_heap.part_blocks, &gc_heap.part_blocks_n, total);
  if (b) {
    young_take(b);
    return 1;
  }
  return 0;
}

/* A medium object in the overflow block, or NULL (no free block and
 * none can be made under the heap limit: the caller goes on as for a
 * small one). */
static AvantHeader *young_overflow(size_t total) {
  Block *o = gc_thread.overflow;
  uint8_t *h;
  if (!o || o->bump + total > o->limit) {
    o = young_from(gc_heap.part_blocks, &gc_heap.part_blocks_n, total);
    if (!o) {
      o = gc_new_block(SPACE_MATURE);
      if (!o) {
        return NULL;
      }
      o->kept = SPLUS_FRESH;
    }
    young_note_hole(o);
    gc_thread.overflow = o;
  }
  h = o->bump;
  set_start(o, (size_t)(h - o->base));
  o->bump = h + total;
  gc_heap.ctr.sticky.overflow++;
  /* The cursor's own hole was not bumped: its limit (gc_fast_settle). */
  gc_fast_settle();
  return (AvantHeader *)h;
}

/* Young allocation of TOTAL bytes without a new block, or NULL. */
static AvantHeader *young_place(size_t total) {
  Block *cur = gc_thread.ycur;
  if (cur) {
    if (gc_thread.cur != cur) {
      gc_use_block(cur);
    }
    if (bump_fits(total)) {
      return (AvantHeader *)bump_take(total);
    }
    if (total > LINE_SIZE) {
      AvantHeader *h = young_overflow(total);
      if (h) {
        return h;
      }
    }
  }
  if (young_hole(total)) {
    return (AvantHeader *)bump_take(total);
  }
  return NULL;
}

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

static AvantHeader *place(size_t total, uint8_t space, int tried_gc) {
  Block *cur = gc_heap.space[space].cur;
  uint8_t bspace = space;
  if (space == SPACE_NURSERY) {
    AvantHeader *yh = young_place(total);
    if (yh) {
      return yh;
    }
    /* A new block, a young one (mature space). */
    bspace = SPACE_MATURE;
    cur = NULL;
  } else if (cur && try_bump(cur, total)) {
    return bump_take(total);
  }
  /* A fresh block has no holes behind it. Scanning the list again
   * walks every full block. Recycle only after a collection cleared
   * the fresh bit, and then only forward from the cursor. */
  if (bspace == space && !gc_heap.space[space].fresh) {
    Block *b;
    /* The walk follows the space's order through Block.next. */
    gc_mature_normalize();
    b = cur ? cur->next : gc_heap.space[space].list;
    for (; b; b = b->next) {
      /* A young block, and a block the verifier holds back with poisoned
       * free lines until the next collection (VERIFY_HELD). */
      if (b->young_hi || b->kept == VERIFY_HELD) {
        continue;
      }
      if (try_bump(b, total)) {
        gc_heap.space[space].cur = b;
        return bump_take(total);
      }
    }
  }
  Block *b = gc_new_block(bspace);
  if (!b && (gc_thread.nogc || gc_thread.hold) && !gc_thread.collecting) {
    /* Never collect inside a no-GC (defer)
     * region, where a runtime helper holds fresh objects the collector
     * cannot see (avant_str_split's items[]). The allocation proceeds
     * past the limit and the full collection runs at the first
     * collection point after the region (gc_maybe_collect), unless the
     * region alone is over the limit. */
    gc_region_limit_check(total);
    gc_heap.over_limit = 1;
    b = gc_new_block(bspace);
    gc_heap.over_limit = 0;
    gc_limit_collect_pending();
  }
  if (!b && gc_thread.collecting && !gc_heap.over_limit) {
    /* An allocation inside a collection past the limit: the collection
     * proceeds and the full collection runs at the next collection point.
     * (S+'s copy does not come here: it stops at the limit, gc_new_block
     * in splus_copy.) */
    gc_heap.over_limit = 1;
    b = gc_new_block(bspace);
    gc_heap.over_limit = 0;
    gc_limit_collect_pending();
  }
  if (!b) {
    if (!tried_gc && !gc_thread.collecting) {
      /* A full collection at the limit, then one retry. */
      gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
      return place(total, space, 1);
    }
    gc_die("heap limit");
  }
  if (space == SPACE_NURSERY) {
    b->kept = SPLUS_FRESH;
    young_take(b);
  } else {
    gc_heap.space[space].cur = b;
    gc_heap.space[space].fresh = 1;
    gc_use_block(b);
  }
  if (!bump_fits(total)) {
    gc_die("object larger than block");
  }
  return bump_take(total);
}

/* Like place into the nursery, but never collects. */
static AvantHeader *place_bump(size_t total) {
  Block *b;
  AvantHeader *yh;
  if (gc_heap.set.nursery_off || gc_thread.alloc_mature || total > LOS_LIMIT) {
    return NULL;
  }
  yh = young_place(total);
  if (yh) {
    return yh;
  }
  b = gc_new_block(SPACE_MATURE);
  if (!b) {
    return NULL;
  }
  b->kept = SPLUS_FRESH;
  young_take(b);
  if (!bump_fits(total)) {
    return NULL;
  }
  return (AvantHeader *)bump_take(total);
}

/* Block memory is never pre-zeroed, so every allocation zeroes its
 * own payload here. Callers pass zeroed=1 only for memory already
 * cleared by its allocator (the large-object path). */
static void init_object(AvantHeader *h, size_t payload, uint32_t type_id, int young, int zeroed) {
  h->size = (uint32_t)payload;
  h->type_id = (uint16_t)type_id;
  h->flags = young ? (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG) : (uint16_t)AVANT_FLAG_HEAP;
  if (!zeroed) {
    memset(payload_of(h), 0, payload);
  }
}

static void account_alloc(size_t total, int nursery) {
  avant_g_stats.allocated_bytes += total;
  if (gc_thread.nogc) {
    gc_thread.region_bytes += total;
  }
  avant_since_gc += total;
  if (nursery) {
    avant_nursery_held += total;
  } else {
    gc_heap.mature_since_full += total;
    /* An object born old (no full collection marked it). */
    gc_proof_break(PROOF_MATURE);
  }
  gc_note_peak();
}

void avant_alloc_in_mature(int32_t on) {
  if (on) {
    gc_fast_clear();
    if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
      /* The charged rest of its young hole goes back. */
      gc_init();
      gc_lock(&gc_heap.world);
      gc_tl_settle(&gc_thread);
      pthread_mutex_unlock(&gc_heap.world);
    }
  }
  gc_thread.alloc_mature = on ? 1 : 0;
}

static int alloc_is_large(uint32_t type_id, size_t payload, size_t total) {
  if (total > LOS_LIMIT) {
    return 1;
  }
  return type_id == AVANT_TYPE_BYTES && payload >= SCALAR_LOS_MIN;
}

void *avant_alloc(uint64_t nbytes, uint32_t type_id) {
  int shared;
  gc_init();
  /* The inline path's allocations since the last sync are counted before
   * anything here reads the counters or moves the cursor. */
  gc_alloc_sync();
  /* AVANT_GC_STRESS=1: every allocation is a stress point, a young
   * collection before it (gc_stress_point: not inside a no-GC region or a
   * collection, and only with a young generation). Generated code's
   * allocations all come here under it, since the inline path is never
   * published (gc_fast_refresh, tl_grant), as do
   * runtime C's and user C's: a caller that holds a reference outside a
   * root slot across any allocation reads a moved or poisoned object at
   * once. (Before, only runtime C's allocations were stress points,
   * through a wrapper of their own, avant_alloc_c; a
   * program whose allocations were all generated code's ran under the
   * stress mode as it ran without it.) Generated code's inline
   * allocations call avant_alloc_slow, which
   * runs the same stress point and then lets the inline sequence itself
   * allocate. */
  if (gc_heap.set.stress) {
    if (gc_thread.stress_done) {
      /* avant_alloc_slow ran this allocation's stress point already. */
      gc_thread.stress_done = 0;
    } else {
      gc_safepoint();
      gc_lock(&gc_heap.world);
      gc_stress_point();
      pthread_mutex_unlock(&gc_heap.world);
    }
  }
  shared = atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0;
  if (!shared) {
    gc_fast_refresh();
  }
  if (nbytes > UINT32_MAX) {
    /* A size the program asked for, not a collector fault: status 1
     * like a trap, no core dump. */
    fflush(stdout);
    fprintf(stderr, "avant runtime: object too large (%llu bytes)\n", (unsigned long long)nbytes);
    exit(1);
  }
  if (type_id > UINT16_MAX) {
    gc_die("type id too large");
  }
  /* No empty payload: a 0-byte object's payload
   * address is the address just past its header, so at the end of a block
   * it is the next block's first address, and a reference to it was taken
   * for the object there (a young copy left the holder's field naming the
   * old copy; found by AVANT_GC_VERIFY on the compiler's own dump). An
   * empty array buffer or an empty class gets one word. */
  if (nbytes == 0) {
    nbytes = 8;
  }

  size_t payload = (size_t)nbytes;
  size_t total = align_up(sizeof(AvantHeader) + payload);
  int large = alloc_is_large(type_id, payload, total);
  /* Single mutator, under the young threshold: bump and return.
   * Collection stays on the locked path, with the caller's roots
   * already spilled.
   * Medium numeric buffers take the large path so their address stays. */
  if (!shared && !gc_thread.nogc &&
      !gc_thread.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature && !large && !gc_heap.limit_pending && !gc_heap.explicit_pending &&
      gc_heap.mature_since_full < gc_heap.full_trigger && avant_nursery_held + total < avant_young_bytes) {
    AvantHeader *fast = place_bump(total);
    if (fast) {
      init_object(fast, payload, type_id, 1, 0);
      account_alloc(total, 1);
      return payload_of(fast);
    }
  }
  /* A shared program: a bump in the thread's own charged
   * young hole, without the world lock (hole accounting counts it). */
  if (shared && !large && total <= LOS_LIMIT) {
    AvantHeader *fast = tl_bump(total);
    if (fast) {
      init_object(fast, payload, type_id, 1, 0);
      return payload_of(fast);
    }
  }

  /* A safepoint: a collecting call, so the caller's
   * references are in root slots; another thread's collection runs while
   * this one waits here or for the world lock (gc_lock: native). */
  gc_safepoint();
  gc_lock(&gc_heap.world);
  if (shared) {
    /* The thread's charge is settled before the policy reads the budget
     * and before its cursor may move to another hole. */
    gc_tl_settle(&gc_thread);
    gc_fast_clear();
  }
  gc_maybe_collect();
  if (large) {
    /* One young budget: young large
     * bytes count in avant_nursery_held, so gc_maybe_collect above
     * already ran the young collection they are due (the eager
     * collection for one replaced buffer and the separate large budget
     * are retired). */
    void *obj;
    /* Before the large object's bytes join the young budget: the inline
     * path's limit (gc_fast_settle). */
    gc_fast_settle();
    obj = gc_los_alloc(payload, type_id);
    avant_g_stats.allocated_bytes += total;
    if (gc_thread.nogc) {
      gc_thread.region_bytes += total;
    }
    avant_since_gc += total;
    if (shared) {
      tl_grant();
    }
    pthread_mutex_unlock(&gc_heap.world);
    return obj;
  }

  /* A thread that is not registered allocates in the mature space in a
   * shared program: no collection would drop its young cursor. */
  uint8_t space = SPACE_NURSERY;
  if (gc_heap.set.nursery_off || gc_thread.alloc_mature || (shared && !gc_thread.reg.head)) {
    space = SPACE_MATURE;
  }
  AvantHeader *h = place(total, space, 0);
  init_object(h, payload, type_id, space == SPACE_NURSERY, 0);
  account_alloc(total, space == SPACE_NURSERY);
  void *obj = payload_of(h);
  if (shared) {
    tl_grant();
  }
  pthread_mutex_unlock(&gc_heap.world);
  return obj;
}

/* The young cursor is in a hole with room for TOTAL bytes (the current
 * hole, the next one that fits, a new block), or 0: place_bump's search
 * without its allocation. The single mutator's. Reached only under the
 * stress mode, and so cold and never inlined; bump_fits and young_take,
 * which it shares with avant_alloc's slow path, are always inlined, so
 * that its calls do not make the C compiler keep them out of line there
 * (a young collection on the collector benchmark's steady loop: 12
 * instructions fewer; no LangArena task more). */
static __attribute__((noinline, cold)) int young_room(size_t total) {
  Block *cur = gc_thread.ycur;
  Block *b;
  if (cur) {
    if (gc_thread.cur != cur) {
      gc_use_block(cur);
    }
    if (bump_fits(total)) {
      return 1;
    }
  }
  if (young_hole(total)) {
    return bump_fits(total);
  }
  b = gc_new_block(SPACE_MATURE);
  if (!b) {
    return 0;
  }
  b->kept = SPLUS_FRESH;
  young_take(b);
  return bump_fits(total);
}

/* The inline allocation path's slow call (avant_rt.h). Without
 * AVANT_GC_STRESS=1 it is avant_alloc. Under it, before the path's
 * rework, the inline sequence never ran: the path was never
 * published, so that every allocation reached avant_alloc and its stress
 * point, and a defect of the sequence that shows only
 * when a collection follows it closely had no pass that forced one. Now
 * the stress point runs here, as in avant_alloc, and then, for an object
 * the inline path serves in a program with one thread, the young cursor
 * is put in a hole with room for it, the limit is published at exactly
 * the object's end and null is returned: the caller, the module's
 * __avant_alloc_slow (compiler/llvm/emit_gc.av, ll_alloc_slow_helper),
 * then makes the inline allocation itself with the emitted sequence
 * (ll_alloc_take): it bumps, writes the header and zeroes the payload.
 * The next allocation finds the limit spent and comes back here: its
 * sync reads what the sequence wrote (inline_walk: the header, the
 * cursor) and its stress point collects right after it. So under the
 * stress mode every allocation still has a collection before it, and
 * the emitted sequence runs for every allocation the inline path can
 * serve. What runs is the helper's copy of the sequence (the size and
 * the type as values); a site's own copy (the limit compare, the size
 * and the type folded into constants) does not run under the stress
 * mode, the limit being spent whenever a site tests it: AVANT_GC_VERIFY=1
 * without the stress mode is the pass that checks the headers the sites
 * write (inline_walk). Any other request (a large object, a program
 * with threads, a no-GC region, the nursery off, a collection pending,
 * no room) is allocated here as avant_alloc allocates it. */
void *avant_alloc_slow(uint64_t nbytes, uint32_t type_id) {
  size_t payload;
  size_t total;
  uint8_t *granted;
  gc_init();
  if (!gc_heap.set.stress) {
    return avant_alloc(nbytes, type_id);
  }
  gc_alloc_sync();
  granted = gc_thread.redo;
  gc_thread.redo = NULL;
  payload = nbytes ? (size_t)nbytes : 8;
  total = align_up(sizeof(AvantHeader) + payload);
  if ((granted && granted == avant_gc_bump) || nbytes > UINT32_MAX || type_id > UINT16_MAX ||
      alloc_is_large(type_id, payload, total) ||
      atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    return avant_alloc(nbytes, type_id);
  }
  gc_safepoint();
  gc_lock(&gc_heap.world);
  gc_stress_point();
  pthread_mutex_unlock(&gc_heap.world);
  if (gc_thread.nogc || gc_thread.collecting || gc_heap.set.nursery_off || gc_thread.alloc_mature ||
      gc_heap.limit_pending || gc_heap.explicit_pending || gc_heap.mature_since_full >= gc_heap.full_trigger ||
      avant_nursery_held + total >= avant_young_bytes || !young_room(total)) {
    /* Not this one: avant_alloc allocates it, without a second
     * collection. */
    gc_thread.stress_done = 1;
    return avant_alloc(nbytes, type_id);
  }
  gc_thread.fast_block = gc_thread.ycur;
  avant_gc_base = gc_thread.ycur->base;
  avant_gc_starts = gc_thread.ycur->start;
  gc_thread.fast_ok = 1;
  gc_fast_publish(avant_gc_bump + total);
  gc_thread.redo = avant_gc_bump;
  return NULL;
}

/* AVANT_GC_VERIFY=1's own pages for C-visible Buf storage (avant_gc.h).
 * Not in a shared program: another thread may hold
 * the storage there, and a growth never drops it. */
__attribute__((visibility("hidden"))) void *avant_gc_verify_paged(void *old, uint64_t nbytes, int move) {
  size_t total;
  void *obj = NULL;
  uint16_t fl;
  if (!old || !gc_heap.set.verify || atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0 ||
      nbytes > UINT32_MAX || gc_thread.collecting) {
    return NULL;
  }
  fl = avant_header(old)->flags;
  if (move ? (fl & (AVANT_FLAG_LARGE | AVANT_FLAG_PINNED)) != 0 || gc_rooted(old) : (fl & AVANT_FLAG_LARGE) == 0) {
    return NULL;
  }
  total = align_up(sizeof(AvantHeader) + (size_t)nbytes);
  pthread_mutex_lock(&gc_heap.world);
  if (!gc_heap.set.heap_max || gc_heap.heap_bytes + total + PAGE_SIZE <= gc_heap.set.heap_max) {
    obj = gc_los_alloc((size_t)nbytes, AVANT_TYPE_BYTES);
    avant_g_stats.allocated_bytes += total;
    if (gc_thread.nogc) {
      gc_thread.region_bytes += total;
    }
    avant_since_gc += total;
  }
  pthread_mutex_unlock(&gc_heap.world);
  return obj;
}

/* A runtime helper about to allocate a young large object of BYTES that
 * replaces one it allocated before, which is usually dead by now
 * (avant_json_parse's pool, the previous document dropped), asks for
 * the due young collection first when BYTES alone fills the young budget
 * and anything young was allocated since the last collection: run then,
 * it frees the previous object before the new one exists instead of
 * holding both until the next collection (Json::ParseMapping 60 -> 35 MB
 * with the pool born young). A collecting call (the caller roots what it
 * holds). As a rule of avant_alloc for every such allocation it cost
 * Template::Regex 4 -> 11 young collections, +10% time and +18% memory:
 * there the large allocations are buffer growths whose old buffer is
 * still live, so the early collection freed nothing and aged the buffers
 * into mature space. */
__attribute__((visibility("hidden"))) void avant_gc_young_before(uint64_t bytes) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)bytes);
  gc_init();
  gc_alloc_sync();
  gc_safepoint();
  gc_lock(&gc_heap.world);
  if (!gc_thread.nogc && !gc_thread.hold && !gc_thread.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature &&
      atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 &&
      avant_nursery_held > gc_heap.young_carried && total >= avant_young_bytes) {
    gc_collect(GC_YOUNG, 1, GC_WHY_BUDGET);
  }
  pthread_mutex_unlock(&gc_heap.world);
}

