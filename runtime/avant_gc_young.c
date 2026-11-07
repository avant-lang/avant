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
 * and remembered tracing it needs), the survival probe that shares its
 * walk, pinned nursery objects promoted in place (C29), the release or
 * parking of from-space, and gc_young_copy_out, the whole young pass. When to
 * run it and how large the nursery is are avant_gc_collect.c's. */

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
    avant_header(gc_heap.noted[i])->flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_LOS_KEEP);
  }
  gc_heap.noted_n = 0;
}

static void probe_note(void *start) {
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  note_marked(start);
  gc_heap.probe_nursery_bytes += gc_obj_total(h);
  gc_work_push(start);
}

/* The slot now names a young object. If that slot lives in a mature
 * object, the next young collection has to find it from the remembered
 * set: the mature object is not walked from the roots. */
static void remember_young_edge(void **slot, void *payload) {
  Block *pb = NULL;
  Block *ob = NULL;
  AvantHeader *ol = NULL;
  void *owner;
  if (!gc_is_heap_payload(payload, &pb, NULL) || !pb) {
    return;
  }
  if (pb->space != SPACE_NURSERY && pb->space != SPACE_TO) {
    return;
  }
  owner = gc_object_payload((void *)slot, &ob, &ol);
  if (!owner) {
    return;
  }
  /* A pinned nursery owner is not copied: it stays in its block and is
   * promoted in place when this collection ends (promote_pinned_one),
   * so its young edge needs the remembered set like a mature owner's. */
  if (ob && (ob->space == SPACE_NURSERY || ob->space == SPACE_TO) &&
      (avant_header(owner)->flags & AVANT_FLAG_PINNED) == 0) {
    return;
  }
  gc_keep_add(owner);
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
 * Scan a mature or large object when a root or a young object points
 * at it. From inside a mature object, scan a remembered child or a
 * young large child. A mature tree that is not remembered is not walked. */
static void note_los_keep(AvantHeader *h, int from_mature) {
  if ((h->flags & AVANT_FLAG_YOUNG) == 0) {
    return;
  }
  if (from_mature || gc_heap.from_heap_slot) {
    h->flags |= AVANT_FLAG_LOS_KEEP;
  }
}

static void trace_mature(void *p, int from_mature) {
  AvantHeader *h = avant_header(p);
  int saved;
  if (h->flags & AVANT_FLAG_MARKED) {
    note_los_keep(h, from_mature);
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
  note_los_keep(h, from_mature);
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
  if (gc_heap.probe) {
    gc_die("probe copied");
  }
  if (!gc_is_heap_payload(p, &b, NULL) || !b || b->space != SPACE_NURSERY) {
    return p;
  }
  void *to = gc_fwd_get(p);
  if (to) {
    return to;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_PINNED) {
    gc_heap.saw_pinned_nursery = 1;
    if (!(h->flags & AVANT_FLAG_MARKED)) {
      h->flags |= AVANT_FLAG_MARKED;
      note_marked(p);
      gc_mark_object_lines(p);
      gc_heap.live += gc_obj_total(h);
      gc_work_push(p);
    }
    return p;
  }
  size_t total = gc_obj_total(h);
  /* Tenure only after a previous collection, or when the caller has
   * already decided this graph is long-lived. A first survivor stays
   * in the nursery (SPACE_TO) so a tree that dies next cycle is not
   * promoted. */
  int tenure = gc_heap.force_tenure || (h->flags & AVANT_FLAG_SURVIVED);
  AvantHeader *nh = tenure ? (AvantHeader *)gc_mature_alloc(total) : gc_to_alloc(total);
  memcpy(nh, h, total);
  nh->flags = (uint16_t)(h->flags &
                         (AVANT_FLAG_PINNED | AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA));
  nh->flags |= AVANT_FLAG_MARKED;
  if (!tenure) {
    nh->flags |= (uint16_t)(AVANT_FLAG_SURVIVED | AVANT_FLAG_YOUNG);
  }
  void *np = payload_of(nh);
  note_marked(np);
  gc_fwd_put(p, np);
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
  if (start != p && !gc_heap.probe) {
    gc_heap.ctr.root.interiors++;
  }
  if (b && b->space == SPACE_NURSERY) {
    if (gc_heap.probe) {
      /* A nursery child is a young edge by definition; the probe must
       * record it, or the frontier would evict this parent as clean. */
      if (gc_heap.rem_track) {
        gc_heap.rem_track_dirty = 1;
      }
      probe_note(start);
      return;
    }
    ptrdiff_t delta = (uint8_t *)p - (uint8_t *)start;
    void *np = gc_copy_nursery(start);
    *slot = (uint8_t *)np + delta;
    remember_young_edge(slot, np);
    if (gc_heap.rem_track && names_young(*slot)) {
      gc_heap.rem_track_dirty = 1;
    }
    return;
  }
  /* Mature and large objects stay put. Follow one when this slot is a
   * root or a field of a young object. A field of a mature object is
   * followed only when that child is remembered. A live object in a
   * from-space block is a pinned survivor promoted in place
   * (promote_pinned_one): it is mature, and its young children are
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
  if (!gc_heap.probe) {
    gc_fwd_reset();
  }
  gc_heap.saw_pinned_nursery = 0;
  gc_heap.from_mature = 0;
  gc_heap.work_n = 0;
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        if (!gc_heap.probe) {
          relocate_root_location(&f->slots[i]);
        }
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
        /* A remembered object is a root this cycle: mark it so the
         * probe pass's gc_rem_scrub keeps it in the set (noted, so the
         * mark ends with the collection, C36). */
        if (!(avant_header(obj)->flags & AVANT_FLAG_MARKED)) {
          avant_header(obj)->flags |= AVANT_FLAG_MARKED;
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
  if (gc_heap.probe) {
    return;
  }
  /*
   * Every copied object is in the forwarding table now: rewrite the
   * forwarded words runtime C frames and register saves left on this
   * thread's stack, then count any that still name from-space. Only
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
static void promote_pinned_one(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    gc_heap.block_has_pin = 1;
    h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  }
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

static int block_keep_pinned(Block *b) {
  gc_heap.block_has_pin = 0;
  gc_foreach_block_object(b, promote_pinned_one);
  return gc_heap.block_has_pin;
}

/* After young copy, free unpinned nursery blocks. Pinned objects keep
 * their address (D34); those blocks stay on gc_heap.from as SPACE_FROM.
 * (AVANT_NURSERY_DISCARD=0, which parked every nursery block instead,
 * the Stage 17 safety net before the leftover stack rewrite, is retired
 * since Stage 28 W14b2.) */
static void discard_nursery_from(void) {
  Block *b = gc_heap.nursery;
  gc_heap.nursery = NULL;
  /* Pins are roots, so a young copy already saw every pinned nursery
   * object. Walking every dead object to recheck the flag was most of
   * a calculator collection: the tree is garbage and nothing is pinned. */
  if (!gc_heap.saw_pinned_nursery) {
    while (b) {
      Block *next = b->next;
      gc_cache_or_free_block(b);
      b = next;
    }
    return;
  }
  while (b) {
    Block *next = b->next;
    if (block_keep_pinned(b)) {
      b->space = SPACE_FROM;
      gc_heap.space_bytes[SPACE_NURSERY] -= BLOCK_SIZE;
      gc_heap.space_bytes[SPACE_FROM] += BLOCK_SIZE;
      b->next = gc_heap.from;
      gc_heap.from = b;
    } else {
      gc_cache_or_free_block(b);
    }
    b = next;
  }
}

static void adopt_to_space(void) {
  Block *head = gc_heap.to;
  Block *b = gc_heap.to;
  gc_heap.to = NULL;
  while (b) {
    Block *next = b->next;
    b->space = SPACE_NURSERY;
    gc_heap.space_bytes[SPACE_TO] -= BLOCK_SIZE;
    gc_heap.space_bytes[SPACE_NURSERY] += BLOCK_SIZE;
    b->next = gc_heap.nursery;
    gc_heap.nursery = b;
    b = next;
  }
  gc_clear_alloc_cur(SPACE_TO);
  gc_clear_alloc_cur(SPACE_NURSERY);
  if (head && head->bump < head->base + BLOCK_SIZE) {
    gc_heap.alloc_cur[SPACE_NURSERY] = head;
    gc_heap.alloc_fresh[SPACE_NURSERY] = 1;
    gc_use_block(head);
  }
}

void gc_young_copy_out(int tenure) {
  gc_heap.force_tenure = tenure;
  gc_heap.to_bytes = 0;
  gc_heap.keep_n = 0;
  gc_zero_line_marks(gc_heap.nursery);
  gc_gen_young_copy();
  if (gc_heap.set.verify) {
    gc_verify_young_copy();
  }
  gc_clear_alloc_cur(SPACE_NURSERY);
  discard_nursery_from();
  adopt_to_space();
  gc_rem_commit_kept();
  if (avant_nursery_held + gc_heap.to_bytes > gc_heap.ctr.yng.cpy_peak) {
    gc_heap.ctr.yng.cpy_peak = avant_nursery_held + gc_heap.to_bytes;
  }
  avant_nursery_held = gc_heap.to_bytes;
  gc_heap.to_bytes = 0;
  gc_heap.force_tenure = 0;
  gc_reclaim_young_large();
  /* The end of the copy's marks: the noted list, not a walk of every
   * object of every space (gc-design.md §2.6; the walk was 7% of the
   * audit's mature.av profile). */
  gc_clear_noted();
}
