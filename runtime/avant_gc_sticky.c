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
 * object is marked and decided after the trace (gc_reclaim_young_large:
 * at its first survival it stays young, Stage 28 W15c). Returns 1 when P
 * names a young large object, whose holder may then have to stay
 * remembered (scan_fields). */
static int trace_ref(void *p, int from_mature) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start) {
    return 0;
  }
  h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return ol && (h->flags & AVANT_FLAG_YOUNG);
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
    return ol != NULL;
  }
  if (from_mature && (h->flags & AVANT_FLAG_REM) == 0) {
    return 0;
  }
  h->flags |= AVANT_FLAG_MARKED;
  gc_note_marked(start);
  if (ol) {
    gc_heap.young_large_live += gc_large_bytes(ol);
  }
  scan_fields(start, 1);
  return 0;
}

/* The fields of P. A holder with a field naming a young large object
 * joins the keep list (gc_keep_add) unless it is itself a young large
 * object (young, so traced whenever it is reached): after the trace,
 * every such holder that is not young and still names a young object is
 * remembered (rem_rebuild), because the young large object may stay
 * young across this collection (Stage 28 W15c, O88). */
static void scan_fields(void *p, int from_mature) {
  AvantHeader *ph = avant_header(p);
  int names_young_large = 0;
  if (ph->type_id == AVANT_TYPE_BYTES) {
    return;
  }
  EACH_REF_SLOT(p, slot, { names_young_large |= trace_ref(*slot, from_mature); });
  if (names_young_large &&
      (ph->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) != (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) {
    gc_keep_add(p);
  }
}

static void trace_root_slot(void **slot) {
  (void)trace_ref(*slot, 0);
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
 * §3.1.1 point 1): after an S young collection no block object is young.
 * Run before any line is freed, while every entry's header is intact (no
 * allocation happens during the collection, and the set holds only
 * objects remembered since the last collection). rem_rebuild then
 * remembers again the holders that name a young large object staying
 * young (§3.1.1 point 1 as amended by Stage 28 W15c: the one case where
 * an object is still young after an S young collection). */
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

/* The holders the trace found naming a young large object (the keep
 * list, scan_fields): each one that is not young and still names a young
 * object after gc_reclaim_young_large is remembered, so the next young
 * collection traces the young large object through it. A holder this
 * collection reached only through a root (R1'(a)) is remembered too: the
 * root may be gone by the next collection. */
static void rem_rebuild(void) {
  gc_rem_keep_young_holders();
}

/* The young lines LO..HI of block B after the trace: a line no survivor
 * marked at the current epoch is free, and its start bits are cleared
 * (32 granules, half a start word; gc-design.md §2.3, §3.1.1 point 2), so
 * its dead objects are no longer heap payloads and a later hole there
 * has clear start bits for the inline allocation path. A dead object
 * whose header lies in a live line keeps its start bit: nothing infers
 * liveness from start bits. */
/* Word-wise (Stage 28 W15c, O87): the block's 128 line marks are read 8
 * at a time and compared with the epoch replicated into every byte; one
 * pass frees the young range's dead lines (their start bits cleared, two
 * lines per start word), counts the block's live lines and finds its
 * first hole, which the byte-wise sweep, gc_block_live_lines and
 * gc_reset_block_bump's find_hole did in three passes. Returns the live
 * line count and leaves the block's bump and limit on its first hole (as
 * gc_reset_block_bump does: search 0; a full block's cursor at its end). */
#define LINE_WORDS (LINES_PER_BLOCK / 8)
static inline uint64_t live_bytes_mask(uint64_t marks, uint64_t epoch8) {
  /* The high bit of every byte of MARKS equal to the epoch (exact: no
   * borrow crosses a byte). */
  uint64_t x = marks ^ epoch8;
  uint64_t t = (x & 0x7f7f7f7f7f7f7f7fULL) + 0x7f7f7f7f7f7f7f7fULL;
  return ~(t | x | 0x7f7f7f7f7f7f7f7fULL);
}

static int sweep_lines(Block *b, int lo, int hi) {
  uint64_t epoch8 = 0x0101010101010101ULL * (uint64_t)gc_heap.line_epoch;
  uint64_t live_bits[LINE_WORDS];
  uint64_t range_lo = (uint64_t)lo;
  uint64_t range_hi = (uint64_t)hi;
  int live = 0;
  int k;
  int first_free = -1;
  int hole_end = LINES_PER_BLOCK;
  for (k = 0; k < LINE_WORDS; k++) {
    uint64_t m;
    memcpy(&m, b->line_mark + 8 * k, sizeof m);
    live_bits[k] = live_bytes_mask(m, epoch8);
    /* The count of set high bits (no popcount instruction is assumed:
     * without -mpopcnt __builtin_popcountll is a library call). */
    live += (int)(((live_bits[k] >> 7) * 0x0101010101010101ULL) >> 56);
  }
  for (k = 0; k < LINE_WORDS; k++) {
    uint64_t freeb = ~live_bits[k] & 0x8080808080808080ULL;
    int base = 8 * k;
    if (first_free < 0 && freeb) {
      first_free = base + (__builtin_ctzll(freeb) >> 3);
    }
    /* The young range's free lines in this group: clear their start bits
     * (lines 2j and 2j + 1 share start word j: the low and high half). */
    if (freeb == 0x8080808080808080ULL && (uint64_t)base >= range_lo && (uint64_t)base + 8 <= range_hi) {
      /* The whole group is free and young: four start words. */
      b->start[base >> 1] = 0;
      b->start[(base >> 1) + 1] = 0;
      b->start[(base >> 1) + 2] = 0;
      b->start[(base >> 1) + 3] = 0;
      gc_heap.ctr.sticky.freed_lines += 8;
    } else if ((uint64_t)base + 8 > range_lo && (uint64_t)base < range_hi && freeb) {
      int j;
      for (j = 0; j < 4; j++) {
        uint64_t pair = (freeb >> (16 * j)) & 0x8080ULL;
        int l0 = base + 2 * j;
        uint64_t clear = 0;
        if ((pair & 0x80ULL) && (uint64_t)l0 >= range_lo && (uint64_t)l0 < range_hi) {
          clear |= 0xffffffffULL;
          gc_heap.ctr.sticky.freed_lines++;
        }
        if ((pair & 0x8000ULL) && (uint64_t)(l0 + 1) >= range_lo && (uint64_t)(l0 + 1) < range_hi) {
          clear |= 0xffffffff00000000ULL;
          gc_heap.ctr.sticky.freed_lines++;
        }
        b->start[l0 >> 1] &= ~clear;
      }
    }
  }
  if (first_free >= 0) {
    /* The hole ends at the first live line after it. */
    int l = first_free + 1;
    while (l < LINES_PER_BLOCK) {
      int kk = l >> 3;
      uint64_t lb = live_bits[kk] >> (8 * (l & 7));
      if (lb) {
        hole_end = l + (__builtin_ctzll(lb) >> 3);
        break;
      }
      l = (kk + 1) * 8;
    }
    b->bump = b->base + (size_t)first_free * LINE_SIZE;
    b->limit = b->base + (size_t)hole_end * LINE_SIZE;
  } else {
    b->bump = b->base + BLOCK_SIZE;
    b->limit = b->base + BLOCK_SIZE;
  }
  b->search = first_free >= 0 ? 0 : LINES_PER_BLOCK;
  gc_heap.ctr.sticky.blocks++;
  return live;
}

/* A young collection under S (gc_young_collect's S arm): the trace, the
 * young large objects, the remembered set emptied, then the sweep of the
 * young blocks. */
/* Block.kept under S (E's field, unused by S otherwise): the young sweep
 * left the block empty, and it leaves mature space (keep_unless_empty). */
#define YOUNG_EMPTY 4
static int keep_unless_empty(Block *b) {
  if (b->kept == YOUNG_EMPTY) {
    b->kept = 0;
    return 0;
  }
  return 1;
}

void gc_sticky_collect(int tenure) {
  Block *b;
  uint32_t empty = 0;
  gc_heap.young_large_live = 0;
  gc_heap.keep_n = 0;
  gc_heap.work_n = 0;
  trace_all();
  if (gc_heap.set.verify) {
    gc_verify_sticky();
  }
  /* The young large objects (Stage 28 W15c, O88/O76/O85): an unreached
   * one is freed; a reached one stays young at its first survival
   * (SURVIVED) and is promoted at its second, as under E (YOUNG_AGE's
   * step for large objects), so a buffer that dies soon dies young; a
   * fixed AVANT_YOUNG_MB tenures at once (TENURE), as under E. Before,
   * S promoted every reached young large object at once and it died
   * mature (the Sort rows +45-52% memory against E). */
  gc_heap.force_tenure = tenure;
  gc_reclaim_young_large();
  gc_heap.force_tenure = 0;
  rem_empty();
  rem_rebuild();
  /* The young blocks (the young-blocks list, gc_heap.young_blocks): only
   * the line range young allocation used in each is swept. A block left
   * wholly free leaves mature space below (O89); one with free lines goes
   * to part_blocks, the young allocator's next holes. */
  {
    uint32_t i;
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      int live;
      b = gc_heap.young_blocks[i];
      live = sweep_lines(b, b->young_lo, b->young_hi);
      b->young_lo = 0;
      b->young_hi = 0;
      if (live == 0) {
        b->kept = YOUNG_EMPTY;
        empty++;
      } else if (live < LINES_PER_BLOCK) {
        gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
      }
    }
    gc_heap.young_blocks_n = 0;
  }
  gc_clear_alloc_cur(SPACE_NURSERY);
  gc_heap.overflow = NULL;
  gc_heap.cur_whole = 0;
  /* The empty young blocks go back to their chunks' free lists (O89):
   * before, they stayed in mature space until a full collection, so a
   * program that never runs one never returned them. The mature list is
   * filtered only when this sweep left a block empty; the return then
   * runs on the hysteresis clock (gc_heap.return_clock), keeping
   * max(4 MiB, a quarter of the heap in use) of free memory resident. */
  if (empty) {
    /* The clock ticks first: the blocks freed now carry the new stamp, so
     * only memory that stayed free since an earlier tick goes back (a
     * block freed and reused every cycle stays resident: Binarytrees::Obj
     * refaulted its 90 MB every cycle with the tick after the frees). */
    gc_heap.return_clock++;
    gc_filter_blocks(&gc_heap.space[SPACE_MATURE].list, keep_unless_empty);
    gc_heap.ctr.sticky.returned += empty;
    gc_heap_return(gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes);
  }
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
