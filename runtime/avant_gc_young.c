#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The young collection (Stage 28 W12, moved from avant_rt.c): the
 * copying nursery's evacuation (copy_from_slot, copy_nursery, the mature
 * and remembered tracing it needs), the survival probe that shares its
 * walk, pinned nursery objects promoted in place (C29), the release or
 * parking of from-space, and gc_young_copy_out, the whole young pass. When to
 * run it and how large the nursery is are avant_gc_collect.c's. */

int g_saw_pinned_nursery;

int g_discard_from;

/* Objects flagged MARKED by the survival probe. Clearing them by walking
 * every nursery object retraced the dead tree. */
static void **g_noted;
static uint32_t g_noted_n;
static uint32_t g_noted_cap;

uint64_t g_to_bytes;

int g_probe;
static int g_force_tenure;
static int g_from_mature;
uint64_t g_probe_nursery_bytes;

static void note_marked(void *payload) {
  if (g_noted_n == g_noted_cap) {
    g_noted_cap = g_noted_cap ? g_noted_cap * 2 : 256;
    g_noted = gc_xrealloc(g_noted, g_noted_cap * sizeof(void *));
  }
  g_noted[g_noted_n++] = payload;
}

void gc_clear_noted(void) {
  uint32_t i;
  for (i = 0; i < g_noted_n; i++) {
    avant_header(g_noted[i])->flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
  }
  g_noted_n = 0;
}

static void probe_note(void *start) {
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  note_marked(start);
  g_probe_nursery_bytes += obj_total(h);
  work_push(start);
}

/* The slot now names a young object. If that slot lives in a mature
 * object, the next young collection has to find it from the remembered
 * set: the mature object is not walked from the roots. */
static void remember_young_edge(void **slot, void *payload) {
  Block *pb = NULL;
  Block *ob = NULL;
  Large *ol = NULL;
  void *owner;
  if (!is_heap_payload(payload, &pb, NULL) || !pb) {
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

static int g_from_heap_slot;
/* Set while a remembered object is being scanned: copy_from_slot then
 * records whether any direct field names a young object afterwards. */
static int g_rem_track;
static int g_rem_track_dirty;

static int names_young(void *p) {
  Block *b = NULL;
  Large *ol = NULL;
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
  int saved = g_from_heap_slot;
  g_from_heap_slot = 1;
  EACH_REF_SLOT(p, slot, { copy_from_slot(slot); });
  g_from_heap_slot = saved;
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
  if (from_mature || g_from_heap_slot) {
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
  if (g_probe) {
    note_marked(p);
  }
  saved = g_from_mature;
  g_from_mature = 1;
  /* Nested mature scans do not dirty the remembered parent: only its
   * direct fields count, and the outer copy_from_slot judges that slot
   * itself once this returns. */
  {
    int track_saved = g_rem_track;
    g_rem_track = 0;
    copy_payload_children(p);
    g_rem_track = track_saved;
  }
  g_from_mature = saved;
}

void *copy_nursery(void *p) {
  Block *b = NULL;
  if (g_probe) {
    gc_die("probe copied");
  }
  if (!is_heap_payload(p, &b, NULL) || !b || b->space != SPACE_NURSERY) {
    return p;
  }
  void *to = fwd_get(p);
  if (to) {
    return to;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_PINNED) {
    g_saw_pinned_nursery = 1;
    if (!(h->flags & AVANT_FLAG_MARKED)) {
      h->flags |= AVANT_FLAG_MARKED;
      mark_object_lines(p);
      g_live += obj_total(h);
      work_push(p);
    }
    return p;
  }
  size_t total = obj_total(h);
  /* Tenure only after a previous collection, or when the caller has
   * already decided this graph is long-lived. A first survivor stays
   * in the nursery (SPACE_TO) so a tree that dies next cycle is not
   * promoted. */
  int tenure = g_force_tenure || (h->flags & AVANT_FLAG_SURVIVED);
  AvantHeader *nh = tenure ? (AvantHeader *)gc_mature_alloc(total) : gc_to_alloc(total);
  memcpy(nh, h, total);
  nh->flags = (uint16_t)(h->flags &
                         (AVANT_FLAG_PINNED | AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA));
  nh->flags |= AVANT_FLAG_MARKED;
  if (!tenure) {
    nh->flags |= (uint16_t)(AVANT_FLAG_SURVIVED | AVANT_FLAG_YOUNG);
  }
  void *np = payload_of(nh);
  fwd_put(p, np);
  g_stats.copied_bytes += total;
  g_live += total;
  mark_object_lines(np);
  work_push(np);
  return np;
}

void copy_from_slot(void **slot) {
  void *p = *slot;
  Block *b = NULL;
  Large *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  if (start != p && !g_probe) {
    g_interior_slots++;
  }
  if (b && b->space == SPACE_NURSERY) {
    if (g_probe) {
      /* A nursery child is a young edge by definition; the probe must
       * record it, or the frontier would evict this parent as clean. */
      if (g_rem_track) {
        g_rem_track_dirty = 1;
      }
      probe_note(start);
      return;
    }
    ptrdiff_t delta = (uint8_t *)p - (uint8_t *)start;
    void *np = copy_nursery(start);
    *slot = (uint8_t *)np + delta;
    remember_young_edge(slot, np);
    if (g_rem_track && names_young(*slot)) {
      g_rem_track_dirty = 1;
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
    trace_mature(start, g_from_mature);
  }
  if (g_rem_track && names_young(*slot)) {
    g_rem_track_dirty = 1;
  }
}

static void relocate_root_location(void ***slot_loc) {
  void *addr = (void *)*slot_loc;
  Block *b = NULL;
  Large *ol = NULL;
  void *start = gc_object_payload(addr, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return;
  }
  if (start != addr) {
    g_interior_slots++;
  }
  ptrdiff_t delta = (uint8_t *)addr - (uint8_t *)start;
  void *np = copy_nursery(start);
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
  if (id >= MAX_TYPES || !g_type_set[id]) {
    return 0;
  }
  if (g_types[id].stride) {
    return (uint64_t)g_types[id].nrefs *
           (uint64_t)((size_t)h->size / sizeof(void *) / g_types[id].stride);
  }
  return (uint64_t)g_types[id].nrefs;
}

void gc_gen_young_copy(void) {
  if (!g_probe) {
    gc_fwd_reset();
  }
  g_saw_pinned_nursery = 0;
  g_from_mature = 0;
  g_work_n = 0;
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        if (!g_probe) {
          relocate_root_location(&f->slots[i]);
        }
        copy_from_slot(f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
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
      pthread_mutex_lock(&g_rem_mu);
    }
    g_rem_scans++;
    g_rem_entries += g_rem_n;
    if (g_rem_n > g_rem_peak) {
      g_rem_peak = g_rem_n;
    }
    for (ri = 0; ri < g_rem_n; ri++) {
      void *obj = g_remset[ri];
      if (is_heap_payload(obj, NULL, NULL)) {
        /* A remembered object is a root this cycle: mark it so the
         * probe pass's gc_rem_scrub keeps it in the set. */
        avant_header(obj)->flags |= AVANT_FLAG_MARKED;
        g_rem_fields += rem_field_count(obj);
        g_rem_track = 1;
        g_rem_track_dirty = 0;
        copy_payload_children(obj);
        g_rem_track = 0;
        if (g_rem_dirty) {
          g_rem_dirty[ri] = (uint8_t)g_rem_track_dirty;
        }
      }
    }
    if (locked) {
      pthread_mutex_unlock(&g_rem_mu);
    }
  }
  if (!g_probe) {
    copy_from_c_stack();
  }
  copy_from_stackmaps();
  /*
   * D34: pinned objects are mark roots. Full collect already walks
   * g_pins; young copy must too. C globals (argv, interned bytes)
   * hold those pointers and would otherwise see a freed nursery.
   */
  for (PinEnt *p = g_pins; p; p = p->next) {
    copy_from_slot(&p->payload);
  }
  avant_hash_walk_tls(copy_from_slot);
  /* Remembered objects are reached from the roots above. The set is
   * not itself a root list: a dead buffer would keep its nodes alive. */
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    copy_payload_children(p);
  }
  if (g_probe) {
    return;
  }
  /*
   * Mapped stackmap slots were roots above. Extra LLVM spills of the
   * same pointer bits are not in gc-live; rewrite them now that every
   * copied object is in the forwarding table.
   */
  rewrite_from_stackmaps();
  rewrite_c_stack();
  count_c_stack_nursery();
}

static int g_block_has_pin;

/*
 * C29 (Stage 28 W8c). A pinned nursery object survives a young copy in
 * place, and its block stays as SPACE_FROM. It is promoted in place
 * here: YOUNG and SURVIVED are cleared, so from now on it is a mature
 * object that happens to live in a from-space block. That restores
 * the generational invariant for it: the barrier (inline and
 * avant_barrier) remembers it when a young child is stored into it,
 * copy_from_slot follows it like a mature object (a root, a young
 * parent, or a remembered parent reaches it), and remember_young_edge
 * keeps it remembered while its fields name young objects. Before,
 * it kept YOUNG (so no barrier) while no later young collection
 * looked inside it (copy_nursery skips non-nursery blocks), and its
 * young children were discarded with their blocks.
 */
static void promote_pinned_one(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    g_block_has_pin = 1;
    h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  }
}

static void note_pinned_in_block(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    g_block_has_pin = 1;
  }
}

int gc_block_has_pinned(Block *b) {
  g_block_has_pin = 0;
  gc_foreach_block_object(b, note_pinned_in_block);
  return g_block_has_pin;
}

static int block_keep_pinned(Block *b) {
  g_block_has_pin = 0;
  gc_foreach_block_object(b, promote_pinned_one);
  return g_block_has_pin;
}

/*
 * After young copy, free unpinned nursery blocks. Pinned objects keep
 * their address (D34); those blocks stay on g_from as SPACE_FROM.
 *
 * AVANT_NURSERY_DISCARD=0 parks every nursery block instead. That was
 * the Stage 17 safety net before stackmap leftover rewrite; it is not
 * the product path.
 */
static void park_nursery_from(void) {
  Block *b = g_nursery;
  g_nursery = NULL;
  while (b) {
    Block *next = b->next;
    if (g_saw_pinned_nursery) {
      (void)block_keep_pinned(b);
    }
    b->space = SPACE_FROM;
    b->next = g_from;
    g_from = b;
    b = next;
  }
}

static void discard_nursery_from(void) {
  Block *b = g_nursery;
  g_nursery = NULL;
  /* Pins are roots, so a young copy already saw every pinned nursery
   * object. Walking every dead object to recheck the flag was most of
   * a calculator collection: the tree is garbage and nothing is pinned. */
  if (!g_saw_pinned_nursery) {
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
      b->next = g_from;
      g_from = b;
    } else {
      gc_cache_or_free_block(b);
    }
    b = next;
  }
}

static void clear_mark_flag_one(AvantHeader *h, Block *b) {
  (void)b;
  h->flags &= (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
}

static void clear_mark_flags(void) {
  Block *b;
  Large *L;
  for (b = g_mature; b; b = b->next) {
    gc_foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_nursery; b; b = b->next) {
    gc_foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_from; b; b = b->next) {
    gc_foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_to; b; b = b->next) {
    gc_foreach_block_object(b, clear_mark_flag_one);
  }
  for (L = g_large; L; L = L->next) {
    L->header.flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_LOS_KEEP);
  }
}

static void adopt_to_space(void) {
  Block *head = g_to;
  Block *b = g_to;
  g_to = NULL;
  while (b) {
    Block *next = b->next;
    b->space = SPACE_NURSERY;
    b->next = g_nursery;
    g_nursery = b;
    b = next;
  }
  gc_clear_alloc_cur(SPACE_TO);
  gc_clear_alloc_cur(SPACE_NURSERY);
  if (head && head->bump < head->base + BLOCK_SIZE) {
    g_alloc_cur[SPACE_NURSERY] = head;
    g_alloc_fresh[SPACE_NURSERY] = 1;
    gc_use_block(head);
  }
}

void gc_young_copy_out(int tenure) {
  g_force_tenure = tenure;
  g_to_bytes = 0;
  g_keep_n = 0;
  gc_zero_line_marks(g_nursery);
  gc_gen_young_copy();
  if (g_verify) {
    gc_verify_young_copy();
  }
  gc_clear_alloc_cur(SPACE_NURSERY);
  if (g_discard_from) {
    discard_nursery_from();
  } else {
    park_nursery_from();
  }
  adopt_to_space();
  gc_rem_commit_kept();
  if (g_nursery_held + g_to_bytes > g_cpy_peak) {
    g_cpy_peak = g_nursery_held + g_to_bytes;
  }
  g_nursery_held = g_to_bytes;
  g_to_bytes = 0;
  g_force_tenure = 0;
  gc_reclaim_young_large();
  clear_mark_flags();
}
