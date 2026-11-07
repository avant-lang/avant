#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The large-object space (Stage 28 W12, moved from avant_rt.c): objects
 * too large for a block (or medium scalar buffers) live in their own
 * calloc chunk on g_large, are never moved, and are found by address
 * through a hash of payloads (large_of) or the address range of every
 * chunk (gc_object_payload). Young large objects, the spare buffer, their
 * reclaim at young collections and their sweep at full ones. */

Large *g_large;
Large *g_large_ht[LARGE_HASH];
/* The address range [g_large_lo, g_large_hi) covers every large chunk
 * ever put on g_large (gc_los_alloc). It only grows: a chunk that
 * is freed leaves it as wide as it was. So an address outside it is
 * inside no object on g_large, and gc_object_payload skips its walk of the
 * list, which for such an address could only miss. That walk ran for
 * every young-copy slot naming a static object in the binary (a string
 * literal), and dominated compiling the compiler (Stage 28 W11d). */
uint8_t *g_large_lo = (uint8_t *)UINTPTR_MAX;
uint8_t *g_large_hi;
/* One dead scalar large buffer, kept for the next same-sized alloc. */
Large *g_los_spare;
/* Bytes in young scalar large objects. They do not move. A young
 * collection frees the ones the mark does not reach. */
uint64_t g_young_large;
int g_young_large_n;
/* Set when a young large object survived a collection. Further
 * same-sized allocs are live rows, not a replaced buffer, so they
 * must not collect on every malloc. */
int g_los_eager_hold;

static void large_hash_add(Large *L) {
  size_t k = large_key(payload_of(&L->header));
  L->hash_next = g_large_ht[k];
  g_large_ht[k] = L;
}

static void large_hash_del(Large *L) {
  size_t k = large_key(payload_of(&L->header));
  Large **pp = &g_large_ht[k];
  while (*pp) {
    if (*pp == L) {
      *pp = L->hash_next;
      L->hash_next = NULL;
      return;
    }
    pp = &(*pp)->hash_next;
  }
}

size_t gc_large_chunk(const Large *L) {
  return align_up(sizeof(Large) + (size_t)L->header.size);
}

void gc_los_spare_drop(void) {
  size_t chunk;
  if (!g_los_spare) {
    return;
  }
  chunk = gc_large_chunk(g_los_spare);
  free(g_los_spare);
  g_los_spare = NULL;
  g_heap_bytes -= chunk;
  g_los_spare_dropped++;
}

static void unlink_large(Large *L) {
  Large **pp = &g_large;
  large_hash_del(L);
  while (*pp) {
    if (*pp == L) {
      *pp = L->next;
      break;
    }
    pp = &(*pp)->next;
  }
  L->next = NULL;
}

/* keep_bytes leaves the allocation charged, for the spare slot. */
static void account_large_gone(Large *L, int keep_bytes) {
  size_t chunk = gc_large_chunk(L);
  if (L->header.flags & AVANT_FLAG_YOUNG) {
    if (g_young_large >= chunk) {
      g_young_large -= chunk;
    }
    if (g_young_large_n > 0) {
      g_young_large_n--;
    }
  } else if (g_mature_since_full >= chunk) {
    g_mature_since_full -= chunk;
  }
  if (!keep_bytes) {
    g_heap_bytes -= chunk;
  }
}

void gc_release_large(Large *L) {
  int cache = (L->header.flags & AVANT_FLAG_YOUNG) &&
              L->header.type_id == AVANT_TYPE_BYTES &&
              (L->header.flags & AVANT_FLAG_PINNED) == 0 &&
              g_los_spare == NULL;
  unlink_large(L);
  account_large_gone(L, cache);
  if (cache) {
    L->hash_next = NULL;
    g_los_spare = L;
    g_los_spare_cached++;
  } else {
    free(L);
    g_los_freed++;
  }
}

static void los_note_live(void) {
  Large *L;
  uint64_t live = 0;
  for (L = g_large; L; L = L->next) {
    if (L->header.flags & AVANT_FLAG_MARKED) {
      live += (uint64_t)gc_large_chunk(L);
    }
  }
  g_los_live_samples++;
  g_los_live_sum += live;
  if (live > g_los_live_peak) {
    g_los_live_peak = live;
  }
}

static void promote_young_large(Large *L) {
  size_t chunk = gc_large_chunk(L);
  g_los_promoted++;
  g_los_promoted_bytes += (uint64_t)chunk;
  L->header.flags = (uint16_t)(L->header.flags &
                                (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_LOS_KEEP));
  if (g_young_large >= chunk) {
    g_young_large -= chunk;
  }
  if (g_young_large_n > 0) {
    g_young_large_n--;
  }
  g_mature_since_full += chunk;
}

/* Unmarked young large objects are unreachable and are freed. A marked
 * one is promoted when a heap object points at it, or when the stack
 * is holding a working set at least as big as the young limit. A
 * stack-only buffer stays young, so the next collection can free it
 * once that local is gone. */
void gc_reclaim_young_large(void) {
  Large *L;
  size_t stay_bytes = 0;
  los_note_live();
  for (L = g_large; L; L = L->next) {
    if ((L->header.flags & AVANT_FLAG_YOUNG) == 0) {
      continue;
    }
    if ((L->header.flags & AVANT_FLAG_MARKED) == 0) {
      continue;
    }
    if (L->header.flags & (AVANT_FLAG_PINNED | AVANT_FLAG_LOS_KEEP)) {
      continue;
    }
    stay_bytes += gc_large_chunk(L);
  }
  int promote_stay = stay_bytes >= g_young_bytes;
  L = g_large;
  while (L) {
    Large *next = L->next;
    if (L->header.flags & AVANT_FLAG_YOUNG) {
      int marked = (L->header.flags & AVANT_FLAG_MARKED) != 0;
      int pinned = (L->header.flags & AVANT_FLAG_PINNED) != 0;
      int keep = (L->header.flags & AVANT_FLAG_LOS_KEEP) != 0;
      if (!marked && !pinned) {
        gc_release_large(L);
      } else if (pinned || keep || promote_stay) {
        promote_young_large(L);
      } else {
        L->header.flags = (uint16_t)(L->header.flags & (uint16_t)~AVANT_FLAG_LOS_KEEP);
      }
    }
    L = next;
  }
}

void gc_sweep_large(void) {
  Large **pp = &g_large;
  los_note_live();
  while (*pp) {
    Large *L = *pp;
    if (L->header.flags & AVANT_FLAG_MARKED) {
      pp = &L->next;
    } else {
      size_t chunk = align_up(sizeof(Large) + (size_t)L->header.size);
      large_hash_del(L);
      *pp = L->next;
      free(L);
      g_heap_bytes -= chunk;
      g_los_freed++;
    }
  }
}

void *gc_los_alloc(size_t payload, uint32_t type_id) {
  size_t chunk = align_up(sizeof(Large) + payload);
  int young = !g_nursery_off && !g_alloc_mature && type_id == AVANT_TYPE_BYTES;
  Large *L;
  if (g_los_spare && g_los_spare->header.size != (uint32_t)payload) {
    gc_los_spare_drop();
  }
  if (g_heap_max && !g_los_spare && g_heap_bytes + chunk > g_heap_max) {
    if (!g_gc_defer) {
      gc_collect(1);
    }
    if (g_heap_max && g_heap_bytes + chunk > g_heap_max) {
      gc_die("heap limit");
    }
  }
  g_los_allocs++;
  g_los_alloc_bytes += (uint64_t)chunk;
  {
    int bucket = payload < 16384 ? 0 : payload < 65536 ? 1 : payload < 262144 ? 2 : payload < 1048576 ? 3 : payload < 4194304 ? 4 : 5;
    g_los_hist[bucket]++;
    if (type_id == AVANT_TYPE_BYTES && align_up(sizeof(AvantHeader) + payload) <= LOS_LIMIT) {
      g_los_scalar_allocs++;
      g_los_scalar_bytes += (uint64_t)chunk;
    } else {
      g_los_big_allocs++;
      g_los_big_bytes += (uint64_t)chunk;
    }
    if (young) {
      g_los_young_allocs++;
      g_los_young_bytes += (uint64_t)chunk;
    } else {
      g_los_mature_allocs++;
      g_los_mature_bytes += (uint64_t)chunk;
    }
  }
  if (g_los_spare && g_los_spare->header.size == (uint32_t)payload) {
    L = g_los_spare;
    g_los_spare = NULL;
    memset(payload_of(&L->header), 0, payload);
    g_los_spare_hits++;
  } else {
    L = calloc(1, chunk);
    if (!L) {
      gc_die("out of memory");
    }
    g_heap_bytes += chunk;
    g_los_callocs++;
  }
  L->header.size = (uint32_t)payload;
  L->header.type_id = (uint16_t)type_id;
  L->header.flags = (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_LARGE);
  if (young) {
    L->header.flags |= AVANT_FLAG_YOUNG;
    g_young_large += chunk;
    g_young_large_n++;
  } else {
    g_mature_since_full += chunk;
  }
  L->next = g_large;
  g_large = L;
  /* The chunk holds the header and payload (chunk is at least
   * sizeof(Large) + payload, aligned; a reused spare has this size). */
  if ((uint8_t *)L < g_large_lo) {
    g_large_lo = (uint8_t *)L;
  }
  if ((uint8_t *)L + chunk > g_large_hi) {
    g_large_hi = (uint8_t *)L + chunk;
  }
  large_hash_add(L);
  gc_note_peak();
  return payload_of(&L->header);
}
