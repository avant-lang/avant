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

/* OBJ joins the set with BIT (REM, or REMC for a carded object). */
static void rem_add_bit(void *obj, uint16_t bit) {
  AvantHeader *h = avant_header(obj);
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&gc_heap.rem_mu);
  }
  if (h->flags & bit) {
    if (locked) {
      pthread_mutex_unlock(&gc_heap.rem_mu);
    }
    return;
  }
  h->flags |= bit;
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

static void rem_add(void *obj) {
  rem_add_bit(obj, gc_carded(avant_header(obj)) ? AVANT_FLAG_REMC : AVANT_FLAG_REM);
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
static int slot_names_young(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  return start && gc_young_at(start, b, ol);
}

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
    /* A carded holder is remembered by its cards (gc_cards_settle). */
    if (!gc_is_heap_payload(obj, NULL, NULL) || (avant_header(obj)->flags & AVANT_FLAG_YOUNG) ||
        (avant_header(obj)->flags & AVANT_FLAG_REM) || gc_carded(avant_header(obj))) {
      continue;
    }
    if (rem_names_young(obj)) {
      rem_add(obj);
    }
  }
  gc_heap.keep_n = 0;
}

/* After a young collection (Stage 28 W16t): the cards of every carded
 * object it reached (the noted list) are made exact, and the object is
 * remembered (REMC) when a card is still marked. One scanned only by its
 * marked cards (CARD_SCAN) has those cards rechecked: a card stays marked
 * when one of its slots names an object still young (an aged copy, a
 * young large object; gc_young_at), else it is cleared. One scanned
 * whole (CARD_ALL, a root or a young object named it, or it was promoted
 * now) has every card cleared and then each slot naming a young object
 * marks its card. Run after the young large objects' fate and the set's
 * emptying (rem_empty), before the noted list is cleared. A young object
 * needs no card (it is traced whole while young). */
void gc_cards_settle(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.noted_n; i++) {
    void *p = gc_heap.noted[i];
    AvantHeader *h = avant_header(p);
    int any = 0;
    if ((h->flags & (AVANT_FLAG_FORWARDED | AVANT_FLAG_YOUNG)) || !gc_carded(h)) {
      continue;
    }
    if (h->flags & AVANT_FLAG_CARD_SCAN) {
      EACH_CARD_SLOT(p, slot, {
        if (slot == (void **)ecs_a_) {
          gc_heap.cards[ecs_c_] = 0;
        }
        if (!gc_heap.cards[ecs_c_] && slot_names_young(*slot)) {
          gc_heap.cards[ecs_c_] = 1;
          any = 1;
        }
      });
    } else {
      gc_cards_clear(p);
      EACH_REF_SLOT(p, slot, {
        if (slot_names_young(*slot)) {
          gc_heap.cards[gc_card_index(slot)] = 1;
          any = 1;
        }
      });
    }
    h->flags &= (uint16_t)~AVANT_FLAG_CARD_ALL;
    if (any) {
      rem_add_bit(p, AVANT_FLAG_REMC);
    }
  }
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
    if ((h->flags & (AVANT_FLAG_REM | AVANT_FLAG_REMC)) == 0 || (h->flags & AVANT_FLAG_MARKED)) {
      continue;
    }
    /* A carded entry (REMC) is kept whole: its cards say nothing about
     * what defragmentation moved, so the next young collection scans it
     * whole and marks its cards again (CARD_ALL; gc_cards_settle). */
    if (!full_marked(h) || gc_heap.set.nursery_off || !rem_names_young(obj)) {
      h->flags &= (uint16_t)~(AVANT_FLAG_REM | AVANT_FLAG_REMC);
      continue;
    }
    if (h->flags & AVANT_FLAG_REMC) {
      h->flags |= AVANT_FLAG_CARD_ALL;
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
      /* A carded object named without its slot (Stage 28 W16t): every
       * card counts as marked until the next young collection. */
      if (gc_carded(h) && gc_is_heap_payload(obj, NULL, NULL)) {
        h->flags |= AVANT_FLAG_CARD_ALL;
        rem_add_bit(obj, AVANT_FLAG_REMC);
        return;
      }
    }
  }
  if (!gc_is_heap_payload(obj, NULL, NULL)) {
    return;
  }
  rem_add(obj);
}

/* The barrier with the slot (Stage 28 W16t, card marking, Q5, O64): a
 * store of a reference into SLOT of OBJ. For a mature carded object (a
 * large pointer buffer) the slot's card is marked and the object is in
 * the set (REMC); a slot outside the object counts as every card. For
 * any other object it is avant_barrier(OBJ). Generated code calls it
 * for an element store whose holder the inline test did not skip;
 * runtime C calls it where it knows the slot. Never collects. */
void avant_barrier_slot(void *obj, void *slot) {
  AvantHeader *h;
  if (!obj || gc_heap.collecting || gc_heap.set.nursery_off) {
    return;
  }
  /* A large object's header is at a fixed place in its run, and the
   * range compare keeps the card index inside the table (no start-bit
   * lookup on this per-store path). */
  if (((uintptr_t)obj & (ALIGN - 1)) == 0 &&
      (uint64_t)((uintptr_t)obj - (uintptr_t)gc_heap.base) < gc_heap.committed) {
    h = avant_header(obj);
    if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG)) == AVANT_FLAG_HEAP && gc_carded(h)) {
      if ((uint8_t *)slot >= (uint8_t *)obj && (uint8_t *)slot < (uint8_t *)obj + h->size) {
        gc_heap.cards[gc_card_index(slot)] = 1;
      } else {
        h->flags |= AVANT_FLAG_CARD_ALL;
      }
      if (!(h->flags & AVANT_FLAG_REMC)) {
        rem_add_bit(obj, AVANT_FLAG_REMC);
      }
      return;
    }
  }
  avant_barrier(obj);
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
