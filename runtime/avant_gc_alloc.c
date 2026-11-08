#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocation (Stage 28 W12, moved from avant_rt.c): block supply and
 * recycling, the per-space bump cursors, the inline nursery fast path the
 * generated code shares (avant_gc_bump and its mirrors), the slow path
 * (place), and the allocation entry points avant_alloc, avant_bump_alloc
 * and avant_alloc_in_mature. Collection decisions are avant_gc_collect.c's;
 * large objects are avant_gc_los.c's. */

/* Inline nursery fast path (Collector II Wave 1): generated code bumps
 * avant_gc_bump directly when avant_gc_fast_ok is set, bounded by
 * avant_gc_limit (the block limit capped by the young threshold) and
 * writing the start bit through avant_gc_starts/avant_gc_base. The C
 * cursor avant_gc_bump is the same storage, so both sides share one cursor.
 * Only the runtime publishes or clears the fast state; see
 * gc_fast_refresh below. */
__thread uint8_t *avant_gc_bump;
__thread uint8_t *avant_gc_limit;
__thread uint8_t *avant_gc_base;
__thread uint64_t *avant_gc_starts;
__thread void *avant_gc_block;
__thread uint32_t avant_gc_fast_ok;

uint64_t avant_since_gc;

/* Nursery allocations taken by generated code without a call. */
uint64_t avant_inline_allocs;

static int find_hole(Block *b, size_t need) {
  int i = b->search;
  while (i < LINES_PER_BLOCK) {
    if (line_live(b, i)) {
      i++;
      continue;
    }
    int j = i;
    while (j < LINES_PER_BLOCK && !line_live(b, j)) {
      j++;
    }
    size_t hole = (size_t)(j - i) * LINE_SIZE;
    if (hole >= need) {
      b->bump = b->base + (size_t)i * LINE_SIZE;
      b->limit = b->base + (size_t)j * LINE_SIZE;
      return 1;
    }
    i = j;
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
  if (gc_heap.space[SPACE_NURSERY].cur == b) {
    gc_clear_alloc_cur(SPACE_NURSERY);
  }
  if (gc_heap.space[SPACE_MATURE].cur == b) {
    gc_clear_alloc_cur(SPACE_MATURE);
  }
  if (gc_heap.cur == b) {
    gc_heap.cur = NULL;
    avant_gc_bump = NULL;
    gc_heap.limit = NULL;
  }
  if (gc_heap.fast_block == b) {
    gc_fast_clear();
  }
  /* Out of use: no lookup finds it, as when it left the block hash. A
   * cached block stays charged to gc_heap.heap_bytes; a released one goes back
   * to its chunk's free list (resident until a full collection's
   * gc_heap_return). */
  b->in_use = 0;
  gc_heap.space[b->space].bytes -= BLOCK_SIZE;
  if (gc_heap.cache_n < CACHE_BLOCKS) {
    b->next = gc_heap.cache;
    gc_heap.cache = b;
    gc_heap.cache_n++;
    return;
  }
  gc_block_give(b);
  gc_heap.heap_bytes -= BLOCK_SIZE;
}

/* Wave 4 step 1: when the heap high-water mark moves, snapshot what
 * the heap is made of, so a peak can be decomposed after the fact.
 * O(1) since W14a2: counters, not walks of the block lists. */
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
  b->next = gc_heap.space[space].list;
  gc_heap.space[space].list = b;
  return b;
}

/* The inline fast path's limit: the current block's limit, capped where
 * the young budget runs out, so a collecting allocation reaches
 * avant_alloc (which collects) when the budget is spent. Once it is spent
 * the cap is the block's end (Stage 28 W15a1): a leaf, whose fallback
 * avant_bump_alloc never collects, then allocates inline to the end of
 * every block instead of calling C for every object past the budget
 * (Binarytrees::Obj builds an 8 MiB tree in a leaf: +59% time at the 8 MiB
 * budget with the old cap), and a collecting allocation overshoots the
 * budget by at most the rest of one block before avant_alloc collects. */
static uint8_t *inline_cap(void) {
  uint64_t rem = avant_young_bytes > avant_nursery_held ? avant_young_bytes - avant_nursery_held : 0;
  uint8_t *cap = avant_gc_bump + rem;
  if (gc_heap.limit && (rem == 0 || cap > gc_heap.limit)) {
    return gc_heap.limit;
  }
  return cap;
}

void gc_use_block(Block *b) {
  gc_heap.cur = b;
  avant_gc_bump = b->bump;
  gc_heap.limit = b->limit;
  /* The young cursor's block is a mature-space block (a fresh one or a
   * recycled one with holes); the inline path follows it into its holes
   * (their start bits are clear, gc-design.md §2.3) unless S_INLINE_HOLES
   * is 0 and the hole is not a whole block. Any other block clears it,
   * so generated code never writes start bits through another block's
   * base. */
  if (b == gc_heap.space[SPACE_NURSERY].cur && !gc_heap.collecting &&
      (S_INLINE_HOLES || gc_heap.cur_whole)) {
    gc_heap.fast_block = b;
    avant_gc_base = b->base;
    avant_gc_starts = b->start;
    avant_gc_block = b;
    avant_gc_limit = inline_cap();
  } else {
    gc_fast_clear();
  }
}

static int bump_fits(size_t need) {
  return gc_heap.cur && avant_gc_bump && avant_gc_bump + need <= gc_heap.limit;
}

static void *bump_take(size_t need) {
  Block *b = gc_heap.cur;
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
  if (b == gc_heap.fast_block) {
    avant_gc_limit = inline_cap();
  }
  return h;
}

void gc_fast_clear(void) {
  avant_gc_fast_ok = 0;
  avant_gc_limit = NULL;
  gc_heap.fast_block = NULL;
}

/* Publish the inline fast path when every C fast-path condition holds
 * and the young cursor may take it (S_INLINE_HOLES, or a whole block).
 * Anything else clears it, so
 * generated code only ever checks fast_ok, the shared flag, and the
 * cursor against the limit. Called at slow allocation entries; state
 * transitions clear eagerly below. */
static void gc_fast_refresh(void) {
  Block *cur = gc_heap.space[SPACE_NURSERY].cur;
  int cur_ok = S_INLINE_HOLES || gc_heap.cur_whole;
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 &&
      !gc_heap.defer && !gc_heap.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature &&
      !gc_heap.limit_pending && !gc_heap.explicit_pending && gc_heap.mature_since_full < gc_heap.full_trigger && cur && gc_heap.cur == cur &&
      cur_ok && avant_gc_bump) {
    gc_heap.fast_block = cur;
    avant_gc_base = cur->base;
    avant_gc_starts = cur->start;
    avant_gc_block = cur;
    avant_gc_limit = inline_cap();
    avant_gc_fast_ok = 1;
  } else {
    gc_fast_clear();
  }
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

/* The young allocator (candidate S's, Stage 28 W15b, gc-design.md §3.1).
 * There is no nursery space: young objects are allocated, YOUNG flagged,
 * into free lines of mature-space blocks, and the space's cursor
 * gc_heap.space[SPACE_NURSERY] is only the young cursor (its block a
 * mature-space block). The block it bumps in is a young block (young_hi
 * set) until the next collection: every hole handed out widens the
 * block's young line range [young_lo, young_hi), and the young sweep
 * visits only those lines (avant_gc_sticky.c). Mature allocation never
 * takes a young block, and young allocation never takes the mature
 * cursor's block, so a hole has one owner. The order: the current
 * block's next hole; a hole of a block with free lines among live ones
 * (part_blocks); a new block from the chunks (where the sweeps return
 * the blocks they leave empty, Stage 28 W15c). A medium object (over a line) that the current
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

static int young_usable(Block *b) {
  return b->in_use && b->space == SPACE_MATURE && !b->young_hi &&
         b != gc_heap.space[SPACE_MATURE].cur && b != gc_heap.overflow;
}

static void young_take(Block *b) {
  young_note_hole(b);
  gc_heap.space[SPACE_NURSERY].cur = b;
  gc_heap.space[SPACE_NURSERY].fresh = 1;
  gc_heap.cur_whole = b->bump == b->base && b->limit == b->base + BLOCK_SIZE;
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
  Block *b = gc_heap.space[SPACE_NURSERY].cur;
  if (b) {
    b->search = (uint16_t)((b->limit - b->base) / LINE_SIZE);
    if (find_hole(b, total)) {
      young_note_hole(b);
      gc_heap.cur_whole = 0;
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
  Block *o = gc_heap.overflow;
  uint8_t *h;
  if (!o || o->bump + total > o->limit) {
    o = young_from(gc_heap.part_blocks, &gc_heap.part_blocks_n, total);
    if (!o) {
      o = gc_new_block(SPACE_MATURE);
      if (!o) {
        return NULL;
      }
    }
    young_note_hole(o);
    gc_heap.overflow = o;
  }
  h = o->bump;
  set_start(o, (size_t)(h - o->base));
  o->bump = h + total;
  gc_heap.ctr.sticky.overflow++;
  return (AvantHeader *)h;
}

/* Young allocation of TOTAL bytes without a new block, or NULL. */
static AvantHeader *young_place(size_t total) {
  Block *cur = gc_heap.space[SPACE_NURSERY].cur;
  if (cur) {
    if (gc_heap.cur != cur) {
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
    Block *b = cur ? cur->next : gc_heap.space[space].list;
    for (; b; b = b->next) {
      if (b->young_hi) {
        continue;
      }
      if (try_bump(b, total)) {
        gc_heap.space[space].cur = b;
        return bump_take(total);
      }
    }
  }
  Block *b = gc_new_block(bspace);
  if (!b && gc_heap.defer && !gc_heap.collecting) {
    /* C32 (gc-design.md §5.4): never collect inside a no-GC (defer)
     * region, where a runtime helper holds fresh objects the collector
     * cannot see (avant_str_split's items[]). The allocation proceeds
     * past the limit and the full collection runs at the first
     * collection point after the region (gc_maybe_collect), unless the
     * region alone is over the limit (§5.4). */
    gc_region_limit_check(total);
    gc_heap.over_limit = 1;
    b = gc_new_block(bspace);
    gc_heap.over_limit = 0;
    gc_limit_collect_pending();
  }
  if (!b && gc_heap.collecting && !gc_heap.over_limit) {
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
    if (!tried_gc && !gc_heap.collecting) {
      /* A full collection at the limit, then one retry. */
      gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
      return place(total, space, 1);
    }
    gc_die("heap limit");
  }
  if (space == SPACE_NURSERY) {
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
  if (gc_heap.defer) {
    gc_heap.region_bytes += total;
  }
  avant_since_gc += total;
  if (nursery) {
    avant_nursery_held += total;
  } else {
    gc_heap.mature_since_full += total;
  }
  gc_note_peak();
}

void avant_alloc_in_mature(int32_t on) {
  if (on) {
    gc_fast_clear();
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
  gc_init();
  gc_fast_refresh();
  if (nbytes > UINT32_MAX) {
    gc_die("object too large");
  }
  if (type_id > UINT16_MAX) {
    gc_die("type id too large");
  }

  size_t payload = (size_t)nbytes;
  size_t total = align_up(sizeof(AvantHeader) + payload);
  int large = alloc_is_large(type_id, payload, total);
  /* Single mutator, under the young threshold: bump and return.
   * Collection stays on the locked path, with the caller's roots
   * already spilled. This is the same contract as avant_bump_alloc.
   * Medium numeric buffers take the large path so their address stays. */
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 && !gc_heap.defer &&
      !gc_heap.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature && !large && !gc_heap.limit_pending && !gc_heap.explicit_pending &&
      gc_heap.mature_since_full < gc_heap.full_trigger && avant_nursery_held + total < avant_young_bytes) {
    AvantHeader *fast = place_bump(total);
    if (fast) {
      init_object(fast, payload, type_id, 1, 0);
      account_alloc(total, 1);
      return payload_of(fast);
    }
  }

  pthread_mutex_lock(&gc_heap.world);
  gc_maybe_collect();

  if (large) {
    /* One young budget (gc-design.md §5.1, Stage 28 W15a1): young large
     * bytes count in avant_nursery_held, so gc_maybe_collect above
     * already ran the young collection they are due (the eager
     * collection for one replaced buffer and the separate large budget
     * are retired). */
    void *obj = gc_los_alloc(payload, type_id);
    avant_g_stats.allocated_bytes += total;
    if (gc_heap.defer) {
      gc_heap.region_bytes += total;
    }
    avant_since_gc += total;
    pthread_mutex_unlock(&gc_heap.world);
    return obj;
  }

  uint8_t space = SPACE_NURSERY;
  if (gc_heap.set.nursery_off || gc_thread.alloc_mature) {
    space = SPACE_MATURE;
  }
  AvantHeader *h = place(total, space, 0);
  init_object(h, payload, type_id, space == SPACE_NURSERY, 0);
  account_alloc(total, space == SPACE_NURSERY);
  void *obj = payload_of(h);
  pthread_mutex_unlock(&gc_heap.world);
  return obj;
}

/* avant_alloc as runtime C calls it (avant_gc.h maps avant_alloc to this
 * name in every runtime unit outside the collector; Stage 28 W14b1). The
 * same allocation, plus AVANT_GC_STRESS's stress point first, so a
 * stress run collects at every runtime C allocation and not at the
 * generated code's, which roots everything across avant_alloc by
 * construction (D165). */
__attribute__((visibility("hidden"))) void *avant_alloc_c(uint64_t nbytes, uint32_t type_id) {
  void *p;
  gc_init();
  /* Runtime C holds its references in its own frame, so a young
   * collection entered here keeps in place the young objects the C stack
   * names (gc_pin_c_stack, Stage 28 W15a2; C37). */
  gc_thread.c_entry++;
  if (gc_heap.set.stress) {
    pthread_mutex_lock(&gc_heap.world);
    gc_stress_point();
    pthread_mutex_unlock(&gc_heap.world);
  }
  p = avant_alloc(nbytes, type_id);
  gc_thread.c_entry--;
  return p;
}

void *avant_bump_alloc(uint64_t nbytes, uint32_t type_id) {
  size_t payload;
  size_t total;
  AvantHeader *h;
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    return NULL;
  }
  if (nbytes > UINT32_MAX || type_id > UINT16_MAX) {
    return NULL;
  }
  gc_init();
  gc_fast_refresh();
  payload = (size_t)nbytes;
  total = align_up(sizeof(AvantHeader) + payload);
  h = place_bump(total);
  if (!h) {
    return NULL;
  }
  init_object(h, payload, type_id, 1, 0);
  account_alloc(total, 1);
  return payload_of(h);
}
