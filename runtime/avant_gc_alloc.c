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

static Block *g_cache;
static int g_cache_n;

Block *g_cur;
uint8_t *g_limit;
/* Inline nursery fast path (Collector II Wave 1): generated code bumps
 * avant_gc_bump directly when avant_gc_fast_ok is set, bounded by
 * avant_gc_limit (the block limit capped by the young threshold) and
 * writing the start bit through avant_gc_starts/avant_gc_base. The C
 * cursor g_bump is the same storage, so both sides share one cursor.
 * Only the runtime publishes or clears the fast state; see
 * gc_fast_refresh below. */
__thread uint8_t *avant_gc_bump;
__thread uint8_t *avant_gc_limit;
__thread uint8_t *avant_gc_base;
__thread uint64_t *avant_gc_starts;
__thread void *avant_gc_block;
__thread uint32_t avant_gc_fast_ok;
static Block *g_fast_block;

/* Per-space bump cursor. A full block used to send the next allocation
 * back through every older block (take_from_list + find_hole). Workers
 * allocate in mature space, and that walk dominated Matmul::T16.
 * g_alloc_fresh means the cursor is a new block: when it fills, allocate
 * another instead of rescanning full blocks. A full collection clears
 * the cursor so the next allocation reuses holes. */
Block *g_alloc_cur[4];
uint8_t g_alloc_fresh[4];

__thread int g_alloc_mature;

uint64_t avant_since_gc;

uint64_t g_heap_bytes;
uint64_t g_heap_max;
/* Block bytes per space (SPACE_*), kept as blocks enter a space, move
 * between spaces and leave (W14a2), so gc_note_peak reads them instead
 * of walking every list. */
uint64_t g_space_bytes[4];

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
  if (g_alloc_cur[SPACE_NURSERY] == b) {
    gc_clear_alloc_cur(SPACE_NURSERY);
  }
  if (g_alloc_cur[SPACE_MATURE] == b) {
    gc_clear_alloc_cur(SPACE_MATURE);
  }
  if (g_alloc_cur[SPACE_TO] == b) {
    gc_clear_alloc_cur(SPACE_TO);
  }
  if (g_cur == b) {
    g_cur = NULL;
    g_bump = NULL;
    g_limit = NULL;
  }
  if (g_fast_block == b) {
    gc_fast_clear();
  }
  /* Out of use: no lookup finds it, as when it left the block hash. A
   * cached block stays charged to g_heap_bytes; a released one goes back
   * to its chunk's free list (resident until a full collection's
   * gc_heap_return). */
  b->in_use = 0;
  g_space_bytes[b->space] -= BLOCK_SIZE;
  if (g_cache_n < CACHE_BLOCKS) {
    b->next = g_cache;
    g_cache = b;
    g_cache_n++;
    return;
  }
  gc_block_give(b);
  g_heap_bytes -= BLOCK_SIZE;
}

/* Wave 4 step 1: when the heap high-water mark moves, snapshot what
 * the heap is made of, so a peak can be decomposed after the fact.
 * O(1) since W14a2: counters, not walks of the block lists. */
void gc_note_peak(void) {
  if (g_heap_bytes <= g_stats.peak_heap) {
    return;
  }
  g_stats.peak_heap = g_heap_bytes;
  g_pk_nursery = g_space_bytes[SPACE_NURSERY];
  g_pk_mature = g_space_bytes[SPACE_MATURE];
  g_pk_from = g_space_bytes[SPACE_FROM];
  g_pk_to = g_space_bytes[SPACE_TO];
  g_pk_cache = (uint64_t)g_cache_n * BLOCK_SIZE;
  g_pk_large = g_large_bytes;
}

/* Wave 4 step 2: outside a collection the copy reserve counts as
 * spent, so fresh block growth stops early enough that the next copy
 * has its destination room under the heap limit. The charge is soft:
 * it makes allocation collect earlier, never fail, so the retry after
 * a collection (g_reserve_retry) and any allocation inside a
 * collection (where the reserve is what is being spent) are charged
 * only the true heap bytes. Cached blocks are already owned and are
 * never refused by this test. */
static int g_reserve_retry;
static int heap_block_fits(void) {
  uint64_t spent;
  if (!g_heap_max) {
    return 1;
  }
  spent = g_heap_bytes + BLOCK_SIZE;
  if (!g_collecting && !g_reserve_retry && g_reserve_mode) {
    spent += g_copy_reserve;
  }
  return spent <= g_heap_max;
}

Block *gc_new_block(uint8_t space) {
  if (!heap_block_fits() && g_cache == NULL) {
    return NULL;
  }
  Block *b = NULL;
  if (g_cache) {
    b = g_cache;
    g_cache = b->next;
    g_cache_n--;
    memset(b->line_mark, 0, LINES_PER_BLOCK);
    memset(b->start, 0, START_WORDS * sizeof(uint64_t));
    b->next = NULL;
  } else {
    if (!heap_block_fits()) {
      return NULL;
    }
    b = gc_block_take();
    g_heap_bytes += BLOCK_SIZE;
    gc_note_peak();
  }
  /* Block memory is not pre-zeroed. Every allocation zeroes its own
   * payload in init_object, so a fresh or recycled block costs no
   * whole-block clearing pass ahead of the mutator's own writes. */
  b->space = space;
  g_space_bytes[space] += BLOCK_SIZE;
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base + BLOCK_SIZE;
  b->in_use = 1;
  if (space == SPACE_NURSERY) {
    b->next = g_nursery;
    g_nursery = b;
  } else if (space == SPACE_TO) {
    b->next = g_to;
    g_to = b;
  } else {
    b->next = g_mature;
    g_mature = b;
  }
  return b;
}

void gc_use_block(Block *b) {
  g_cur = b;
  g_bump = b->bump;
  g_limit = b->limit;
  if (b->space == SPACE_NURSERY) {
    /* Keep the inline fast path's mirror on the block the cursor is
     * actually in. A slow allocation that switches blocks would
     * otherwise leave generated code writing start bits through the
     * previous block's base. A recycled block clears the fast path:
     * its start bits need the clearing only bump_take does. */
    if (!g_collecting && b == g_alloc_cur[SPACE_NURSERY] &&
        g_alloc_fresh[SPACE_NURSERY]) {
      uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
      uint8_t *cap = g_bump + rem;
      g_fast_block = b;
      avant_gc_base = b->base;
      avant_gc_starts = b->start;
      avant_gc_block = b;
      avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
    } else {
      gc_fast_clear();
    }
  }
}

static int bump_fits(size_t need) {
  return g_cur && g_bump && g_bump + need <= g_limit;
}

static void *bump_take(size_t need) {
  Block *b = g_cur;
  size_t off = (size_t)(g_bump - b->base);
  /* A fresh block is zeroed and only bumps forward. Clearing start bits
   * is for a hole in a recycled block. */
  if (!g_alloc_fresh[b->space]) {
    clear_starts(b, off, need);
  }
  set_start(b, off);
  void *h = g_bump;
  g_bump += need;
  b->bump = g_bump;
  if (b == g_fast_block) {
    uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
    uint8_t *cap = g_bump + rem;
    avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
  }
  return h;
}

void gc_fast_clear(void) {
  avant_gc_fast_ok = 0;
  avant_gc_limit = NULL;
  g_fast_block = NULL;
}

/* Publish the inline fast path when every C fast-path condition holds
 * and the current nursery block is fresh (a recycled block needs the
 * start-bit clearing only bump_take does). Anything else clears it, so
 * generated code only ever checks fast_ok, the shared flag, and the
 * cursor against the limit. Called at slow allocation entries; state
 * transitions clear eagerly below. */
static void gc_fast_refresh(void) {
  Block *cur = g_alloc_cur[SPACE_NURSERY];
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 &&
      !g_gc_defer && !g_collecting && !g_nursery_off && !g_alloc_mature &&
      g_mature_since_full < FULL_BYTES && cur && g_cur == cur &&
      g_alloc_fresh[SPACE_NURSERY] && g_bump) {
    uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
    uint8_t *cap = g_bump + rem;
    g_fast_block = cur;
    avant_gc_base = cur->base;
    avant_gc_starts = cur->start;
    avant_gc_block = cur;
    avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
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
  g_alloc_cur[space] = NULL;
  g_alloc_fresh[space] = 0;
}

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

static AvantHeader *place(size_t total, uint8_t space, int tried_gc) {
  Block *cur = g_alloc_cur[space];
  if (cur && try_bump(cur, total)) {
    return bump_take(total);
  }
  /* A fresh block has no holes behind it. Scanning the list again
   * walks every full block. Recycle only after a collection cleared
   * the fresh bit, and then only forward from the cursor. */
  if (!g_alloc_fresh[space]) {
    Block *list = g_mature;
    if (space == SPACE_NURSERY) {
      list = g_nursery;
    } else if (space == SPACE_TO) {
      list = g_to;
    }
    Block *b = cur ? cur->next : list;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        g_alloc_cur[space] = b;
        return bump_take(total);
      }
    }
  }
  Block *b = gc_new_block(space);
  if (!b) {
    if (!tried_gc && !g_collecting) {
      /* The reserve charge is soft (see heap_block_fits): collect,
       * then retry charged only the true heap bytes, so a reserve
       * refusal can never become a spurious heap-limit death. */
      gc_collect(1);
      g_reserve_retry = 1;
      {
        AvantHeader *retry = place(total, space, 1);
        g_reserve_retry = 0;
        return retry;
      }
    }
    gc_die("heap limit");
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
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
  if (g_nursery_off || g_alloc_mature || total > LOS_LIMIT) {
    return NULL;
  }
  cur = g_alloc_cur[space];
  /* Steady state: the global cursor already mirrors the nursery block.
   * Take from it directly instead of re-syncing through try_bump. */
  if (cur && g_cur == cur && g_bump && g_bump + total <= g_limit) {
    return (AvantHeader *)bump_take(total);
  }
  if (cur && try_bump(cur, total)) {
    return (AvantHeader *)bump_take(total);
  }
  if (!g_alloc_fresh[space]) {
    b = cur ? cur->next : g_nursery;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        g_alloc_cur[space] = b;
        return (AvantHeader *)bump_take(total);
      }
    }
  }
  b = gc_new_block(space);
  if (!b) {
    return NULL;
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
  gc_use_block(b);
  if (!bump_fits(total)) {
    return NULL;
  }
  return (AvantHeader *)bump_take(total);
}

void *gc_mature_alloc(size_t total) {
  /* Young copy promotes into mature space while g_collecting is set.
   * tried_gc stays 1 so a failed block does not re-enter collect. */
  g_mature_since_full += total;
  return place(total, SPACE_MATURE, 1);
}

AvantHeader *gc_to_alloc(size_t total) {
  AvantHeader *h = place(total, SPACE_TO, 1);
  g_to_bytes += total;
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
  g_stats.allocated_bytes += total;
  g_since_gc += total;
  if (nursery) {
    g_nursery_held += total;
  } else {
    g_mature_since_full += total;
  }
  gc_note_peak();
}

void avant_alloc_in_mature(int32_t on) {
  if (on) {
    gc_fast_clear();
  }
  g_alloc_mature = on ? 1 : 0;
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
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 && !g_gc_defer &&
      !g_collecting && !g_nursery_off && !g_alloc_mature && !large &&
      g_mature_since_full < FULL_BYTES && g_nursery_held + total < g_young_bytes) {
    AvantHeader *fast = place_bump(total);
    if (fast) {
      init_object(fast, payload, type_id, 1, 0);
      account_alloc(total, 1);
      return payload_of(fast);
    }
  }

  pthread_mutex_lock(&g_world);
  gc_maybe_collect();

  if (large) {
    /* One replaced scalar buffer is reclaimed into the spare slot.
     * A survived buffer sets the hold so a run of live rows does not
     * collect on every allocation. The byte threshold still runs. */
    if (!g_nursery_off && !g_alloc_mature && !g_gc_defer && g_young_large_n > 0 &&
        (g_young_large >= g_young_bytes || (!g_los_eager_hold && g_young_large_n == 1))) {
      uint64_t mature_before = g_mature_since_full;
      gc_collect(0);
      if (g_young_large_n > 0 || g_mature_since_full > mature_before) {
        g_los_eager_hold = 1;
      } else {
        g_los_eager_hold = 0;
      }
    }
    void *obj = gc_los_alloc(payload, type_id);
    g_stats.allocated_bytes += total;
    g_since_gc += total;
    pthread_mutex_unlock(&g_world);
    return obj;
  }

  uint8_t space = SPACE_NURSERY;
  if (g_nursery_off || g_alloc_mature) {
    space = SPACE_MATURE;
  }
  AvantHeader *h = place(total, space, 0);
  init_object(h, payload, type_id, space == SPACE_NURSERY, 0);
  account_alloc(total, space == SPACE_NURSERY);
  void *obj = payload_of(h);
  pthread_mutex_unlock(&g_world);
  return obj;
}

/* avant_alloc as runtime C calls it (avant_gc.h maps avant_alloc to this
 * name in every runtime unit outside the collector; Stage 28 W14b1). The
 * same allocation, plus AVANT_GC_STRESS's stress point first, so a
 * stress run collects at every runtime C allocation and not at the
 * generated code's, which roots everything across avant_alloc by
 * construction (D165). */
void *avant_alloc_c(uint64_t nbytes, uint32_t type_id) {
  gc_init();
  if (g_stress) {
    pthread_mutex_lock(&g_world);
    gc_stress_point();
    pthread_mutex_unlock(&g_world);
  }
  return avant_alloc(nbytes, type_id);
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
