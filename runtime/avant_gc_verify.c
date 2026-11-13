#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

/* AVANT_GC_VERIFY (Stage 28 W12, moved from avant_rt.c): independent
 * checks after a young collection's trace and copy (gc_verify_sticky: no
 * reachable young object left unpromoted, no reachable line about to be
 * freed, no reference to a moved object's old copy), before every young
 * collection (gc_verify_remembered: the generational invariant, R1' and,
 * for records, the invariant itself: yholders_check), at
 * every collection's start (gc_verify_no_stale_marks) and after a
 * defragmenting full collection (no reference to a block being freed).
 * Diagnostics only: they print and never change the heap. Under
 * soft-dirty tracking (Stage 28 W18M, W18V2) a young collection's checks
 * examine only what the kernel reports written and what the verifier's
 * own record names, and fall back to the whole checks whenever they
 * cannot prove those silent (below). (E's checks
 * after its young copy, gc_verify_young_copy with its stale, lost and
 * untraced reports, went with E in Stage 28 W15d.) */

/* The verifier's own marks (Stage 28 W18V2): "traced by this check"
 * (vb_seen, the header's VSEEN until then) and "named by R1' this cycle"
 * (vb_named, the header's MARKED until then), one bit per 8-byte granule
 * of the committed heap at the object's header, like the full
 * collection's side marks. So the verifier writes no heap page, and the
 * kernel's soft-dirty record of the heap holds the program's and the
 * collector's writes alone: the incremental checks below rest on it. An
 * object outside the heap (the static image) keeps the header bit. Every
 * bit is cleared when its check ends (the noted lists), as before. */
static uint64_t *vb_seen;
static uint64_t *vb_named;
/* What the last whole T reached, and what incremental Ts visited since
 * (vr_valid; Stage 28 W18V2, the incremental checks below). */
static uint64_t *vb_reach;
static int vr_valid;
static size_t vb_words;
/* The side bits are used from the first soft-dirty clear on (vb_side);
 * until then no page record is read, and the header bits, which cost
 * nothing more than the header the check reads anyway, serve as before
 * (W18V2's tuning: the side bits cost the compiler's dump under VERIFY,
 * 55 whole verifications, 5% more time). Every bit is clear between
 * checks, so the switch happens between two checks. */
static int vb_side;

static void vb_ensure(void) {
  size_t words = (size_t)(((gc_heap.committed >> 3) + 63) / 64);
  if (words > vb_words) {
    vb_seen = gc_xrealloc(vb_seen, words * sizeof(uint64_t));
    vb_named = gc_xrealloc(vb_named, words * sizeof(uint64_t));
    vb_reach = gc_xrealloc(vb_reach, words * sizeof(uint64_t));
    memset(vb_seen + vb_words, 0, (words - vb_words) * sizeof(uint64_t));
    memset(vb_named + vb_words, 0, (words - vb_words) * sizeof(uint64_t));
    memset(vb_reach + vb_words, 0, (words - vb_words) * sizeof(uint64_t));
    vb_words = words;
  }
}

/* Sets START's bit in BITS (FLAG in its header outside the heap); 1 when
 * it was clear. */
static inline __attribute__((always_inline)) int vb_set(uint64_t *bits, void *start, uint16_t flag) {
  AvantHeader *h = avant_header(start);
  uintptr_t off;
  if (vb_side && heap_offset(h, &off)) {
    uint64_t g = (uint64_t)off >> 3;
    uint64_t m = 1ULL << (g & 63);
    if (bits[g >> 6] & m) {
      return 0;
    }
    bits[g >> 6] |= m;
    return 1;
  }
  if (h->flags & flag) {
    return 0;
  }
  h->flags |= flag;
  return 1;
}

static inline int vb_test(const uint64_t *bits, void *start, uint16_t flag) {
  AvantHeader *h = avant_header(start);
  uintptr_t off;
  if (vb_side && heap_offset(h, &off)) {
    uint64_t g = (uint64_t)off >> 3;
    return (int)((bits[g >> 6] >> (g & 63)) & 1ULL);
  }
  return (h->flags & flag) != 0;
}

static inline void vb_clear(uint64_t *bits, void *start, uint16_t flag) {
  AvantHeader *h = avant_header(start);
  uintptr_t off;
  if (vb_side && heap_offset(h, &off)) {
    uint64_t g = (uint64_t)off >> 3;
    bits[g >> 6] &= ~(1ULL << (g & 63));
  } else {
    h->flags &= (uint16_t)~flag;
  }
}

/* START (an object's start, after any forward) is traced: flagged, noted
 * and pushed, unless this check traced it already. */
static inline __attribute__((always_inline)) void verify_visit(void *start) {
  if (!vb_set(vb_seen, start, AVANT_FLAG_VSEEN)) {
    return;
  }
  if (gc_heap.vnoted_n == gc_heap.vnoted_cap) {
    gc_heap.vnoted_cap = gc_heap.vnoted_cap ? gc_heap.vnoted_cap * 2 : 1024;
    gc_heap.vnoted = gc_xrealloc(gc_heap.vnoted, gc_heap.vnoted_cap * sizeof(void *));
  }
  gc_heap.vnoted[gc_heap.vnoted_n++] = start;
  gc_work_push(start);
}

/* The trace of one reference P (Stage 28 W18M, O113: one object lookup
 * per reference serves every check made on it; before, the lost, named,
 * stale and missing-barrier checks each looked the same field up again).
 * Returns the object P names (its start, before following a forward), or
 * NULL when P names no object. */
static inline __attribute__((always_inline)) void *verify_trace_r(void *p) {
  void *found = gc_object_payload(p, NULL, NULL);
  void *start = found;
  if (!start) {
    return NULL;
  }
  AvantHeader *h = avant_header(start);
  /* A moved object is reached through its copy: its own header holds the
   * forward, not a size or a type (gc-design.md §3.2). */
  if (h->flags & AVANT_FLAG_FORWARDED) {
    start = gc_forward_of(h);
  }
  verify_visit(start);
  return found;
}

static void verify_trace(void *p) {
  (void)verify_trace_r(p);
}

/* The next object of the verifier's work stack; the one after it is
 * prefetched (its header and first fields are read next). */
static void *verify_pop(void) {
  void *p = gc_heap.work[--gc_heap.work_n];
  if (gc_heap.work_n) {
    __builtin_prefetch(avant_header(gc_heap.work[gc_heap.work_n - 1]));
  }
  return p;
}

/* The objects the check that just ran traced join vb_reach (FRESH: they
 * are all of it). */
static void vr_record(int fresh) {
  size_t i;
  if (!vb_side) {
    return;
  }
  if (fresh) {
    memset(vb_reach, 0, vb_words * sizeof(uint64_t));
    vr_valid = 1;
  }
  for (i = 0; vr_valid && i < gc_heap.vnoted_n; i++) {
    (void)vb_set(vb_reach, gc_heap.vnoted[i], 0);
  }
}

static void verify_clear_traced(void) {
  size_t i;
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    vb_clear(vb_seen, gc_heap.vnoted[i], AVANT_FLAG_VSEEN);
  }
  gc_heap.vnoted_n = 0;
}

static void verify_tls_slot(void **slot) {
  verify_trace(*slot);
}

/* FN on every root slot the verifier starts from, in this order: every
 * thread's frame slots, the pins, the task roots, with REM the remembered
 * set's entries (T traces from them), and the runtime roots. */
static void verify_each_root(void (*fn)(void **), int rem) {
  ThreadReg *t;
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, { fn(slot); });
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    fn(&p->payload);
  }
  EACH_TASK_ROOT(slot, fn(slot));
  for (uint32_t ri = 0; rem && ri < gc_heap.rem_n; ri++) {
    fn(&gc_heap.remset[ri]);
  }
  gc_each_runtime_root(fn);
}

/* Independent reachability, ignoring the collector's own tracing
 * gates: a holder counts as live when a root reaches it through any
 * field. This is what keeps the checker sensitive to the F1 class
 * (a live holder the young copy itself never traced) while staying
 * silent about dead mature objects, whose fields legitimately name
 * copied objects. The visited bit is VSEEN (the verifier's own);
 * verify_clear_traced resets it before gc_verify_young_copy returns. */
#if GC_STICKY_COPY
static uint64_t verify_stale_found(void *start, void *holder);
#endif

/* STALE (S+): count the fields of the reached objects that name a moved
 * object's old copy, as the trace reads them (verify_stale_found). */
static uint64_t verify_mark_reachable(int stale) {
  uint64_t bad = 0;
  (void)stale;
  verify_each_root(verify_tls_slot, 1);
  while (gc_heap.work_n) {
    void *p = verify_pop();
#if GC_STICKY_COPY
    if (stale) {
      EACH_REF_SLOT_WALK(p, slot, {
        void *st = verify_trace_r(*slot);
        if (st) {
          bad += verify_stale_found(st, p);
          if (st && (avant_header(st)->flags & AVANT_FLAG_FORWARDED)) {
            /* The holder's kind and the slot (Stage 29 W-INT1). */
            fprintf(stderr, "avant gc verify:   holder type %u size %u flags %#x, slot at +%ld\n",
                    (unsigned)avant_header(p)->type_id, (unsigned)avant_header(p)->size,
                    (unsigned)avant_header(p)->flags, (long)((char *)(void *)slot - (char *)p));
          }
        }
      });
      continue;
    }
#endif
    EACH_REF_SLOT_WALK(p, slot, { verify_trace(*slot); });
  }
  return bad;
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
static uint64_t verify_stale_start(void *start, const char *where, void *holder) {
  if (!start || !(avant_header(start)->flags & AVANT_FLAG_FORWARDED)) {
    return 0;
  }
  fprintf(stderr, "avant gc verify: stale %s reference holder=%p names moved object %p (now %p) coll=%llu\n",
          where, holder, start, gc_forward_of(avant_header(start)),
          (unsigned long long)avant_g_stats.collections);
  return 1;
}

static uint64_t verify_stale(void *p, const char *where, void *holder) {
  return verify_stale_start(gc_object_payload(p, NULL, NULL), where, holder);
}

static uint64_t verify_stale_found(void *start, void *holder) {
  return verify_stale_start(start, "field", holder);
}

static void verify_stale_tls(void **slot) {
  gc_heap.verify_untraced += verify_stale(*slot, "runtime root", (void *)slot);
}
#endif

/* T's checks of one reached object V: NULL, or what is wrong. */
static const char *sticky_why(void *v) {
  AvantHeader *h = avant_header(v);
  Block *b = NULL;
  AvantHeader *ol = NULL;
  if (!gc_is_heap_payload(v, &b, &ol)) {
    return NULL;
  }
  if (ol) {
    return (h->flags & AVANT_FLAG_YOUNG) && !(h->flags & AVANT_FLAG_MARKED) ? "young large object not marked" : NULL;
  }
  if (h->flags & AVANT_FLAG_YOUNG) {
    /* S+'s aged survivors (YOUNG_AGE 2) stay young in survivor blocks,
     * with SURVIVED. */
    return (h->flags & AVANT_FLAG_SURVIVED) && b->kept == SPLUS_SURV ? NULL : "young object not promoted";
  }
  if (b->young_hi) {
    size_t off = (size_t)((uint8_t *)h - b->base);
    size_t l;
    size_t end = (off + gc_obj_total(h) - 1) / LINE_SIZE;
    for (l = off / LINE_SIZE; l <= end && l < LINES_PER_BLOCK; l++) {
      if (!line_live(b, (int)l)) {
        return "line of a reachable object not live";
      }
    }
  }
  return NULL;
}

static void verify_sticky_whole(void) {
  size_t i;
  uint64_t bad = 0;
  vb_ensure();
#if GC_STICKY_COPY
  /* The fields of every reached object are checked for stale
   * references as the trace reads them (verify_mark_reachable). */
  bad += verify_mark_reachable(1);
#else
  (void)verify_mark_reachable(0);
#endif
#if GC_STICKY_COPY
  /* After S+'s copy and fix-up (before the old copies lose their start
   * bits): no root slot, runtime-held slot or field of a reachable object
   * may still name a moved object; verify_mark_reachable followed the
   * forwards, so the reachable objects are the copies. */
  {
    ThreadReg *t;
    for (t = gc_heap.threads; t; t = t->next) {
      EACH_FRAME(gc_thread_frames(t), f, {
        EACH_SLOT_OF(f, slot, {
          bad += verify_stale(*slot, "root", (void *)slot);
          /* A slot location inside a moved object (Stage 28 W15d). */
          bad += verify_stale((void *)slot, "root location", (void *)f);
        });
      });
    }
    gc_each_runtime_root(verify_stale_tls);
  }
#endif
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    void *v = gc_heap.vnoted[i];
    AvantHeader *h = avant_header(v);
    const char *why = sticky_why(v);
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
  vr_record(1);
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
 * A record is asked more since the Valk stage's VALK-T2 (O610): it must
 * be remembered even when R1' names it (yholders_check).
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
/* FOUND is gc_object_payload(P) (verify_trace_r's result). */
static uint64_t verify_lost_found(void *p, void *found, const char *where, void *holder) {
  uintptr_t off;
  if (!p || found || !heap_offset(p, &off)) {
    return 0;
  }
  if (gc_heap.verify_lost++ < 32) {
    fprintf(stderr,
            "avant gc verify: lost reference: %s %p names freed heap memory %p before coll=%llu\n",
            where, holder, p, (unsigned long long)avant_g_stats.collections);
  }
  return 1;
}

/* The field rule's check (the Valk stage, VALK-T, O585; D342 Q6; the
 * rule is stated at the top of avant_gc_internal.h): FOUND is
 * gc_object_payload(P) for the word P of HOLDER's field SLOT. A field
 * that names the inside of an object breaks what the traces rely on (they
 * classify a field by the range test alone and read the header right
 * before the address), so it is not counted like the other reports: the
 * program stops here, before the collection that would misread it. A
 * field naming heap memory where no object lives is verify_lost_found's
 * report. Asked of every field the verifier reads before a collection:
 * every field of every object the roots reach and of every remembered
 * object (the whole checks; an incremental check counts a broken field as
 * a candidate, which runs the whole check). */
static inline int field_rule_broken(void *p, void *found) {
  return found && found != p;
}

static void verify_field_rule(void *p, void *found, void **slot, void *holder) {
  if (!field_rule_broken(p, found)) {
    return;
  }
  fprintf(stderr,
          "avant gc verify: field rule: holder %p (type %u size %u flags 0x%x) field %ld holds %p, an address "
          "inside object %p (type %u size %u) before coll=%llu\n",
          holder, (unsigned)avant_header(holder)->type_id, (unsigned)avant_header(holder)->size,
          (unsigned)avant_header(holder)->flags, (long)((uint8_t *)slot - (uint8_t *)holder), p, found,
          (unsigned)avant_header(found)->type_id, (unsigned)avant_header(found)->size,
          (unsigned long long)avant_g_stats.collections);
  gc_die("avant gc verify: a reference-typed field holds an address inside an object (the field rule)");
}

static void lost_root_slot(void **slot) {
  void *st = verify_trace_r(*slot);
  verify_lost_found(*slot, st, "root", (void *)slot);
}

/* Before a full collection (a young one runs the precise half inside
 * gc_verify_remembered's pass over the same roots and fields). */
void gc_verify_lost(void) {
  vb_ensure();
  verify_each_root(lost_root_slot, 0);
  while (gc_heap.work_n) {
    void *p = verify_pop();
    EACH_REF_SLOT_WALK(p, slot, {
      void *st = verify_trace_r(*slot);
      verify_lost_found(*slot, st, "field of", p);
      verify_field_rule(*slot, st, slot, p);
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

/* START is the object a reference names (gc_object_payload's result). */
static void rem_named_found(void *start) {
  if (!start || verify_young_at(start, NULL, NULL)) {
    return;
  }
  if (!vb_set(vb_named, start, AVANT_FLAG_MARKED)) {
    return;
  }
  if (gc_heap.rnamed_n == gc_heap.rnamed_cap) {
    gc_heap.rnamed_cap = gc_heap.rnamed_cap ? gc_heap.rnamed_cap * 2 : 1024;
    gc_heap.rnamed = gc_xrealloc(gc_heap.rnamed, gc_heap.rnamed_cap * sizeof(void *));
  }
  gc_heap.rnamed[gc_heap.rnamed_n++] = start;
}

/* A remembered object's field (R1'(c)), in the whole check: the young
 * collection traces it whether or not the roots reach its holder, so the
 * field rule and the lost-reference check are asked of it here too (the
 * Valk stage, VALK-T, O585). */
static void rem_named_field(void *holder, void **slot) {
  void *st = gc_object_payload(*slot, NULL, NULL);
  verify_lost_found(*slot, st, "field of remembered", holder);
  verify_field_rule(*slot, st, slot, holder);
  rem_named_found(st);
}

/* The reached holders that are not young and name a young object in a
 * field, gathered by gc_verify_remembered's trace: the only holders its
 * missing-barrier check can report. */
static void **g_yholders;
static size_t g_yholders_n;
static size_t g_yholders_cap;

static void yholder_add(void *v) {
  if (g_yholders_n == g_yholders_cap) {
    g_yholders_cap = g_yholders_cap ? g_yholders_cap * 2 : 1024;
    g_yholders = gc_xrealloc(g_yholders, g_yholders_cap * sizeof(void *));
  }
  g_yholders[g_yholders_n++] = v;
}

/* Is H a carded object the young collection scans by its marked cards
 * (in the set as REMC, without CARD_ALL)? */
static int rem_by_cards(const AvantHeader *h) {
  return gc_carded(h) && (h->flags & AVANT_FLAG_REMC) && !(h->flags & AVANT_FLAG_CARD_ALL);
}

static void rem_root_slot(void **slot) {
  void *st = verify_trace_r(*slot);
  verify_lost_found(*slot, st, "root", (void *)slot);
  rem_named_found(st);
}

/* Is H an element buffer: a pointer buffer (AVANT_TYPE_PTRS) or a buffer
 * of by-value elements (a type map with a stride)? Every other object
 * with references is a record (a class instance, a box, a closure's
 * environment, a capture record, an array's or a hash's header). */
static int verify_is_buffer(const AvantHeader *h) {
  uint32_t id = h->type_id;
  return id == AVANT_TYPE_PTRS || (id < MAX_TYPES && gc_heap.type_set[id] && gc_heap.types[id].stride != 0);
}

/* The check: a reached non-young holder that is not remembered must name
 * no young object. Two forms.
 *
 * The invariant itself, for a record (the Valk stage, VALK-T2, O610): a
 * record that names a young object is remembered (REM), whatever names
 * the record. Generated code stores a reference into a record behind a
 * barrier or while the record is open (allocated, barriered or an
 * initialize's receiver since the last collecting call: young, or
 * remembered until the next collection; D347, compiler/mir/fresh.av), and
 * runtime C barriers its stores into records, so at every young
 * collection the invariant holds of every record without the help of
 * R1'. Until VALK-T2 a record R1' names this cycle (a root, a young
 * object's field or a remembered object's field names it) was passed
 * over, because the trace scans it and so repairs the miss: a store that
 * lost its barrier (VALK-A1's planted defect: a fresh pass that ignores
 * collecting calls, V14 off) was reported only once nothing named the
 * record any more, which no program of the suite reaches. It is reported
 * now at the first young collection after the store, with "not
 * remembered" in the line.
 *
 * R1' (gc-design.md section 1.3), for an element buffer: one that a root,
 * a young object's field or a remembered object's field names this cycle
 * may hold young references unremembered, as before. That is the window
 * the contract leaves open on purpose: the fill of a fresh buffer held in
 * a slot, closed by one barrier when the fill ends (V14's older form, the
 * only stores generated code makes with no barrier into a holder that is
 * not open: compiler/mir/lower_arr.av, lower_block.av; mir.md 5.4), in
 * which a buffer a collection promoted in the middle of its fill holds
 * young elements until the closing barrier; and the runtime's builders
 * of buffers a young header or a root names (gc-design.md 1.3). A
 * missed closing barrier on such a buffer therefore stays invisible here
 * while something names the buffer; V14 is its check.
 *
 * A carded holder remembered by its cards (REMC without CARD_ALL) must
 * name young objects only from slots of its marked cards (the card
 * invariant, Stage 28 W16t): a young object named from a clear card is
 * reported as a missing barrier too, since the young collection would not
 * scan that slot. Only holders the verifier's own trace reaches are
 * asked: a dead object's fields are never read by the program or the
 * collector, and may name an address a new young object has taken since.
 * Returns the fields found; the first 32 are printed when PRINT. */
static uint64_t yholders_check(int print) {
  size_t i;
  uint64_t found = 0;
  for (i = 0; i < g_yholders_n; i++) {
    void *v = g_yholders[i];
    void *st;
    AvantHeader *h = avant_header(v);
    int by_cards;
    int named;
    if (verify_names_young(v, &st) || (h->flags & AVANT_FLAG_REM)) {
      continue;
    }
    named = vb_test(vb_named, v, AVANT_FLAG_MARKED);
    if (named && verify_is_buffer(h)) {
      continue;
    }
    by_cards = (h->flags & AVANT_FLAG_REMC) != 0;
    if (by_cards && !rem_by_cards(h)) {
      continue;
    }
    EACH_REF_SLOT_WALK(v, slot, {
      void *y;
      if ((!by_cards || !gc_heap.cards[gc_card_index(slot)]) && verify_names_young(*slot, &y)) {
        found++;
        if (print && found <= 32) {
          Block *hb = block_of(h);
          fprintf(stderr,
                  "avant gc verify: missing barrier: holder %p field %ld names young %p "
                  "(holder flags=0x%x type=%u space=%d; young flags=0x%x)%s before coll=%llu\n",
                  v, (long)((uint8_t *)slot - (uint8_t *)v), y, h->flags, (unsigned)h->type_id,
                  hb ? (int)hb->space : -1, avant_header(y)->flags,
                  named ? ", not remembered (a root, a young object or a remembered one names the holder, so "
                          "this collection's trace still reaches the field)"
                        : "",
                  (unsigned long long)avant_g_stats.collections);
        }
      }
    });
  }
  return found;
}

static void verify_remembered_whole(void) {
  size_t i;
  uint64_t found;
  vb_ensure();
  /* Roots: reachability and R1'(a). */
  verify_each_root(rem_root_slot, 0);
  /* The trace from the roots; with the same lookup of each field, R1'(b)
   * (a young object's fields name what they reach) and the holders the
   * check below must look at (a non-young one naming a young object). */
  g_yholders_n = 0;
  while (gc_heap.work_n) {
    void *p = verify_pop();
    int py = (avant_header(p)->flags & AVANT_FLAG_YOUNG) != 0;
    int ny = 0;
    EACH_REF_SLOT_WALK(p, slot, {
      void *st = verify_trace_r(*slot);
      verify_lost_found(*slot, st, "field of", p);
      verify_field_rule(*slot, st, slot, p);
      if (st) {
        if (py) {
          rem_named_found(st);
        } else if (verify_young_at(st, NULL, NULL)) {
          ny = 1;
        }
      }
    });
    if (ny) {
      yholder_add(p);
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
          EACH_CARD_SLOT(obj, slot, { rem_named_field(obj, slot); });
        } else {
          EACH_REF_SLOT_WALK(obj, slot, { rem_named_field(obj, slot); });
        }
      }
    }
  }
  found = yholders_check(1);
  if (found > 32) {
    fprintf(stderr, "avant gc verify: missing barrier: %llu fields in all before coll=%llu\n",
            (unsigned long long)found, (unsigned long long)avant_g_stats.collections);
  }
  gc_heap.missing_barriers += found;
  for (i = 0; i < gc_heap.rnamed_n; i++) {
    vb_clear(vb_named, gc_heap.rnamed[i], AVANT_FLAG_MARKED);
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
  LargeIter it;
  AvantHeader *lh;
  EACH_MATURE_BLOCK(mb, { gc_foreach_block_object(mb, verify_no_stale_one); });
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_no_stale_one(lh, NULL);
  }
}

/* The incremental verifier (Stage 28 W18M item 7, O119, the plan's W18V).
 * The kernel's soft-dirty bits (Documentation/admin-guide/mm/soft-dirty.rst)
 * say which pages of the process were written since they were last
 * cleared: writing "4" to /proc/self/clear_refs clears them all, and bit
 * 55 of a page's /proc/self/pagemap entry is set again by any write to
 * the page after that, by any thread, by the collector, by runtime C or
 * by the kernel on the program's behalf (a read(2) into a buffer). Only
 * the kernel's record decides what is skipped: never the collector's own
 * barriers, remembered set or cards, whose missing entry is what the
 * verifier exists to catch.
 *
 * What it makes incremental is C36's check (gc_verify_no_stale_marks:
 * no live header carries MARKED, FORWARDED, VSEEN, PROMOTED_NOW or
 * CARD_SCAN when a collection starts), which walked every object of every
 * mature block at every collection: 97% of closures_churn's time under
 * AVANT_GC_VERIFY=1 AVANT_GC_STRESS=1. A stale mark is a header bit, and
 * a header bit changes only by a write to the header's page. So at a
 * young collection's start the check reads only the headers on pages
 * written since the last verification, which read every other header
 * (directly or, by the same argument, through an earlier one), right
 * before the bits were cleared (gc_verify_begin clears them as soon as
 * the check ends, before anything else of the collection writes). A
 * header on an untouched page holds what it held then. A block that was
 * not in the mature list at the last verification is read whole (the
 * stamp below), every large object's header is read, and a full
 * collection, the first verification and the program's exit read the
 * whole heap. The checks that depend on reachability (gc_verify_remembered,
 * gc_verify_sticky) are made incremental by the same bits and a record of
 * their own (Stage 28 W18V2, below); gc_verify_lost (full collections)
 * stays whole.
 *
 * Where soft-dirty tracking is unavailable (a kernel without
 * CONFIG_MEM_SOFT_DIRTY, an unreadable pagemap, a refused clear) the probe
 * (once, at the SD_WARM-th verification; a shorter program is whole) says so, sd_mode stays 0 and every verification is whole, as
 * before; a later refusal of either file turns it off for the rest of the
 * run, as does a forked child (sd_pid). The statistics line says which
 * mode ran (verify=). */
#define SD_BIT (1ULL << 55)

static int sd_pagemap = -1;
static int sd_clear = -1;
/* The process the two files were opened by: /proc/self is resolved at
 * open, so a forked child (no exec) would read and clear its parent's
 * bits, which would make the parent's next skip unsound. A child runs the
 * whole verifier. */
static pid_t sd_pid;
/* The bits were cleared at an earlier verification and every
 * verification since read the pages they report (incremental may follow;
 * Stage 28 W18V2: the bits are kept across verifications, see
 * gc_verify_begin). */
static int sd_based;
/* The pages the last incremental read reported written, and the
 * verifications since the last clear. */
static size_t sd_dirty;
static uint32_t sd_since_clear;
static uint64_t *sd_map;
static size_t sd_map_cap;
/* One bit per block of the committed heap: in the mature list at the last
 * verification. */
static uint64_t *sd_seen;
static size_t sd_seen_words;

static int sd_clear_all(void) {
  return pwrite(sd_clear, "4", 1, 0) == 1;
}

/* The pagemap entries of N pages from ADDR into OUT; 1 when all read. */
static int sd_read(const void *addr, size_t n, uint64_t *out) {
  size_t want = n * sizeof(uint64_t);
  off_t at = (off_t)((uintptr_t)addr / PAGE_SIZE * sizeof(uint64_t));
  size_t got = 0;
  while (got < want) {
    ssize_t k = pread(sd_pagemap, (uint8_t *)out + got, want - got, at + (off_t)got);
    if (k <= 0) {
      return 0;
    }
    got += (size_t)k;
  }
  return 1;
}

static void sd_off(void) {
  gc_heap.sd_mode = 0;
  sd_based = 0;
}

/* The probe: a fresh page is written, the bits are cleared, the page must
 * read clean, it is written again, and it must read dirty. */
static int sd_probe(void) {
  volatile uint8_t *pg;
  uint64_t e = 0;
  int ok = 0;
  sd_pid = getpid();
  sd_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  sd_clear = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
  if (sd_pagemap < 0 || sd_clear < 0) {
    return 0;
  }
  pg = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pg == MAP_FAILED) {
    return 0;
  }
  pg[0] = 1;
  if (sd_clear_all() && sd_read((const void *)pg, 1, &e) && !(e & SD_BIT)) {
    pg[0] = 2;
    ok = sd_read((const void *)pg, 1, &e) && (e & SD_BIT);
  }
  munmap((void *)pg, PAGE_SIZE);
  return ok;
}

static void verify_at_exit_check(void) {
  gc_verify_begin(1);
  gc_verify_remembered();
}

static void verify_at_exit(void) {
  gc_verify_exit_run(verify_at_exit_check);
}

/* The probe runs once, at the SD_WARM-th verification (below): a short
 * program never opens the two files (the gate's VERIFY pass runs
 * thousands of them). */
static int sd_probed;

static void sd_probe_once(void) {
  sd_probed = 1;
  gc_heap.sd_mode = sd_probe();
  if (!gc_heap.sd_mode) {
    if (sd_pagemap >= 0) {
      close(sd_pagemap);
    }
    if (sd_clear >= 0) {
      close(sd_clear);
    }
    sd_pagemap = sd_clear = -1;
  }
}

void gc_verify_init(void) {
  atexit(verify_at_exit);
}

static void sd_walk_page(Block *b, int page) {
  int w;
  for (w = page * (PAGE_SIZE / 512); w < (page + 1) * (int)(PAGE_SIZE / 512); w++) {
    uint64_t bits = b->start[w];
    while (bits) {
      int bit = __builtin_ctzll(bits);
      verify_no_stale_one((AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8), b);
      bits &= bits - 1;
    }
  }
}

/* C36's check over the pages written since the last verification; 0 when
 * the pagemap could not be read (the caller then walks the whole heap). */
static int verify_no_stale_marks_incr(void) {
  size_t pages = gc_heap.committed / PAGE_SIZE;
  size_t blocks = gc_heap.committed >> BLOCK_SHIFT;
  LargeIter it;
  AvantHeader *lh;
  if (pages > sd_map_cap) {
    sd_map_cap = pages;
    sd_map = gc_xrealloc(sd_map, sd_map_cap * sizeof(uint64_t));
  }
  if (!sd_read(gc_heap.base, pages, sd_map)) {
    return 0;
  }
  {
    size_t i;
    sd_dirty = 0;
    for (i = 0; i < pages; i++) {
      sd_dirty += (size_t)((sd_map[i] >> 55) & 1ULL);
    }
  }
  EACH_MATURE_BLOCK(b, {
    size_t bi = (size_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
    size_t p0 = (size_t)(b->base - gc_heap.base) / PAGE_SIZE;
    int p;
    if (bi >= blocks || bi / 64 >= sd_seen_words || !((sd_seen[bi / 64] >> (bi % 64)) & 1)) {
      gc_foreach_block_object(b, verify_no_stale_one);
      gc_heap.v_pages_checked += BLOCK_SIZE / PAGE_SIZE;
    } else {
      for (p = 0; p < (int)(BLOCK_SIZE / PAGE_SIZE); p++) {
        if (sd_map[p0 + (size_t)p] & SD_BIT) {
          sd_walk_page(b, p);
          gc_heap.v_pages_checked++;
        } else {
          gc_heap.v_pages_skipped++;
        }
      }
    }
  });
  gc_large_iter_init(&it);
  while ((lh = gc_large_next(&it))) {
    verify_no_stale_one(lh, NULL);
  }
  return 1;
}

/* The mature list as of this verification (the stamp the next one reads). */
static void sd_stamp(void) {
  size_t blocks = gc_heap.committed >> BLOCK_SHIFT;
  size_t words = (blocks + 63) / 64;
  if (words > sd_seen_words) {
    sd_seen = gc_xrealloc(sd_seen, words * sizeof(uint64_t));
    sd_seen_words = words;
  }
  memset(sd_seen, 0, sd_seen_words * sizeof(uint64_t));
  EACH_MATURE_BLOCK(b, {
    size_t bi = (size_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
    sd_seen[bi / 64] |= 1ULL << (bi % 64);
  });
}

/* The choice between the two forms (Stage 28 W18S, O150; W18M item 7
 * priced them). The first SD_WARM verifications are whole (a short
 * program never write-protects its pages); from then on, where soft-dirty
 * tracking works, every young collection's verification is incremental
 * for the rest of the run, unless a check reports (then every check is
 * whole, so the reports are the whole verifier's) or soft-dirty stops
 * (a fork, a refused read: sd_off). Full collections stay whole.
 *
 * W18M priced the two forms as the program ran and took the cheaper one,
 * trying the other every 64 verifications. Each switch restarts the
 * record and the soft-dirty bits, so the first incremental checks after
 * one rebase and fall back to the whole check: the cheaper form per
 * measurement was the dearer one over a run (W18C, O148: the gate's
 * verified compiler dump 52-104 s priced, 36.65 s and 0 fallbacks
 * incremental throughout). Measured at W18S on W18M's four VERIFY +
 * STRESS rows, the incremental form throughout was not slower than the
 * whole form nor than the priced choice on any of them except
 * inc_slice_mature, where it is within a few percent of both
 * (records/2026-11-10-s28-w18s, o150/). Both forms check the same, so
 * the choice changes the time only, never the reports.
 *
 * Which pages are skipped is still decided by the kernel's record alone. */
#ifndef SD_WARM
#define SD_WARM 256u
#endif
/* The bits are cleared again when the last read found more than a
 * SD_CLEAR_FRAC-th of the heap's pages written, or after SD_CLEAR_MAX
 * verifications (Stage 28 W18V2): a clear write-protects every page of the
 * process, so each page's first write after it faults in the kernel (under
 * AVANT_GC_STRESS about 0.14 ms per collection on an 8 MB heap, the clear
 * alone 45 us); between clears the reads report every page written since
 * the last clear, a superset of those written since the last verification,
 * which is all an incremental check needs. */
#ifndef SD_CLEAR_FRAC
#define SD_CLEAR_FRAC 4u
#endif
#ifndef SD_CLEAR_MAX
#define SD_CLEAR_MAX 64u
#endif

/* The incremental reachability checks (Stage 28 W18V2, O119's second
 * half). The checks that depend on reachability, gc_verify_remembered
 * before a young collection (M: lost references, missing barriers) and
 * gc_verify_sticky after its trace and copy (T: references to a moved
 * object's old copy, young objects not promoted, lines of reached objects
 * about to be freed), traced the whole live heap at every collection. A
 * young collection moves and frees only objects that were young when it
 * began, so a slot that names a mature object keeps naming that live,
 * unmoved object as long as the slot is not written, no full collection
 * runs and nothing frees a mature object outside a collection. The
 * record (pg_rec) is the verifier's own: the heap pages where its last
 * examination found a slot naming a young object, a moved object's old
 * copy or heap memory where no object lives (a volatile slot), plus the
 * pages of the young blocks and of the young large objects. An
 * incremental check examines the pages the kernel reports written since
 * the last verification (soft-dirty, sd_map) and the pages of the record
 * (together pg_hot), and every young object it reaches; every other slot
 * is one the last examination found naming a mature object, unwritten
 * since, so it gives no report now. Nothing is decided by the
 * collector's barriers, remembered set or cards.
 *
 * Which objects are reached is not known without the whole trace, so an
 * incremental check proves only that the whole one would be silent: it
 * traces from the roots (and, for T, the remembered set) into every
 * object that is young or has a slot on a hot page, then treats every
 * object with a slot on a hot page as reached too (pg_hot's "sources",
 * whose slots there it examines), and applies every check as the whole
 * check would, counting candidates instead of printing. Its reached set
 * contains every reached object that has a slot on a hot page or is
 * young; R1' names only what the roots, the remembered objects and the
 * young objects reached from the roots name (so it names no more than the
 * whole check does). So every report of the whole check is a candidate.
 * With no candidate the whole check would have been silent; with any,
 * the whole check runs at once and prints the reports (a fallback), and
 * from the first report on every check is whole (v_reported). The
 * reports are therefore the whole verifier's, always.
 *
 * The record starts empty-and-unknown (v_rec_all: every page is hot)
 * after every whole verification, every full collection (it moves and
 * frees mature objects after its own verification), every free of a
 * mature object outside a collection (gc_verify_rebase: a growth's freed
 * buffer, a trim), and a fallback; such a "rebasing" pass examines every
 * object, so objects the last whole trace did not reach are recorded too.
 * Full collections, the first SD_WARM verifications and the program's
 * exit stay whole; C36's check (gc_verify_begin) takes the same form. */

/* VI_FORCE, settable at build time for a proof (W18V2's side by side):
 * 1 makes every M and T whole, 2 makes every verification after the first
 * incremental (no warm-up when SD_WARM is set to 1 too; since W18S the
 * normal build is incremental after the warm-up anyway). */
#ifndef VI_FORCE
#define VI_FORCE 0
#endif
/* This collection's M and T are incremental. */
static int v_incr_now;
/* A check has reported: every later one is whole. */
static int v_reported;
/* The record holds nothing yet: every page is hot. */
static int v_rec_all = 1;
/* The reports of gc_verify_defrag (no counter of its own before). */
static uint64_t v_defrag_reports;
/* Set by a free of a mature object outside a collection (avant_gc_los.c). */
int gc_verify_rebase;

static uint64_t v_report_count(void) {
  return gc_heap.verify_untraced + gc_heap.missing_barriers + gc_heap.stale_seen + gc_heap.verify_lost +
         v_defrag_reports;
}

/* A collection's first verification step (gc_collect, under
 * AVANT_GC_VERIFY, after the quarantine's release): C36's check, whole at
 * a full collection (FULL), during the warm-up and without soft-dirty
 * tracking; else over the written pages. It also chooses the form of
 * this collection's M and T: incremental exactly when C36's check is
 * (sd_map then holds the pages written since the last verification) and
 * no check has reported. After the warm-up the bits are kept cleared for
 * the next verification, so every later write, the collection's own
 * included, is seen by it. */
void gc_verify_begin(int full) {
  int incr = 0;
  if (!sd_probed && gc_heap.v_whole + gc_heap.v_incr + 1 >= SD_WARM) {
    sd_probe_once();
  }
  if (gc_heap.sd_mode && getpid() != sd_pid) {
    sd_off();
  }
  if (v_report_count()) {
    v_reported = 1;
  }
  if (gc_heap.sd_mode && !full && sd_based && verify_no_stale_marks_incr()) {
    gc_heap.v_incr++;
    incr = 1;
  } else {
    gc_verify_no_stale_marks();
    gc_heap.v_whole++;
  }
  if (gc_verify_rebase) {
    gc_verify_rebase = 0;
    v_rec_all = 1;
  }
  v_incr_now = incr && !v_reported && VI_FORCE != 1;
  if (full) {
    v_rec_all = 1;
    vr_valid = 0;
  } else if (!v_incr_now) {
    v_rec_all = 1;
  }
  if (!gc_heap.sd_mode) {
    return;
  }
  /* A short program (the suite's thousands of small ones) never asks the
   * kernel: its first SD_WARM verifications are whole, so it pays neither
   * the probe, the clear nor the write faults after it (with SD_WARM at
   * 16 the gate's VERIFY pass spent about 4 s more system time). */
  if (gc_heap.v_whole + gc_heap.v_incr < SD_WARM && VI_FORCE != 2) {
    sd_based = 0;
    return;
  }
  /* The bits stay while this verification read them (incremental), few
   * pages were written and the last clear is recent. */
  if (sd_based && incr && sd_dirty <= (size_t)(gc_heap.committed / PAGE_SIZE) / SD_CLEAR_FRAC &&
      ++sd_since_clear < SD_CLEAR_MAX) {
    return;
  }
  sd_stamp();
  sd_since_clear = 0;
  vb_side = 1;
  if (!sd_clear_all()) {
    sd_off();
    return;
  }
  sd_based = 1;
}

/* One bit per heap page: the record (pg_rec, from the last T), the record
 * M builds for T (pg_recm), the one being built (pg_out) and the pages
 * the running check examines (pg_hot). */
static uint64_t *pg_rec;
static uint64_t *pg_recm;
static uint64_t *pg_out;
static uint64_t *pg_hot;
static size_t pg_words;
static uint64_t vi_cand;
/* M's trace from the roots (its young objects name for R1'), not from
 * the sources. */
static int vi_from_roots;

static uint64_t *pg_grow(uint64_t *m, size_t words) {
  m = gc_xrealloc(m, words * sizeof(uint64_t));
  memset(m + pg_words, 0, (words - pg_words) * sizeof(uint64_t));
  return m;
}

static size_t pg_ensure(void) {
  size_t pages = (size_t)(gc_heap.committed / PAGE_SIZE);
  size_t words = (pages + 63) / 64;
  if (words > pg_words) {
    pg_rec = pg_grow(pg_rec, words);
    pg_recm = pg_grow(pg_recm, words);
    pg_out = pg_grow(pg_out, words);
    pg_hot = pg_grow(pg_hot, words);
    pg_words = words;
  }
  return pages;
}

static inline void pg_set(uint64_t *m, size_t pi) {
  m[pi >> 6] |= 1ULL << (pi & 63);
}

/* pg_hot: the pages of REC and the pages sd_map reports written (PAGES of
 * them), or every page while the record is unknown; pg_out is cleared. */
static void pg_build_hot(const uint64_t *rec, size_t pages) {
  size_t words = (pages + 63) / 64;
  size_t w;
  memset(pg_out, 0, pg_words * sizeof(uint64_t));
  if (v_rec_all) {
    memset(pg_hot, 0xff, words * sizeof(uint64_t));
    return;
  }
  for (w = 0; w < words; w++) {
    uint64_t bits = rec[w];
    size_t i;
    for (i = 0; i < 64 && w * 64 + i < pages; i++) {
      bits |= ((sd_map[w * 64 + i] >> 55) & 1ULL) << i;
    }
    pg_hot[w] = bits;
  }
}

/* Is the slot or byte at A on a hot page (outside the heap: always)? */
static inline int vi_hot(const void *a) {
  uintptr_t off;
  size_t pi;
  if (!heap_offset(a, &off)) {
    return 1;
  }
  pi = (size_t)(off / PAGE_SIZE);
  return (int)((pg_hot[pi >> 6] >> (pi & 63)) & 1ULL);
}

static inline int vi_has_refs(const AvantHeader *h) {
  uint32_t id = h->type_id;
  return id == AVANT_TYPE_PTRS || (id != AVANT_TYPE_BYTES && id < MAX_TYPES && gc_heap.type_set[id]);
}

/* Does the trace go into START: a young object, one outside the heap, or
 * one with a slot on a hot page? */
static int vi_wants(void *start) {
  AvantHeader *h = avant_header(start);
  uintptr_t off;
  size_t p0;
  size_t p1;
  if (h->flags & AVANT_FLAG_YOUNG) {
    return 1;
  }
  if (!heap_offset(start, &off)) {
    return 1;
  }
  if (!vi_has_refs(h) || !h->size) {
    return 0;
  }
  p0 = (size_t)(off / PAGE_SIZE);
  p1 = (size_t)((off + h->size - 1) / PAGE_SIZE);
  while (p0 <= p1) {
    uint64_t w = pg_hot[p0 >> 6] >> (p0 & 63);
    if (w) {
      return p0 + (size_t)__builtin_ctzll(w) <= p1;
    }
    p0 = (p0 | 63) + 1;
  }
  return 0;
}

/* verify_trace_r for an incremental check: the object P names (its start
 * before a forward), traced when vi_wants it. */
static void *vi_trace(void *p) {
  void *found = gc_object_payload(p, NULL, NULL);
  void *start = found;
  if (!start) {
    return NULL;
  }
  if (avant_header(start)->flags & AVANT_FLAG_FORWARDED) {
    start = gc_forward_of(avant_header(start));
  }
  if (vi_wants(start)) {
    verify_visit(start);
  }
  return found;
}

/* A slot the check examined: its page joins pg_out when the slot is
 * volatile (names a young object, a moved one's old copy, or heap memory
 * with no object). FOUND is what vi_trace returned. */
static inline void vi_note(void **slot, void *found) {
  uintptr_t off;
  uintptr_t o2;
  if (!heap_offset(slot, &off)) {
    return;
  }
  if (found ? (avant_header(found)->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_FORWARDED)) != 0
            : heap_offset(*slot, &o2)) {
    pg_set(pg_out, (size_t)(off / PAGE_SIZE));
  }
}

static inline int vi_lost(void *p, void *found) {
  uintptr_t off;
  return p && !found && heap_offset(p, &off);
}

/* The slots of P the check examines: every slot of a young object or one
 * outside the heap (ALL), else the slots on hot pages (a pointer array
 * page by page). */
#define VI_EACH_SLOT(P, ALL, SLOT, CALL)                                                  \
  do {                                                                                  \
    void **vis_p_ = (void **)(P);                                                       \
    const AvantHeader *vis_h_ = avant_header(vis_p_);                                   \
    if (!(ALL) && vis_h_->type_id == AVANT_TYPE_PTRS) {                                 \
      void **vis_s_ = vis_p_;                                                           \
      void **vis_e_ = vis_p_ + vis_h_->size / sizeof(void *);                           \
      while (vis_s_ < vis_e_) {                                                         \
        void **vis_n_ = (void **)(((uintptr_t)vis_s_ & ~(uintptr_t)(PAGE_SIZE - 1)) + PAGE_SIZE); \
        if (vis_n_ > vis_e_) {                                                          \
          vis_n_ = vis_e_;                                                              \
        }                                                                               \
        if (vi_hot(vis_s_)) {                                                           \
          void **SLOT;                                                                  \
          for (SLOT = vis_s_; SLOT < vis_n_; SLOT++) {                                  \
            CALL;                                                                       \
          }                                                                             \
        }                                                                               \
        vis_s_ = vis_n_;                                                                \
      }                                                                                 \
    } else {                                                                            \
      EACH_REF_SLOT_WALK(vis_p_, SLOT, {                                                     \
        if ((ALL) || vi_hot(SLOT)) {                                                    \
          CALL;                                                                         \
        }                                                                               \
      });                                                                               \
    }                                                                                   \
  } while (0)

static inline int vi_all(void *p) {
  uintptr_t off;
  return (avant_header(p)->flags & AVANT_FLAG_YOUNG) || !heap_offset(p, &off);
}

/* Was START, a block object that is not young, unreached by the last
 * whole T (and not visited since)? The last whole trace proved it
 * unreached; nothing has named it since, or M would have found the slot
 * or root that does (vi_m_slot: a first such name is a slot written since,
 * or a root, and both are examined). So an incremental check passes over
 * it as a source and in the young blocks' line check: an object that is
 * unreached gives no report in the whole check either. Large objects are
 * never passed over (their runs are reused for objects allocated mature,
 * which the last trace cannot know). */
static int vr_unreached(void *start) {
  AvantHeader *h = avant_header(start);
  uintptr_t off;
  uint64_t g;
  if (!vr_valid || (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_PROMOTED_NOW)) || !heap_offset(h, &off) ||
      gc_heap.chunks[off >> CHUNK_SHIFT].kind != CHUNK_SMALL) {
    return 0;
  }
  g = (uint64_t)off >> 3;
  return !((vb_reach[g >> 6] >> (g & 63)) & 1ULL);
}

/* An object of a hot page that is not young becomes a source. */
static void vi_source(void *start) {
  AvantHeader *h = avant_header(start);
  if ((h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_FORWARDED)) || !vi_has_refs(h) || vr_unreached(start)) {
    return;
  }
  verify_visit(start);
}

static void vi_source_page(size_t pi) {
  uintptr_t off = (uintptr_t)pi * PAGE_SIZE;
  uint8_t *a = gc_heap.base + off;
  uint8_t kind = gc_heap.chunks[off >> CHUNK_SHIFT].kind;
  void *c;
  if (kind == CHUNK_SMALL) {
    size_t wd;
    size_t w0 = (size_t)(off >> 3) / 64;
    if (!gc_heap.blocks[off >> BLOCK_SHIFT].in_use) {
      return;
    }
    /* The object that began before the page and covers its first byte,
     * then every object that begins on it. */
    c = gc_object_payload(a, NULL, NULL);
    if (c) {
      vi_source(c);
    }
    for (wd = w0; wd < w0 + PAGE_SIZE / 512; wd++) {
      uint64_t sb = gc_heap.starts[wd];
      while (sb) {
        size_t g = wd * 64 + (size_t)__builtin_ctzll(sb);
        sb &= sb - 1;
        vi_source(gc_heap.base + g * 8 + sizeof(AvantHeader));
      }
    }
  } else if (kind == CHUNK_LARGE || kind == CHUNK_HUGE || kind == CHUNK_HUGE_CONT) {
    c = gc_object_payload(a, NULL, NULL);
    if (!c) {
      c = gc_object_payload(a + sizeof(AvantHeader), NULL, NULL);
    }
    if (c) {
      vi_source(c);
    }
  }
}

/* Every source of the hot pages, each traced to the end by EXAMINE. */
static void vi_sources(size_t pages, void (*examine)(void *)) {
  size_t w;
  for (w = 0; w * 64 < pages; w++) {
    uint64_t bits = pg_hot[w];
    while (bits) {
      size_t pi = w * 64 + (size_t)__builtin_ctzll(bits);
      bits &= bits - 1;
      if (pi >= pages) {
        break;
      }
      vi_source_page(pi);
      while (gc_heap.work_n) {
        examine(verify_pop());
      }
    }
  }
}

/* M's examination of one slot of P (PY: P is young). */
static inline void vi_m_slot(void **slot, int py, int *ny) {
  void *st = vi_trace(*slot);
  vi_cand += (uint64_t)vi_lost(*slot, st);
  /* The field rule (verify_field_rule: the whole check reports it). */
  vi_cand += (uint64_t)field_rule_broken(*slot, st);
  /* An object the last whole T proved unreached is named again. */
  vi_cand += (uint64_t)(st && vr_unreached(st));
  vi_note(slot, st);
  if (st) {
    if (py) {
      if (vi_from_roots) {
        rem_named_found(st);
      }
    } else if (avant_header(st)->flags & AVANT_FLAG_YOUNG) {
      *ny = 1;
    }
  }
}

/* rem_named_field in an incremental check: a broken or lost field is a
 * candidate. */
static void vi_rem_named_field(void **slot) {
  void *st = gc_object_payload(*slot, NULL, NULL);
  vi_cand += (uint64_t)vi_lost(*slot, st);
  vi_cand += (uint64_t)field_rule_broken(*slot, st);
  rem_named_found(st);
}

static void vi_m_object(void *p) {
  int py = (avant_header(p)->flags & AVANT_FLAG_YOUNG) != 0;
  int ny = 0;
  VI_EACH_SLOT(p, vi_all(p), slot, vi_m_slot(slot, py, &ny));
  if (ny) {
    yholder_add(p);
  }
}

static void vi_m_root(void **slot) {
  void *st = vi_trace(*slot);
  vi_cand += (uint64_t)vi_lost(*slot, st);
  vi_cand += (uint64_t)(st && vr_unreached(st));
  rem_named_found(st);
}

static void vi_m_end(void) {
  size_t i;
  for (i = 0; i < gc_heap.rnamed_n; i++) {
    vb_clear(vb_named, gc_heap.rnamed[i], AVANT_FLAG_MARKED);
  }
  gc_heap.rnamed_n = 0;
  verify_clear_traced();
}

/* M, incremental: 1 when it proved the whole check silent (pg_recm then
 * holds its record), 0 when it found a candidate (nothing is left set). */
static int vi_remembered(void) {
  size_t pages;
  vb_ensure();
  pages = pg_ensure();
  pg_build_hot(pg_rec, pages);
  vi_cand = 0;
  g_yholders_n = 0;
  vi_from_roots = 1;
  verify_each_root(vi_m_root, 0);
  while (gc_heap.work_n) {
    vi_m_object(verify_pop());
  }
  vi_from_roots = 0;
  vi_sources(pages, vi_m_object);
  /* R1'(c) from the remembered objects' slots on hot pages: a slot
   * elsewhere names a mature object, which may go unnamed here (R1' then
   * names less than the whole check's, never more). */
  {
    uint32_t ri;
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      void *obj = gc_heap.remset[ri];
      if (gc_is_heap_payload(obj, NULL, NULL)) {
        if (rem_by_cards(avant_header(obj))) {
          EACH_CARD_SLOT(obj, slot, {
            if (vi_hot(slot)) {
              vi_rem_named_field(slot);
            }
          });
        } else {
          VI_EACH_SLOT(obj, vi_all(obj), slot, vi_rem_named_field(slot));
        }
      }
    }
  }
  vi_cand += yholders_check(0);
  vi_m_end();
  if (vi_cand) {
    return 0;
  }
  {
    uint64_t *x = pg_recm;
    pg_recm = pg_out;
    pg_out = x;
  }
  /* pg_recm is complete: T reads it, not every page. */
  v_rec_all = 0;
  return 1;
}

/* T's examination of one slot, and of one object. */
static inline void vi_t_slot(void **slot) {
  void *st = vi_trace(*slot);
#if GC_STICKY_COPY
  if (st && (avant_header(st)->flags & AVANT_FLAG_FORWARDED)) {
    vi_cand++;
  }
#endif
  vi_note(slot, st);
}

static void vi_t_object(void *p) {
  VI_EACH_SLOT(p, vi_all(p), slot, vi_t_slot(slot));
}

static void vi_t_root(void **slot) {
  (void)vi_trace(*slot);
}

#if GC_STICKY_COPY
static inline int vi_stale(void *p) {
  void *st = gc_object_payload(p, NULL, NULL);
  return st && (avant_header(st)->flags & AVANT_FLAG_FORWARDED);
}

static void vi_stale_root(void **slot) {
  vi_cand += (uint64_t)vi_stale(*slot);
}
#endif


/* The young blocks' objects that are not young (promoted in place, or
 * mature in a block young allocation reused): their lines, reached or
 * not (the lines are side state no page write shows). */
static void vi_t_block_object(AvantHeader *h, Block *b) {
  (void)b;
  if (!(h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_FORWARDED)) && !vr_unreached(payload_of(h))) {
    vi_cand += sticky_why(payload_of(h)) != NULL;
  }
}

static void pg_set_range(uint64_t *m, const void *lo, size_t n) {
  uintptr_t off;
  size_t p;
  if (!n || !heap_offset(lo, &off)) {
    return;
  }
  for (p = (size_t)(off / PAGE_SIZE); p <= (size_t)((off + n - 1) / PAGE_SIZE); p++) {
    pg_set(m, p);
  }
}

/* T, incremental: 1 when it proved the whole check silent (pg_rec then
 * holds the new record), 0 when it found a candidate or could not read
 * the pagemap (nothing is left set). */
static int vi_sticky(void) {
  ThreadReg *t;
  size_t i;
  size_t pages;
  vb_ensure();
  pages = pg_ensure();
  if (pages > sd_map_cap) {
    sd_map_cap = pages;
    sd_map = gc_xrealloc(sd_map, sd_map_cap * sizeof(uint64_t));
  }
  /* The pages written since the clear (or, without one at this
   * collection, since the last): this collection's own writes. */
  if (!sd_read(gc_heap.base, pages, sd_map)) {
    return 0;
  }
  pg_build_hot(pg_recm, pages);
  vi_cand = 0;
  verify_each_root(vi_t_root, 1);
  while (gc_heap.work_n) {
    vi_t_object(verify_pop());
  }
  vi_sources(pages, vi_t_object);
#if GC_STICKY_COPY
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, {
      vi_cand += (uint64_t)vi_stale(*slot);
      vi_cand += (uint64_t)vi_stale((void *)slot);
    });
  }
  gc_each_runtime_root(vi_stale_root);
#endif
  for (i = 0; i < gc_heap.vnoted_n; i++) {
    vi_cand += sticky_why(gc_heap.vnoted[i]) != NULL;
  }
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    gc_foreach_block_object(b, vi_t_block_object);
    pg_set_range(pg_out, b->base, BLOCK_SIZE);
  }
  if (gc_heap.young_large_n) {
    LargeIter it;
    AvantHeader *lh;
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      if (lh->flags & AVANT_FLAG_YOUNG) {
        pg_set_range(pg_out, lh, gc_obj_total(lh));
      }
    }
  }
  if (!vi_cand) {
    /* What this check visited, and what this collection promoted (the
     * collector's noted list, followed to the copies): an object the
     * collector reached may be named by roots the young copy rewrote, and
     * vb_reach only says which objects may be passed over (adding one
     * passes over less). */
    vr_record(0);
    for (i = 0; vr_valid && i < gc_heap.noted_n; i++) {
      void *n = gc_noted_object((uint32_t)i);
      if (avant_header(n)->flags & AVANT_FLAG_FORWARDED) {
        n = gc_forward_of(avant_header(n));
      }
      (void)vb_set(vb_reach, n, 0);
    }
  }
  verify_clear_traced();
  if (vi_cand) {
    return 0;
  }
  {
    uint64_t *x = pg_rec;
    pg_rec = pg_out;
    pg_out = x;
  }
  v_rec_all = 0;
  return 1;
}

/* An incremental check found a candidate: the whole one runs now, and the
 * record is rebuilt by the next rebasing pass. */
static void vi_fallback(void) {
  gc_heap.v_fallback++;
  v_incr_now = 0;
  v_rec_all = 1;
}

void gc_verify_remembered(void) {
  if (!(v_incr_now && vi_remembered())) {
    if (v_incr_now) {
      vi_fallback();
    }
    verify_remembered_whole();
  }
}

void gc_verify_sticky(void) {
  if (v_incr_now && vi_sticky()) {
    gc_heap.v_tincr++;
  } else {
    if (v_incr_now) {
      vi_fallback();
    }
    verify_sticky_whole();
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
  v_defrag_reports++;
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
  EACH_REF_SLOT_WALK(p, slot, { verify_defrag_word(*slot, "field", (void *)slot); });
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
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, { verify_defrag_word(*slot, "root slot", (void *)slot); });
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
 * that was installed before. Every freed block is held (Stage 28 W18C,
 * O120: until then at most 4,096 were, and past them a freed block was
 * reused at once, unchecked); a refused mprotect (the mapping-count
 * limit) leaves a block held but open, and is counted
 * (verify_unprotected in the stats line), so a run never checks less
 * silently.
 * Freed lines in blocks that keep live ones cannot be protected (a line
 * is 256 bytes); instead the young sweep holds such blocks back from the
 * young allocator until the next collection begins, so the precise half
 * still finds a reference to a dead object's lines naming freed memory.
 * Freed large runs are not quarantined (their reuse is page-run based;
 * the precise half reports a reference that names one). */
#define QUARANTINED 2
#define QUARANTINE_OPENED 3
#define QUARANTINE_HELD 4

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

static void quarantine_report(void *addr, uint64_t coll, const char *what, const char *why) {
  if (gc_heap.verify_lost++ < 32) {
    char buf[320];
    size_t n = 0;
    quarantine_write_str(buf, &n, "avant gc verify: lost reference: access to ");
    quarantine_write_hex(buf, &n, (uint64_t)(uintptr_t)addr);
    quarantine_write_str(buf, &n, what);
    quarantine_write_dec(buf, &n, coll);
    quarantine_write_str(buf, &n, why);
    (void)!write(2, buf, n);
  }
}

static const char *const quarantine_across = " (a reference kept outside the roots across a collection)\n";

/* Only async-signal-safe calls: write, mprotect, sigaction. */
static void quarantine_on_fault(int sig, siginfo_t *si, void *uc) {
  uintptr_t off;
  void *addr = si ? si->si_addr : NULL;
  uint32_t i;
  (void)uc;
  for (i = 0; addr && i < gc_heap.held_runs_n; i++) {
    struct GcHeldRun *r = &gc_heap.held_runs[i];
    if (r->state == QUARANTINED && (uint8_t *)addr >= r->lo && (uint8_t *)addr < r->hi) {
      if (r->drop == 1) {
        quarantine_report(addr, r->coll, " in a Buf's storage that a growth replaced after coll=",
                          " (a pointer from avant_buf_bytes kept across a growth of its Buf: call avant_buf_bytes again after anything that may grow it)\n");
      } else if (r->drop) {
        quarantine_report(addr, r->coll, " in a runtime buffer that a growth replaced after coll=",
                          " (a pointer into the old buffer kept across its growth)\n");
      } else {
        quarantine_report(addr, r->coll, " in a large run freed at coll=", quarantine_across);
      }
      r->state = QUARANTINE_OPENED;
      if (mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_READ | PROT_WRITE) == 0) {
        return;
      }
    }
  }
  if (addr && heap_offset(addr, &off) && gc_heap.chunks[off >> CHUNK_SHIFT].kind == CHUNK_SMALL) {
    Block *b = &gc_heap.blocks[off >> BLOCK_SHIFT];
    if (b->clean == QUARANTINED) {
      quarantine_report(addr, b->idle, " in a block freed at coll=", quarantine_across);
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
  if (!gc_heap.set.verify) {
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
  if (gc_heap.quarantine_n > gc_heap.v_held_blocks_max) {
    gc_heap.v_held_blocks_max = gc_heap.quarantine_n;
  }
  return 1;
}

/* A dead large object's run (gc_release_large, gc_sweep_large): its
 * start bit is cleared (no lookup finds it) and its pages stay allocated,
 * protected whole at the collection's end (the large space's iterator
 * steps over a run without its start bit by the page table, never its
 * header: Stage 28 W18C; until then the first page stayed open), until
 * the next collection begins. 1: held. */
int gc_quarantine_run(AvantHeader *h) {
  uint8_t *base;
  struct GcHeldRun *r;
  if (!gc_heap.set.verify) {
    return 0;
  }
  if (gc_heap.held_runs_n == gc_heap.held_runs_cap) {
    uint32_t cap = gc_heap.held_runs_cap ? gc_heap.held_runs_cap * 2 : 256;
    struct GcHeldRun *nr = realloc(gc_heap.held_runs, (size_t)cap * sizeof *nr);
    if (!nr) {
      gc_heap.v_unprotected++;
      return 0;
    }
    gc_heap.held_runs = nr;
    gc_heap.held_runs_cap = cap;
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
  r->lo = base;
  r->hi = base + gc_large_bytes(h);
  r->state = QUARANTINE_HELD;
  r->drop = 0;
  r->coll = avant_g_stats.collections;
  if (gc_heap.held_runs_n > gc_heap.v_held_runs_max) {
    gc_heap.v_held_runs_max = gc_heap.held_runs_n;
  }
  return 1;
}

/* A growth dropped the large buffer H outside a collection
 * (avant_gc_large_drop; Stage 28 W18C, O145) and gc_release_large held
 * it: it is protected now, not at the next collection's end, so a C
 * pointer into it (avant_buf_bytes' result kept across the growth, DROP
 * 1, or any runtime pointer into the old buffer, DROP 2) faults at its
 * first use and is reported. It goes back when the next collection
 * begins, as every held run does. */
void gc_quarantine_drop_seal(AvantHeader *h, int drop) {
  uint32_t i = gc_heap.held_runs_n;
  while (i > 0) {
    struct GcHeldRun *r = &gc_heap.held_runs[--i];
    if (r->h == h) {
      r->drop = drop;
      if (r->state == QUARANTINE_HELD && r->hi > r->lo) {
        if (mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_NONE) == 0) {
          r->state = QUARANTINED;
        } else {
          gc_heap.v_unprotected++;
        }
      }
      return;
    }
  }
}

/* At the end of a collection: the blocks and runs it freed become
 * PROT_NONE (a refused mprotect leaves one held, unprotected, until the
 * release). */
void gc_quarantine_seal(void) {
  Block *b;
  uint32_t i;
  for (i = 0; i < gc_heap.held_runs_n; i++) {
    struct GcHeldRun *r = &gc_heap.held_runs[i];
    if (r->state == QUARANTINE_HELD && r->hi > r->lo) {
      if (mprotect(r->lo, (size_t)(r->hi - r->lo), PROT_NONE) == 0) {
        r->state = QUARANTINED;
      } else {
        gc_heap.v_unprotected++;
      }
    }
  }
  for (b = gc_heap.quarantine; b; b = b->next) {
    if (b->clean == QUARANTINE_HELD) {
      if (mprotect(b->base, BLOCK_SIZE, PROT_NONE) == 0) {
        b->clean = QUARANTINED;
      } else {
        gc_heap.v_unprotected++;
      }
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
