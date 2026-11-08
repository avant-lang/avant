#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The young collection (Stage 28 W12, moved from avant_rt.c): the
 * copying nursery's evacuation (gc_copy_from_slot, gc_copy_nursery, the mature
 * and remembered tracing it needs), pinned nursery objects promoted in
 * place (C29), the release of from-space, and gc_young_copy_out, the whole
 * young pass. When to run it and how large the nursery is are
 * avant_gc_collect.c's (the survival probe that shared this walk is
 * retired since Stage 28 W15a1). */

static void note_marked(void *payload) {
  if (gc_heap.noted_n == gc_heap.noted_cap) {
    gc_heap.noted_cap = gc_heap.noted_cap ? gc_heap.noted_cap * 2 : 256;
    gc_heap.noted = gc_xrealloc(gc_heap.noted, gc_heap.noted_cap * sizeof(void *));
  }
  gc_heap.noted[gc_heap.noted_n++] = payload;
}

void gc_clear_noted(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.noted_n; i++) {
    avant_header(gc_heap.noted[i])->flags &= (uint16_t)~AVANT_FLAG_MARKED;
  }
  gc_heap.noted_n = 0;
}

/* The nursery objects this young collection keeps in place (Stage 28
 * W15a2): pinned ones (D205). Each is marked (MARKED on a nursery object
 * means "kept"), traced from the work list, its block flagged kept, and
 * it is listed here, so the end of the collection keeps its block and
 * promotes it in place (discard_nursery_from) without walking any
 * nursery block object by object: the moved objects in those blocks
 * hold forwards in their headers, not sizes. */
static void keep_in_place(void *p);

/* A nursery object a word of the collecting thread's C stack names, at a
 * collection runtime C entered (gc_pin_c_stack): kept in place and
 * promoted in place like a pinned one (D205's mechanism), so the word,
 * whatever it is, stays valid and is never changed (mostly-copying: a
 * word that is not a reference only keeps an object alive). */
void gc_keep_conservative(void *p) {
  AvantHeader *h = avant_header(p);
  if (h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED)) {
    return;
  }
  gc_heap.ctr.keep.cons_objects++;
  gc_heap.ctr.keep.cons_bytes += gc_obj_total(h);
  keep_in_place(p);
}

static void keep_in_place(void *p) {
  AvantHeader *h = avant_header(p);
  Block *b = block_of(h);
  h->flags |= AVANT_FLAG_MARKED;
  note_marked(p);
  gc_mark_object_lines(p);
  gc_heap.live += gc_obj_total(h);
  gc_work_push(p);
  gc_heap.saw_pinned_nursery = 1;
  /* kept: 1 a pinned object is kept (the block becomes from-space, D205),
   * 2 only conservatively kept ones (the block joins mature space). */
  if (h->flags & AVANT_FLAG_PINNED) {
    b->kept = 1;
  } else if (!b->kept) {
    b->kept = 2;
  }
  if (gc_heap.kept_n == gc_heap.kept_cap) {
    gc_heap.kept_cap = gc_heap.kept_cap ? gc_heap.kept_cap * 2 : 64;
    gc_heap.kept = gc_xrealloc(gc_heap.kept, gc_heap.kept_cap * sizeof(void *));
  }
  gc_heap.kept[gc_heap.kept_n++] = p;
}

/* The slot now names a young object. If that slot lives in a mature
 * object, the next young collection has to find it from the remembered
 * set: the mature object is not walked from the roots. */
static void remember_young_edge(void **slot, void *payload) {
  Block *pb = NULL;
  Block *ob = NULL;
  AvantHeader *ol = NULL;
  AvantHeader *pl = NULL;
  void *owner;
  void *target = gc_object_payload(payload, &pb, &pl);
  /* The YOUNG flag decides for a large target, the block's space for a
   * block object (Stage 28 W15a1, gc-design.md §5.3: an edge to a young
   * large object is remembered like any young edge; before, only edges
   * to block objects were, and LOS_KEEP promoted the large object). */
  if (!target) {
    return;
  }
  if (pl) {
    if ((avant_header(target)->flags & AVANT_FLAG_YOUNG) == 0) {
      return;
    }
  } else if (!pb || (pb->space != SPACE_NURSERY && pb->space != SPACE_TO)) {
    return;
  }
  owner = gc_object_payload((void *)slot, &ob, &ol);
  if (!owner) {
    return;
  }
  /* A nursery owner kept in place (pinned, or named by a C stack word:
   * MARKED, keep_in_place) is not copied: it stays in its block and is
   * promoted in place when this collection ends (promote_kept_one), so
   * its young edge needs the remembered set like a mature owner's. */
  if (ob && (ob->space == SPACE_TO ||
             (ob->space == SPACE_NURSERY &&
              (avant_header(owner)->flags & (AVANT_FLAG_PINNED | AVANT_FLAG_MARKED)) == 0))) {
    return;
  }
  /* A young large owner is traced whenever it is reached; when it is
   * promoted it is remembered then (promote_young_large). */
  if (ol && (avant_header(owner)->flags & AVANT_FLAG_YOUNG)) {
    return;
  }
  gc_keep_add(owner);
}

/* The large-object bytes this young collection marks (gc_heap.
 * young_large_live): the live sample of the los family, kept without a
 * walk of every large object (O65). */
static void note_large_live(AvantHeader *h) {
  if (h->flags & AVANT_FLAG_LARGE) {
    gc_heap.young_large_live += gc_large_bytes(h);
  }
}

static int names_young(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return 0;
  }
  if (ol) {
    return (avant_header(start)->flags & AVANT_FLAG_YOUNG) != 0;
  }
  return b && (b->space == SPACE_NURSERY || b->space == SPACE_TO);
}
static void trace_mature(void *p, int from_mature);

static void copy_payload_children(void *p) {
  if (avant_header(p)->type_id == AVANT_TYPE_BYTES) {
    return;
  }
  int saved = gc_heap.from_heap_slot;
  gc_heap.from_heap_slot = 1;
  EACH_REF_SLOT(p, slot, { gc_copy_from_slot(slot); });
  gc_heap.from_heap_slot = saved;
}

/* A remembered object is an old-to-young edge, not a root. Scanning
 * every remembered buffer keeps a dead array's nodes alive: the
 * calculator replaces that array each iteration, the buffer is a large
 * object, and the young collection was tenuring the whole history.
 * Scan a mature object (block or large) when a root or a young object
 * points at it. From inside a mature object, scan a remembered child. A
 * mature tree that is not remembered is not walked. Young large objects
 * do not come here: gc_copy_from_slot traces them as young objects. */
static void trace_mature(void *p, int from_mature) {
  AvantHeader *h = avant_header(p);
  int saved;
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  /* A mature parent's young child is usually in the nursery and is
   * copied before this function runs. A young large object is not in a
   * block, so it arrives here. Leaving it unmarked lets the young
   * collection free a string the parent array still holds. */
  if (from_mature && (h->flags & AVANT_FLAG_REM) == 0 && (h->flags & AVANT_FLAG_YOUNG) == 0) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  note_large_live(h);
  note_marked(p);
  saved = gc_heap.from_mature;
  gc_heap.from_mature = 1;
  /* Nested mature scans do not dirty the remembered parent: only its
   * direct fields count, and the outer gc_copy_from_slot judges that slot
   * itself once this returns. */
  {
    int track_saved = gc_heap.rem_track;
    gc_heap.rem_track = 0;
    copy_payload_children(p);
    gc_heap.rem_track = track_saved;
  }
  gc_heap.from_mature = saved;
}

void *gc_copy_nursery(void *p) {
  Block *b = NULL;
  if (!gc_is_heap_payload(p, &b, NULL) || !b || b->space != SPACE_NURSERY) {
    return p;
  }
  AvantHeader *h = avant_header(p);
  /* Moved already: the forward is in the header (gc-design.md §3.2). */
  if (h->flags & AVANT_FLAG_FORWARDED) {
    return gc_forward_of(h);
  }
  /* Kept in place already (keep_in_place). */
  if (h->flags & AVANT_FLAG_MARKED) {
    return p;
  }
  if (h->flags & AVANT_FLAG_PINNED) {
    keep_in_place(p);
    return p;
  }
  size_t total = gc_obj_total(h);
  /* Tenure after YOUNG_AGE - 1 earlier young collections (Q4), or when
   * the caller has already decided this graph is long-lived. With one
   * aging step a first survivor stays in the nursery (SPACE_TO) so a
   * tree that dies next cycle is not promoted. */
  int tenure = gc_heap.force_tenure || YOUNG_AGE <= 1 || (h->flags & AVANT_FLAG_SURVIVED);
  AvantHeader *nh = tenure ? (AvantHeader *)gc_mature_alloc(total) : gc_to_alloc(total);
  memcpy(nh, h, total);
  nh->flags = (uint16_t)(h->flags &
                         (AVANT_FLAG_PINNED | AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA));
  nh->flags |= AVANT_FLAG_MARKED;
  if (!tenure) {
    nh->flags |= (uint16_t)(AVANT_FLAG_SURVIVED | AVANT_FLAG_YOUNG);
  }
  void *np = payload_of(nh);
  if (tenure) {
    gc_heap.ctr.prom.copied += total;
    gc_heap.promoted_since_full += total;
  }
  note_marked(np);
  gc_set_forward(h, np);
  avant_g_stats.copied_bytes += total;
  gc_heap.live += total;
  gc_mark_object_lines(np);
  gc_work_push(np);
  return np;
}

void gc_copy_from_slot(void **slot) {
  void *p = *slot;
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  if (start != p) {
    gc_heap.ctr.root.interiors++;
  }
  if (b && b->space == SPACE_NURSERY) {
    ptrdiff_t delta = (uint8_t *)p - (uint8_t *)start;
    void *np = gc_copy_nursery(start);
    *slot = (uint8_t *)np + delta;
    remember_young_edge(slot, np);
    if (gc_heap.rem_track && names_young(*slot)) {
      gc_heap.rem_track_dirty = 1;
    }
    return;
  }
  /* A young large object (gc-design.md §5.3, Stage 28 W15a1) stays put
   * and is a young object: marked, and its fields traced from the work
   * list like a copied object's (R1'(b): one level of the non-young
   * objects they name). An edge to it from a non-young owner is
   * remembered. Whether it is freed, stays young or is promoted is
   * gc_reclaim_young_large's, after the trace. */
  if (ol && (ol->flags & AVANT_FLAG_YOUNG)) {
    if ((ol->flags & AVANT_FLAG_MARKED) == 0) {
      ol->flags |= AVANT_FLAG_MARKED;
      note_marked(start);
      note_large_live(ol);
      if (ol->type_id != AVANT_TYPE_BYTES) {
        gc_work_push(start);
      }
    }
    remember_young_edge(slot, start);
    if (gc_heap.rem_track) {
      gc_heap.rem_track_dirty = 1;
    }
    return;
  }
  /* Mature and large objects stay put. Follow one when this slot is a
   * root or a field of a young object. A field of a mature object is
   * followed only when that child is remembered. A live object in a
   * from-space block is a pinned survivor promoted in place
   * (promote_kept_one): it is mature, and its young children are
   * reached the same way (C29). */
  if (ol || (b && (b->space == SPACE_MATURE || b->space == SPACE_FROM))) {
    trace_mature(start, gc_heap.from_mature);
  }
  if (gc_heap.rem_track && names_young(*slot)) {
    gc_heap.rem_track_dirty = 1;
  }
}

static void relocate_root_location(void ***slot_loc) {
  void *addr = (void *)*slot_loc;
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(addr, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return;
  }
  if (start != addr) {
    gc_heap.ctr.root.interiors++;
  }
  ptrdiff_t delta = (uint8_t *)addr - (uint8_t *)start;
  void *np = gc_copy_nursery(start);
  *slot_loc = (void **)((uint8_t *)np + delta);
}

/* Field slots copy_payload_children would visit for one object. */
static uint64_t rem_field_count(void *p) {
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

void gc_gen_young_copy(void) {
  gc_heap.young_large_live = 0;
  gc_heap.saw_pinned_nursery = 0;
  gc_heap.from_mature = 0;
  gc_heap.work_n = 0;
  /* Runtime C entered this collection (avant_alloc_c, a stress point at a
   * defer entry, a named safepoint): first keep in place every nursery
   * object a word of this thread's C stack names, before any root can
   * copy it (Stage 28 W15a2, gc-design.md §3.2, §4.3's first half). The
   * helper's C locals then stay valid without being rewritten. */
  if (gc_thread.c_entry) {
    gc_pin_c_stack();
  }
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        relocate_root_location(&f->slots[i]);
        gc_copy_from_slot(f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  /* Remembered mature objects are roots for the young generation.
   * Their nursery children must be copied and their fields updated
   * even when no root or young object reaches them this cycle: a
   * mature tree interior that tenured together with its children is
   * otherwise never scanned again, and its fields keep pointing at
   * from-space addresses that later collections reuse. */
  {
    uint32_t ri;
    int locked = rem_locked();
    if (locked) {
      pthread_mutex_lock(&gc_heap.rem_mu);
    }
    gc_heap.ctr.rem.scans++;
    gc_heap.ctr.rem.entries += gc_heap.rem_n;
    if (gc_heap.rem_n > gc_heap.ctr.rem.peak) {
      gc_heap.ctr.rem.peak = gc_heap.rem_n;
    }
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      void *obj = gc_heap.remset[ri];
      if (gc_is_heap_payload(obj, NULL, NULL)) {
        /* A remembered object is a root this cycle: marked (and noted,
         * so the mark ends with the collection, C36) like every object
         * the collection scans. */
        if (!(avant_header(obj)->flags & AVANT_FLAG_MARKED)) {
          avant_header(obj)->flags |= AVANT_FLAG_MARKED;
          note_large_live(avant_header(obj));
          note_marked(obj);
        }
        gc_heap.ctr.rem.fields += rem_field_count(obj);
        gc_heap.rem_track = 1;
        gc_heap.rem_track_dirty = 0;
        copy_payload_children(obj);
        gc_heap.rem_track = 0;
        if (gc_heap.rem_dirty) {
          gc_heap.rem_dirty[ri] = (uint8_t)gc_heap.rem_track_dirty;
        }
      }
    }
    if (locked) {
      pthread_mutex_unlock(&gc_heap.rem_mu);
    }
  }
  /*
   * D34: pinned objects are mark roots. Full collect already walks
   * gc_heap.pins; young copy must too. C globals (argv, interned bytes)
   * hold those pointers and would otherwise see a freed nursery.
   */
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    gc_copy_from_slot(&p->payload);
  }
  gc_each_runtime_root(gc_copy_from_slot);
  /* Remembered objects are reached from the roots above. The set is
   * not itself a root list: a dead buffer would keep its nodes alive. */
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    copy_payload_children(p);
  }
  /*
   * Every copied object is forwarded now: rewrite the forwarded words
   * runtime C frames and register saves left on this thread's stack, then
   * count any that still name from-space. Since Stage 28 W15a2 every
   * nursery object a stack word names was kept in place first
   * (gc_pin_c_stack), so no such word exists and the rewrite finds
   * nothing; it stays, with its switch and the sm_* fields, until
   * gc-design.md §4.3's proof retires it (W16/W17). Only
   * when runtime C entered this collection (C37, Stage 28 W14b1): the
   * rewrite takes every stack word equal to a moved object's start for
   * a reference, so it also rewrote integers and character buffers that
   * happened to equal one (a generated-code i32 slot whose upper half
   * kept a stale pointer's bits; split_held.av's junk sum was wrong
   * whenever the heap base was 4 GiB-aligned, about one run in 1,500).
   * A collection entered from generated code needs no rewrite: every
   * reference generated code holds across avant_alloc is in a slot,
   * which the copy updates (D165).
   */
  if (gc_thread.c_entry) {
    gc_rewrite_c_stack();
    gc_count_c_stack_nursery();
  }
}

/*
 * C29 (Stage 28 W8c). A pinned nursery object survives a young copy in
 * place, and its block stays as SPACE_FROM. It is promoted in place
 * here: YOUNG and SURVIVED are cleared, so from now on it is a mature
 * object that happens to live in a from-space block. That restores
 * the generational invariant for it: the barrier (inline and
 * avant_barrier) remembers it when a young child is stored into it,
 * gc_copy_from_slot follows it like a mature object (a root, a young
 * parent, or a remembered parent reaches it), and remember_young_edge
 * keeps it remembered while its fields name young objects. Before,
 * it kept YOUNG (so no barrier) while no later young collection
 * looked inside it (gc_copy_nursery skips non-nursery blocks), and its
 * young children were discarded with their blocks.
 */
static void promote_kept_one(void *p) {
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_YOUNG) {
    gc_heap.ctr.prom.pinned += gc_obj_total(h);
  }
  h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
}

static void note_pinned_in_block(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    gc_heap.block_has_pin = 1;
  }
}

int gc_block_has_pinned(Block *b) {
  gc_heap.block_has_pin = 0;
  gc_foreach_block_object(b, note_pinned_in_block);
  return gc_heap.block_has_pin;
}

/* After young copy, free the nursery blocks. A block holding an object
 * kept in place (keep_in_place: pinned, D34) stays as SPACE_FROM, and
 * only its kept objects keep their start bits (Stage 28 W15a2): the
 * other objects in it are moved (their headers hold forwards) or dead,
 * and no lookup or walk may take one for an object again (before, their
 * start bits stayed and a dead object stayed visible to
 * gc_is_heap_payload until a full collection, H2). Nothing walks a
 * nursery block object by object here: the kept list names the
 * survivors. (AVANT_NURSERY_DISCARD=0, which parked every nursery block
 * instead, the Stage 17 safety net before the leftover stack rewrite, is
 * retired since Stage 28 W14b2.) */
static void discard_nursery_from(void) {
  Block *b = gc_heap.space[SPACE_NURSERY].list;
  uint32_t i;
  gc_heap.space[SPACE_NURSERY].list = NULL;
  while (b) {
    Block *next = b->next;
    if (b->kept == 2) {
      /* Only conservatively kept objects (gc_keep_conservative): the
       * block joins mature space, promoted in place with them. Its other
       * lines are moved or dead objects, so it is a mature block with
       * holes: mature allocation reuses them after the next full
       * collection, and defragmentation may evacuate the block once no
       * stack word names it (C31). Kept as from-space, a block per named
       * object stayed until a full collection and was never compacted
       * (one young string per 32 KiB block under AVANT_GC_STRESS=1). */
      b->kept = 0;
      gc_heap.ctr.keep.blocks++;
      gc_heap.mature_since_full += BLOCK_SIZE;
      gc_heap.promoted_since_full += BLOCK_SIZE;
      memset(b->start, 0, START_WORDS * sizeof(uint64_t));
      b->space = SPACE_MATURE;
      gc_heap.space[SPACE_NURSERY].bytes -= BLOCK_SIZE;
      gc_heap.space[SPACE_MATURE].bytes += BLOCK_SIZE;
      b->next = gc_heap.space[SPACE_MATURE].list;
      gc_heap.space[SPACE_MATURE].list = b;
      gc_reset_block_bump(b);
    } else if (b->kept) {
      b->kept = 0;
      /* A retained block is mature growth until a full collection
       * releases it (gc-design.md §5.4: in-place promotions count). */
      gc_heap.ctr.keep.blocks++;
      gc_heap.mature_since_full += BLOCK_SIZE;
      gc_heap.promoted_since_full += BLOCK_SIZE;
      memset(b->start, 0, START_WORDS * sizeof(uint64_t));
      b->space = SPACE_FROM;
      gc_heap.space[SPACE_NURSERY].bytes -= BLOCK_SIZE;
      gc_heap.space[SPACE_FROM].bytes += BLOCK_SIZE;
      b->next = gc_heap.space[SPACE_FROM].list;
      gc_heap.space[SPACE_FROM].list = b;
    } else {
      gc_cache_or_free_block(b);
    }
    b = next;
  }
  for (i = 0; i < gc_heap.kept_n; i++) {
    void *p = gc_heap.kept[i];
    Block *kb = block_of(avant_header(p));
    set_start(kb, (size_t)((uint8_t *)avant_header(p) - kb->base));
    promote_kept_one(p);
  }
  gc_heap.kept_n = 0;
}

static void adopt_to_space(void) {
  Block *head = gc_heap.space[SPACE_TO].list;
  Block *b = gc_heap.space[SPACE_TO].list;
  gc_heap.space[SPACE_TO].list = NULL;
  while (b) {
    Block *next = b->next;
    b->space = SPACE_NURSERY;
    gc_heap.space[SPACE_TO].bytes -= BLOCK_SIZE;
    gc_heap.space[SPACE_NURSERY].bytes += BLOCK_SIZE;
    b->next = gc_heap.space[SPACE_NURSERY].list;
    gc_heap.space[SPACE_NURSERY].list = b;
    b = next;
  }
  gc_clear_alloc_cur(SPACE_TO);
  gc_clear_alloc_cur(SPACE_NURSERY);
  if (head && head->bump < head->base + BLOCK_SIZE) {
    gc_heap.space[SPACE_NURSERY].cur = head;
    gc_heap.space[SPACE_NURSERY].fresh = 1;
    gc_use_block(head);
  }
}

void gc_young_copy_out(int tenure) {
  gc_heap.force_tenure = tenure;
  gc_heap.to_bytes = 0;
  gc_heap.keep_n = 0;
  gc_zero_line_marks(gc_heap.space[SPACE_NURSERY].list);
  gc_gen_young_copy();
  if (gc_heap.set.verify) {
    gc_verify_young_copy();
  }
  gc_clear_alloc_cur(SPACE_NURSERY);
  discard_nursery_from();
  adopt_to_space();
  /* The young large objects first: a promoted one with reference fields
   * joins the kept list the commit adds to the remembered set. */
  gc_reclaim_young_large();
  gc_rem_commit_kept();
  if (avant_nursery_held + gc_heap.to_bytes > gc_heap.ctr.yng.cpy_peak) {
    gc_heap.ctr.yng.cpy_peak = avant_nursery_held + gc_heap.to_bytes;
  }
  {
    uint64_t to_bytes = gc_heap.to_bytes;
    gc_heap.to_bytes = 0;
    gc_heap.force_tenure = 0;
    /* The one young budget: the survivors in to-space and the young
     * large objects that stay young. */
    avant_nursery_held = to_bytes + gc_heap.young_large;
  }
  /* The end of the copy's marks: the noted list, not a walk of every
   * object of every space (gc-design.md §2.6; the walk was 7% of the
   * audit's mature.av profile). */
  gc_clear_noted();
}
