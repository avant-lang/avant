#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Candidate S, the in-place young generation (Stage 28 W15b,
 * gc-design.md §3.1, StickyImmix): compiled only when GC_CANDIDATE
 * selects it. A young collection moves nothing: every young object it
 * reaches is promoted where it lies (YOUNG cleared, its lines marked
 * with the current epoch), and every young object it does not reach is
 * garbage. */
#if GC_CANDIDATE == GC_CANDIDATE_S

/* Promote one young block object in place: it is mature from now on. Its
 * lines are marked live at the current epoch, and its bytes are mature
 * growth for the full-collection trigger (gc-design.md §5.4: in-place
 * promotions count) and the survival young_size reads (sticky.inplace). */
static void promote_in_place(AvantHeader *h, Block *b) {
  size_t total = gc_obj_total(h);
  h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  mark_lines(b, (size_t)((uint8_t *)h - b->base), total);
  gc_heap.ctr.sticky.inplace += total;
  gc_heap.promoted_since_full += total;
  gc_heap.mature_since_full += total;
  gc_heap.live += total;
}

static void promote_if_young(AvantHeader *h, Block *b) {
  if (h->flags & AVANT_FLAG_YOUNG) {
    promote_in_place(h, b);
  }
}

/* Every remembered entry leaves the set, REM cleared (gc-design.md
 * §3.1.1 point 1): after an S young collection no object is young, so no
 * entry can name one. Run before any line is freed, while every entry's
 * header is intact (no allocation happens during the collection, and
 * the set holds only objects remembered since the last collection). */
static void rem_empty(void) {
  uint32_t i;
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&gc_heap.rem_mu);
  }
  for (i = 0; i < gc_heap.rem_n; i++) {
    void *obj = gc_heap.remset[i];
    if (gc_is_heap_payload(obj, NULL, NULL)) {
      avant_header(obj)->flags &= (uint16_t)~AVANT_FLAG_REM;
    }
  }
  gc_heap.ctr.rem.cleared += gc_heap.rem_n;
  gc_heap.rem_n = 0;
  if (locked) {
    pthread_mutex_unlock(&gc_heap.rem_mu);
  }
}

/* The young large objects: every one is marked (it survives) and
 * gc_reclaim_young_large promotes the marked ones at once
 * (force_tenure: S has no aging step). The promoted ones' entries for the
 * remembered set (gc_keep_add) are dropped: nothing is young after the
 * collection. */
static void promote_young_large_all(void) {
  LargeIter it;
  AvantHeader *h;
  if (gc_heap.young_large_n) {
    gc_large_iter_init(&it);
    while ((h = gc_large_next(&it))) {
      if ((h->flags & AVANT_FLAG_YOUNG) && !(h->flags & AVANT_FLAG_MARKED)) {
        h->flags |= AVANT_FLAG_MARKED;
        gc_note_marked(payload_of(h));
      }
    }
  }
  gc_heap.force_tenure = 1;
  gc_reclaim_young_large();
  gc_heap.force_tenure = 0;
  gc_heap.keep_n = 0;
}

/* The nursery blocks join mature space with every object promoted in
 * place. (Item 0 of W15b: every young object survives its first young
 * collection; the trace comes next.) */
void gc_sticky_collect(void) {
  Block *b = gc_heap.space[SPACE_NURSERY].list;
  gc_heap.young_large_live = 0;
  gc_heap.keep_n = 0;
  gc_heap.space[SPACE_NURSERY].list = NULL;
  gc_clear_alloc_cur(SPACE_NURSERY);
  while (b) {
    Block *next = b->next;
    gc_foreach_block_object(b, promote_if_young);
    b->space = SPACE_MATURE;
    gc_heap.space[SPACE_NURSERY].bytes -= BLOCK_SIZE;
    gc_heap.space[SPACE_MATURE].bytes += BLOCK_SIZE;
    b->next = gc_heap.space[SPACE_MATURE].list;
    gc_heap.space[SPACE_MATURE].list = b;
    gc_reset_block_bump(b);
    b = next;
  }
  promote_young_large_all();
  rem_empty();
  gc_clear_noted();
  avant_nursery_held = gc_heap.young_large;
}

/* A full collection under S promotes every object it marks (gc_mark's
 * mark clears YOUNG): after it no object is young. Before its mark,
 * nothing to do yet; after its sweep the surviving nursery blocks join
 * mature space and the young budget is empty. */
void gc_sticky_full_begin(void) {
}

void gc_sticky_full_end(void) {
  Block *b = gc_heap.space[SPACE_NURSERY].list;
  gc_heap.space[SPACE_NURSERY].list = NULL;
  while (b) {
    Block *next = b->next;
    b->space = SPACE_MATURE;
    gc_heap.space[SPACE_NURSERY].bytes -= BLOCK_SIZE;
    gc_heap.space[SPACE_MATURE].bytes += BLOCK_SIZE;
    b->next = gc_heap.space[SPACE_MATURE].list;
    gc_heap.space[SPACE_MATURE].list = b;
    b = next;
  }
  avant_nursery_held = 0;
}

#endif /* GC_CANDIDATE == GC_CANDIDATE_S */
