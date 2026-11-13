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

/* The object at START (header H, in block B, or a large object when B is
 * NULL), whose side mark the caller has just set: its lines through the
 * block the lookup already gave (until Stage 29 W-FIX1c a second lookup
 * found it again, O203), its bytes, and the work list. A full collection
 * promotes every object it reaches (Stage 28 W15b), so no object is young
 * after it (its remembered set and young budget are empty,
 * gc_sticky_full_end); the flags are written only when one is set, so a
 * collection of a heap that is already mature dirties no header. */
static inline void mark_object(void *start, AvantHeader *h, Block *b) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  if (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED)) {
    h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  }
  if (b) {
    mark_lines(b, (size_t)((uint8_t *)h - b->base), total);
  }
  gc_heap.live += total;
  if (gc_heap.work_n == gc_heap.work_cap) {
    gc_work_push(start);
  } else {
    gc_heap.work[gc_heap.work_n++] = start;
  }
}

/* Any reference (a root's, which may name the inside of an object; a
 * large object's; a static object's): one lookup, then the mark. */
static void mark(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start) {
    return;
  }
  h = avant_header(start);
  if (full_marked(h)) {
    return;
  }
  full_set_mark(h);
  mark_object(start, h, b);
}

/* A traced object's field, with the common case in one function (Stage
 * 29 W-FIX1c, O203): the exact start of an object in a small-object block
 * in use is looked up here, by gc_is_heap_payload's own tests in its
 * order (as gc_object_payload_fast), and marked with no further call and
 * the side mark set through the granule already in hand. Anything else
 * takes mark(). Before, a field cost eight calls (mark, gc_object_payload
 * with gc_is_heap_payload inside it, gc_mark_object_lines with
 * gc_is_heap_payload and gc_obj_total inside it, gc_obj_total,
 * gc_work_push; 276 instructions per object of a full collection, 149
 * now). It is kept
 * out of line on purpose: inlined into scan_payload's loop, the loop's
 * registers were spilled around it and the same collection cost 167. */
static __attribute__((noinline)) void mark_field(void *p) {
  uintptr_t off;
  uint64_t g;
  Block *b;
  if (!p) {
    return;
  }
  off = (uintptr_t)p - (uintptr_t)gc_heap.base - sizeof(AvantHeader);
  if (((uintptr_t)p & (ALIGN - 1)) || off >= gc_heap.committed) {
    mark(p);
    return;
  }
  g = off >> 3;
  if (!((gc_heap.starts[g >> 6] >> (g & 63)) & 1ULL) || gc_heap.chunks[off >> CHUNK_SHIFT].kind != CHUNK_SMALL) {
    mark(p);
    return;
  }
  b = &gc_heap.blocks[off >> BLOCK_SHIFT];
  if (!b->in_use) {
    mark(p);
    return;
  }
  if ((gc_heap.marks[g >> 6] >> (g & 63)) & 1ULL) {
    return;
  }
  gc_heap.marks[g >> 6] |= 1ULL << (g & 63);
  mark_object(p, avant_header(p), b);
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
  EACH_REF_SLOT(p, slot, { mark_field(*slot); });
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

