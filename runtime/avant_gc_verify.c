#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AVANT_GC_VERIFY (Stage 28 W12, moved from avant_rt.c): independent
 * checks after a young copy (no reachable reference to from-space, no
 * untraced reference word, no lost nursery object) and after a
 * defragmenting full collection (no reference to a block being freed).
 * Diagnostics only: they print and never change the heap. */

static int verify_ptr(void *p, const char *where, void *holder, long off) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return 0;
  }
  void *to = gc_fwd_get(start);
  if (to && to != start) {
    AvantHeader *ch = avant_header(start);
    fprintf(stderr,
            "avant gc verify: stale %s reference holder=%p off=%ld "
            "points at from-space %p (forwarded to %p) coll=%llu "
            "child_flags=0x%x\n",
            where, holder, off, start, to,
            (unsigned long long)avant_g_stats.collections, ch->flags);
    return 1;
  }
  /* C29: a reachable reference to a nursery object the copy neither
   * moved nor kept in place. Its block is discarded after this check,
   * so the reference is about to dangle (a live holder the young copy
   * did not trace, or a missing barrier). */
  if (!to && (avant_header(start)->flags & AVANT_FLAG_PINNED) == 0) {
    fprintf(stderr,
            "avant gc verify: lost %s reference holder=%p off=%ld "
            "names nursery object %p the young copy did not keep coll=%llu "
            "child_flags=0x%x\n",
            where, holder, off, start, (unsigned long long)avant_g_stats.collections,
            avant_header(start)->flags);
    return 1;
  }
  return 0;
}

static void verify_trace(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_FORWARDED) {
    return;
  }
  h->flags |= AVANT_FLAG_FORWARDED;
  if (gc_heap.vnoted_n == gc_heap.vnoted_cap) {
    gc_heap.vnoted_cap = gc_heap.vnoted_cap ? gc_heap.vnoted_cap * 2 : 1024;
    gc_heap.vnoted = gc_xrealloc(gc_heap.vnoted, gc_heap.vnoted_cap * sizeof(void *));
  }
  gc_heap.vnoted[gc_heap.vnoted_n++] = start;
  gc_work_push(start);
}

static void verify_clear_traced(void) {
  size_t i;
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    avant_header(gc_heap.vnoted[i])->flags &= (uint16_t)~AVANT_FLAG_FORWARDED;
  }
  gc_heap.vnoted_n = 0;
}

static void verify_tls_slot(void **slot) {
  verify_trace(*slot);
}

/* Independent reachability, ignoring the collector's own tracing
 * gates: a holder counts as live when a root reaches it through any
 * field. This is what keeps the checker sensitive to the F1 class
 * (a live holder the young copy itself never traced) while staying
 * silent about dead mature objects, whose fields legitimately name
 * copied objects. FORWARDED is free here (no collector pass uses it);
 * verify_clear_traced resets it before gc_verify_young_copy returns. */
static void verify_mark_reachable(void) {
  ThreadReg *t;
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_trace(*f->slots[i]);
      }
    }
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    verify_trace(p->payload);
  }
  {
    uint32_t ri;
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      verify_trace(gc_heap.remset[ri]);
    }
  }
  gc_each_runtime_root(verify_tls_slot);
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    EACH_REF_SLOT(p, slot, { verify_trace(*slot); });
  }
}

static void verify_untraced_words(AvantHeader *h) {
  size_t n, i;
  void **p;
  if (h->flags & AVANT_FLAG_STRMETA) {
    return;
  }
  if (h->type_id == AVANT_TYPE_PTRS) {
    return;
  }
  p = (void **)payload_of(h);
  n = (size_t)h->size / sizeof(void *);
  for (i = 0; i < n; i++) {
    void *w = p[i];
    Block *cb = NULL;
    void *to;
    if (!w || ((uintptr_t)w & (ALIGN - 1)) != 0) {
      continue;
    }
    if (!gc_is_heap_payload(w, &cb, NULL) || !cb || cb->space != SPACE_NURSERY) {
      continue;
    }
    if (avant_header(w)->flags & AVANT_FLAG_PINNED) {
      continue;
    }
    if (gc_type_word_is_ref(h, i)) {
      continue;
    }
    to = gc_fwd_get(w);
    gc_heap.verify_untraced++;
    fprintf(stderr,
            "avant gc verify: untraced reference holder=%p type=%u size=%u "
            "word=%lu names young object %p (%s %p) coll=%llu\n",
            (void *)p, (unsigned)h->type_id, (unsigned)h->size, (unsigned long)i, w,
            to && to != w ? "forwarded to" : "not copied, from-space", to,
            (unsigned long long)avant_g_stats.collections);
  }
}

static void verify_obj_fields(AvantHeader *h, Block *b) {
  if (!(h->flags & AVANT_FLAG_FORWARDED)) {
    return;
  }
  void *p = payload_of(h);
  verify_untraced_words(h);
  EACH_REF_SLOT(p, slot, {
    if (verify_ptr(*slot, "field", p, (long)((uint8_t *)slot - (uint8_t *)p))) {
      fprintf(stderr, "avant gc verify: holder %p flags=0x%x space=%d\n", p, h->flags,
              b ? (int)b->space : -1);
    }
  });
}

/* Pinned nursery objects stay in their block; their fields are checked
 * like a to-space object's (C29). */
static void verify_pinned_fields(AvantHeader *h, Block *b) {
  if (h->flags & AVANT_FLAG_PINNED) {
    verify_obj_fields(h, b);
  }
}

void gc_verify_young_copy(void) {
  ThreadReg *t;
  Block *b;
  if (gc_heap.ctr.stack_left > gc_heap.verify_left_seen) {
    fprintf(stderr,
            "avant gc verify: %llu stack words still name from-space "
            "after rewrite (cumulative %llu)\n",
            (unsigned long long)(gc_heap.ctr.stack_left - gc_heap.verify_left_seen),
            (unsigned long long)gc_heap.ctr.stack_left);
    gc_heap.verify_left_seen = gc_heap.ctr.stack_left;
  }
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_ptr(*f->slots[i], "root", (void *)f->slots[i], (long)i);
      }
    }
  }
  verify_mark_reachable();
  for (b = gc_heap.to; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  for (b = gc_heap.mature; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  for (b = gc_heap.from; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  if (gc_heap.saw_pinned_nursery) {
    for (b = gc_heap.nursery; b; b = b->next) {
      gc_foreach_block_object(b, verify_pinned_fields);
    }
  }
  {
    LargeIter it;
    AvantHeader *lh;
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      verify_obj_fields(lh, NULL);
    }
  }
  verify_clear_traced();
}

/* The remembered-set check (Stage 28 W14b1, gc-design.md §4.2 item 2):
 * before a young collection traces, every field of a non-young object
 * that names a young object must belong to a remembered holder or to an
 * object the three-way rule R1' (§1.3) names this cycle: a root (slot,
 * pin, runtime TLS slot), a field of a young object, or a field of a
 * remembered object. Those three are computed by a first pass over the
 * roots, the young objects and the remembered set. Only holders the
 * roots reach are checked: a dead object's fields are never read, and
 * may name a nursery address a new young object reuses. Reported as
 * "missing barrier: holder ... field ... names young ...": the next
 * young collection would neither scan that field nor update it, so the
 * young object is lost or the field dangles after the copy (C34's
 * class, reported at the first collection after the bad store). The
 * five runtime builders that fill a born-mature buffer named by a young
 * array header (audit N2b) are not reported: R1'(b) names the buffer.
 * Visited is FORWARDED (verify_trace), named is MARKED; both are clear
 * when a collection starts (gc_verify_no_stale_marks) and cleared here.
 * The named objects are gc_heap.rnamed; the reports are counted in
 * gc_heap.missing_barriers. */

static int verify_young_at(void *start, Block *b, AvantHeader *ol) {
  if (ol) {
    return (avant_header(start)->flags & AVANT_FLAG_YOUNG) != 0;
  }
  return b && (b->space == SPACE_NURSERY || b->space == SPACE_TO);
}

static int verify_names_young(void *p, void **out) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  *out = start;
  return start && verify_young_at(start, b, ol);
}

static void rem_named(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start || verify_young_at(start, b, ol)) {
    return;
  }
  h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  if (gc_heap.rnamed_n == gc_heap.rnamed_cap) {
    gc_heap.rnamed_cap = gc_heap.rnamed_cap ? gc_heap.rnamed_cap * 2 : 1024;
    gc_heap.rnamed = gc_xrealloc(gc_heap.rnamed, gc_heap.rnamed_cap * sizeof(void *));
  }
  gc_heap.rnamed[gc_heap.rnamed_n++] = start;
}

static void rem_root_slot(void **slot) {
  verify_trace(*slot);
  rem_named(*slot);
}

void gc_verify_remembered(void) {
  ThreadReg *t;
  size_t i;
  uint64_t found = 0;
  /* Roots: reachability and R1'(a). */
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t k;
      for (k = 0; k < f->n; k++) {
        rem_root_slot(f->slots[k]);
      }
    }
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    rem_root_slot(&p->payload);
  }
  gc_each_runtime_root(rem_root_slot);
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    EACH_REF_SLOT(p, slot, { verify_trace(*slot); });
  }
  /* R1'(b): the fields of the young objects the roots reach. */
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    void *st;
    if (verify_names_young(v, &st)) {
      EACH_REF_SLOT(v, slot, { rem_named(*slot); });
    }
  }
  /* R1'(c): the fields of every remembered object. */
  {
    uint32_t ri;
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      void *obj = gc_heap.remset[ri];
      if (gc_is_heap_payload(obj, NULL, NULL)) {
        EACH_REF_SLOT(obj, slot, { rem_named(*slot); });
      }
    }
  }
  /* The check: a reached non-young holder that is neither remembered
   * nor named must name no young object. */
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    void *st;
    AvantHeader *h = avant_header(v);
    if (verify_names_young(v, &st) || (h->flags & (AVANT_FLAG_REM | AVANT_FLAG_MARKED))) {
      continue;
    }
    EACH_REF_SLOT(v, slot, {
      void *y;
      if (verify_names_young(*slot, &y)) {
        found++;
        if (found <= 32) {
          Block *hb = block_of(h);
          fprintf(stderr,
                  "avant gc verify: missing barrier: holder %p field %ld names young %p "
                  "(holder flags=0x%x type=%u space=%d; young flags=0x%x) before coll=%llu\n",
                  v, (long)((uint8_t *)slot - (uint8_t *)v), y, h->flags, (unsigned)h->type_id,
                  hb ? (int)hb->space : -1, avant_header(y)->flags,
                  (unsigned long long)avant_g_stats.collections);
        }
      }
    });
  }
  if (found > 32) {
    fprintf(stderr, "avant gc verify: missing barrier: %llu fields in all before coll=%llu\n",
            (unsigned long long)found, (unsigned long long)avant_g_stats.collections);
  }
  gc_heap.missing_barriers += found;
  for (i = 0; i < gc_heap.rnamed_n; i++) {
    avant_header(gc_heap.rnamed[i])->flags &= (uint16_t)~AVANT_FLAG_MARKED;
  }
  gc_heap.rnamed_n = 0;
  verify_clear_traced();
}

/* C36's check (W14a2): when a collection starts, no object's header
 * carries MARKED or FORWARDED. A young collection's marks end with it
 * (the noted list) and a full collection marks in the side bitmap, so a
 * header bit left over is a mark a later young collection would read as
 * "already scanned this cycle" and skip (R1'). Reports are counted in
 * gc_heap.stale_seen. */

static void verify_no_stale_one(AvantHeader *h, Block *b) {
  if (h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED)) {
    if (gc_heap.stale_seen++ < 16) {
      fprintf(stderr,
              "avant gc verify: stale mark on %p flags=0x%x space=%d at the start of coll=%llu\n",
              payload_of(h), h->flags, b ? (int)b->space : -1,
              (unsigned long long)avant_g_stats.collections);
    }
  }
}

void gc_verify_no_stale_marks(void) {
  Block *b;
  LargeIter it;
  AvantHeader *lh;
  Block *lists[4];
  int k;
  lists[0] = gc_heap.mature;
  lists[1] = gc_heap.nursery;
  lists[2] = gc_heap.from;
  lists[3] = gc_heap.to;
  for (k = 0; k < 4; k++) {
    for (b = lists[k]; b; b = b->next) {
      gc_foreach_block_object(b, verify_no_stale_one);
    }
  }
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_no_stale_one(lh, NULL);
  }
}

/* AVANT_GC_VERIFY after defragmentation (C31): no reference the
 * program or the runtime can still use names a block about to be
 * freed: root slots, pins, the remembered set, the fields of marked
 * objects, and the words of the collecting thread's stack from this
 * function's frame up (the C frames of the runtime helper that
 * collected and of the generated code that called it). */
static void verify_defrag_word(void *w, const char *where, void *at) {
  Block *b = block_of(w);
  Block *hb = w ? block_of((uint8_t *)w - sizeof(AvantHeader)) : NULL;
  void *start = gc_object_payload(w, NULL, NULL);
  /* A word that names an object is judged by the object's block, the
   * block of its header: the payload of an empty object that ends a
   * block is the next block's first byte, and since W14a1 that next
   * block is a real neighbour in the reservation, not a stranger. A
   * word that names no object keeps the two-block test. */
  if (start) {
    Block *ob = block_of(avant_header(start));
    if (!(ob && gc_block_set_has(&gc_heap.def_release, ob))) {
      return;
    }
  } else if (!(b && gc_block_set_has(&gc_heap.def_release, b)) &&
             !(hb && gc_block_set_has(&gc_heap.def_release, hb))) {
    return;
  }
  fprintf(stderr,
          "avant gc verify: %s %p names %p in a mature block defragmentation "
          "is freeing (object %p moved to %p) coll=%llu\n",
          where, at, w, start, start ? gc_fwd_get(start) : NULL,
          (unsigned long long)avant_g_stats.collections);
}

static void verify_defrag_fields(AvantHeader *h, Block *b) {
  void *p;
  if (!full_marked(h) || (b && gc_block_set_has(&gc_heap.def_release, b))) {
    return;
  }
  p = payload_of(h);
  EACH_REF_SLOT(p, slot, { verify_defrag_word(*slot, "field", (void *)slot); });
}

static void verify_defrag_tls(void **slot) {
  verify_defrag_word(*slot, "runtime TLS slot", (void *)slot);
}

__attribute__((noinline)) void gc_verify_defrag(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = gc_thread.reg.stack_hi;
  uint8_t *lo = gc_thread.reg.stack_lo;
  ThreadReg *t;
  Block *b;
  LargeIter it;
  AvantHeader *lh;
  uint32_t i;
  if (hi && p && p < hi && !(lo && p < lo)) {
    p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
    for (; p + sizeof(void *) <= hi; p += ALIGN) {
      verify_defrag_word(*(void **)p, "C stack word at", (void *)p);
    }
  }
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        verify_defrag_word(*f->slots[i], "root slot", (void *)f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  for (PinEnt *pe = gc_heap.pins; pe; pe = pe->next) {
    verify_defrag_word(pe->payload, "pin", (void *)pe);
  }
  /* A remembered entry may name a dead object (full collections do not
   * scrub the set, and the young copy skips an entry whose block is
   * gone); only a live one that moved must have been rewritten. */
  for (i = 0; i < gc_heap.rem_n; i++) {
    void *obj = gc_heap.remset[i];
    Block *rb = obj ? block_of(avant_header(obj)) : NULL;
    if (rb && gc_block_set_has(&gc_heap.def_release, rb) && gc_is_heap_payload(obj, NULL, NULL) &&
        full_marked(avant_header(obj))) {
      verify_defrag_word(obj, "remembered entry", (void *)&gc_heap.remset[i]);
    }
  }
  gc_each_runtime_root(verify_defrag_tls);
  for (b = gc_heap.mature; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  for (b = gc_heap.nursery; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  for (b = gc_heap.from; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_defrag_fields(lh, NULL);
  }
}
