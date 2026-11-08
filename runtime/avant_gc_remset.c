#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The remembered set and the store barriers (Stage 28 W12, moved from
 * avant_rt.c): mature objects that may name young ones, the set's upkeep
 * after young and full collections, and the barrier entry points
 * avant_barrier and avant_barrier_at. (E's frontier policy, which kept a
 * remembered entry across young collections while its last scan found a
 * young field, Collector II Wave 2, went with E in Stage 28 W15d: S+
 * empties the set at every young collection and remembers again only the
 * holders of objects that stay young, gc_rem_keep_young_holders.) */

static void rem_add(void *obj) {
  AvantHeader *h = avant_header(obj);
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&gc_heap.rem_mu);
  }
  if (h->flags & AVANT_FLAG_REM) {
    if (locked) {
      pthread_mutex_unlock(&gc_heap.rem_mu);
    }
    return;
  }
  h->flags |= AVANT_FLAG_REM;
  if (gc_heap.rem_n == gc_heap.rem_cap) {
    gc_heap.rem_cap = gc_heap.rem_cap ? gc_heap.rem_cap * 2 : 64;
    gc_heap.remset = gc_xrealloc(gc_heap.remset, gc_heap.rem_cap * sizeof(void *));
  }
  gc_heap.remset[gc_heap.rem_n++] = obj;
  gc_heap.ctr.rem.adds++;
  if (locked) {
    pthread_mutex_unlock(&gc_heap.rem_mu);
  }
}

void gc_keep_add(void *obj) {
  if (gc_heap.keep_n == gc_heap.keep_cap) {
    gc_heap.keep_cap = gc_heap.keep_cap ? gc_heap.keep_cap * 2 : 64;
    gc_heap.keep = gc_xrealloc(gc_heap.keep, gc_heap.keep_cap * sizeof(void *));
  }
  gc_heap.keep[gc_heap.keep_n++] = obj;
}

/* Does a field of OBJ name a young object (gc_young_at: one with
 * YOUNG)? */
static int rem_names_young(void *obj) {
  int found = 0;
  if (avant_header(obj)->type_id == AVANT_TYPE_BYTES) {
    return 0;
  }
  EACH_REF_SLOT(obj, slot, {
    if (!found) {
      Block *b = NULL;
      AvantHeader *ol = NULL;
      void *start = gc_object_payload(*slot, &b, &ol);
      if (start) {
        found = gc_young_at(start, b, ol);
      }
    }
  });
  return found;
}

/* Candidate S (Stage 28 W15c, O88): after an S young collection emptied
 * the remembered set, the holders on the keep list (the trace's holders
 * of a young large object, and the young large objects promoted at this
 * collection, promote_young_large) are remembered when they are not young
 * and a field still names a young object: a young large object survives
 * its first young collection young. The keep list is emptied. */
void gc_rem_keep_young_holders(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.keep_n; i++) {
    void *obj = gc_heap.keep[i];
    if (!gc_is_heap_payload(obj, NULL, NULL) || (avant_header(obj)->flags & AVANT_FLAG_YOUNG) ||
        (avant_header(obj)->flags & AVANT_FLAG_REM)) {
      continue;
    }
    if (rem_names_young(obj)) {
      rem_add(obj);
    }
  }
  gc_heap.keep_n = 0;
}

/* C33 and O9 (Stage 28 W14b1, gc-design.md §7): the remembered set after
 * a full collection's mark (and its defragmentation, which rewrote the
 * entries it moved), before the sweep, O(set). An entry whose holder is
 * unmarked is dead and dropped: before, it stayed, and a later young
 * collection scanned the dead object's stale fields as roots, or, once
 * its address held a new object, scanned that unrelated object (start
 * bits cannot tell, gc-design.md §2.3). An entry whose header lacks REM
 * is such an address (a new object the barrier never remembered) or a
 * duplicate left by one, and is dropped too. A live holder that names
 * no young object leaves the set with REM cleared (the barrier re-adds
 * it at its next store); with the nursery off (AVANT_NURSERY=0) no young
 * collection reads the set, so every entry leaves it, and avant_barrier
 * remembers nothing. Kept entries are made unique with the header's
 * MARKED, which no full collection uses (C36), cleared before return. */
void gc_rem_full_scrub(void) {
  uint32_t i;
  uint32_t w = 0;
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&gc_heap.rem_mu);
  }
  for (i = 0; i < gc_heap.rem_n; i++) {
    void *obj = gc_heap.remset[i];
    AvantHeader *h;
    if (!gc_is_heap_payload(obj, NULL, NULL)) {
      continue;
    }
    h = avant_header(obj);
    if ((h->flags & AVANT_FLAG_REM) == 0 || (h->flags & AVANT_FLAG_MARKED)) {
      continue;
    }
    if (!full_marked(h) || gc_heap.set.nursery_off || !rem_names_young(obj)) {
      h->flags &= (uint16_t)~AVANT_FLAG_REM;
      continue;
    }
    h->flags |= AVANT_FLAG_MARKED;
    gc_heap.remset[w] = obj;
    w++;
  }
  gc_heap.rem_n = w;
  for (i = 0; i < gc_heap.rem_n; i++) {
    avant_header(gc_heap.remset[i])->flags &= (uint16_t)~AVANT_FLAG_MARKED;
  }
  if (locked) {
    pthread_mutex_unlock(&gc_heap.rem_mu);
  }
}

void avant_barrier(void *obj) {
  AvantHeader *h;
  /* O9: with the nursery off no young collection runs, so nothing is
   * remembered (the set grew without bound: churn.av kept 1.6 M
   * entries). */
  if (!obj || gc_heap.collecting || gc_heap.set.nursery_off) {
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
  if (!gc_is_heap_payload(obj, NULL, NULL)) {
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
  if (!addr || gc_heap.collecting) {
    return;
  }
  if (gc_thread.reg.stack_lo && (uint8_t *)addr >= (uint8_t *)gc_thread.reg.stack_lo &&
      (uint8_t *)addr < (uint8_t *)gc_thread.reg.stack_hi) {
    return;
  }
  start = gc_object_payload(addr, &b, NULL);
  if (!start) {
    return;
  }
  avant_barrier(start);
}
