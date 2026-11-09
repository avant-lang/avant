#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
  EACH_TASK_ROOT(slot, verify_trace(*slot));
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
      /* S+'s aged survivors (YOUNG_AGE 2) stay young in survivor
       * blocks, with SURVIVED. */
      if (!((h->flags & AVANT_FLAG_SURVIVED) && b->kept == SPLUS_SURV)) {
        why = "young object not promoted";
      }
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

/* The lost-reference check (Stage 28 W16b; R11, O107, gc-design.md
 * section 4.3): a reference the program can still use that names heap
 * memory where no object lives. It replaces the conservative C-stack
 * cross-check (the words of the collecting thread's stack naming a block
 * defragmentation frees), which could not tell a live reference from a
 * dead copy and reported dead spills by the ten thousand once both
 * C-stack scans were off. Two halves:
 *   - precise (verify_lost): before every collection, every root slot,
 *     pin, runtime root and field of an object the roots reach must name
 *     a live object (its exact start or an interior address) or lie
 *     outside the heap: a value in the committed heap that resolves to
 *     no object names freed lines, a freed block, a freed large run or
 *     a returned chunk ("lost reference: ... names freed heap memory");
 *   - on use (gc_quarantine_*, below): the blocks a collection frees are
 *     kept out of use and PROT_NONE until the next collection begins, so
 *     a reference runtime C kept in a C local across a collecting call
 *     (an unrooted operand that S+ moved, a fresh buffer the collection
 *     freed) faults at its first read or write and is reported with the
 *     address and the collection that freed the block; the block is then
 *     opened again and the program goes on. A dead copy on the stack is
 *     never read, so it is never reported: the check is silent on a
 *     correct program by construction.
 * Diagnostics only: both report, neither repairs. Counted in
 * gc_heap.verify_lost. */
static uint64_t verify_lost(void *p, const char *where, void *holder) {
  uintptr_t off;
  if (!p || !heap_offset(p, &off) || gc_object_payload(p, NULL, NULL)) {
    return 0;
  }
  if (gc_heap.verify_lost++ < 32) {
    fprintf(stderr,
            "avant gc verify: lost reference: %s %p names freed heap memory %p before coll=%llu\n",
            where, holder, p, (unsigned long long)avant_g_stats.collections);
  }
  return 1;
}

static void lost_root_slot(void **slot) {
  verify_lost(*slot, "root", (void *)slot);
  verify_trace(*slot);
}

/* Before a full collection (a young one runs the precise half inside
 * gc_verify_remembered's pass over the same roots and fields). */
void gc_verify_lost(void) {
  ThreadReg *t;
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t k;
      for (k = 0; k < f->n; k++) {
        lost_root_slot(f->slots[k]);
      }
    }
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    lost_root_slot(&p->payload);
  }
  EACH_TASK_ROOT(slot, lost_root_slot(slot));
  gc_each_runtime_root(lost_root_slot);
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    EACH_REF_SLOT(p, slot, {
      verify_lost(*slot, "field of", p);
      verify_trace(*slot);
    });
  }
  verify_clear_traced();
}

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

/* Is H a carded object the young collection scans by its marked cards
 * (in the set as REMC, without CARD_ALL)? */
static int rem_by_cards(const AvantHeader *h) {
  return gc_carded(h) && (h->flags & AVANT_FLAG_REMC) && !(h->flags & AVANT_FLAG_CARD_ALL);
}

static void rem_root_slot(void **slot) {
  verify_lost(*slot, "root", (void *)slot);
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
  EACH_TASK_ROOT(slot, rem_root_slot(slot));
  gc_each_runtime_root(rem_root_slot);
  while (gc_heap.work_n) {
    void *p = gc_heap.work[--gc_heap.work_n];
    EACH_REF_SLOT(p, slot, {
      verify_lost(*slot, "field of", p);
      verify_trace(*slot);
    });
  }
  /* R1'(b): the fields of the young objects the roots reach. */
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    void *st;
    if (verify_names_young(v, &st)) {
      EACH_REF_SLOT(v, slot, { rem_named(*slot); });
    }
  }
  /* R1'(c): the fields of every remembered object; of a carded one
   * (REMC, Stage 28 W16t), the slots of its marked cards, which are all
   * the young collection scans of it, unless every card counts
   * (CARD_ALL). */
  {
    uint32_t ri;
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      void *obj = gc_heap.remset[ri];
      if (gc_is_heap_payload(obj, NULL, NULL)) {
        if (rem_by_cards(avant_header(obj))) {
          EACH_CARD_SLOT(obj, slot, { rem_named(*slot); });
        } else {
          EACH_REF_SLOT(obj, slot, { rem_named(*slot); });
        }
      }
    }
  }
  /* The check: a reached non-young holder that is neither remembered
   * nor named must name no young object. A carded one remembered by its
   * cards (REMC without CARD_ALL) must name young objects only from
   * slots of its marked cards (the card invariant, Stage 28 W16t):
   * a young object named from a clear card is reported as a missing
   * barrier too, since the young collection would not scan that slot. */
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    void *st;
    AvantHeader *h = avant_header(v);
    int by_cards;
    if (verify_names_young(v, &st) || (h->flags & (AVANT_FLAG_REM | AVANT_FLAG_MARKED))) {
      continue;
    }
    by_cards = (h->flags & AVANT_FLAG_REMC) != 0;
    if (by_cards && !rem_by_cards(h)) {
      continue;
    }
    EACH_REF_SLOT(v, slot, {
      void *y;
      if ((!by_cards || !gc_heap.cards[gc_card_index(slot)]) && verify_names_young(*slot, &y)) {
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
  if (h->flags & (AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_VSEEN | AVANT_FLAG_PROMOTED_NOW |
                  AVANT_FLAG_CARD_SCAN)) {
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
 * freed: root slots, pins, the remembered set and the fields of marked
 * objects. (Until Stage 28 W16b it also read every word of the
 * collecting thread's stack, a conservative cross-check that reported
 * dead copies once C31's exclusion was off; a C local that is used
 * after the collection is now caught when it is used: the freed blocks
 * are quarantined, gc_quarantine_block.) */
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

void gc_verify_defrag(void) {
  ThreadReg *t;
  Block *b;
  LargeIter it;
  AvantHeader *lh;
  uint32_t i;
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
  EACH_TASK_ROOT(slot, verify_defrag_word(*slot, "task root", (void *)slot));
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

/* The on-use half of the lost-reference check (Stage 28 W16b; see
 * verify_lost above). Under AVANT_GC_VERIFY a block a collection frees
 * (an evacuated young block, a young block the sweep left empty, a
 * block defragmentation evacuated or a full sweep freed) is out of use
 * as usual (in_use 0: no lookup finds it, no list holds it) but it is
 * not reused: it is made PROT_NONE and kept on gc_heap.quarantine
 * (Block.clean QUARANTINED) until the next collection begins
 * (gc_quarantine_release), which opens it and frees it as the
 * collection would have. Correct code never touches a freed block's
 * memory; a C local that still names an object there faults, and the
 * handler below reports the address and the collection that freed it,
 * opens that block (QUARANTINE_OPENED) and returns, so the access is
 * retried and the program continues (the old bytes are still there, or
 * the stress mode's poison). A fault anywhere else goes to the action
 * that was installed before. At most QUARANTINE_MAX blocks are held
 * (past it a freed block is reused at once, unprotected); a refused
 * mprotect (the mapping-count limit) also frees the block at once.
 * Freed lines in blocks that keep live ones cannot be protected (a line
 * is 256 bytes); instead the young sweep holds such blocks back from the
 * young allocator until the next collection begins, so the precise half
 * still finds a reference to a dead object's lines naming freed memory.
 * Freed large runs are not quarantined (their reuse is page-run based;
 * the precise half reports a reference that names one). */
#define QUARANTINED 2
#define QUARANTINE_OPENED 3
#define QUARANTINE_HELD 4
#define QUARANTINE_MAX 4096

static struct sigaction quarantine_old;
static int quarantine_installed;

static void quarantine_write_hex(char *buf, size_t *n, uint64_t v) {
  static const char dig[] = "0123456789abcdef";
  char tmp[16];
  int k = 0;
  buf[(*n)++] = '0';
  buf[(*n)++] = 'x';
  do {
    tmp[k++] = dig[v & 15];
    v >>= 4;
  } while (v && k < 16);
  while (k) {
    buf[(*n)++] = tmp[--k];
  }
}

static void quarantine_write_dec(char *buf, size_t *n, uint64_t v) {
  char tmp[24];
  int k = 0;
  do {
    tmp[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && k < 24);
  while (k) {
    buf[(*n)++] = tmp[--k];
  }
}

static void quarantine_write_str(char *buf, size_t *n, const char *s) {
  while (*s) {
    buf[(*n)++] = *s++;
  }
}

static void quarantine_report(void *addr, uint64_t coll, const char *what) {
  if (gc_heap.verify_lost++ < 32) {
    char buf[256];
    size_t n = 0;
    quarantine_write_str(buf, &n, "avant gc verify: lost reference: access to ");
    quarantine_write_hex(buf, &n, (uint64_t)(uintptr_t)addr);
    quarantine_write_str(buf, &n, what);
    quarantine_write_dec(buf, &n, coll);
    quarantine_write_str(buf, &n, " (a reference kept outside the roots across a collection)\n");
    (void)!write(2, buf, n);
  }
}

/* Only async-signal-safe calls: write, mprotect, sigaction. */
static void quarantine_on_fault(int sig, siginfo_t *si, void *uc) {
  uintptr_t off;
  void *addr = si ? si->si_addr : NULL;
  uint32_t i;
  (void)uc;
  for (i = 0; addr && i < gc_heap.held_runs_n; i++) {
    struct GcHeldRun *r = &gc_heap.held_runs[i];
    if (r->state == QUARANTINED && (uint8_t *)addr >= r->lo && (uint8_t *)addr < r->hi) {
      quarantine_report(addr, gc_heap.held_runs_coll, " in a large run freed at coll=");
      r->state = QUARANTINE_OPENED;
      if (mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_READ | PROT_WRITE) == 0) {
        return;
      }
    }
  }
  if (addr && heap_offset(addr, &off) && gc_heap.chunks[off >> CHUNK_SHIFT].kind == CHUNK_SMALL) {
    Block *b = &gc_heap.blocks[off >> BLOCK_SHIFT];
    if (b->clean == QUARANTINED) {
      quarantine_report(addr, b->idle, " in a block freed at coll=");
      b->clean = QUARANTINE_OPENED;
      if (mprotect(b->base, BLOCK_SIZE, PROT_READ | PROT_WRITE) == 0) {
        return;
      }
    }
  }
  sigaction(sig, &quarantine_old, NULL);
}

static void quarantine_install(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = quarantine_on_fault;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, &quarantine_old) == 0) {
    quarantine_installed = 1;
  }
}

/* B leaves use (gc_cache_or_free_block, before its cache-or-give). 1:
 * it is quarantined, and gc_quarantine_release frees it later (every
 * caller frees blocks inside a collection). */
int gc_quarantine_block(Block *b) {
  if (!gc_heap.set.verify || gc_heap.quarantine_n >= QUARANTINE_MAX) {
    return 0;
  }
  if (!quarantine_installed) {
    quarantine_install();
    if (!quarantine_installed) {
      return 0;
    }
  }
  /* idle names the collection that freed the block while it is held
   * (gc_block_give sets it again). It is protected when the collection
   * ends (gc_quarantine_seal): the collection itself still reads the
   * headers of objects it moved out of it (gc_clear_noted). */
  b->idle = avant_g_stats.collections;
  b->clean = QUARANTINE_HELD;
  b->next = gc_heap.quarantine;
  gc_heap.quarantine = b;
  gc_heap.quarantine_n++;
  return 1;
}

/* A dead large object's run (gc_release_large, gc_sweep_large): its
 * start bit is cleared (no lookup finds it) and its pages stay allocated,
 * protected at the collection's end from the run's second page on (the
 * first page keeps the header, which the large space's iterator reads to
 * step over the run), until the next collection begins. 1: held. */
int gc_quarantine_run(AvantHeader *h) {
  uint8_t *base;
  struct GcHeldRun *r;
  if (!gc_heap.set.verify || gc_heap.held_runs_n >= sizeof gc_heap.held_runs / sizeof gc_heap.held_runs[0]) {
    return 0;
  }
  if (!quarantine_installed) {
    quarantine_install();
    if (!quarantine_installed) {
      return 0;
    }
  }
  base = (uint8_t *)h - 8;
  start_bit_set(h, 0);
  r = &gc_heap.held_runs[gc_heap.held_runs_n++];
  r->h = h;
  r->lo = base + PAGE_SIZE;
  r->hi = base + gc_large_bytes(h);
  r->state = QUARANTINE_HELD;
  gc_heap.held_runs_coll = avant_g_stats.collections;
  return 1;
}

/* At the end of a collection: the blocks and runs it freed become
 * PROT_NONE (a refused mprotect leaves one held, unprotected, until the
 * release). */
void gc_quarantine_seal(void) {
  Block *b;
  uint32_t i;
  for (i = 0; i < gc_heap.held_runs_n; i++) {
    struct GcHeldRun *r = &gc_heap.held_runs[i];
    if (r->state == QUARANTINE_HELD && r->hi > r->lo &&
        mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_NONE) == 0) {
      r->state = QUARANTINED;
    }
  }
  for (b = gc_heap.quarantine; b; b = b->next) {
    if (b->clean == QUARANTINE_HELD && mprotect(b->base, BLOCK_SIZE, PROT_NONE) == 0) {
      b->clean = QUARANTINED;
    }
  }
}

/* At the start of a collection: every held block is opened and freed,
 * and the blocks with lines the last young sweep freed become the young
 * allocator's holes (avant_gc_sticky.c holds them back under
 * AVANT_GC_VERIFY). */
void gc_quarantine_release(void) {
  Block *b = gc_heap.quarantine;
  uint32_t i;
  for (i = 0; i < gc_heap.verify_holes_n; i++) {
    gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap,
                   gc_heap.verify_holes[i]);
  }
  gc_heap.verify_holes_n = 0;
  for (i = 0; i < gc_heap.held_runs_n; i++) {
    struct GcHeldRun *r = &gc_heap.held_runs[i];
    if (r->state == QUARANTINED) {
      mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_READ | PROT_WRITE);
    }
    gc_large_give_held(r->h);
  }
  gc_heap.held_runs_n = 0;
  gc_heap.quarantine = NULL;
  gc_heap.quarantine_n = 0;
  while (b) {
    Block *next = b->next;
    if (b->clean == QUARANTINED) {
      mprotect(b->base, BLOCK_SIZE, PROT_READ | PROT_WRITE);
    }
    b->clean = 0;
    gc_free_block_now(b);
    b = next;
  }
}
