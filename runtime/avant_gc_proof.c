#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The proof that the old heap is unchanged (the statement is GcProof's
 * comment in avant_gc_internal.h).
 *
 * The argument. Let F be the last full collection. At F's end every
 * object that is not young was marked by F: the old objects. An old
 * object dies only when every path F followed to it is broken, and a path
 * of F is a root's reference to an old object followed by references
 * between old objects as they were at F.
 *
 *   A root's reference is broken when no root names its referent any
 *   more. The roots are the ones the full mark starts from
 *   (gc_mark_roots): every thread's frame slots, the pins, the task roots
 *   and the runtime-held slots; gc_proof_each_root walks them, and F's
 *   end saves each one's word and the object it names (gc_proof_full_end).
 *   The check (gc_proof_check) walks them again: a saved referent whose
 *   root holds another word must be named by some other root.
 *
 *   A reference between old objects is broken only by a store into its
 *   holder. Generated code runs the barrier before every store of a
 *   reference into an object (V13), unless the holder is open (V14: since
 *   the last collecting call it was allocated, or barriered). The inline
 *   test skips the call only for a holder that is young or remembered
 *   (REM), and after F no object is young and none is remembered
 *   (gc_rem_full_scrub), so an old holder's first store since F reaches
 *   avant_barrier (or avant_barrier_slot, avant_barrier_at) before the
 *   store: gc_proof_store logs there, once (LOGGED), every reference word
 *   of the holder that names an object that is not young, with its
 *   address. A later store into the holder, with or without the call,
 *   changes nothing the log needs. The check reads each logged word
 *   again: it must hold the logged reference. (The same word, not the
 *   same holder: a reference moved to another word of its holder fails
 *   the proof. That is on the safe side and keeps the check one compare
 *   a word.) Runtime C keeps the same order (avant_gc.h, RC5): the
 *   barrier, or avant_gc_overwrite where it only removes or moves
 *   references, before its first store into an object it did not just
 *   allocate.
 *
 *   What the log cannot carry breaks the proof until the next full
 *   collection (gc_proof_break): a holder over PROOF_HOLDER_MAX bytes, a
 *   log over PROOF_LOG_MAX words, a program with threads (the log is the
 *   single mutator's), no young generation. A free of an old object
 *   outside a collection breaks it too.
 *
 * So when the proof holds every old object is reachable (and, with the
 * anchors below, a root's referent needs no root when a root before it
 * reaches it). What is not reachable is then young, provided no object
 * became old since F: a young collection that promoted and an allocation
 * in mature space are noted (gc_proof_grow), and then a full collection
 * is the sticky one (at the end of this file) instead of a young one. A
 * young collection frees every young object that neither a root nor a
 * remembered holder reaches, and every remembered holder is an old
 * object, so reachable: it frees exactly the unreachable objects.
 *
 * Old objects do not move between F's end and the check (a young
 * collection moves only objects that were young when it began; the
 * defragmenter runs inside a full collection, before F's end saves the
 * roots and empties the log), so the addresses in the log and in the
 * saved roots stay true.
 *
 * Before the first full collection F is the program's start: no object is
 * old, the log and the saved roots are empty, and the proof holds (with
 * nothing old to log). */

static int proof_ptr_cmp(const void *a, const void *b);

static const char *const proof_why_names[PROOF_WHYS] = {
    "intact",  "promoted", "mature", "freed",    "big",       "log_full",    "threads",
    "nursery", "off",      "ref_lost", "root_lost", "young_heavy", "?"};

const char *gc_proof_why_name(GcProofWhy why) {
  return proof_why_names[(unsigned)why < PROOF_WHYS ? (unsigned)why : PROOF_WHYS - 1];
}

/* The barrier's slow call logs (avant_gc.h): from a full collection's
 * end, while the proof is intact or the verifier is on (its oracle reads
 * the log whatever the proof says), in a program with one thread and a
 * young generation. Before the first full collection no object is old
 * and nothing is logged. */
uint8_t avant_gc_proof_logging;

static void proof_push(void ***v, uint32_t *n, uint32_t *cap, void *a, void *b) {
  if (*n + 2 > *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *v = gc_xrealloc(*v, (size_t)*cap * sizeof(void *));
  }
  (*v)[(*n)++] = a;
  (*v)[(*n)++] = b;
}

/* The proof cannot hold until the next full collection ends. The first
 * reason stays, except that the states in which nothing is logged
 * (threads, no young generation, the proof switched off) replace any
 * other. */
void gc_proof_break(GcProofWhy why) {
  if (gc_heap.proof.broken == PROOF_INTACT || (why >= PROOF_THREADS && why <= PROOF_OFF)) {
    gc_heap.proof.broken = (uint8_t)why;
  }
  /* Under the verifier the log goes on (its oracle reads it), once a
   * full collection has given it old objects to check. */
  avant_gc_proof_logging = (uint8_t)(avant_gc_proof_logging && gc_heap.set.verify && gc_heap.proof.broken < PROOF_THREADS);
}

/* An object became old that no full collection marked (a promotion, a
 * birth in mature space): the old objects are still all reachable while
 * nothing below breaks the proof, but no longer every object that is not
 * young, so a full collection is the sticky one (below), not a young
 * collection. The log goes on. */
void gc_proof_grow(GcProofWhy why) {
  if (gc_heap.proof.grown == PROOF_INTACT) {
    gc_heap.proof.grown = (uint8_t)why;
  }
}

/* The first barrier's slow call on OBJ (header H: in the heap, not young,
 * not LOGGED) since the last full collection, before the store: its
 * reference words that name objects that are not young, logged with their
 * addresses. An object with no reference word is passed over each time
 * (not marked: a large one may be freed before the next full collection,
 * avant_gc_large_drop, and the holders list is read then). Not in a
 * program with threads or without a young generation (the log is one
 * mutator's; nothing reads it there). */
__attribute__((noinline)) void gc_proof_log(void *obj, AvantHeader *h) {
  GcProof *p = &gc_heap.proof;
  uint32_t n0 = p->log_n;
  /* Only an old object is logged: one that became old since F breaks no
   * path of F (its words are not F's references) and the sticky full
   * collection traces it when it is reachable. The side mark says which
   * (F left it on every old object and on nothing else, below). */
  if (p->broken >= PROOF_THREADS || h->type_id == AVANT_TYPE_BYTES || !full_marked(h)) {
    return;
  }
  if (gc_heap.set.verify) {
    gc_verify_proof_logging(obj);
  }
  h->flags |= AVANT_FLAG_LOGGED;
  if (p->holders_n == p->holders_cap) {
    p->holders_cap = p->holders_cap ? p->holders_cap * 2 : 64;
    p->holders = gc_xrealloc(p->holders, (size_t)p->holders_cap * sizeof(void *));
  }
  p->holders[p->holders_n++] = obj;
  gc_heap.ctr.proof.logged++;
  if (h->size > PROOF_HOLDER_MAX) {
    gc_proof_break(PROOF_BIG);
    if (!gc_heap.set.verify) {
      return;
    }
  }
  EACH_REF_SLOT(obj, slot, {
    void *v = *slot;
    Block *vb;
    AvantHeader *vl;
    if (gc_field_object(v, &vb, &vl) && !(avant_header(v)->flags & AVANT_FLAG_YOUNG) &&
        full_marked(avant_header(v))) {
      proof_push(&p->log, &p->log_n, &p->log_cap, (void *)slot, v);
    }
  });
  gc_heap.ctr.proof.refs += (uint64_t)(p->log_n - n0) / 2;
  if (p->log_n > PROOF_LOG_MAX) {
    gc_proof_break(PROOF_LOG_FULL);
  }
}

/* Runtime C is about to remove or move references among the words of
 * OBJ, an object it did not allocate in this call, and stores no
 * reference OBJ does not already hold (so nothing is remembered: that is
 * avant_barrier's): the proof's part of the barrier alone (avant_gc.h,
 * RC5; avant_gc_overwrite there makes the call only while something is
 * logged). OBJ is an object's start, a static object's or null. Never
 * collects. */
__attribute__((visibility("hidden"))) void avant_gc_overwrite_slow(void *obj) {
  uintptr_t off = (uintptr_t)obj - (uintptr_t)gc_heap.base - sizeof(AvantHeader);
  AvantHeader *h;
  if (off >= gc_heap.committed || gc_thread.collecting) {
    return;
  }
  h = avant_header(obj);
  if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG)) == AVANT_FLAG_HEAP) {
    gc_proof_store(obj, h);
  }
}

/* FN on every root's word, in one order: every thread's frame slots, the
 * pins, the task roots, the runtime-held slots (gc_mark_roots' roots;
 * the last as gc_each_runtime_root walks them). */
void gc_proof_each_root(void (*fn)(void *word, void *ctx), void *ctx) {
  ThreadReg *t;
  uint32_t i;
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, { fn(*slot, ctx); });
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    fn(p->payload, ctx);
  }
  EACH_TASK_ROOT(slot, fn(*slot, ctx));
  for (t = gc_heap.threads; t; t = t->next) {
    if (t->owner) {
      for (i = 0; i < t->owner->nroots; i++) {
        if (t->owner->roots[i]) {
          fn(*t->owner->roots[i], ctx);
        }
      }
    }
  }
  if (!gc_thread.reg.head) {
    for (i = 0; i < gc_thread.nroots; i++) {
      if (gc_thread.roots[i]) {
        fn(*gc_thread.roots[i], ctx);
      }
    }
  }
}

/* A full collection begins: the logged holders' bits are cleared while
 * every holder is where it was logged (before the mark, the
 * defragmenter and the sweep), and the log is emptied. */
void gc_proof_full_begin(void) {
  GcProof *p = &gc_heap.proof;
  uint32_t i;
  for (i = 0; i < p->holders_n; i++) {
    avant_header(p->holders[i])->flags &= (uint16_t)~AVANT_FLAG_LOGGED;
  }
  gc_side_use(GC_SIDE_HOLDERS, (uint64_t)p->holders_n * sizeof(void *));
  p->holders_n = 0;
  p->log_n = 0;
}

static void proof_save_root(void *word, void *ctx) {
  GcProof *p = &gc_heap.proof;
  (void)ctx;
  proof_push(&p->roots, &p->roots_n, &p->roots_cap, word, word ? gc_object_payload(word, NULL, NULL) : NULL);
}

/* A full collection ends (after its sweep: every object in the heap that
 * is not young was marked, and nothing moves again before the check):
 * the roots are saved and the proof is intact, unless the program has
 * threads or no young generation. */
void gc_proof_full_end(void) {
  GcProof *p = &gc_heap.proof;
  /* Young objects this trace marked and left young (the young
   * generation, or its buffers: keep_large). */
  int kept = gc_heap.keep_young || (TUNE(keep_large, FULL_KEEP_LARGE) && gc_heap.keep_large && gc_heap.keep_young_live);
  p->roots_n = 0;
  p->broken = PROOF_INTACT;
  p->grown = PROOF_INTACT;
  /* A young object a trace that left the young generation young marked
   * keeps its side mark, and may become old later: the next full
   * collection cannot take the marks for the old objects. */
  p->no_sticky = (uint8_t)kept;
  gc_proof_lines_save();
  /* The old objects' bytes: a trace that left the young generation young
   * marked young objects too (gc_heap.keep_young_live). */
  p->old_live = gc_heap.live - gc_heap.keep_young_live;
  if (!gc_heap.set.proof) {
    p->broken = PROOF_OFF;
  } else if (gc_heap.set.nursery_off) {
    p->broken = PROOF_NURSERY;
  } else if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    p->broken = PROOF_THREADS;
  }
  avant_gc_proof_logging = (uint8_t)(p->broken == PROOF_INTACT || (gc_heap.set.verify && p->broken < PROOF_THREADS));
  if (p->anchors_ok) {
    qsort(p->anchors, p->anchors_n, sizeof(void *), proof_ptr_cmp);
  }
  if (p->broken == PROOF_INTACT) {
    gc_proof_each_root(proof_save_root, NULL);
    gc_heap.ctr.proof.roots = (uint64_t)p->roots_n / 2;
  }
  if (gc_heap.set.verify) {
    gc_verify_proof_rebase();
  }
  /* After a trace that left the young generation (or its buffers,
   * keep_large) young the remembered set is not empty: an old holder
   * that names a young object stays remembered, and the inline barrier
   * test skips a remembered holder, so its first store since this collection would not reach the slow
   * call. Each is logged now instead, as that first store would log it
   * (after the oracle took its copy, which the log must match). */
  if (kept && avant_gc_proof_logging) {
    uint32_t i;
    for (i = 0; i < gc_heap.rem_n; i++) {
      void *obj = gc_heap.remset[i];
      AvantHeader *h = avant_header(obj);
      if ((h->flags & (AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG | AVANT_FLAG_LOGGED)) == AVANT_FLAG_HEAP) {
        gc_proof_log(obj, h);
      }
    }
  }
}

/* The check's walk of the roots: K counts them; a saved referent whose
 * root now holds another word (or has no root at that place) is missing
 * until another root is found naming it. */
struct proof_check_ctx {
  uint32_t k;
};

static void proof_miss(void *start) {
  GcProof *p = &gc_heap.proof;
  if (p->miss_n == p->miss_cap) {
    p->miss_cap = p->miss_cap ? p->miss_cap * 2 : 64;
    p->miss = gc_xrealloc(p->miss, (size_t)p->miss_cap * sizeof(void *));
  }
  p->miss[p->miss_n++] = start;
}

static void proof_check_root(void *word, void *ctx) {
  GcProof *p = &gc_heap.proof;
  struct proof_check_ctx *c = ctx;
  uint32_t i = c->k++ * 2;
  if (p->cur_n == p->cur_cap) {
    p->cur_cap = p->cur_cap ? p->cur_cap * 2 : 64;
    p->cur = gc_xrealloc(p->cur, (size_t)p->cur_cap * sizeof(void *));
  }
  p->cur[p->cur_n++] = word;
  if (i < p->roots_n && p->roots[i] != word && p->roots[i + 1]) {
    proof_miss(p->roots[i + 1]);
  }
}

static int proof_ptr_cmp(const void *a, const void *b) {
  uintptr_t x = (uintptr_t) * (void *const *)a;
  uintptr_t y = (uintptr_t) * (void *const *)b;
  return x < y ? -1 : x > y;
}

/* Does the proof hold now? PROOF_INTACT, or the first reason it does not.
 * The caller is at a collection's start (the world stopped, the inline
 * allocations counted). No heap word is written. The roots are compared
 * place by place with the saved ones first: a root that holds the word it
 * held at F names the object it named (an old object neither moves nor is
 * freed while the proof is intact), so with the same frames in place the
 * check resolves nothing; only when a saved referent's root changed are
 * the current roots resolved (gc_object_payload: a root may name the
 * inside of an object) and searched. */
GcProofWhy gc_proof_check(void) {
  GcProof *p = &gc_heap.proof;
  struct proof_check_ctx c;
  uint32_t i;
  if (p->broken != PROOF_INTACT) {
    return (GcProofWhy)p->broken;
  }
  if (p->log_n > gc_heap.ctr.proof.log_peak) {
    gc_heap.ctr.proof.log_peak = p->log_n;
  }
  for (i = 0; i < p->log_n; i += 2) {
    if (*(void **)p->log[i] != p->log[i + 1]) {
      return PROOF_REF_LOST;
    }
  }
  c.k = 0;
  p->miss_n = 0;
  p->cur_n = 0;
  gc_proof_each_root(proof_check_root, &c);
  /* The saved roots past the last current one. */
  for (i = c.k * 2; i < p->roots_n; i += 2) {
    if (p->roots[i + 1]) {
      proof_miss(p->roots[i + 1]);
    }
  }
  if (p->miss_n == 0) {
    return PROOF_INTACT;
  }
  for (i = 0; i < p->cur_n; i++) {
    p->cur[i] = p->cur[i] ? gc_object_payload(p->cur[i], NULL, NULL) : NULL;
  }
  qsort(p->cur, p->cur_n, sizeof(void *), proof_ptr_cmp);
  for (i = 0; i < p->miss_n; i++) {
    if (!bsearch(&p->miss[i], p->cur, p->cur_n, sizeof(void *), proof_ptr_cmp) &&
        (!p->anchors_ok || bsearch(&p->miss[i], p->anchors, p->anchors_n, sizeof(void *), proof_ptr_cmp))) {
      return PROOF_ROOT_LOST;
    }
  }
  return PROOF_INTACT;
}

/* Anchors.
 *
 * An ordinary trace marks the roots' referents one at a time, in the
 * order gc_proof_each_root walks them, each with everything it reaches
 * before the next (gc_mark_roots). A referent the marks of the roots
 * before it already reached is not an anchor: some path of F leads to it
 * from a root marked before it. A referent that is an anchor is noted.
 *
 * So the root check needs no root for a referent that is not an anchor.
 * Take the roots in that order and suppose every logged word holds its
 * logged reference (else the check failed already): every path of F is
 * intact. A root's referent is still reachable when a root names it; an
 * anchor that no root names fails the check; a referent that is not an
 * anchor is reached by an intact path of F from a referent of a root
 * before it, reachable by the same argument, one root earlier. Every old
 * object is reached by a path of F from some root's referent, so every
 * old object is reachable.
 *
 * Only an ordinary trace notes anchors: a trace that left the young
 * generation young marks no old object through a young one, and the
 * sticky full collection marks none through an old one, so neither knows
 * what each root reaches; after them every referent counts as an anchor
 * (anchors_ok is 0). A program's chain whose tail a local names is the
 * case: the tail moves on at each node, and the node a root named at F
 * is reached from the chain's head, a root before it. */
void gc_proof_anchors_begin(void) {
  gc_heap.proof.anchors_n = 0;
  gc_heap.proof.anchors_ok = 0;
}

void gc_proof_anchor(void *start) {
  GcProof *p = &gc_heap.proof;
  if (p->anchors_n == p->anchors_cap) {
    p->anchors_cap = p->anchors_cap ? p->anchors_cap * 2 : 64;
    p->anchors = gc_xrealloc(p->anchors, (size_t)p->anchors_cap * sizeof(void *));
  }
  p->anchors[p->anchors_n++] = start;
}

void gc_proof_anchors_end(void) {
  gc_heap.proof.anchors_ok = 1;
}

/* The defragmenter moved objects (before it frees their blocks): an
 * anchor it moved is noted at its new place. */
void gc_proof_anchors_forward(void) {
  GcProof *p = &gc_heap.proof;
  uint32_t i;
  for (i = 0; i < p->anchors_n; i++) {
    AvantHeader *h = avant_header(p->anchors[i]);
    if (h->flags & AVANT_FLAG_FORWARDED) {
      p->anchors[i] = gc_forward_of(h);
    }
  }
}

/* The sticky full collection.
 *
 * When the proof holds for the old objects but some object became old
 * since F (gc_proof_grow: a promotion, a birth in mature space), every
 * old object is still reachable (the argument above), and what may be
 * unreachable is what is not old: the young objects and the ones that
 * became old since F. A full collection then marks only those: it keeps
 * F's side marks, which are on every old object and on nothing else
 * (below), so the mark stops at an old object as at one it already
 * marked, and it starts from the roots and from every logged holder's
 * words (gc_mark_sticky_holders).
 *
 * That marks every reachable object that is not old. Take a path from a
 * root to such an object X, and the last old object A on it (if there is
 * none, the mark follows the whole path from the root). A's word that
 * names the next object of the path names an object that is not old; at
 * F every word of A named an old object or nothing (F marked what A named
 * and promoted it), so the word was stored into since F, and A's first
 * store since F was logged (the barrier before the store): A is a logged
 * holder, the mark reads its words as they are now, and follows the rest
 * of the path, which has no old object. (After a trace that left the
 * young generation young an old object may name a young one; then F's
 * marks are on young objects too and this collection does not run:
 * GcProof.no_sticky.) And it marks nothing unreachable: the logged
 * holders it starts from are old, so reachable.
 *
 * The line marks are F's, saved at F's end (gc_proof_lines_save) and put
 * back before the mark (gc_proof_lines_restore): an old object's lines
 * are live, and the lines of everything that became old since F are
 * marked again only when the mark reaches it. The sweep then frees what
 * a trace would have freed. Nothing is evacuated (an evacuation rewrites
 * every reference to what it moves, a walk of the whole heap). At its end
 * the collection is a full collection's end like any other: every object
 * is old and marked, or freed.
 *
 * F's side marks are only on old objects: a full collection clears them
 * all before it marks, a block the defragmenter evacuated has its marks
 * (and line marks) cleared when it is freed (gc_defrag_release), an
 * object the defragmenter moved out of a block that stays has its mark
 * cleared, no other block leaves use with a marked object, and nothing
 * else sets one between two full collections. */

/* A full collection ended (its sweep done): the line marks of every
 * block in use are saved, each block's stamped with the save's number (a
 * block that is not in use at F holds no line of an old object: its
 * stamp is older). The copy is an anonymous mapping as large as the
 * heap's whole line table, and the stamps one word a block of the
 * reservation, taken at the first save; only the pages written become
 * memory. */
void gc_proof_lines_save(void) {
  gc_heap.proof.lines_saved = 0;
  if (gc_heap.proof.broken >= PROOF_THREADS || gc_heap.proof.no_sticky) {
    return;
  }
  if (!gc_heap.lines_f) {
    uint64_t blocks = gc_heap.reserved >> BLOCK_SHIFT;
    void *m = mmap(NULL, (size_t)(blocks * LINES_PER_BLOCK), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    void *g = mmap(NULL, (size_t)(blocks * sizeof(uint32_t)), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED || g == MAP_FAILED) {
      if (m != MAP_FAILED) {
        munmap(m, (size_t)(blocks * LINES_PER_BLOCK));
      }
      if (g != MAP_FAILED) {
        munmap(g, (size_t)(blocks * sizeof(uint32_t)));
      }
      return;
    }
    gc_heap.lines_f = m;
    gc_heap.lines_f_block = g;
  }
  gc_heap.lines_f_gen++;
  EACH_MATURE_BLOCK(b, {
    uint64_t k = (uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
    memcpy(&gc_heap.lines_f[k * LINES_PER_BLOCK], b->line_mark, LINES_PER_BLOCK);
    gc_heap.lines_f_block[k] = gc_heap.lines_f_gen;
  });
  gc_heap.proof.lines_saved = 1;
}

/* The sticky full collection's start: the line marks of every block in
 * use are F's again; a block that was not in use at F has none live. */
void gc_proof_lines_restore(void) {
  EACH_MATURE_BLOCK(b, {
    uint64_t k = (uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
    if (gc_heap.lines_f_block[k] == gc_heap.lines_f_gen) {
      memcpy(b->line_mark, &gc_heap.lines_f[k * LINES_PER_BLOCK], LINES_PER_BLOCK);
    } else {
      memset(b->line_mark, 0, LINES_PER_BLOCK);
    }
  });
}
