#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 *   single mutator's), no young generation.
 *
 * So when the proof holds every old object is reachable. What is not
 * reachable is then young, provided no object became old since F without
 * being marked: a young collection that promoted, an allocation in mature
 * space and a free of an old object outside a collection break the proof
 * too. A young collection frees every young object that neither a root
 * nor a remembered holder reaches, and every remembered holder is an old
 * object, so reachable: it frees exactly the unreachable objects.
 *
 * Old objects do not move between F's end and the check (a young
 * collection moves only objects that were young when it began; the
 * defragmenter runs inside a full collection, before F's end saves the
 * roots and empties the log), so the addresses in the log and in the
 * saved roots stay true.
 *
 * Before the first full collection F is the program's start: no object is
 * old, the log and the saved roots are empty, and the proof holds until
 * something is promoted or born old. */

static const char *const proof_why_names[PROOF_WHYS] = {
    "intact",  "promoted", "mature", "freed",    "big",       "log_full",    "threads",
    "nursery", "off",      "ref_lost", "root_lost", "young_heavy", "?"};

const char *gc_proof_why_name(GcProofWhy why) {
  return proof_why_names[(unsigned)why < PROOF_WHYS ? (unsigned)why : PROOF_WHYS - 1];
}

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
  gc_heap.proof.logging = (uint8_t)(gc_heap.set.verify && gc_heap.proof.broken < PROOF_THREADS);
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
  if (p->broken >= PROOF_THREADS || h->type_id == AVANT_TYPE_BYTES) {
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
    if (gc_field_object(v, &vb, &vl) && !(avant_header(v)->flags & AVANT_FLAG_YOUNG)) {
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
 * RC5). OBJ is an object's start, a static object's or null. Never
 * collects. */
__attribute__((visibility("hidden"))) void avant_gc_overwrite(void *obj) {
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
  p->roots_n = 0;
  p->broken = PROOF_INTACT;
  p->old_live = gc_heap.live;
  if (!gc_heap.set.proof) {
    p->broken = PROOF_OFF;
  } else if (gc_heap.set.nursery_off) {
    p->broken = PROOF_NURSERY;
  } else if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    p->broken = PROOF_THREADS;
  }
  p->logging = (uint8_t)(p->broken == PROOF_INTACT || (gc_heap.set.verify && p->broken < PROOF_THREADS));
  if (p->broken == PROOF_INTACT) {
    gc_proof_each_root(proof_save_root, NULL);
    gc_heap.ctr.proof.roots = (uint64_t)p->roots_n / 2;
  }
  if (gc_heap.set.verify) {
    gc_verify_proof_rebase();
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
    if (!bsearch(&p->miss[i], p->cur, p->cur_n, sizeof(void *), proof_ptr_cmp)) {
      return PROOF_ROOT_LOST;
    }
  }
  return PROOF_INTACT;
}
