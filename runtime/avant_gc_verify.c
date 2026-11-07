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

int g_verify;

static int verify_ptr(void *p, const char *where, void *holder, long off) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return 0;
  }
  void *to = fwd_get(start);
  if (to && to != start) {
    AvantHeader *ch = avant_header(start);
    fprintf(stderr,
            "avant gc verify: stale %s reference holder=%p off=%ld "
            "points at from-space %p (forwarded to %p) coll=%llu "
            "child_flags=0x%x\n",
            where, holder, off, start, to,
            (unsigned long long)g_stats.collections, ch->flags);
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
            where, holder, off, start, (unsigned long long)g_stats.collections,
            avant_header(start)->flags);
    return 1;
  }
  return 0;
}

/* The objects verify_trace flagged FORWARDED (its visited bit), cleared
 * when gc_verify_young_copy ends, before the young copy discards
 * from-space (W14a2: no whole-heap clearing walk follows any more). */
static void **g_vnoted;
static size_t g_vnoted_n;
static size_t g_vnoted_cap;

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
  if (g_vnoted_n == g_vnoted_cap) {
    g_vnoted_cap = g_vnoted_cap ? g_vnoted_cap * 2 : 1024;
    g_vnoted = gc_xrealloc(g_vnoted, g_vnoted_cap * sizeof(void *));
  }
  g_vnoted[g_vnoted_n++] = start;
  work_push(start);
}

static void verify_clear_traced(void) {
  size_t i;
  for (i = 0; i < g_vnoted_n; i++) {
    avant_header(g_vnoted[i])->flags &= (uint16_t)~AVANT_FLAG_FORWARDED;
  }
  g_vnoted_n = 0;
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
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_trace(*f->slots[i]);
      }
    }
  }
  for (PinEnt *p = g_pins; p; p = p->next) {
    verify_trace(p->payload);
  }
  {
    uint32_t ri;
    for (ri = 0; ri < g_rem_n; ri++) {
      verify_trace(g_remset[ri]);
    }
  }
  avant_hash_walk_tls(verify_tls_slot);
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    EACH_REF_SLOT(p, slot, { verify_trace(*slot); });
  }
}

/* A word the type map does not call a reference, in an object the
 * verifier reached, that is exactly the start of an unpinned object in
 * from-space (moved or about to be discarded): the program stored a
 * reference where the collector does not look (a wrong or missing type
 * map, audit-abi 8.1). Heap strings (STRMETA) hold text and are
 * skipped; a scalar equal to such an address by chance is the only
 * false positive. */
static uint64_t g_verify_untraced;

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
    if (!is_heap_payload(w, &cb, NULL) || !cb || cb->space != SPACE_NURSERY) {
      continue;
    }
    if (avant_header(w)->flags & AVANT_FLAG_PINNED) {
      continue;
    }
    if (gc_type_word_is_ref(h, i)) {
      continue;
    }
    to = fwd_get(w);
    g_verify_untraced++;
    fprintf(stderr,
            "avant gc verify: untraced reference holder=%p type=%u size=%u "
            "word=%lu names young object %p (%s %p) coll=%llu\n",
            (void *)p, (unsigned)h->type_id, (unsigned)h->size, (unsigned long)i, w,
            to && to != w ? "forwarded to" : "not copied, from-space", to,
            (unsigned long long)g_stats.collections);
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

static uint64_t g_verify_left_seen;

void gc_verify_young_copy(void) {
  ThreadReg *t;
  Block *b;
  if (g_c_stack_left > g_verify_left_seen) {
    fprintf(stderr,
            "avant gc verify: %llu stack words still name from-space "
            "after rewrite (cumulative %llu)\n",
            (unsigned long long)(g_c_stack_left - g_verify_left_seen),
            (unsigned long long)g_c_stack_left);
    g_verify_left_seen = g_c_stack_left;
  }
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_ptr(*f->slots[i], "root", (void *)f->slots[i], (long)i);
      }
    }
  }
  verify_mark_reachable();
  for (b = g_to; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  for (b = g_mature; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  for (b = g_from; b; b = b->next) {
    gc_foreach_block_object(b, verify_obj_fields);
  }
  if (g_saw_pinned_nursery) {
    for (b = g_nursery; b; b = b->next) {
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

/* C36's check (W14a2): when a collection starts, no object's header
 * carries MARKED or FORWARDED. A young collection's marks end with it
 * (the noted list) and a full collection marks in the side bitmap, so a
 * header bit left over is a mark a later young collection would read as
 * "already scanned this cycle" and skip (R1'). */
static uint64_t g_stale_seen;

static void verify_no_stale_one(AvantHeader *h, Block *b) {
  if (h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED)) {
    if (g_stale_seen++ < 16) {
      fprintf(stderr,
              "avant gc verify: stale mark on %p flags=0x%x space=%d at the start of coll=%llu\n",
              payload_of(h), h->flags, b ? (int)b->space : -1,
              (unsigned long long)g_stats.collections);
    }
  }
}

void gc_verify_no_stale_marks(void) {
  Block *b;
  LargeIter it;
  AvantHeader *lh;
  Block *lists[4];
  int k;
  lists[0] = g_mature;
  lists[1] = g_nursery;
  lists[2] = g_from;
  lists[3] = g_to;
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
    if (!(ob && gc_block_set_has(&g_def_release, ob))) {
      return;
    }
  } else if (!(b && gc_block_set_has(&g_def_release, b)) &&
             !(hb && gc_block_set_has(&g_def_release, hb))) {
    return;
  }
  fprintf(stderr,
          "avant gc verify: %s %p names %p in a mature block defragmentation "
          "is freeing (object %p moved to %p) coll=%llu\n",
          where, at, w, start, start ? fwd_get(start) : NULL,
          (unsigned long long)g_stats.collections);
}

static void verify_defrag_fields(AvantHeader *h, Block *b) {
  void *p;
  if (!full_marked(h) || (b && gc_block_set_has(&g_def_release, b))) {
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
  uint8_t *hi = g_self_reg.stack_hi;
  uint8_t *lo = g_self_reg.stack_lo;
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
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        verify_defrag_word(*f->slots[i], "root slot", (void *)f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
  for (PinEnt *pe = g_pins; pe; pe = pe->next) {
    verify_defrag_word(pe->payload, "pin", (void *)pe);
  }
  /* A remembered entry may name a dead object (full collections do not
   * scrub the set, and the young copy skips an entry whose block is
   * gone); only a live one that moved must have been rewritten. */
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    Block *rb = obj ? block_of(avant_header(obj)) : NULL;
    if (rb && gc_block_set_has(&g_def_release, rb) && is_heap_payload(obj, NULL, NULL) &&
        full_marked(avant_header(obj))) {
      verify_defrag_word(obj, "remembered entry", (void *)&g_remset[i]);
    }
  }
  avant_hash_walk_tls(verify_defrag_tls);
  for (b = g_mature; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  for (b = g_nursery; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  for (b = g_from; b; b = b->next) {
    gc_foreach_block_object(b, verify_defrag_fields);
  }
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_defrag_fields(lh, NULL);
  }
}
