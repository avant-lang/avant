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
 * payload is 16-aligned) and has its start bit; the page table (gc_heap.pages)
 * names every page's run, so an interior address is one table read
 * (avant_gc_heap.c). Large objects never move. No list, hash or address
 * range: every walk goes over the chunks (gc_large_next), and the bytes
 * the large objects hold are a counter (gc_heap.large_bytes). Every large
 * object is born young and dies young (Stage 28 W15a1, gc-design.md
 * §5.3): gc_reclaim_young_large. */

static uint64_t run_pages(size_t payload) {
  return ((uint64_t)payload + 16 + PAGE_SIZE - 1) >> PAGE_SHIFT;
}

/* The bytes of H's page run: what the object charges to the heap. */
size_t gc_large_bytes(const AvantHeader *h) {
  return (size_t)(run_pages((size_t)h->size) << PAGE_SHIFT);
}

static uint64_t chunk_index(const void *p) {
  return (uint64_t)((uintptr_t)p - (uintptr_t)gc_heap.base) >> CHUNK_SHIFT;
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
  GcChunk *ch = &gc_heap.chunks[c];
  int dirty = ch->dirty;
  memset(&gc_heap.starts[c * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
  memset(&gc_heap.pages[c * PAGES_PER_CHUNK], 0, PAGES_PER_CHUNK * sizeof(uint32_t));
  memset(ch, 0, sizeof(GcChunk));
  ch->kind = CHUNK_LARGE;
  if (dirty) {
    memset(ch->dirty_pages, 0xff, sizeof(ch->dirty_pages));
  }
  if (gc_heap.lchunks_n == gc_heap.lchunks_cap) {
    gc_heap.lchunks_cap = gc_heap.lchunks_cap ? gc_heap.lchunks_cap * 2 : 16;
    gc_heap.lchunks = gc_xrealloc(gc_heap.lchunks, gc_heap.lchunks_cap * sizeof(uint32_t));
  }
  gc_heap.lchunks[gc_heap.lchunks_n++] = (uint32_t)c;
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
    for (i = 0; i < gc_heap.lchunks_n; i++) {
      ch = &gc_heap.chunks[gc_heap.lchunks[i]];
      if (PAGES_PER_CHUNK - ch->used >= n) {
        p = first_fit(ch->alloc, (uint32_t)n);
        if (p >= 0) {
          c = gc_heap.lchunks[i];
          break;
        }
      }
    }
    if (p < 0) {
      c = make_large_chunk();
      p = 0;
    }
    ch = &gc_heap.chunks[c];
    run = chunk_base(c) + ((size_t)p << PAGE_SHIFT);
    bits_set(ch->alloc, (uint32_t)p, (uint32_t)n, 1);
    ch->used = (uint16_t)(ch->used + n);
    for (k = 0; k < n; k++) {
      uint32_t pg = (uint32_t)p + k;
      gc_heap.pages[c * PAGES_PER_CHUNK + pg] = (uint32_t)p + 1;
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
      GcChunk *ch = &gc_heap.chunks[c + k];
      int dirty = ch->dirty;
      memset(&gc_heap.starts[(c + k) * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
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

/* A large-object chunk leaves gc_heap.lchunks (it is being decommitted). */
void gc_large_chunk_gone(uint64_t c) {
  uint32_t i;
  for (i = 0; i < gc_heap.lchunks_n; i++) {
    if (gc_heap.lchunks[i] == (uint32_t)c) {
      memmove(&gc_heap.lchunks[i], &gc_heap.lchunks[i + 1], (gc_heap.lchunks_n - i - 1) * sizeof(uint32_t));
      gc_heap.lchunks_n--;
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
  GcChunk *ch = &gc_heap.chunks[c];
  uint64_t n = run_pages((size_t)h->size);
  start_bit_set(h, 0);
  if (ch->kind == CHUNK_LARGE) {
    uint32_t p = (uint32_t)((((uintptr_t)h - 8) >> PAGE_SHIFT) & (PAGES_PER_CHUNK - 1));
    uint32_t k;
    bits_set(ch->alloc, p, (uint32_t)n, 0);
    ch->used = (uint16_t)(ch->used - n);
    for (k = 0; k < n; k++) {
      gc_heap.pages[c * PAGES_PER_CHUNK + p + k] = 0;
    }
    ch->idle = (uint32_t)gc_heap.return_clock;
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
  uint64_t hw = gc_heap.committed >> CHUNK_SHIFT;
  while (it->c < hw) {
    GcChunk *ch = &gc_heap.chunks[it->c];
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
  if (!gc_heap.los_spare) {
    return;
  }
  bytes = gc_large_bytes(gc_heap.los_spare);
  give_run(gc_heap.los_spare);
  gc_heap.los_spare = NULL;
  gc_heap.heap_bytes -= bytes;
  gc_heap.ctr.los.spare_dropped++;
}

/* keep_bytes leaves the allocation charged, for the spare slot. */
static void account_large_gone(AvantHeader *h, int keep_bytes) {
  size_t bytes = gc_large_bytes(h);
  gc_heap.large_bytes -= bytes;
  if (h->flags & AVANT_FLAG_YOUNG) {
    if (gc_heap.young_large >= bytes) {
      gc_heap.young_large -= bytes;
    }
    if (gc_heap.young_large_n > 0) {
      gc_heap.young_large_n--;
    }
    avant_nursery_held = avant_nursery_held >= bytes ? avant_nursery_held - bytes : 0;
  } else if (gc_heap.mature_since_full >= bytes) {
    gc_heap.mature_since_full -= bytes;
  }
  if (!keep_bytes) {
    gc_heap.heap_bytes -= bytes;
  }
}

/* A dead object's run: held out of use under AVANT_GC_VERIFY (the
 * lost-reference check, gc_quarantine_run), else given back. */
static void free_run(AvantHeader *h) {
  if (!gc_quarantine_run(h)) {
    give_run(h);
  }
}

/* gc_quarantine_release's: the held run of H goes back now. */
void gc_large_give_held(AvantHeader *h) {
  give_run(h);
}

void gc_release_large(AvantHeader *h) {
  /* AVANT_GC_STRESS=1: the payload of a freed run is poisoned (Stage 28
   * W16a); a reused run is zeroed again (take_run's dirty pages, the
   * spare's memset). */
  gc_poison(payload_of(h), (size_t)h->size);
  int cache = (h->flags & AVANT_FLAG_YOUNG) &&
              h->type_id == AVANT_TYPE_BYTES &&
              (h->flags & AVANT_FLAG_PINNED) == 0 &&
              gc_heap.los_spare == NULL && !gc_heap.set.verify;
  account_large_gone(h, cache);
  if (cache) {
    start_bit_set(h, 0);
    gc_heap.los_spare = h;
    gc_heap.ctr.los.spare_cached++;
  } else {
    free_run(h);
    gc_heap.ctr.los.freed++;
  }
}

static void note_live_sample(uint64_t live) {
  gc_heap.ctr.los.live_samples++;
  gc_heap.ctr.los.live_sum += live;
  if (live > gc_heap.ctr.los.live_peak) {
    gc_heap.ctr.los.live_peak = live;
  }
}

/* A young large object leaves the young generation (gc-design.md §5.3):
 * YOUNG and its age bit SURVIVED are cleared and its bytes become mature
 * growth. One with reference fields joins the kept list, so the
 * remembered set holds it while its fields name young objects (the
 * frontier drops it at the next young collection otherwise). */
static void promote_young_large(AvantHeader *h) {
  size_t bytes = gc_large_bytes(h);
  gc_heap.ctr.los.promoted++;
  gc_heap.ctr.los.promoted_bytes += (uint64_t)bytes;
  gc_heap.ctr.prom.large += (uint64_t)bytes;
  gc_heap.promoted_since_full += (uint64_t)bytes;
  h->flags = (uint16_t)(h->flags & (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED));
  if (gc_heap.young_large >= bytes) {
    gc_heap.young_large -= bytes;
  }
  if (gc_heap.young_large_n > 0) {
    gc_heap.young_large_n--;
  }
  avant_nursery_held = avant_nursery_held >= bytes ? avant_nursery_held - bytes : 0;
  gc_heap.mature_since_full += bytes;
  if (h->type_id != AVANT_TYPE_BYTES) {
    gc_keep_add(payload_of(h));
  }
}

/* After a young collection's trace (Stage 28 W15a1, gc-design.md §5.3:
 * young large objects die young). Every large object is born young. An
 * unreached young large object is freed at once (or kept as the spare);
 * a reached one stays young at its first survival (SURVIVED marks it)
 * and is promoted at a later one once LARGE_AGE_BYTES of young
 * allocation passed since its birth (Stage 28 W15e, O98: the stamp in
 * the run's padding, gc_heap.young_clock), or at once when the collection tenures
 * every survivor (a fixed AVANT_YOUNG_MB) or the object is pinned.
 * Before, LOS_KEEP promoted every young large object a heap field
 * reached at its first survival, and a stack-held working set as large
 * as the young budget was promoted too. With no young large object the
 * walk is skipped (O47, O65): the live sample is the bytes the trace
 * marked (gc_heap.young_large_live). */
void gc_reclaim_young_large(void) {
  LargeIter it;
  AvantHeader *h;
  note_live_sample(gc_heap.young_large_live);
  if (gc_heap.young_large_n == 0) {
    return;
  }
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    if ((h->flags & AVANT_FLAG_YOUNG) == 0) {
      continue;
    }
    if ((h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_PINNED)) == 0) {
      gc_release_large(h);
    } else if (/* A stress point (a collection at every helper call) makes no age
                * decision by bytes: the clock hardly moves between them. */
               (h->flags & AVANT_FLAG_PINNED) || gc_heap.force_tenure ||
               ((h->flags & AVANT_FLAG_SURVIVED) &&
                (gc_heap.stress_now ||
                 gc_heap.young_clock - ((uint64_t *)h)[-1] >= (uint64_t)LARGE_AGE_BYTES))) {
      promote_young_large(h);
    } else {
      h->flags |= AVANT_FLAG_SURVIVED;
    }
  }
}

/* A full collection's sweep of the large objects: unmarked ones are
 * freed; survivors keep their generation (a young one stays young, with
 * its age), so young_large and its count are recounted here. */
void gc_sweep_large(void) {
  LargeIter it;
  AvantHeader *h;
  uint64_t live = 0;
  gc_heap.young_large = 0;
  gc_heap.young_large_n = 0;
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    size_t bytes = gc_large_bytes(h);
    if (!full_marked(h)) {
      gc_poison(payload_of(h), (size_t)h->size);
      free_run(h);
      gc_heap.large_bytes -= bytes;
      gc_heap.heap_bytes -= bytes;
      gc_heap.ctr.los.freed++;
      continue;
    }
    live += (uint64_t)bytes;
    if (h->flags & AVANT_FLAG_YOUNG) {
      gc_heap.young_large += (uint64_t)bytes;
      gc_heap.young_large_n++;
    }
  }
  note_live_sample(live);
}

void *gc_los_alloc(size_t payload, uint32_t type_id) {
  size_t bytes = (size_t)(run_pages(payload) << PAGE_SHIFT);
  /* Every large object is born young (gc-design.md §5.3, Stage 28
   * W15a1; before, only scalar ones were), except a mature-allocating
   * thread's and with the nursery off. */
  int young = !gc_heap.set.nursery_off && !gc_thread.alloc_mature;
  AvantHeader *h;
  if (gc_heap.los_spare && gc_heap.los_spare->size != (uint32_t)payload) {
    gc_los_spare_drop();
  }
  if (gc_heap.set.heap_max && !gc_heap.los_spare && gc_heap.heap_bytes + bytes > gc_heap.set.heap_max) {
    if (gc_thread.nogc) {
      /* C32: inside a no-GC region the allocation proceeds past the
       * limit and the collection runs after the region, unless the
       * region alone is over the limit (gc-design.md §5.4). */
      gc_region_limit_check(bytes);
      gc_limit_collect_pending();
    } else {
      gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
      if (gc_heap.heap_bytes + bytes > gc_heap.set.heap_max) {
        gc_die("heap limit");
      }
    }
  }
  gc_heap.ctr.los.allocs++;
  gc_heap.ctr.los.alloc_bytes += (uint64_t)bytes;
  {
    int bucket = payload < 16384 ? 0 : payload < 65536 ? 1 : payload < 262144 ? 2 : payload < 1048576 ? 3 : payload < 4194304 ? 4 : 5;
    gc_heap.ctr.los.hist[bucket]++;
    if (type_id == AVANT_TYPE_BYTES && align_up(sizeof(AvantHeader) + payload) <= LOS_LIMIT) {
      gc_heap.ctr.los.scalar_allocs++;
      gc_heap.ctr.los.scalar_bytes += (uint64_t)bytes;
    } else {
      gc_heap.ctr.los.big_allocs++;
      gc_heap.ctr.los.big_bytes += (uint64_t)bytes;
    }
    if (young) {
      gc_heap.ctr.los.young_allocs++;
      gc_heap.ctr.los.young_bytes += (uint64_t)bytes;
    } else {
      gc_heap.ctr.los.mature_allocs++;
      gc_heap.ctr.los.mature_bytes += (uint64_t)bytes;
    }
  }
  if (gc_heap.los_spare && gc_heap.los_spare->size == (uint32_t)payload) {
    h = gc_heap.los_spare;
    gc_heap.los_spare = NULL;
    memset(payload_of(h), 0, payload);
    gc_heap.ctr.los.spare_hits++;
  } else {
    h = take_run(payload);
    gc_heap.heap_bytes += bytes;
    gc_heap.ctr.los.callocs++;
  }
  h->size = (uint32_t)payload;
  h->type_id = (uint16_t)type_id;
  h->flags = (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_LARGE);
  start_bit_set(h, 1);
  /* A carded object starts with clear cards (a freed run's may be left
   * marked; Stage 28 W16t). */
  if (gc_carded(h) && payload) {
    gc_cards_clear(payload_of(h));
  }
  gc_heap.large_bytes += bytes;
  if (young) {
    h->flags |= AVANT_FLAG_YOUNG;
    /* Its birth on the young-allocation clock (O98), in the run's
     * padding before the header. */
    ((uint64_t *)h)[-1] = gc_heap.young_clock + (avant_nursery_held > gc_heap.young_carried
                                                     ? avant_nursery_held - gc_heap.young_carried
                                                     : 0);
    gc_heap.young_large += bytes;
    gc_heap.young_large_n++;
    /* The one young budget (gc-design.md §5.1). */
    avant_nursery_held += bytes;
  } else {
    gc_heap.mature_since_full += bytes;
  }
  gc_note_peak();
  return payload_of(h);
}

/* Shrink the large object H in place to NEW_SIZE payload bytes (no larger
 * than its size): the pages past the new run go back to its chunk, a huge
 * run's whole chunks past it are given back, and its charge falls by the
 * run bytes it no longer holds. Under AVANT_GC_STRESS the bytes past
 * NEW_SIZE are poisoned. */
static void trim_run(AvantHeader *h, size_t new_size) {
  uint64_t c = chunk_index(h);
  GcChunk *ch = &gc_heap.chunks[c];
  uint64_t n_old = run_pages((size_t)h->size);
  uint64_t n_new = run_pages(new_size);
  size_t gone = (size_t)((n_old - n_new) << PAGE_SHIFT);
  gc_poison((uint8_t *)payload_of(h) + new_size, (size_t)h->size - new_size);
  h->size = (uint32_t)new_size;
  if (n_new == n_old) {
    return;
  }
  if (ch->kind == CHUNK_LARGE) {
    uint32_t p = (uint32_t)((((uintptr_t)h - 8) >> PAGE_SHIFT) & (PAGES_PER_CHUNK - 1)) + (uint32_t)n_new;
    uint32_t n = (uint32_t)(n_old - n_new);
    uint32_t k;
    bits_set(ch->alloc, p, n, 0);
    ch->used = (uint16_t)(ch->used - n);
    for (k = 0; k < n; k++) {
      gc_heap.pages[c * PAGES_PER_CHUNK + p + k] = 0;
    }
    ch->idle = (uint32_t)gc_heap.return_clock;
  } else {
    uint64_t nch = ch->head;
    uint64_t keep = (n_new + PAGES_PER_CHUNK - 1) / PAGES_PER_CHUNK;
    uint64_t k;
    for (k = keep; k < nch; k++) {
      gc_chunk_give(c + k);
    }
    ch->head = (uint32_t)keep;
  }
  gc_heap.large_bytes -= gone;
  gc_heap.heap_bytes -= gone;
  if (h->flags & AVANT_FLAG_YOUNG) {
    gc_heap.young_large = gc_heap.young_large >= gone ? gc_heap.young_large - gone : 0;
    avant_nursery_held = avant_nursery_held >= gone ? avant_nursery_held - gone : 0;
  } else if (gc_heap.mature_since_full >= gone) {
    gc_heap.mature_since_full -= gone;
  }
  gc_heap.ctr.los.trims++;
  gc_heap.ctr.los.trim_bytes += (uint64_t)gone;
}

/* A runtime helper that owns the large Bytes object PAYLOAD (no other
 * reference to it is used again, and its bytes past NEW_SIZE are not
 * read) keeps only its first NEW_SIZE bytes (Stage 28 W18M, O52: a dead
 * Buf's storage becomes its string; O60: a parsed document's pool keeps
 * what the parse used). Returns 1 when PAYLOAD was trimmed, 0 when it is
 * not a large, unpinned Bytes object, or NEW_SIZE is larger than its size, or
 * NEW_SIZE is under SCALAR_LOS_MIN (an object that small is allocated in
 * a block, and a page run kept for it would waste most of a page;
 * nothing changes then). Takes the world lock; not a collecting call
 * (a large object never moves, so PAYLOAD stays valid if the lock wait
 * lets another thread collect). Hidden. */
__attribute__((visibility("hidden"))) int avant_gc_large_trim(void *payload, uint64_t new_size) {
  AvantHeader *h;
  int done = 0;
  if (!payload || new_size < SCALAR_LOS_MIN) {
    return 0;
  }
  gc_lock(&gc_heap.world);
  h = (AvantHeader *)payload - 1;
  if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_LARGE | AVANT_FLAG_PINNED)) == (AVANT_FLAG_HEAP | AVANT_FLAG_LARGE) &&
      h->type_id == AVANT_TYPE_BYTES && new_size <= (uint64_t)h->size && h != gc_heap.los_spare) {
    trim_run(h, (size_t)new_size);
    done = 1;
  }
  pthread_mutex_unlock(&gc_heap.world);
  return done;
}
