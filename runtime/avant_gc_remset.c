#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The remembered set and the store barriers (Stage 28 W12, moved from
 * avant_rt.c): mature (and pinned in-place) objects that may name young
 * ones, the frontier policy that drops clean entries, the set's upkeep
 * after each young collection, and the barrier entry points avant_barrier
 * and avant_barrier_at. */

void **g_remset;
uint32_t g_rem_n;
static uint32_t g_rem_cap;
/* Workers call avant_barrier without the world lock (a fresh array
 * header). The remembered-set realloc is shared. */
pthread_mutex_t g_rem_mu = PTHREAD_MUTEX_INITIALIZER;
/* Mature objects that still point at a young survivor after this collection. */
static void **g_keep;
uint32_t g_keep_n;
static uint32_t g_keep_cap;

/* Frontier clearing (Collector II Wave 2): AVANT_GC_REM_FRONTIER=0
 * disables it. Dirty is tracked during the remembered scan itself, per
 * set index, so no second field walk is needed. Hysteresis (evict after
 * N clean scans) and probation (a second list scanned every K young
 * collections) were measured against immediate clearing on
 * bench/graph_frontier.av and lost (0.20s vs 0.19s at AVANT_YOUNG_MB=1,
 * 0.16s vs 0.15s adaptive), so only immediate clearing is kept. */
int g_rem_mode = 1;
uint8_t *g_rem_dirty;

static void rem_add(void *obj) {
  AvantHeader *h = avant_header(obj);
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  if (h->flags & AVANT_FLAG_REM) {
    if (locked) {
      pthread_mutex_unlock(&g_rem_mu);
    }
    return;
  }
  h->flags |= AVANT_FLAG_REM;
  if (g_rem_n == g_rem_cap) {
    g_rem_cap = g_rem_cap ? g_rem_cap * 2 : 64;
    g_remset = gc_xrealloc(g_remset, g_rem_cap * sizeof(void *));
    g_rem_dirty = gc_xrealloc(g_rem_dirty, g_rem_cap * sizeof(uint8_t));
  }
  g_rem_dirty[g_rem_n] = 0;
  g_remset[g_rem_n++] = obj;
  g_rem_adds++;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

void gc_keep_add(void *obj) {
  if (g_keep_n == g_keep_cap) {
    g_keep_cap = g_keep_cap ? g_keep_cap * 2 : 64;
    g_keep = gc_xrealloc(g_keep, g_keep_cap * sizeof(void *));
  }
  g_keep[g_keep_n++] = obj;
}

/* Frontier decision for one live, marked hot entry whose scan result
 * is dirty (a direct field names a young object). Returns 1 to keep the
 * entry. On 0 the caller drops it with REM already clear; the barrier
 * re-adds it on the next young store. Caller holds g_rem_mu when the
 * set is shared. */
static int rem_frontier_decide(void *obj, int dirty) {
  if (g_rem_mode == 0 || dirty) {
    return 1;
  }
  avant_header(obj)->flags &= (uint16_t)~AVANT_FLAG_REM;
  g_rem_cleared++;
  return 0;
}

void gc_rem_scrub(void) {
  uint32_t i;
  uint32_t w = 0;
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    AvantHeader *h;
    if (!is_heap_payload(obj, NULL, NULL)) {
      continue;
    }
    h = avant_header(obj);
    if (h->flags & AVANT_FLAG_MARKED) {
      int dirty = g_rem_dirty ? g_rem_dirty[i] : 1;
      if (!rem_frontier_decide(obj, dirty)) {
        continue;
      }
      g_remset[w] = obj;
      if (g_rem_dirty) {
        g_rem_dirty[w] = (uint8_t)dirty;
      }
      w++;
    } else if ((h->flags & AVANT_FLAG_PINNED) == 0) {
      Large *L = large_of(obj);
      /* A dead expression buffer is a large object. Leaving it on the
       * large list makes every missed pointer walk that list. */
      if (L) {
        gc_release_large(L);
      } else {
        h->flags &= (uint16_t)~AVANT_FLAG_REM;
      }
    } else {
      h->flags &= (uint16_t)~AVANT_FLAG_REM;
    }
  }
  g_rem_n = w;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

void gc_rem_commit_kept(void) {
  uint32_t n = g_keep_n;
  void **kept = g_keep;
  uint32_t i;
  uint32_t w = 0;
  int locked = rem_locked();
  g_keep = NULL;
  g_keep_n = 0;
  g_keep_cap = 0;
  /* A remembered object with a nursery child stays remembered: the
   * scan just proved that edge. A live member with no nursery child
   * leaves the hot set under the frontier policy (the barrier re-adds
   * it on the next young store); only members whose memory is gone
   * leave unconditionally, as before. */
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    if (is_heap_payload(obj, NULL, NULL)) {
      int dirty = g_rem_dirty ? g_rem_dirty[i] : 1;
      if (!rem_frontier_decide(obj, dirty)) {
        continue;
      }
      g_remset[w] = obj;
      if (g_rem_dirty) {
        g_rem_dirty[w] = (uint8_t)dirty;
      }
      w++;
    }
  }
  g_rem_n = w;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
  for (i = 0; i < n; i++) {
    if (is_heap_payload(kept[i], NULL, NULL)) {
      rem_add(kept[i]);
    }
  }
  free(kept);
}

void avant_barrier(void *obj) {
  Block *b = NULL;
  AvantHeader *h;
  if (!obj || g_collecting) {
    return;
  }
  /* The compiler passes the object being stored into. A nursery object
   * is scanned from the roots, so the hot path is the flag in the header
   * and not a block-table lookup. */
  if (((uintptr_t)obj & (ALIGN - 1)) == 0) {
    h = avant_header(obj);
    if (h->flags & AVANT_FLAG_HEAP) {
      /* Already scanned from the roots, or already in the remembered set. */
      if (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_REM)) {
        return;
      }
    }
  }
  if (!is_heap_payload(obj, &b, NULL)) {
    return;
  }
  if (b && (b->space == SPACE_NURSERY || b->space == SPACE_TO)) {
    return;
  }
  rem_add(obj);
}

/* Stage 28 W1b. The barrier for a store through an address that may be
 * inside a heap object (a struct method's self, a struct field reached
 * through such a self): remembers the object containing addr, like
 * avant_barrier(object). An address on this thread's stack, or outside
 * the heap, is a no-op. Never collects. */
void avant_barrier_at(void *addr) {
  Block *b = NULL;
  void *start;
  if (!addr || g_collecting) {
    return;
  }
  if (g_self_reg.stack_lo && (uint8_t *)addr >= (uint8_t *)g_self_reg.stack_lo &&
      (uint8_t *)addr < (uint8_t *)g_self_reg.stack_hi) {
    return;
  }
  start = gc_object_payload(addr, &b, NULL);
  if (!start) {
    return;
  }
  avant_barrier(start);
}
