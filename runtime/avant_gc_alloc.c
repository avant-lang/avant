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
    if (b->line_mark[i]) {
      i++;
      continue;
    }
    int j = i;
    while (j < LINES_PER_BLOCK && !b->line_mark[j]) {
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
  if (gc_heap.space[SPACE_TO].cur == b) {
    gc_clear_alloc_cur(SPACE_TO);
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
  gc_heap.ctr.pk.from = gc_heap.space[SPACE_FROM].bytes;
  gc_heap.ctr.pk.to = gc_heap.space[SPACE_TO].bytes;
  gc_heap.ctr.pk.cache = (uint64_t)gc_heap.cache_n * BLOCK_SIZE;
  gc_heap.ctr.pk.large = gc_heap.large_bytes;
}

static int heap_block_fits(void) {
  uint64_t spent;
  if (!gc_heap.set.heap_max || gc_heap.over_limit) {
    return 1;
  }
  spent = gc_heap.heap_bytes + BLOCK_SIZE;
  if (!gc_heap.collecting && !gc_heap.reserve_retry) {
    spent += gc_heap.copy_reserve;
  }
  return spent <= gc_heap.set.heap_max;
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
  /* Blocks are made for the nursery, to-space and mature space only. */
  b->next = gc_heap.space[space].list;
  gc_heap.space[space].list = b;
  return b;
}

void gc_use_block(Block *b) {
  gc_heap.cur = b;
  avant_gc_bump = b->bump;
  gc_heap.limit = b->limit;
  if (b->space == SPACE_NURSERY) {
    /* Keep the inline fast path's mirror on the block the cursor is
     * actually in. A slow allocation that switches blocks would
     * otherwise leave generated code writing start bits through the
     * previous block's base. A recycled block clears the fast path:
     * its start bits need the clearing only bump_take does. */
    if (!gc_heap.collecting && b == gc_heap.space[SPACE_NURSERY].cur &&
        gc_heap.space[SPACE_NURSERY].fresh) {
      uint64_t rem = avant_young_bytes > avant_nursery_held ? avant_young_bytes - avant_nursery_held : 0;
      uint8_t *cap = avant_gc_bump + rem;
      gc_heap.fast_block = b;
      avant_gc_base = b->base;
      avant_gc_starts = b->start;
      avant_gc_block = b;
      avant_gc_limit = gc_heap.limit && cap > gc_heap.limit ? gc_heap.limit : cap;
    } else {
      gc_fast_clear();
    }
  }
}

static int bump_fits(size_t need) {
  return gc_heap.cur && avant_gc_bump && avant_gc_bump + need <= gc_heap.limit;
}

static void *bump_take(size_t need) {
  Block *b = gc_heap.cur;
  size_t off = (size_t)(avant_gc_bump - b->base);
  /* A fresh block is zeroed and only bumps forward. Clearing start bits
   * is for a hole in a recycled block. */
  if (!gc_heap.space[b->space].fresh) {
    clear_starts(b, off, need);
  }
  set_start(b, off);
  void *h = avant_gc_bump;
  avant_gc_bump += need;
  b->bump = avant_gc_bump;
  if (b == gc_heap.fast_block) {
    uint64_t rem = avant_young_bytes > avant_nursery_held ? avant_young_bytes - avant_nursery_held : 0;
    uint8_t *cap = avant_gc_bump + rem;
    avant_gc_limit = gc_heap.limit && cap > gc_heap.limit ? gc_heap.limit : cap;
  }
  return h;
}

void gc_fast_clear(void) {
  avant_gc_fast_ok = 0;
  avant_gc_limit = NULL;
  gc_heap.fast_block = NULL;
}

/* Publish the inline fast path when every C fast-path condition holds
 * and the current nursery block is fresh (a recycled block needs the
 * start-bit clearing only bump_take does). Anything else clears it, so
 * generated code only ever checks fast_ok, the shared flag, and the
 * cursor against the limit. Called at slow allocation entries; state
 * transitions clear eagerly below. */
static void gc_fast_refresh(void) {
  Block *cur = gc_heap.space[SPACE_NURSERY].cur;
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 &&
      !gc_heap.defer && !gc_heap.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature &&
      !gc_heap.limit_pending && gc_heap.mature_since_full < FULL_BYTES && cur && gc_heap.cur == cur &&
      gc_heap.space[SPACE_NURSERY].fresh && avant_gc_bump) {
    uint64_t rem = avant_young_bytes > avant_nursery_held ? avant_young_bytes - avant_nursery_held : 0;
    uint8_t *cap = avant_gc_bump + rem;
    gc_heap.fast_block = cur;
    avant_gc_base = cur->base;
    avant_gc_starts = cur->start;
    avant_gc_block = cur;
    avant_gc_limit = gc_heap.limit && cap > gc_heap.limit ? gc_heap.limit : cap;
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

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

static AvantHeader *place(size_t total, uint8_t space, int tried_gc) {
  Block *cur = gc_heap.space[space].cur;
  if (cur && try_bump(cur, total)) {
    return bump_take(total);
  }
  /* A fresh block has no holes behind it. Scanning the list again
   * walks every full block. Recycle only after a collection cleared
   * the fresh bit, and then only forward from the cursor. */
  if (!gc_heap.space[space].fresh) {
    Block *b = cur ? cur->next : gc_heap.space[space].list;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        gc_heap.space[space].cur = b;
        return bump_take(total);
      }
    }
  }
  Block *b = gc_new_block(space);
  if (!b && gc_heap.defer && !gc_heap.collecting) {
    /* C32 (gc-design.md §5.4): never collect inside a no-GC (defer)
     * region, where a runtime helper holds fresh objects the collector
     * cannot see (avant_str_split's items[]). The allocation proceeds
     * past the limit and the full collection runs at the first
     * collection point after the region (gc_maybe_collect). */
    gc_heap.over_limit = 1;
    b = gc_new_block(space);
    gc_heap.over_limit = 0;
    gc_limit_collect_pending();
  }
  if (!b) {
    if (!tried_gc && !gc_heap.collecting) {
      /* The reserve charge is soft (see heap_block_fits): collect,
       * then retry charged only the true heap bytes, so a reserve
       * refusal can never become a spurious heap-limit death. */
      gc_collect(GC_FULL, 1);
      gc_heap.reserve_retry = 1;
      {
        AvantHeader *retry = place(total, space, 1);
        gc_heap.reserve_retry = 0;
        return retry;
      }
    }
    gc_die("heap limit");
  }
  gc_heap.space[space].cur = b;
  gc_heap.space[space].fresh = 1;
  gc_use_block(b);
  if (!bump_fits(total)) {
    gc_die("object larger than block");
  }
  return bump_take(total);
}

/* Like place into the nursery, but never collects. */
static AvantHeader *place_bump(size_t total) {
  uint8_t space = SPACE_NURSERY;
  Block *cur;
  Block *b;
  if (gc_heap.set.nursery_off || gc_thread.alloc_mature || total > LOS_LIMIT) {
    return NULL;
  }
  cur = gc_heap.space[space].cur;
  /* Steady state: the global cursor already mirrors the nursery block.
   * Take from it directly instead of re-syncing through try_bump. */
  if (cur && gc_heap.cur == cur && avant_gc_bump && avant_gc_bump + total <= gc_heap.limit) {
    return (AvantHeader *)bump_take(total);
  }
  if (cur && try_bump(cur, total)) {
    return (AvantHeader *)bump_take(total);
  }
  if (!gc_heap.space[space].fresh) {
    b = cur ? cur->next : gc_heap.space[SPACE_NURSERY].list;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        gc_heap.space[space].cur = b;
        return (AvantHeader *)bump_take(total);
      }
    }
  }
  b = gc_new_block(space);
  if (!b) {
    return NULL;
  }
  gc_heap.space[space].cur = b;
  gc_heap.space[space].fresh = 1;
  gc_use_block(b);
  if (!bump_fits(total)) {
    return NULL;
  }
  return (AvantHeader *)bump_take(total);
}

void *gc_mature_alloc(size_t total) {
  /* Young copy promotes into mature space while gc_heap.collecting is set.
   * tried_gc stays 1 so a failed block does not re-enter collect. */
  gc_heap.mature_since_full += total;
  return place(total, SPACE_MATURE, 1);
}

AvantHeader *gc_to_alloc(size_t total) {
  AvantHeader *h = place(total, SPACE_TO, 1);
  gc_heap.to_bytes += total;
  return h;
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
      !gc_heap.collecting && !gc_heap.set.nursery_off && !gc_thread.alloc_mature && !large && !gc_heap.limit_pending &&
      gc_heap.mature_since_full < FULL_BYTES && avant_nursery_held + total < avant_young_bytes) {
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
    /* One replaced scalar buffer is reclaimed into the spare slot.
     * A survived buffer sets the hold so a run of live rows does not
     * collect on every allocation. The byte threshold still runs. */
    if (!gc_heap.set.nursery_off && !gc_thread.alloc_mature && !gc_heap.defer && gc_heap.young_large_n > 0 &&
        (gc_heap.young_large >= avant_young_bytes || (!gc_heap.los_eager_hold && gc_heap.young_large_n == 1))) {
      uint64_t mature_before = gc_heap.mature_since_full;
      gc_collect(GC_YOUNG, 1);
      if (gc_heap.young_large_n > 0 || gc_heap.mature_since_full > mature_before) {
        gc_heap.los_eager_hold = 1;
      } else {
        gc_heap.los_eager_hold = 0;
      }
    }
    void *obj = gc_los_alloc(payload, type_id);
    avant_g_stats.allocated_bytes += total;
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
   * collection entered here rewrites the C stack (C37). */
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
