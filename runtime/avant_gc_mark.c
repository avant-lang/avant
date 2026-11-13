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

/* A traced object's field (Stage 29 W-FIX1c, O203; the Valk stage,
 * VALK-T, O585). By the field rule (avant_gc_internal.h: a field holds
 * null, an exact start or a static address) its word is classified by one
 * compare, the header's offset against the committed size, which null and
 * every static address fail; what passes is the start of a live object,
 * marked through the granule already in hand, with no start-bit lookup,
 * no chunk kind and no block-in-use test (until VALK-T those three were
 * asked of every field, gc_is_heap_payload's tests in line, and an
 * address that failed one took mark(); before W-FIX1c a field cost eight
 * calls). A large object takes the same path with no block (the header's
 * LARGE flag): it has no line marks. Roots, pins and runtime-held slots
 * take mark(), which resolves an address inside an object. It is kept
 * out of line on purpose: inlined into scan_payload's loop, the loop's
 * registers were spilled around it (W-FIX1c: 167 instructions per object
 * of a full collection against 149). */
static __attribute__((noinline)) void mark_field(void *p) {
  uintptr_t off = (uintptr_t)p - (uintptr_t)gc_heap.base - sizeof(AvantHeader);
  uint64_t g;
  AvantHeader *h;
  if (off >= gc_heap.committed) {
    return;
  }
  g = off >> 3;
  if ((gc_heap.marks[g >> 6] >> (g & 63)) & 1ULL) {
    return;
  }
  gc_heap.marks[g >> 6] |= 1ULL << (g & 63);
  h = (AvantHeader *)((uint8_t *)p - sizeof(AvantHeader));
  mark_object(p, h, (h->flags & AVANT_FLAG_LARGE) ? NULL : &gc_heap.blocks[off >> BLOCK_SHIFT]);
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

