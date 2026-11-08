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
 * selects it. A young collection moves nothing. It traces from the roots
 * (frame slots, pins, runtime-held slots) and the remembered set, and
 * promotes each young object it reaches where it lies: YOUNG cleared,
 * its lines marked live at the current line epoch. A young object it
 * does not reach is garbage, reclaimed by the line sweep of the blocks
 * young allocation used, never visited. Mature objects are scanned one
 * level where R1' says (gc-design.md §1.3): when a root, a young
 * object's field or a remembered object's field names them; deeper ones
 * only when remembered. After the collection no object is young, so the
 * remembered set is emptied (§3.1.1 point 1).
 *
 * Nothing moves, so nothing is rewritten and nothing needs pinning: no
 * frame slot, C stack word or register is changed, and a young object a
 * C local names stays where it is. Liveness still needs every reference
 * to be found: generated code keeps its references in slots across
 * collecting calls (D165), and runtime C holds only its caller-rooted
 * arguments at the points a collection may run (outside defer regions;
 * gc-design.md §4.2 RC1 makes the rule general in W16). */
#if GC_CANDIDATE == GC_CANDIDATE_S

/* Promote one young block object in place: it is mature from now on. Its
 * lines are marked live at the current epoch. Its bytes are the survival
 * young_size reads (sticky.inplace); the mature growth it causes for the
 * full-collection trigger (gc-design.md §5.4: in-place promotions count)
 * is the lines it newly makes live, whole: what the heap loses to it until
 * a full collection, since nothing compacts it (a survivor alone in a line
 * costs the line; counted by its bytes, a heap of scattered survivors grew
 * without a full collection: heap/churn.av 9 MB under E, 53 MB). */
static void promote_in_place(AvantHeader *h, Block *b) {
  size_t total = gc_obj_total(h);
  size_t off = (size_t)((uint8_t *)h - b->base);
  size_t l;
  size_t end = (off + total - 1) / LINE_SIZE;
  uint64_t grown = 0;
  h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  for (l = off / LINE_SIZE; l <= end && l < LINES_PER_BLOCK; l++) {
    if (!line_live(b, (int)l)) {
      b->line_mark[l] = gc_heap.line_epoch;
      grown += LINE_SIZE;
    }
  }
  gc_heap.ctr.sticky.inplace += total;
  gc_heap.promoted_since_full += grown;
  gc_heap.mature_since_full += grown;
  gc_heap.live += total;
}

static void scan_fields(void *p, int from_mature);

/* One reference P (an object's start or an interior address).
 * FROM_MATURE: P is a field of a mature object that is not remembered
 * (R1': not followed unless remembered). Every object the collection
 * reaches is flagged MARKED ("traced this cycle") and noted, so the flag
 * ends with the collection (gc_clear_noted). A young object is promoted
 * at once and its fields are traced from the work list; a young large
 * object is marked and promoted after the trace
 * (gc_reclaim_young_large). */
static void trace_ref(void *p, int from_mature) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start) {
    return;
  }
  h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  if (h->flags & AVANT_FLAG_YOUNG) {
    h->flags |= AVANT_FLAG_MARKED;
    gc_note_marked(start);
    if (ol) {
      gc_heap.young_large_live += gc_large_bytes(ol);
    } else {
      promote_in_place(h, b);
    }
    if (h->type_id != AVANT_TYPE_BYTES) {
      gc_work_push(start);
    }
    return;
  }
  if (from_mature && (h->flags & AVANT_FLAG_REM) == 0) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  gc_note_marked(start);
  if (ol) {
    gc_heap.young_large_live += gc_large_bytes(ol);
  }
  scan_fields(start, 1);
}

static void scan_fields(void *p, int from_mature) {
  if (avant_header(p)->type_id == AVANT_TYPE_BYTES) {
    return;
  }
  EACH_REF_SLOT(p, slot, { trace_ref(*slot, from_mature); });
}

static void trace_root_slot(void **slot) {
  trace_ref(*slot, 0);
}

/* Field slots scan_fields visits for one object (the rem_fields
 * counter, Q5). */
static uint64_t field_count(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_BYTES) {
    return 0;
  }
  if (id == AVANT_TYPE_PTRS) {
    return (uint64_t)((size_t)h->size / sizeof(void *));
  }
  if (id >= MAX_TYPES || !gc_heap.type_set[id]) {
    return 0;
  }
  if (gc_heap.types[id].stride) {
    return (uint64_t)gc_heap.types[id].nrefs *
           (uint64_t)((size_t)h->size / sizeof(void *) / gc_heap.types[id].stride);
  }
  return (uint64_t)gc_heap.types[id].nrefs;
}

/* The trace: frame slots (the value, and the location when it lies in a
 * young object, which keeps that object), the remembered set (each entry
 * marked and its fields traced as R1'(c) says), pins, the runtime-held
 * slots, then the work list (the young objects' fields, R1'(b)). */
static void trace_all(void) {
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        void *loc = (void *)f->slots[i];
        Block *lb = NULL;
        AvantHeader *ll = NULL;
        void *ls = gc_object_payload(loc, &lb, &ll);
        if (ls && (avant_header(ls)->flags & AVANT_FLAG_YOUNG)) {
          trace_ref(ls, 0);
        }
        trace_ref(*f->slots[i], 0);
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  {
    int locked = rem_locked();
    if (locked) {
      pthread_mutex_lock(&gc_heap.rem_mu);
    }
    gc_heap.ctr.rem.scans++;
    gc_heap.ctr.rem.entries += gc_heap.rem_n;
    if (gc_heap.rem_n > gc_heap.ctr.rem.peak) {
      gc_heap.ctr.rem.peak = gc_heap.rem_n;
    }
    for (i = 0; i < gc_heap.rem_n; i++) {
      void *obj = gc_heap.remset[i];
      AvantHeader *ol = NULL;
      if (!gc_is_heap_payload(obj, NULL, &ol)) {
        continue;
      }
      if (!(avant_header(obj)->flags & AVANT_FLAG_MARKED)) {
        avant_header(obj)->flags |= AVANT_FLAG_MARKED;
        gc_note_marked(obj);
        if (ol) {
          gc_heap.young_large_live += gc_large_bytes(ol);
        }
      }
      gc_heap.ctr.rem.fields += field_count(obj);
      scan_fields(obj, 0);
    }
    if (locked) {
      pthread_mutex_unlock(&gc_heap.rem_mu);
    }
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    trace_ref(p->payload, 0);
  }
  gc_each_runtime_root(trace_root_slot);
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    scan_fields(p, 0);
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

/* The young lines LO..HI of block B after the trace: a line no survivor
 * marked at the current epoch is free, and its start bits are cleared
 * (32 granules, half a start word; gc-design.md §2.3, §3.1.1 point 2), so
 * its dead objects are no longer heap payloads and a later hole there
 * has clear start bits for the inline allocation path. A dead object
 * whose header lies in a live line keeps its start bit: nothing infers
 * liveness from start bits. */
static void sweep_lines(Block *b, int lo, int hi) {
  int i;
  for (i = lo; i < hi; i++) {
    if (!line_live(b, i)) {
      b->start[i >> 1] &= ~(0xffffffffULL << (32 * (i & 1)));
      gc_heap.ctr.sticky.freed_lines++;
    }
  }
  gc_heap.ctr.sticky.blocks++;
}

/* A young collection under S (gc_young_collect's S arm): the trace, the
 * young large objects, the remembered set emptied, then the sweep of the
 * young blocks. */
void gc_sticky_collect(void) {
  Block *b;
  gc_heap.young_large_live = 0;
  gc_heap.keep_n = 0;
  gc_heap.work_n = 0;
  trace_all();
  if (gc_heap.set.verify) {
    gc_verify_sticky();
  }
  /* The young large objects: the marked ones are promoted at once (S has
   * no aging step: force_tenure), the others freed. The entries
   * promote_young_large adds for the remembered set are dropped: nothing
   * is young after this collection. */
  gc_heap.force_tenure = 1;
  gc_reclaim_young_large();
  gc_heap.force_tenure = 0;
  gc_heap.keep_n = 0;
  rem_empty();
  /* The young blocks (the young-blocks list, gc_heap.young_blocks): only
   * the line range young allocation used in each is swept. A block left
   * wholly free goes to free_blocks, one with free lines to part_blocks:
   * the young allocator's next holes. Every block stays in mature space
   * (an empty one until a full collection releases it). */
  {
    uint32_t i;
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      int live;
      b = gc_heap.young_blocks[i];
      sweep_lines(b, b->young_lo, b->young_hi);
      b->young_lo = 0;
      b->young_hi = 0;
      live = gc_block_live_lines(b);
      gc_reset_block_bump(b);
      if (live == 0) {
        gc_sticky_push(&gc_heap.free_blocks, &gc_heap.free_blocks_n, &gc_heap.free_blocks_cap, b);
      } else if (live < LINES_PER_BLOCK) {
        gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
      }
    }
    gc_heap.young_blocks_n = 0;
  }
  gc_clear_alloc_cur(SPACE_NURSERY);
  gc_heap.overflow = NULL;
  gc_heap.cur_whole = 0;
  gc_clear_noted();
  avant_nursery_held = gc_heap.young_large;
}

/* A full collection under S promotes every object it marks (the full
 * mark clears YOUNG): after it no object is young. Before its mark the
 * young blocks become ordinary mature blocks (the full mark and sweep
 * see every line, and defragmentation may evacuate them) and the young
 * allocator's lists are dropped; after its sweep, which released every
 * empty block, part_blocks is rebuilt from the blocks with free lines and
 * the young budget is empty. */
void gc_sticky_full_begin(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    gc_heap.young_blocks[i]->young_lo = 0;
    gc_heap.young_blocks[i]->young_hi = 0;
  }
  gc_heap.young_blocks_n = 0;
  gc_heap.free_blocks_n = 0;
  gc_heap.part_blocks_n = 0;
  gc_heap.overflow = NULL;
  gc_heap.cur_whole = 0;
}

void gc_sticky_full_end(void) {
  Block *b;
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    if (gc_block_live_lines(b) < LINES_PER_BLOCK) {
      gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
    }
  }
  avant_nursery_held = 0;
}

#endif /* GC_CANDIDATE == GC_CANDIDATE_S */
