#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AVANT_GC_VERIFY (Stage 28 W12, moved from avant_rt.c): independent
 * checks after a young collection's trace and copy (gc_verify_sticky: no
 * reachable young object left unpromoted, no reachable line about to be
 * freed, no reference to a moved object's old copy), before every young
 * collection (gc_verify_remembered: the generational invariant), at
 * every collection's start (gc_verify_no_stale_marks) and after a
 * defragmenting full collection (no reference to a block being freed).
 * Diagnostics only: they print and never change the heap. (E's checks
 * after its young copy, gc_verify_young_copy with its stale, lost and
 * untraced reports, went with E in Stage 28 W15d.) */

static void verify_trace(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  AvantHeader *h = avant_header(start);
  /* A moved object is reached through its copy: its own header holds the
   * forward, not a size or a type (gc-design.md §3.2). */
  if (h->flags & AVANT_FLAG_FORWARDED) {
    start = gc_forward_of(h);
    h = avant_header(start);
  }
  if (h->flags & AVANT_FLAG_VSEEN) {
    return;
  }
  h->flags |= AVANT_FLAG_VSEEN;
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
    avant_header(gc_heap.vnoted[i])->flags &= (uint16_t)~AVANT_FLAG_VSEEN;
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
 * copied objects. The visited bit is VSEEN (the verifier's own);
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

/* AVANT_GC_VERIFY after an S young collection's trace, before its sweep
 * (Stage 28 W15b, gc-design.md §3.1): independent reachability from the
 * roots and the remembered set (verify_mark_reachable, which ignores the
 * collector's own tracing rules), then for every object it reaches:
 * a block object must no longer be young (the collection promoted it in
 * place) and, in a young block, every line it covers must be marked live
 * at the current epoch, or the sweep is about to free it; a young large
 * object must be marked (gc_reclaim_young_large then promotes it, else it
 * is freed). Each report names the object; the count is
 * gc_heap.verify_untraced. */
#if GC_STICKY_COPY
/* S+: a reference the collection did not rewrite names a moved object's
 * old copy (its header holds the forward). */
static uint64_t verify_stale(void *p, const char *where, void *holder) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start || !(avant_header(start)->flags & AVANT_FLAG_FORWARDED)) {
    return 0;
  }
  fprintf(stderr, "avant gc verify: stale %s reference holder=%p names moved object %p (now %p) coll=%llu\n",
          where, holder, start, gc_forward_of(avant_header(start)),
          (unsigned long long)avant_g_stats.collections);
  return 1;
}

static void verify_stale_tls(void **slot) {
  gc_heap.verify_untraced += verify_stale(*slot, "runtime root", (void *)slot);
}
#endif

void gc_verify_sticky(void) {
  size_t i;
  uint64_t bad = 0;
  verify_mark_reachable();
#if GC_STICKY_COPY
  /* After S+'s copy and fix-up (before the old copies lose their start
   * bits): no root slot, runtime-held slot or field of a reachable object
   * may still name a moved object; verify_mark_reachable followed the
   * forwards, so the reachable objects are the copies. */
  {
    ThreadReg *t;
    for (t = gc_heap.threads; t; t = t->next) {
      Frame *f;
      for (f = t->head ? *t->head : NULL; f; f = f->prev) {
        uint32_t k;
        for (k = 0; k < f->n; k++) {
          bad += verify_stale(*f->slots[k], "root", (void *)f->slots[k]);
          /* A slot location inside a moved object (Stage 28 W15d). */
          bad += verify_stale((void *)f->slots[k], "root location", (void *)f);
        }
      }
    }
    gc_each_runtime_root(verify_stale_tls);
    for (i = 0; i < gc_heap.vnoted_n; i++) {
      void *v = gc_heap.vnoted[i];
      if (avant_header(v)->type_id == AVANT_TYPE_BYTES) {
        continue;
      }
      EACH_REF_SLOT(v, slot, { bad += verify_stale(*slot, "field", v); });
    }
  }
#endif
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    AvantHeader *h = avant_header(v);
    Block *b = NULL;
    AvantHeader *ol = NULL;
    const char *why = NULL;
    if (!gc_is_heap_payload(v, &b, &ol)) {
      continue;
    }
    if (ol) {
      if ((h->flags & AVANT_FLAG_YOUNG) && !(h->flags & AVANT_FLAG_MARKED)) {
        why = "young large object not marked";
      }
    } else if (h->flags & AVANT_FLAG_YOUNG) {
      why = "young object not promoted";
    } else if (b->young_hi) {
      size_t off = (size_t)((uint8_t *)h - b->base);
      size_t l;
      size_t end = (off + gc_obj_total(h) - 1) / LINE_SIZE;
      for (l = off / LINE_SIZE; l <= end && l < LINES_PER_BLOCK; l++) {
        if (!line_live(b, (int)l)) {
          why = "line of a reachable object not live";
          break;
        }
      }
    }
    if (why) {
      if (bad++ < 32) {
        fprintf(stderr, "avant gc verify: %s: %p flags=0x%x type=%u size=%u coll=%llu\n", why, v,
                h->flags, (unsigned)h->type_id, (unsigned)h->size,
                (unsigned long long)avant_g_stats.collections);
      }
    }
  }
  if (bad > 32) {
    fprintf(stderr, "avant gc verify: %llu reachable objects in all at coll=%llu\n",
            (unsigned long long)bad, (unsigned long long)avant_g_stats.collections);
  }
  gc_heap.verify_untraced += bad;
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
 * Visited is VSEEN (verify_trace), named is MARKED; both are clear
 * when a collection starts (gc_verify_no_stale_marks) and cleared here.
 * The named objects are gc_heap.rnamed; the reports are counted in
 * gc_heap.missing_barriers. */

static int verify_young_at(void *start, Block *b, AvantHeader *ol) {
  return gc_young_at(start, b, ol);
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

/* C36's check (W14a2): when a collection starts, no live object's
 * header carries MARKED, FORWARDED or VSEEN (no walk reaches a moved
 * object: their blocks are released, or keep only their survivors' start
 * bits). A young collection's marks end with it
 * (the noted list) and a full collection marks in the side bitmap, so a
 * header bit left over is a mark a later young collection would read as
 * "already scanned this cycle" and skip (R1'). Reports are counted in
 * gc_heap.stale_seen. */

static void verify_no_stale_one(AvantHeader *h, Block *b) {
  if (h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_VSEEN)) {
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
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    gc_foreach_block_object(b, verify_no_stale_one);
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
          where, at, w, start, start ? gc_forwarded(start) : NULL,
          (unsigned long long)avant_g_stats.collections);
}

static void verify_defrag_fields(AvantHeader *h, Block *b) {
  void *p;
  /* A moved object (forwarded: its fields are read in its copy) and the
   * blocks being freed are not holders. */
  if (!full_marked(h) || (h->flags & AVANT_FLAG_FORWARDED) ||
      (b && gc_block_set_has(&gc_heap.def_release, b))) {
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
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_defrag_fields(lh, NULL);
  }
}
