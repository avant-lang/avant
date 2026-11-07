#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The large-object space (Stage 28 W12; in the heap's chunks since W14a2,
 * gc-design.md §2.4, O8). An object too large for a block (or a medium
 * scalar buffer) takes an exact run of whole 4 KiB pages,
 * ceil((16 + size) / 4096), first fit from the free-page bitmap of a
 * large-object chunk (CHUNK_LARGE); an object larger than a chunk holds
 * takes a run of whole contiguous chunks (CHUNK_HUGE, then
 * CHUNK_HUGE_CONT). Its header is 8 bytes into the run's first page (the
 * payload is 16-aligned) and has its start bit; the page table (g_pages)
 * names every page's run, so an interior address is one table read
 * (avant_gc_heap.c). Large objects never move. No list, hash or address
 * range: every walk goes over the chunks (gc_large_next), and the bytes
 * the large objects hold are a counter (g_large_bytes). Young large
 * objects, the spare buffer, their reclaim at young collections and
 * their sweep at full ones keep their policy (LOS_KEEP and the rest). */

/* One dead scalar large buffer, kept for the next same-sized alloc: its
 * pages stay allocated, its start bit is clear (no lookup finds it). */
AvantHeader *g_los_spare;
/* The page-run bytes of every large object (the spare excluded): the
 * peak decomposition's pk_large and the stats line's los_now_bytes. */
uint64_t g_large_bytes;
/* Bytes in young scalar large objects. They do not move. A young
 * collection frees the ones the mark does not reach. */
uint64_t g_young_large;
int g_young_large_n;
/* Set when a young large object survived a collection. Further
 * same-sized allocs are live rows, not a replaced buffer, so they
 * must not collect on every malloc. */
int g_los_eager_hold;

/* The large-object chunks (CHUNK_LARGE), in the order they were made:
 * the first fit looks in each, in this order. */
static uint32_t *g_lchunks;
static uint32_t g_lchunks_n;
static uint32_t g_lchunks_cap;

static uint64_t run_pages(size_t payload) {
  return ((uint64_t)payload + 16 + PAGE_SIZE - 1) >> PAGE_SHIFT;
}

/* The bytes of H's page run: what the object charges to the heap. */
size_t gc_large_bytes(const AvantHeader *h) {
  return (size_t)(run_pages((size_t)h->size) << PAGE_SHIFT);
}

static uint64_t chunk_index(const void *p) {
  return (uint64_t)((uintptr_t)p - (uintptr_t)g_heap_base) >> CHUNK_SHIFT;
}

static void bits_set(uint64_t *a, uint32_t from, uint32_t n, int on) {
  uint32_t i;
  for (i = from; i < from + n;) {
    if ((i & 63) == 0 && i + 64 <= from + n) {
      a[i >> 6] = on ? ~0ULL : 0;
      i += 64;
      continue;
    }
    if (on) {
      a[i >> 6] |= 1ULL << (i & 63);
    } else {
      a[i >> 6] &= ~(1ULL << (i & 63));
    }
    i++;
  }
}

/* The first run of N free pages in A (a set bit is an allocated page),
 * or -1. */
static int first_fit(const uint64_t *a, uint32_t n) {
  uint32_t p = 0;
  uint32_t run = 0;
  uint32_t start = 0;
  while (p < PAGES_PER_CHUNK) {
    uint64_t w = a[p >> 6];
    if ((p & 63) == 0 && w == ~0ULL) {
      run = 0;
      p += 64;
      continue;
    }
    if ((p & 63) == 0 && w == 0) {
      if (run == 0) {
        start = p;
      }
      run += 64;
      if (run >= n) {
        return (int)start;
      }
      p += 64;
      continue;
    }
    if ((w >> (p & 63)) & 1ULL) {
      run = 0;
    } else {
      if (run == 0) {
        start = p;
      }
      run++;
      if (run >= n) {
        return (int)start;
      }
    }
    p++;
  }
  return -1;
}

/* A chunk made a large-object chunk: no page allocated, its start bits
 * and page table clear; a reused chunk's pages are all dirty. */
static uint64_t make_large_chunk(void) {
  uint64_t c = gc_chunk_take(1);
  GcChunk *ch = &g_chunks[c];
  int dirty = ch->dirty;
  memset(&g_starts[c * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
  memset(&g_pages[c * PAGES_PER_CHUNK], 0, PAGES_PER_CHUNK * sizeof(uint32_t));
  memset(ch, 0, sizeof(GcChunk));
  ch->kind = CHUNK_LARGE;
  if (dirty) {
    memset(ch->dirty_pages, 0xff, sizeof(ch->dirty_pages));
  }
  if (g_lchunks_n == g_lchunks_cap) {
    g_lchunks_cap = g_lchunks_cap ? g_lchunks_cap * 2 : 16;
    g_lchunks = gc_xrealloc(g_lchunks, g_lchunks_cap * sizeof(uint32_t));
  }
  g_lchunks[g_lchunks_n++] = (uint32_t)c;
  return c;
}

/* Take a run for PAYLOAD bytes; returns the header (8 bytes into the
 * run), the payload zeroed, the start bit not yet set. */
static AvantHeader *take_run(size_t payload) {
  uint64_t n = run_pages(payload);
  if (n <= PAGES_PER_CHUNK) {
    uint32_t i;
    uint64_t c = 0;
    int p = -1;
    GcChunk *ch;
    uint8_t *run;
    uint32_t k;
    for (i = 0; i < g_lchunks_n; i++) {
      ch = &g_chunks[g_lchunks[i]];
      if (PAGES_PER_CHUNK - ch->used >= n) {
        p = first_fit(ch->alloc, (uint32_t)n);
        if (p >= 0) {
          c = g_lchunks[i];
          break;
        }
      }
    }
    if (p < 0) {
      c = make_large_chunk();
      p = 0;
    }
    ch = &g_chunks[c];
    run = chunk_base(c) + ((size_t)p << PAGE_SHIFT);
    bits_set(ch->alloc, (uint32_t)p, (uint32_t)n, 1);
    ch->used = (uint16_t)(ch->used + n);
    for (k = 0; k < n; k++) {
      uint32_t pg = (uint32_t)p + k;
      g_pages[c * PAGES_PER_CHUNK + pg] = (uint32_t)p + 1;
      if ((ch->dirty_pages[pg >> 6] >> (pg & 63)) & 1ULL) {
        memset(run + ((size_t)k << PAGE_SHIFT), 0, PAGE_SIZE);
      }
    }
    bits_set(ch->dirty_pages, (uint32_t)p, (uint32_t)n, 1);
    return (AvantHeader *)(run + 8);
  }
  {
    uint64_t nch = (n + PAGES_PER_CHUNK - 1) / PAGES_PER_CHUNK;
    uint64_t c = gc_chunk_take(nch);
    uint64_t k;
    for (k = 0; k < nch; k++) {
      GcChunk *ch = &g_chunks[c + k];
      int dirty = ch->dirty;
      memset(&g_starts[(c + k) * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
      memset(ch, 0, sizeof(GcChunk));
      ch->kind = k ? CHUNK_HUGE_CONT : CHUNK_HUGE;
      ch->head = k ? (uint32_t)c : (uint32_t)nch;
      if (dirty) {
        uint64_t used = (n << PAGE_SHIFT) - k * (uint64_t)CHUNK_SIZE;
        memset(chunk_base(c + k), 0, (size_t)(used < CHUNK_SIZE ? used : CHUNK_SIZE));
      }
    }
    return (AvantHeader *)(chunk_base(c) + 8);
  }
}

/* A large-object chunk leaves g_lchunks (it is being decommitted). */
void gc_large_chunk_gone(uint64_t c) {
  uint32_t i;
  for (i = 0; i < g_lchunks_n; i++) {
    if (g_lchunks[i] == (uint32_t)c) {
      memmove(&g_lchunks[i], &g_lchunks[i + 1], (g_lchunks_n - i - 1) * sizeof(uint32_t));
      g_lchunks_n--;
      return;
    }
  }
}

/* Give back the run of the object (or spare) whose header is H: its
 * pages free in its chunk, or its huge run's chunks free. The memory
 * stays resident for reuse; a full collection's gc_heap_return gives
 * back what stays idle (gc-design.md §2.4 asked for an immediate madvise
 * of runs of 64 KiB or more: measured, it made every reuse fault its
 * pages in again, Json::ParseDom +53%, so returning waits for the
 * hysteresis, Q12). */
static void give_run(AvantHeader *h) {
  uint64_t c = chunk_index(h);
  GcChunk *ch = &g_chunks[c];
  uint64_t n = run_pages((size_t)h->size);
  start_bit_set(h, 0);
  if (ch->kind == CHUNK_LARGE) {
    uint32_t p = (uint32_t)((((uintptr_t)h - 8) >> PAGE_SHIFT) & (PAGES_PER_CHUNK - 1));
    uint32_t k;
    bits_set(ch->alloc, p, (uint32_t)n, 0);
    ch->used = (uint16_t)(ch->used - n);
    for (k = 0; k < n; k++) {
      g_pages[c * PAGES_PER_CHUNK + p + k] = 0;
    }
    ch->idle = (uint32_t)g_stats.full_collections;
    return;
  }
  {
    uint64_t nch = ch->head;
    uint64_t k;
    for (k = 0; k < nch; k++) {
      gc_chunk_give(c + k);
    }
  }
}

/* The next large object in address order (spares skipped), or NULL. The
 * position moves past it before it is returned, so the caller may free
 * it. */
AvantHeader *gc_large_next(LargeIter *it) {
  uint64_t hw = g_heap_committed >> CHUNK_SHIFT;
  while (it->c < hw) {
    GcChunk *ch = &g_chunks[it->c];
    if (ch->kind == CHUNK_HUGE) {
      AvantHeader *h = (AvantHeader *)(chunk_base(it->c) + 8);
      it->c += ch->head;
      it->p = 0;
      if (start_bit_at(h)) {
        return h;
      }
      continue;
    }
    if (ch->kind != CHUNK_LARGE) {
      it->c++;
      it->p = 0;
      continue;
    }
    while (it->p < PAGES_PER_CHUNK) {
      uint32_t p = it->p;
      uint64_t w = ch->alloc[p >> 6] >> (p & 63);
      if (!w) {
        it->p = (p | 63) + 1;
        continue;
      }
      if (!(w & 1ULL)) {
        it->p = p + (uint32_t)__builtin_ctzll(w);
        continue;
      }
      {
        AvantHeader *h = (AvantHeader *)(chunk_base(it->c) + ((size_t)p << PAGE_SHIFT) + 8);
        it->p = p + (uint32_t)run_pages((size_t)h->size);
        if (start_bit_at(h)) {
          return h;
        }
      }
    }
    it->c++;
    it->p = 0;
  }
  return NULL;
}

void gc_los_spare_drop(void) {
  size_t bytes;
  if (!g_los_spare) {
    return;
  }
  bytes = gc_large_bytes(g_los_spare);
  give_run(g_los_spare);
  g_los_spare = NULL;
  g_heap_bytes -= bytes;
  g_los_spare_dropped++;
}

/* keep_bytes leaves the allocation charged, for the spare slot. */
static void account_large_gone(AvantHeader *h, int keep_bytes) {
  size_t bytes = gc_large_bytes(h);
  g_large_bytes -= bytes;
  if (h->flags & AVANT_FLAG_YOUNG) {
    if (g_young_large >= bytes) {
      g_young_large -= bytes;
    }
    if (g_young_large_n > 0) {
      g_young_large_n--;
    }
  } else if (g_mature_since_full >= bytes) {
    g_mature_since_full -= bytes;
  }
  if (!keep_bytes) {
    g_heap_bytes -= bytes;
  }
}

void gc_release_large(AvantHeader *h) {
  int cache = (h->flags & AVANT_FLAG_YOUNG) &&
              h->type_id == AVANT_TYPE_BYTES &&
              (h->flags & AVANT_FLAG_PINNED) == 0 &&
              g_los_spare == NULL;
  account_large_gone(h, cache);
  if (cache) {
    start_bit_set(h, 0);
    g_los_spare = h;
    g_los_spare_cached++;
  } else {
    give_run(h);
    g_los_freed++;
  }
}

/* FULL: the full collection's marks (full_marked), else the young
 * collection's (the header's MARKED). */
static void los_note_live(int full) {
  LargeIter it;
  AvantHeader *h;
  uint64_t live = 0;
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    if (full ? full_marked(h) : (h->flags & AVANT_FLAG_MARKED) != 0) {
      live += (uint64_t)gc_large_bytes(h);
    }
  }
  g_los_live_samples++;
  g_los_live_sum += live;
  if (live > g_los_live_peak) {
    g_los_live_peak = live;
  }
}

static void promote_young_large(AvantHeader *h) {
  size_t bytes = gc_large_bytes(h);
  g_los_promoted++;
  g_los_promoted_bytes += (uint64_t)bytes;
  h->flags = (uint16_t)(h->flags & (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_LOS_KEEP));
  if (g_young_large >= bytes) {
    g_young_large -= bytes;
  }
  if (g_young_large_n > 0) {
    g_young_large_n--;
  }
  g_mature_since_full += bytes;
}

/* Unmarked young large objects are unreachable and are freed. A marked
 * one is promoted when a heap object points at it, or when the stack
 * is holding a working set at least as big as the young limit. A
 * stack-only buffer stays young, so the next collection can free it
 * once that local is gone. */
void gc_reclaim_young_large(void) {
  LargeIter it;
  AvantHeader *h;
  size_t stay_bytes = 0;
  int promote_stay;
  los_note_live(0);
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    if ((h->flags & AVANT_FLAG_YOUNG) == 0) {
      continue;
    }
    if ((h->flags & AVANT_FLAG_MARKED) == 0) {
      continue;
    }
    if (h->flags & (AVANT_FLAG_PINNED | AVANT_FLAG_LOS_KEEP)) {
      continue;
    }
    stay_bytes += gc_large_bytes(h);
  }
  promote_stay = stay_bytes >= g_young_bytes;
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    if (h->flags & AVANT_FLAG_YOUNG) {
      int marked = (h->flags & AVANT_FLAG_MARKED) != 0;
      int pinned = (h->flags & AVANT_FLAG_PINNED) != 0;
      int keep = (h->flags & AVANT_FLAG_LOS_KEEP) != 0;
      if (!marked && !pinned) {
        gc_release_large(h);
      } else if (pinned || keep || promote_stay) {
        promote_young_large(h);
      } else {
        h->flags = (uint16_t)(h->flags & (uint16_t)~AVANT_FLAG_LOS_KEEP);
      }
    }
  }
}

void gc_sweep_large(void) {
  LargeIter it;
  AvantHeader *h;
  los_note_live(1);
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    if (!full_marked(h)) {
      size_t bytes = gc_large_bytes(h);
      give_run(h);
      g_large_bytes -= bytes;
      g_heap_bytes -= bytes;
      g_los_freed++;
    }
  }
}

void *gc_los_alloc(size_t payload, uint32_t type_id) {
  size_t bytes = (size_t)(run_pages(payload) << PAGE_SHIFT);
  int young = !g_nursery_off && !g_alloc_mature && type_id == AVANT_TYPE_BYTES;
  AvantHeader *h;
  if (g_los_spare && g_los_spare->size != (uint32_t)payload) {
    gc_los_spare_drop();
  }
  if (g_heap_max && !g_los_spare && g_heap_bytes + bytes > g_heap_max) {
    if (g_gc_defer) {
      /* C32: inside a no-GC region the allocation proceeds past the
       * limit and the collection runs after the region. */
      gc_limit_collect_pending();
    } else {
      gc_collect(1);
      if (g_heap_bytes + bytes > g_heap_max) {
        gc_die("heap limit");
      }
    }
  }
  g_los_allocs++;
  g_los_alloc_bytes += (uint64_t)bytes;
  {
    int bucket = payload < 16384 ? 0 : payload < 65536 ? 1 : payload < 262144 ? 2 : payload < 1048576 ? 3 : payload < 4194304 ? 4 : 5;
    g_los_hist[bucket]++;
    if (type_id == AVANT_TYPE_BYTES && align_up(sizeof(AvantHeader) + payload) <= LOS_LIMIT) {
      g_los_scalar_allocs++;
      g_los_scalar_bytes += (uint64_t)bytes;
    } else {
      g_los_big_allocs++;
      g_los_big_bytes += (uint64_t)bytes;
    }
    if (young) {
      g_los_young_allocs++;
      g_los_young_bytes += (uint64_t)bytes;
    } else {
      g_los_mature_allocs++;
      g_los_mature_bytes += (uint64_t)bytes;
    }
  }
  if (g_los_spare && g_los_spare->size == (uint32_t)payload) {
    h = g_los_spare;
    g_los_spare = NULL;
    memset(payload_of(h), 0, payload);
    g_los_spare_hits++;
  } else {
    h = take_run(payload);
    g_heap_bytes += bytes;
    g_los_callocs++;
  }
  h->size = (uint32_t)payload;
  h->type_id = (uint16_t)type_id;
  h->flags = (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_LARGE);
  start_bit_set(h, 1);
  g_large_bytes += bytes;
  if (young) {
    h->flags |= AVANT_FLAG_YOUNG;
    g_young_large += bytes;
    g_young_large_n++;
  } else {
    g_mature_since_full += bytes;
  }
  gc_note_peak();
  return payload_of(h);
}
