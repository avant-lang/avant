#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The full collection's marking and sweeping (Stage 28 W12, moved from
 * avant_rt.c): non-moving mark from the roots with line marks, and the
 * block sweep that frees or recycles blocks by their live lines. The
 * defragmentation a full collection may run between the two is
 * avant_gc_defrag.c's. */

static void scan_payload(void *p);

static void mark(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  AvantHeader *h = avant_header(start);
  if (full_marked(h)) {
    return;
  }
  full_set_mark(h);
  /* A full collection promotes every object it reaches (Stage 28 W15b),
   * so no object is young after it (its remembered set and young budget
   * are empty, gc_sticky_full_end). */
  h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  gc_mark_object_lines(start);
  gc_heap.live += gc_obj_total(h);
  gc_work_push(start);
}

static void drain(void) {
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    scan_payload(p);
  }
}

static void mark_tls_slot(void **slot) {
  if (slot) {
    mark(*slot);
  }
}

static void scan_payload(void *p) {
  EACH_REF_SLOT(p, slot, { mark(*slot); });
}

void gc_mark_roots(void) {
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        mark(*f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    mark(p->payload);
  }
  EACH_TASK_ROOT(slot, mark(*slot));
  gc_each_runtime_root(mark_tls_slot);
  drain();
}

/* The sweep of one block after a full mark: a block with no live line
 * is released; a kept block has the start bits of every line it frees
 * cleared (32 granules a line: half a start word), so a hole's start
 * bits are clear before reuse (gc-design.md §2.3) and a dead object
 * whose header lies in a freed line is no longer a heap payload. A dead
 * object whose header lies in a live line keeps its bit (start bits are
 * not a liveness test). The inline allocation path still runs only in
 * fresh blocks (holes are W15b's). */
int gc_keep_if_live(Block *b) {
  uint64_t freed = 0;
  if (gc_block_live_lines(b) == 0) {
    return 0;
  }
  /* Word-wise (Stage 28 W15d, O94): the young sweep's pass over the whole
   * block clears the free lines' start bits and leaves the cursor on the
   * first hole, as the byte loop and gc_reset_block_bump did. */
  (void)gc_sweep_block_lines(b, 0, LINES_PER_BLOCK, &freed);
  return 1;
}

void gc_filter_blocks(Block **list, int (*keep)(Block *)) {
  Block *b = *list;
  Block *kept = NULL;
  while (b) {
    Block *next = b->next;
    if (keep && keep(b)) {
      b->next = kept;
      kept = b;
    } else {
      gc_cache_or_free_block(b);
    }
    b = next;
  }
  *list = kept;
}

