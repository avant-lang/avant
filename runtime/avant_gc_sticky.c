#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The young generation: S+ (Stage 28 W15c, D233), candidate S with the
 * opportunistic young copy. Candidate E, the evacuating nursery, was
 * deleted as a separate young generation in Stage 28 W15d (D232); the
 * mechanisms of E this file uses stay: header forwarding (gc_set_forward,
 * avant_gc_internal.h), the copy (splus_copy) and the noted list (below, moved
 * from avant_gc_young.c, which held E's young copy).
 *
 * Candidate S, the in-place young generation (Stage 28 W15b,
 * gc-design.md §3.1, StickyImmix), is this file with GC_STICKY_COPY 0
 * (an internal constant, avant_gc_internal.h). A young collection under
 * S moves nothing. It traces from the roots
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
 * collecting calls (D165), and runtime C keeps what it holds across a
 * collecting call in handle scopes (RC1, avant_gc.h; Stage 28 W16a). */
/* The noted list: every header MARKED a young collection sets, cleared
 * when it ends (C36, gc-design.md §2.6). */
void gc_note_marked(void *payload) {
  if (gc_heap.noted_n == gc_heap.noted_cap) {
    gc_heap.noted_cap = gc_heap.noted_cap ? gc_heap.noted_cap * 2 : 256;
    gc_heap.noted = gc_xrealloc(gc_heap.noted, gc_heap.noted_cap * sizeof(void *));
  }
  gc_heap.noted[gc_heap.noted_n++] = payload;
}

/* Also the end of SURVIVED on every object this collection promoted:
 * promote_in_place leaves it so that S+'s copy can tell a first survivor
 * from an aged one (Stage 28 W15d); an object still young keeps it (an
 * aged copy is not on the list; a young large object at its first
 * survival is). */
void gc_clear_noted(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.noted_n; i++) {
    AvantHeader *h = avant_header(gc_heap.noted[i]);
    uint16_t f = (uint16_t)(h->flags & ~(AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW | AVANT_FLAG_CARD_SCAN));
    if (!(f & AVANT_FLAG_YOUNG)) {
      f = (uint16_t)(f & ~AVANT_FLAG_SURVIVED);
    }
    h->flags = f;
  }
  gc_heap.noted_n = 0;
}

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
  /* SURVIVED (an aged object's) ends with the collection
   * (gc_clear_noted): S+'s copy reads it to decide where the copy goes. */
  h->flags = (uint16_t)((h->flags & ~AVANT_FLAG_YOUNG) | AVANT_FLAG_PROMOTED_NOW);
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
static uint64_t scan_cards(void *p, int from_mature);

#if GC_STICKY_COPY
/* Variant S+ (Stage 28 W15c, gc-design.md §3.1, §3.3): the opportunistic
 * young copy. A young collection traces and promotes in place as S does,
 * and sums each young block's survivor bytes. After the trace a young
 * block that held no mature line when the collection began and whose
 * survivors' bytes fill at most SPLUS_SPARSE_LINES lines is evacuated:
 * its survivors (MARKED, found by its start bits) are copied into fresh
 * blocks (forwarding in the header, as defragmentation does) and its
 * line marks are cleared, so the young sweep frees it whole; every reference
 * to them is rewritten by one more pass over the references the trace
 * followed (splus_fix: frame slots, runtime-held slots and the fields of
 * every object the trace scanned, which the noted list holds: young ones,
 * remembered ones and the ones R1' scans one level; by the generational
 * invariant no other field names a young object); and the block's lines
 * are freed by the young sweep. Other survivors are promoted in
 * place, as under S. A block stays in place when any of its young
 * objects is named imprecisely: a pin or PINNED, or a registered slot
 * location inside it. (Until Stage 28 W16b also a word of the collecting
 * thread's C stack at a collection runtime C entered, W15a2's pin scan;
 * runtime C roots what it holds since W16a, RC1, and gc-design.md
 * section 4.3's proof retired the scan.) Nothing moves in a
 * shared program (no other thread is stopped, D191) or with the nursery
 * off (no young collection). Copying is optional: a block refused at the
 * heap limit leaves the remaining survivors in place. */
#endif

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
static int trace_ref(void **slot, void *p, int from_mature) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start) {
    return 0;
  }
  h = avant_header(start);
#if GC_STICKY_COPY
  /* S+: a young block object named other than by a slot (a pin, a
   * registered slot location inside it), or pinned, keeps its block in
   * place: splus_fix rewrites slots only. */
  if (gc_heap.splus_on && !ol && (h->flags & AVANT_FLAG_YOUNG) &&
      (!slot || (h->flags & AVANT_FLAG_PINNED))) {
    b->kept = SPLUS_KEPT;
  }
#else
  (void)slot;
#endif
  if (h->flags & AVANT_FLAG_MARKED) {
    /* A carded object scanned so far only by its marked cards, now named
     * by a root or a young object's field (R1'(a), (b)): scanned whole,
     * as R1' says (Stage 28 W16t). */
    if (!from_mature && (h->flags & AVANT_FLAG_CARD_SCAN)) {
      h->flags &= (uint16_t)~AVANT_FLAG_CARD_SCAN;
      scan_fields(start, 1);
    }
    return ol && (h->flags & AVANT_FLAG_YOUNG);
  }
  if (h->flags & AVANT_FLAG_YOUNG) {
    h->flags |= AVANT_FLAG_MARKED;
    gc_note_marked(start);
    if (ol) {
      gc_heap.young_large_live += gc_large_bytes(ol);
    } else {
      promote_in_place(h, b);
#if GC_STICKY_COPY
      /* S+: the block's survivor bytes, summed in its idle field. */
      b->idle += gc_obj_total(h);
#endif
    }
    if (h->type_id != AVANT_TYPE_BYTES) {
      gc_work_push(start);
    }
    return ol != NULL;
  }
  if (from_mature && (h->flags & (AVANT_FLAG_REM | AVANT_FLAG_REMC)) == 0) {
    return 0;
  }
  h->flags |= AVANT_FLAG_MARKED;
  gc_note_marked(start);
  if (ol) {
    gc_heap.young_large_live += gc_large_bytes(ol);
  }
  /* Followed only because it is remembered: a carded object's marked
   * cards are what the barrier recorded (Stage 28 W16t). */
  if (from_mature && (h->flags & AVANT_FLAG_REMC) && !(h->flags & AVANT_FLAG_CARD_ALL)) {
    h->flags |= AVANT_FLAG_CARD_SCAN;
    (void)scan_cards(start, 1);
  } else {
    scan_fields(start, 1);
  }
  return 0;
}

/* The slots of carded P's marked cards (Stage 28 W16t), traced as
 * scan_fields traces fields; returns their count (rem_fields counts the
 * remembered set's scans, as field_count does for whole objects). A
 * young large object they name needs no keep entry: gc_cards_settle
 * keeps the card. */
static uint64_t scan_cards(void *p, int from_mature) {
  uint64_t n = 0;
  EACH_CARD_SLOT(p, slot, {
    (void)trace_ref(slot, *slot, from_mature);
    n++;
  });
  return n;
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
  EACH_REF_SLOT(p, slot, { names_young_large |= trace_ref(slot, *slot, from_mature); });
  if (names_young_large &&
      (ph->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) != (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) {
    gc_keep_add(p);
  }
}

static void trace_root_slot(void **slot) {
  (void)trace_ref(slot, *slot, 0);
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
          trace_ref(NULL, ls, 0);
        }
#if GC_STICKY_COPY
        /* A slot location inside an object an earlier slot already
         * reached (promoted, so no longer YOUNG): its block must stay in
         * place all the same, since splus_fix rewrites what a slot holds,
         * never where the frame keeps it (Stage 28 W15d: before, such an
         * object moved and the frame's location named its old copy). A
         * candidate block holds only objects young when the collection
         * began. */
        else if (ls && gc_heap.splus_on && lb && !ll && lb->young_hi && lb->kept != SPLUS_KEPT) {
          lb->kept = SPLUS_KEPT;
        }
#endif
        trace_ref(f->slots[i], *f->slots[i], 0);
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
      {
        AvantHeader *oh = avant_header(obj);
        int carded = (oh->flags & AVANT_FLAG_REMC) && !(oh->flags & AVANT_FLAG_CARD_ALL);
        if (!(oh->flags & AVANT_FLAG_MARKED)) {
          oh->flags |= AVANT_FLAG_MARKED;
          gc_note_marked(obj);
          if (ol) {
            gc_heap.young_large_live += gc_large_bytes(ol);
          }
          if (carded) {
            oh->flags |= AVANT_FLAG_CARD_SCAN;
          }
        }
        /* A carded entry: the slots of its marked cards (R1'(c) for a
         * carded object, Stage 28 W16t); every other entry: every field. */
        if (carded) {
          gc_heap.ctr.rem.fields += scan_cards(obj, 0);
          continue;
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
    trace_ref(NULL, p->payload, 0);
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
      avant_header(obj)->flags &= (uint16_t)~(AVANT_FLAG_REM | AVANT_FLAG_REMC);
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
  gc_cards_settle();
}

/* The young lines LO..HI of block B after the trace: a line no survivor
 * marked at the current epoch is free, and its start bits are cleared
 * (32 granules, half a start word; gc-design.md §2.3, §3.1.1 point 2), so
 * its dead objects are no longer heap payloads and a later hole there
 * has clear start bits for the inline allocation path. A dead object
 * whose header lies in a live line keeps its start bit: nothing infers
 * liveness from start bits. */
/* The line sweep itself is gc_sweep_block_lines (avant_gc_heap.c, word
 * by word since Stage 28 W15c, O87; shared with the full sweep since
 * W15d, O94); here it is counted in the sticky family. */
static int sweep_lines(Block *b, int lo, int hi) {
  uint64_t freed = 0;
  int live = gc_sweep_block_lines(b, lo, hi, &freed);
  gc_heap.ctr.sticky.freed_lines += freed;
  gc_heap.ctr.sticky.blocks++;
  return live;
}

/* A young collection under S (gc_young_collect's S arm): the trace, the
 * young large objects, the remembered set emptied, then the sweep of the
 * young blocks. */
#if GC_STICKY_COPY
/* The new address of P when it lies in an object S+ moved (its exact
 * start or an interior address, resolved through the old copy's start
 * bit and forward), else P. Only addresses in an evacuated block (P's
 * block or, for a payload that starts a block, its header's) can. *AGED
 * is set when the copy is an aged one (still young, YOUNG_AGE 2). */
/* Block BI had objects moved out (the map covers the heap as committed
 * when the fix-up began; blocks past it are new, never evacuated). */
static inline int evac_at(uint64_t bi) {
  return bi < gc_heap.evac_map_n && gc_heap.evac_map[bi];
}

static void *splus_new_addr(void *p, int *aged) {
  uint8_t *a = (uint8_t *)p;
  uintptr_t off = (uintptr_t)a - (uintptr_t)gc_heap.base;
  uint64_t bi;
  Block *ob = NULL;
  AvantHeader *ol = NULL;
  void *start;
  void *to;
  /* The evacuated-block map (O93): P's block, or, for a payload that
   * starts a block, its header's (the block before). */
  if (off >= gc_heap.committed) {
    return p;
  }
  bi = (uint64_t)off >> BLOCK_SHIFT;
  if (!evac_at(bi) && !((off & (BLOCK_SIZE - 1)) == 0 && bi > 0 && evac_at(bi - 1))) {
    return p;
  }
  /* An exact start (nearly every reference): its header's start bit,
   * without the interior lookup. */
  if ((off & (ALIGN - 1)) == 0 && off >= sizeof(AvantHeader) && start_bit_at(a - sizeof(AvantHeader))) {
    const AvantHeader *h = (const AvantHeader *)(a - sizeof(AvantHeader));
    if (!(h->flags & AVANT_FLAG_FORWARDED)) {
      return p;
    }
    to = gc_forward_of(h);
    if (avant_header(to)->flags & AVANT_FLAG_YOUNG) {
      *aged = 1;
    }
    return to;
  }
  start = gc_object_payload(p, &ob, &ol);
  if (!start) {
    return p;
  }
  to = gc_forwarded(start);
  if (!to) {
    return p;
  }
  if (avant_header(to)->flags & AVANT_FLAG_YOUNG) {
    *aged = 1;
  }
  return (void *)((uint8_t *)to + (a - (uint8_t *)start));
}

/* Copy the young block object H into the copy block, promoted: its
 * header keeps its flags without YOUNG, SURVIVED and MARKED, its start bit
 * is set and its lines are marked live; the old header holds the forward.
 * AGED (YOUNG_AGE 2, a first survivor): into the survivor block instead,
 * still young (YOUNG | SURVIVED), its start bit set and its lines left
 * unmarked like any young object's; it is not mature growth. 0 when no
 * block can be had (the heap limit): H stays in place. */
static int splus_copy(AvantHeader *h, int aged) {
  size_t total = gc_obj_total(h);
  Block *d = aged ? gc_heap.splus_sdst : gc_heap.splus_dst;
  uint8_t *to;
  AvantHeader *nh;
  if (aged && (!d || d->bump + total > d->base + BLOCK_SIZE)) {
    gc_heap.splus_sdst = NULL;
    d = gc_new_block(SPACE_MATURE);
    if (!d) {
      return 0;
    }
    d->bump = d->base;
    d->limit = d->base + BLOCK_SIZE;
    d->kept = SPLUS_SURV;
    gc_sticky_push(&gc_heap.surv_blocks, &gc_heap.surv_blocks_n, &gc_heap.surv_blocks_cap, d);
    gc_heap.splus_sdst = d;
    gc_heap.ctr.sticky.surv_blocks++;
  } else if (!d || d->bump + total > d->base + BLOCK_SIZE) {
    if (d && d->bump < d->base + BLOCK_SIZE) {
      gc_reset_block_bump(d);
      gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, d);
    }
    gc_heap.splus_dst = NULL;
    d = gc_new_block(SPACE_MATURE);
    if (!d) {
      return 0;
    }
    d->bump = d->base;
    d->limit = d->base + BLOCK_SIZE;
    gc_heap.splus_dst = d;
    gc_heap.ctr.sticky.copy_blocks++;
  }
  to = d->bump;
  d->bump = to + total;
  memcpy(to, h, total);
  nh = (AvantHeader *)to;
  set_start(d, (size_t)(to - d->base));
  if (aged) {
    nh->flags = (uint16_t)((nh->flags & ~(AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW)) | AVANT_FLAG_YOUNG |
                           AVANT_FLAG_SURVIVED);
    gc_heap.ctr.sticky.aged += total;
    gc_heap.aged_now += total;
  } else {
    nh->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED | AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW);
    mark_lines(d, (size_t)(to - d->base), total);
    gc_heap.ctr.prom.copied += total;
    gc_heap.promoted_since_full += total;
    gc_heap.mature_since_full += total;
  }
  gc_set_forward(h, payload_of(nh));
  avant_g_stats.copied_bytes += total;
  gc_heap.live += total;
  return 1;
}

static void splus_fix_slot(void **slot) {
  int aged = 0;
  *slot = splus_new_addr(*slot, &aged);
}

/* Every reference the trace followed, rewritten to the copies: the frame
 * slots of every registered thread, the runtime-held slots, the fields of
 * every object the trace scanned (the noted list; a moved object's fields
 * are its copy's), and the keep list's holders. A scanned object that is
 * not young and now names an aged copy (YOUNG_AGE 2) joins the keep list,
 * so the remembered set holds it after the collection
 * (gc_rem_keep_young_holders): by the generational invariant these are
 * the only holders of the aged copies. */
static void splus_fix(void) {
  ThreadReg *t;
  uint32_t i;
  {
    uint64_t need = gc_heap.committed >> BLOCK_SHIFT;
    if (need > gc_heap.evac_map_n) {
      gc_heap.evac_map = gc_xrealloc(gc_heap.evac_map, (size_t)need);
      memset(gc_heap.evac_map + gc_heap.evac_map_n, 0, (size_t)(need - gc_heap.evac_map_n));
      gc_heap.evac_map_n = need;
    }
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      Block *b = gc_heap.young_blocks[i];
      if (b->kept == SPLUS_EVAC || b->kept == SPLUS_PART) {
        gc_heap.evac_map[(uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT] = 1;
      }
    }
  }
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        splus_fix_slot(f->slots[i]);
      }
    }
  }
  gc_each_runtime_root(splus_fix_slot);
  for (i = 0; i < gc_heap.noted_n; i++) {
    void *p = gc_heap.noted[i];
    void *to = gc_forwarded(p);
    if (to) {
      p = to;
    }
    if (avant_header(p)->type_id == AVANT_TYPE_BYTES) {
      continue;
    }
    /* A carded object the trace scanned by its marked cards only: by the
     * card invariant no other slot names a young (movable) object. */
    if (avant_header(p)->flags & AVANT_FLAG_CARD_SCAN) {
      int aged = 0;
      EACH_CARD_SLOT(p, slot, { *slot = splus_new_addr(*slot, &aged); });
      continue;
    }
    {
      int aged = 0;
      EACH_REF_SLOT(p, slot, { *slot = splus_new_addr(*slot, &aged); });
      if (aged && !(avant_header(p)->flags & AVANT_FLAG_YOUNG)) {
        gc_keep_add(p);
      }
    }
  }
  for (i = 0; i < gc_heap.keep_n; i++) {
    int aged = 0;
    gc_heap.keep[i] = splus_new_addr(gc_heap.keep[i], &aged);
  }
}

/* After the trace: choose the evacuated blocks (a young block with no
 * mature line at the collection's start, SPLUS_PURE, not kept in place,
 * whose survivors fill at most SPLUS_SPARSE_LINES lines), then copy their
 * survivors in the order the trace reached them (the noted list), so
 * objects the program reaches together end up together (the locality a
 * copying collector gives; copied in address order instead, Etc::GameOfLife
 * kept S's time), then clear each evacuated block's line marks: it held no
 * mature line, so its lines were all made live by this collection's
 * promotions, whose charge to the full trigger and bytes in place are
 * taken back (the copies charge their own bytes). A block one of whose
 * copies the heap limit refused (SPLUS_PART) keeps its lines live and its
 * remaining survivors in place. Then every reference to a moved object is
 * rewritten (splus_fix). Before the verifier and the sweep. */
/* Holder aging (Stage 28 W15d, with YOUNG_AGE 2 and S_LARGE_AGE 2): a
 * small first survivor that names a young large object (scan_fields put
 * it on the keep list) and lies in a young block S+ does not evacuate
 * (one with mature lines: young allocation took a hole of a recycled
 * block) would be promoted in place and, remembered, keep its buffer
 * alive into the buffer's second survival (the W15c finding, O88): it is
 * copied into a survivor block like an evacuated block's survivor, so it
 * dies young with its buffer. Its old copy stays in its line, which this
 * collection's promotion made live, until a full collection; the block
 * becomes SPLUS_PART (its lines stay, its forwarded start bits go,
 * splus_finish). Only a block not kept in place (SPLUS_KEPT: a young
 * object in it is named imprecisely) gives one up. */
static int splus_age_holders(void) {
  uint32_t i;
  int moved = 0;
  for (i = 0; i < gc_heap.keep_n; i++) {
    void *p = gc_heap.keep[i];
    AvantHeader *h = avant_header(p);
    Block *b = block_of(h);
    size_t total;
    if (!b || !b->young_hi || (b->kept != 0 && b->kept != SPLUS_PART) ||
        (h->flags & (AVANT_FLAG_FORWARDED | AVANT_FLAG_SURVIVED | AVANT_FLAG_LARGE | AVANT_FLAG_PINNED)) ||
        !(h->flags & AVANT_FLAG_PROMOTED_NOW)) {
      continue;
    }
    total = gc_obj_total(h);
    if (!splus_copy(h, 1)) {
      break;
    }
    b->kept = SPLUS_PART;
    gc_heap.ctr.sticky.inplace -= total;
    gc_heap.live -= total;
    gc_heap.ctr.sticky.aged_holders++;
    moved = 1;
  }
  return moved;
}

static void splus_evacuate(void) {
  uint32_t i;
  int any = 0;
  /* O92 (Stage 28 W15d item 2): the blocks evacuated hold at most one
   * young budget of survivors (avant_young_bytes); the candidates past it
   * keep their survivors in place, promoted (S's rule). A collection whose
   * survivors outgrow the budget ran late, after a burst allocated where
   * no collection could run (a leaf, a no-GC region, a large live set
   * built at once: Calculator::Interpreter's 16.7 MB tree at a budget of
   * 8 MiB), and copying all of it doubled the peak (S 26.4 MB, S+ 42.8);
   * a collection on time copies everything as before (Etc::GameOfLife,
   * the compiler: unchanged). The copy's room is then bounded by what the
   * budget already allows, as E's copy reserve bounded its copy. */
  uint64_t sel = 0;
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    if (b->kept == SPLUS_KEPT) {
      gc_heap.ctr.sticky.kept_blocks++;
    } else if (gc_heap.set.stress) {
      /* The full stress mode (AVANT_GC_STRESS=1, Stage 28 W16a): every
       * young block nothing names imprecisely is evacuated, pure or
       * mixed, dense or sparse, past the O92 cap, so every young object
       * S+ can move is moved at every young collection; a function that
       * kept a reference outside a handle (RC1, RC2) then holds an old
       * copy, which the sweep fills with poison. */
      if (b->idle > 0) {
        b->kept = SPLUS_EVAC;
        gc_heap.ctr.sticky.evac_blocks++;
        any = 1;
      } else {
        b->kept = 0;
      }
    } else if (b->kept == SPLUS_PURE_AGED &&
               (b->idle + LINE_SIZE - 1) / LINE_SIZE > SPLUS_AGED_SPARSE_LINES) {
      /* O97 (Stage 28 W15e): a survivor block whose aged survivors are
       * dense keeps them in place, promoted (no second copy). */
      b->kept = 0;
      gc_heap.ctr.sticky.aged_inplace_blocks++;
    } else if ((b->kept == SPLUS_PURE || b->kept == SPLUS_PURE_AGED) && b->idle > 0 &&
               (b->idle + LINE_SIZE - 1) / LINE_SIZE <= SPLUS_SPARSE_LINES &&
               sel + b->idle <= avant_young_bytes) {
      sel += b->idle;
      b->kept = SPLUS_EVAC;
      gc_heap.ctr.sticky.evac_blocks++;
      any = 1;
    } else if (b->kept == SPLUS_PURE_AGED) {
      b->kept = SPLUS_PURE;
    }
  }
  if (!any) {
    if (gc_heap.splus_age && S_LARGE_AGE >= 2 && gc_heap.keep_n && splus_age_holders()) {
      splus_fix();
    }
    return;
  }
  for (i = 0; i < gc_heap.noted_n; i++) {
    void *p = gc_heap.noted[i];
    Block *b = block_of(avant_header(p));
    if (!b || b->kept != SPLUS_EVAC) {
      continue;
    }
    /* A mixed block (the stress mode) also holds mature objects the trace
     * marked (remembered ones, ones R1' scans): only what this collection
     * promoted, young when it began, may move. */
    if (!(avant_header(p)->flags & AVANT_FLAG_PROMOTED_NOW)) {
      continue;
    }
    /* A first survivor ages (YOUNG_AGE 2): SURVIVED marks one that
     * survived a collection before (an aged copy). */
    if (!splus_copy(avant_header(p),
                    gc_heap.splus_age && !(avant_header(p)->flags & AVANT_FLAG_SURVIVED))) {
      b->kept = SPLUS_PART;
    }
  }
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    if (b->kept == SPLUS_EVAC && gc_heap.set.stress && gc_heap.stress_mixed[i]) {
      /* A mixed block (stress): its mature lines stay live, the lines
       * this collection's promotions marked are free again. */
      uint64_t lines = (uint64_t)gc_block_live_lines(b);
      memcpy(b->line_mark, gc_heap.stress_lines + (size_t)i * LINES_PER_BLOCK, LINES_PER_BLOCK);
      lines = (lines - (uint64_t)gc_block_live_lines(b)) * LINE_SIZE;
      gc_heap.ctr.sticky.inplace -= b->idle;
      gc_heap.live -= b->idle;
      gc_heap.promoted_since_full -= lines;
      gc_heap.mature_since_full -= lines;
    } else if (b->kept == SPLUS_EVAC) {
      uint64_t lines = (uint64_t)gc_block_live_lines(b) * LINE_SIZE;
      memset(b->line_mark, 0, LINES_PER_BLOCK);
      gc_heap.ctr.sticky.inplace -= b->idle;
      gc_heap.live -= b->idle;
      gc_heap.promoted_since_full -= lines;
      gc_heap.mature_since_full -= lines;
    }
  }
  if (gc_heap.splus_age && S_LARGE_AGE >= 2 && gc_heap.keep_n) {
    (void)splus_age_holders();
  }
  if (avant_g_stats.copied_bytes != gc_heap.splus_copied0) {
    splus_fix();
  }
}

/* After the verifier: the moved objects' old copies lose their start bits
 * (no lookup may find a forwarded header; the young sweep frees their
 * lines unless a mature object keeps one live), and the copy block's free
 * rest joins part_blocks. */
static void splus_finish(void) {
  uint32_t i;
  Block *d = gc_heap.splus_dst;
  if (avant_g_stats.copied_bytes != gc_heap.splus_copied0) {
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      Block *b = gc_heap.young_blocks[i];
      int w;
      if (b->kept != SPLUS_EVAC && b->kept != SPLUS_PART) {
        continue;
      }
      for (w = 0; w < START_WORDS; w++) {
        uint64_t bits = b->start[w];
        while (bits) {
          int bit = __builtin_ctzll(bits);
          AvantHeader *h = (AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8);
          bits &= bits - 1;
          if (h->flags & AVANT_FLAG_FORWARDED) {
            b->start[w] &= ~(1ULL << bit);
          }
        }
      }
      if ((uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT < gc_heap.evac_map_n) {
        gc_heap.evac_map[(uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT] = 0;
      }
    }
  }
  /* The full stress mode (Stage 28 W16a) collects before every runtime C
   * allocation and copies a few objects each time: the copy block stays
   * the copy block across young collections until it is full (or a full
   * collection drops it, gc_sticky_full_begin), or every collection left a
   * fresh block holding one copied object, the rest of it holes young
   * allocation then took and the next collection evacuated (a block per
   * collection: limit_defer.av grew to 1 GB). Nothing else allocates in
   * it meanwhile: it is on no allocation list. */
  if (gc_heap.set.stress && d && d->bump < d->base + BLOCK_SIZE) {
    return;
  }
  if (d && d->bump < d->base + BLOCK_SIZE) {
    gc_reset_block_bump(d);
    gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, d);
  }
  gc_heap.splus_dst = NULL;
}
#endif

/* Block.kept in a young collection: the young sweep
 * left the block empty, and it leaves mature space (keep_unless_empty);
 * under S+ also SPLUS_KEPT and SPLUS_EVAC during a young collection. */
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
  int ticked = 0;
  gc_heap.young_large_live = 0;
  gc_heap.keep_n = 0;
  gc_heap.work_n = 0;
#if GC_STICKY_COPY
  gc_heap.splus_on = atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0;
  gc_heap.splus_copied0 = avant_g_stats.copied_bytes;
  gc_heap.splus_age = YOUNG_AGE >= 2 && !tenure;
  gc_heap.aged_now = 0;
  if (gc_heap.splus_on) {
    uint32_t i;
    if (gc_heap.set.stress && gc_heap.young_blocks_n > gc_heap.stress_lines_cap) {
      gc_heap.stress_lines_cap = gc_heap.young_blocks_n * 2;
      gc_heap.stress_lines = gc_xrealloc(gc_heap.stress_lines, (size_t)gc_heap.stress_lines_cap * LINES_PER_BLOCK);
      gc_heap.stress_mixed = gc_xrealloc(gc_heap.stress_mixed, (size_t)gc_heap.stress_lines_cap);
    }
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      Block *yb = gc_heap.young_blocks[i];
      int mature_lines = gc_block_live_lines(yb) != 0;
      if (gc_heap.set.stress) {
        /* The full stress mode (Stage 28 W16a): a block with mature
         * lines is evacuated too (splus_evacuate), its marks restored
         * from this copy afterwards. */
        memcpy(gc_heap.stress_lines + (size_t)i * LINES_PER_BLOCK, yb->line_mark, LINES_PER_BLOCK);
        gc_heap.stress_mixed[i] = (uint8_t)mature_lines;
      }
      yb->kept = mature_lines ? (gc_heap.set.stress ? SPLUS_MIXED : 0)
                              : yb->kept == SPLUS_WAS_SURV ? SPLUS_PURE_AGED : SPLUS_PURE;
      yb->idle = 0;
    }
  }
#endif
  trace_all();
#if GC_STICKY_COPY
  if (gc_heap.splus_on) {
    splus_evacuate();
  }
#endif
  if (gc_heap.set.verify) {
    gc_verify_sticky();
  }
#if GC_STICKY_COPY
  if (gc_heap.splus_on) {
    splus_finish();
  }
#endif
  /* The young large objects: an unreached one is freed; a reached one is
   * stays young at its first survival (SURVIVED, S_LARGE_AGE 2, the
   * default by measurement since Stage 28 W15d) and is promoted at its
   * second (O88, O76, O85), its holders that are not young remembered by
   * rem_rebuild; with S_LARGE_AGE 1, or a fixed AVANT_YOUNG_MB (TENURE),
   * it is promoted at once. */
  gc_heap.force_tenure = tenure || S_LARGE_AGE <= 1;
  /* The return clock ticks before the young large objects are freed
   * (Stage 28 W15e): a run freed now must carry the new stamp, or the
   * return below gives it back at once and its next use faults every
   * page in again (Binarytrees::Arena's 25 MB arena freed young: 13 k ->
   * 101 k page faults, +0.12 s; W15e record, item 2). */
  if (gc_heap.young_large_n) {
    gc_heap.return_clock++;
    ticked = 1;
  }
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
      if (gc_heap.set.stress) {
        /* Freed young lines are poisoned (Stage 28 W16a). */
        int l;
        for (l = b->young_lo; l < b->young_hi && l < LINES_PER_BLOCK; l++) {
          if (!line_live(b, l)) {
            gc_poison(b->base + (size_t)l * LINE_SIZE, LINE_SIZE);
          }
        }
      }
      b->young_lo = 0;
      b->young_hi = 0;
      b->kept = 0;
      b->idle = 0;
      if (live == 0) {
        b->kept = YOUNG_EMPTY;
        empty++;
      } else if (live < LINES_PER_BLOCK) {
        /* AVANT_GC_VERIFY: the lines this sweep freed are not
         * allocatable until the next collection begins
         * (gc_quarantine_release), so a reference to an object that died
         * here still names freed memory there, where the lost-reference
         * check (avant_gc_verify.c) sees it. */
        if (gc_heap.set.verify) {
          gc_sticky_push(&gc_heap.verify_holes, &gc_heap.verify_holes_n, &gc_heap.verify_holes_cap, b);
        } else {
          gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
        }
      }
    }
    gc_heap.young_blocks_n = 0;
  }
#if GC_STICKY_COPY
  /* The survivor blocks (YOUNG_AGE 2) are young blocks until the next
   * collection: their used lines are its young range, and neither young
   * nor mature allocation takes a block with one (young_hi set). */
  {
    uint32_t i;
    for (i = 0; i < gc_heap.surv_blocks_n; i++) {
      b = gc_heap.surv_blocks[i];
      b->kept = SPLUS_WAS_SURV;
      b->idle = 0;
      b->young_lo = 0;
      b->young_hi = (uint8_t)((b->bump - b->base + LINE_SIZE - 1) / LINE_SIZE);
      b->bump = b->base + BLOCK_SIZE;
      b->limit = b->base + BLOCK_SIZE;
      b->search = LINES_PER_BLOCK;
      gc_sticky_push(&gc_heap.young_blocks, &gc_heap.young_blocks_n, &gc_heap.young_blocks_cap, b);
    }
    gc_heap.surv_blocks_n = 0;
    gc_heap.splus_sdst = NULL;
  }
#endif
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
    if (!ticked) {
      gc_heap.return_clock++;
    }
    gc_filter_blocks(&gc_heap.space[SPACE_MATURE].list, keep_unless_empty);
    gc_heap.ctr.sticky.returned += empty;
    gc_heap_return(gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes);
  }
  gc_clear_noted();
  /* The one young budget: the aged survivors that stay young (as E
   * counted its to-space); the young large objects that stay young only
   * under W15d's age rule (LARGE_AGE_BYTES 0): an age in allocated bytes
   * needs the budget to measure new allocation (O98). */
#if LARGE_AGE_BYTES
  avant_nursery_held = gc_heap.aged_now;
#else
  avant_nursery_held = gc_heap.young_large + gc_heap.aged_now;
#endif
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
#if GC_STICKY_COPY
  /* A copy block the stress mode kept (splus_finish) becomes an ordinary
   * mature block: its free rest is free lines for the full sweep. */
  if (gc_heap.splus_dst) {
    gc_reset_block_bump(gc_heap.splus_dst);
    gc_heap.splus_dst = NULL;
  }
#endif
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    gc_heap.young_blocks[i]->young_lo = 0;
    gc_heap.young_blocks[i]->young_hi = 0;
    gc_heap.young_blocks[i]->kept = 0;
  }
  gc_heap.young_blocks_n = 0;
  gc_heap.part_blocks_n = 0;
  gc_heap.verify_holes_n = 0;
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
