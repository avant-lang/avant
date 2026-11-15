#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The full collection's marking and sweeping (Stage 28 W12, moved from
 * avant_rt.c): non-moving mark from the roots with line marks, and the
 * block sweep that frees or recycles blocks by their live lines. The
 * defragmentation a full collection may run between the two is
 * avant_gc_defrag.c's. */

/* The object at START (header H, in block B, or a large object when B is
 * NULL), whose side mark the caller has just set: its lines through the
 * block the lookup already gave (until Stage 29 W-FIX1c a second lookup
 * found it again, O203), its bytes, and the work list. A full collection
 * promotes every object it reaches (Stage 28 W15b), so no object is young
 * after it (its remembered set and young budget are empty,
 * gc_sticky_full_end); the flags are written only when one is set, so a
 * collection of a heap that is already mature dirties no header. An
 * object the inline path allocated may have no start bit yet
 * (avant_gc_alloc.c, "Late start bits"), and the sweep and the
 * defragmenter find live objects by the bits. A root's object has its
 * bit by the lookup that found it (gc_object_payload builds its block's),
 * so none is set here; a field's object gets it in the mark's loop or
 * after the mark (gc_mark_roots). */
static inline void mark_object(void *start, AvantHeader *h, Block *b) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  if (gc_heap.keep_young && (h->flags & AVANT_FLAG_YOUNG)) {
    /* A trace that leaves the young generation young: the object stays
     * young, and its lines stay unmarked (its block is a young block). */
    gc_heap.keep_young_live += total;
    b = NULL;
  } else if (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED)) {
    h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  }
  if (b) {
    mark_lines(b, (size_t)((uint8_t *)h - b->base), total);
  }
  gc_heap.live += total;
  if (gc_heap.work_n == gc_heap.work_cap) {
    gc_work_push(start);
  } else {
    gc_heap.work[gc_heap.work_n++] = start;
  }
}

/* Any reference (a root's, which may name the inside of an object; a
 * large object's; a static object's): one lookup, then the mark. */
static void mark(void *p) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload(p, &b, &ol);
  AvantHeader *h;
  if (!start) {
    return;
  }
  h = avant_header(start);
  if (full_marked(h)) {
    return;
  }
  full_set_mark(h);
  mark_object(start, h, b);
}

/* The mark loop's state in locals (the Valk stage, VALK-T, O578 and O585):
 * what a field's mark reads of gc_heap, loaded once, so the loop below
 * keeps it in registers; nothing in a full mark changes the committed
 * size, the side tables or the epoch. base8 is gc_heap.base plus a
 * header's size: a payload address less it is its header's offset. live
 * and the work list's count are written back when the loop leaves
 * (mark_close). */
typedef struct MarkCtx {
  uintptr_t base8;
  uint64_t committed;
  uint64_t *marks;
  uint8_t *lines;
  uint8_t epoch;
  uint64_t live;
  void **work;
  uint32_t n;
  uint32_t cap;
} MarkCtx;

static inline __attribute__((always_inline)) void mark_open(MarkCtx *c) {
  c->base8 = (uintptr_t)gc_heap.base + sizeof(AvantHeader);
  c->committed = gc_heap.committed;
  c->marks = gc_heap.marks;
  c->lines = gc_heap.lines;
  c->epoch = gc_heap.line_epoch;
  c->live = 0;
  c->work = gc_heap.work;
  c->n = gc_heap.work_n;
  c->cap = gc_heap.work_cap;
}

static inline __attribute__((always_inline)) void mark_close(MarkCtx *c) {
  gc_heap.live += c->live;
  gc_heap.work_n = c->n;
}

/* The lines after the first two of an object that covers more than two
 * (over 256 bytes: the uncommon case, kept out of the loop so the loop
 * makes no call). */
static __attribute__((noinline)) void mark_lines_rest(uint8_t *lines, size_t l, size_t e, uint8_t epoch) {
  memset(lines + l, epoch, e - l);
}

/* A traced object's field (Stage 29 W-FIX1c, O203; the Valk stage,
 * VALK-T, O585 and O578): 1 when its word V names an object this call
 * marked that has references of its own to scan (the caller pushes it,
 * or scans it next), else 0. By the field rule (avant_gc_internal.h: a
 * field holds null, an exact start or a static address) V is classified
 * by one compare, the header's offset against the committed size, which
 * null and every static address fail; what passes is the start of a live
 * object, marked through the granule in hand, with no start-bit lookup,
 * no chunk kind and no block-in-use test (until VALK-T those were asked
 * of every field, and an address that failed one took mark(); before
 * W-FIX1c a field cost eight calls). Then what mark_object does: the
 * flags written only when one is set, the lines (through the heap's line
 * table at the header's offset: an object lies in one block; a large
 * object, which the LARGE flag tells, has no line marks) and the bytes.
 * An object without references (AVANT_TYPE_BYTES: a string, a numeric
 * buffer) is not scanned: its scan would visit nothing. Roots, pins and
 * runtime-held slots take mark(), which resolves an address inside an
 * object.
 *
 * In line in the two loops below, over MarkCtx's locals. W-FIX1c kept it
 * out of line because, inlined into EACH_REF_SLOT's one loop nest, the
 * nest's registers were spilled around it (167 instructions per object of
 * a full collection against 149); the small record's loop is now its own
 * (drain) and the bitmap walk is a function of its own (scan_walk). */
static inline __attribute__((always_inline)) int mark_field(MarkCtx *c, void *v, const int bits, const int keep) {
  uintptr_t off = (uintptr_t)v - c->base8;
  uint64_t g;
  uint64_t w;
  AvantHeader *h;
  uint16_t fl;
  size_t total;
  size_t l;
  size_t e;
  if (off >= c->committed) {
    return 0;
  }
  g = off >> 3;
  w = c->marks[g >> 6];
  if ((w >> (g & 63)) & 1ULL) {
    return 0;
  }
  c->marks[g >> 6] = w | (1ULL << (g & 63));
  h = (AvantHeader *)((uint8_t *)v - sizeof(AvantHeader));
  fl = h->flags;
  total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  c->live += total;
  if (__builtin_expect((fl & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED | AVANT_FLAG_LARGE)) != 0, 0)) {
    if (keep && (fl & AVANT_FLAG_YOUNG)) {
      /* Left young (KEEP, the trace that leaves the young generation
       * young): no flag cleared, no line marked. */
      gc_heap.keep_young_live += total;
      return h->type_id != AVANT_TYPE_BYTES;
    }
    if (fl & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED)) {
      h->flags = (uint16_t)(fl & ~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED));
      /* Its late start bit, in the loop that sets them (BITS, a
       * constant in each loop: gc_mark_roots' comment); the other loop
       * leaves it to gc_lazy_build_live after the mark. */
      if (bits && !(fl & AVANT_FLAG_SURVIVED)) {
        start_bit_set(h, 1);
      }
    }
    if (fl & AVANT_FLAG_LARGE) {
      return h->type_id != AVANT_TYPE_BYTES;
    }
  }
  l = off >> LINE_SHIFT;
  e = (off + total - 1) >> LINE_SHIFT;
  c->lines[l] = c->epoch;
  if (__builtin_expect(l != e, 0)) {
    c->lines[e] = c->epoch;
    if (e - l > 1) {
      mark_lines_rest(c->lines, l + 1, e, c->epoch);
    }
  }
  return h->type_id != AVANT_TYPE_BYTES;
}

/* Does the young object P name an object that is not young? (Out of the
 * loop: only a trace that leaves the young generation young asks it, of
 * the few young objects a young collection just left.) */
static __attribute__((noinline)) int young_names_old(void *p) {
  EACH_REF_SLOT(p, slot, {
    void *v = *slot;
    Block *vb;
    AvantHeader *vl;
    if (gc_field_object(v, &vb, &vl) && !(avant_header(v)->flags & AVANT_FLAG_YOUNG)) {
      return 1;
    }
  });
  return 0;
}

static inline __attribute__((always_inline)) void mark_push(MarkCtx *c, void *v) {
  if (__builtin_expect(c->n == c->cap, 0)) {
    gc_heap.work_n = c->n;
    gc_work_push(v);
    c->work = gc_heap.work;
    c->cap = gc_heap.work_cap;
    c->n = gc_heap.work_n;
  } else {
    c->work[c->n++] = v;
  }
}

/* The fields of an object that is not a small record (a pointer buffer,
 * an element buffer, a record with more than seven reference words), by
 * its bitmap. A function of its own, with its own copy of the loop's
 * state, so that drain's loop keeps its registers. */
static __attribute__((noinline)) void scan_walk(void *p, int bits, int keep) {
  MarkCtx c;
  mark_open(&c);
  EACH_REF_SLOT_WALK(p, slot, {
    void *v = *slot;
    if (keep ? mark_field(&c, v, 0, 1) : bits ? mark_field(&c, v, 1, 0) : mark_field(&c, v, 0, 0)) {
      mark_push(&c, v);
    }
  });
  mark_close(&c);
}

/* The mark loop: each object of the work list, its reference words read
 * from its type's list (gc_heap.refs, EACH_REF_SLOT's small record) and
 * marked in line; any other object through scan_walk. The object a
 * record's last reference word names is scanned next without passing
 * through the work list (a list's nodes, a tree's last child: no push and
 * no pop); the others are pushed. The order of a full mark changes
 * nothing it leaves: marks, line marks and the live bytes are sets and a
 * sum. */
static inline __attribute__((always_inline)) void drain(const int bits, const int keep) {
  MarkCtx c;
  mark_open(&c);
  while (c.n) {
    void **p = (void **)c.work[--c.n];
    for (;;) {
      const AvantHeader *ph = avant_header(p);
      uint64_t d = gc_heap.refs[ph->type_id];
      unsigned k = (unsigned)(d & 0xffu);
      void *next = NULL;
      if (keep && (ph->flags & AVANT_FLAG_YOUNG) && young_names_old(p)) {
        /* An old object reached through a young one: the trace cannot
         * leave the young generation young (gc_mark_roots). */
        gc_heap.keep_yold = 1;
        c.n = 0;
        break;
      }
      if (__builtin_expect(k > TYPE_REFS_MAX, 0)) {
        mark_close(&c);
        scan_walk(p, bits, keep);
        mark_open(&c);
        break;
      }
      if (k) {
        size_t nw = (size_t)ph->size / sizeof(void *);
        for (; k > 1; k--) {
          d >>= 8;
          if ((size_t)(d & 0xffu) < nw) {
            void *v = p[d & 0xffu];
            if (mark_field(&c, v, bits, keep)) {
              mark_push(&c, v);
            }
          }
        }
        d >>= 8;
        if ((size_t)(d & 0xffu) < nw) {
          void *v = p[d & 0xffu];
          if (mark_field(&c, v, bits, keep)) {
            next = v;
          }
        }
      }
      if (!next) {
        break;
      }
      p = (void **)next;
    }
  }
  mark_close(&c);
}

/* The loop twice, once for each way a young object the mark promotes
 * gets its late start bit (gc_mark_roots chooses). */
static __attribute__((noinline)) void drain_bits(void) {
  drain(1, 0);
}

static __attribute__((noinline)) void drain_plain(void) {
  drain(0, 0);
}

/* The trace that leaves the young generation young (gc_mark_roots). */
static __attribute__((noinline)) void drain_keep(void) {
  drain(0, 1);
}

static void mark_tls_slot(void **slot) {
  if (slot) {
    mark(*slot);
  }
}

void gc_mark_roots(void) {
  ThreadReg *t;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, { mark(*slot); });
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    mark(p->payload);
  }
  EACH_TASK_ROOT(slot, mark(*slot));
  gc_each_runtime_root(mark_tls_slot);
  /* The late start bits of the young objects the mark promotes, one of
   * two ways. Set in the loop (drain_bits), each costs the promoted
   * object a few instructions and every object marked one (the heap's
   * address is made again in the loop: 50 instructions a node for 49).
   * Left to gc_lazy_build_live after the mark (drain_plain), the loop
   * has its 49 and every object of an inline run in a block with a live
   * line is walked, about 13 instructions each. So the walk is taken
   * when the young generation is small beside what the last full
   * collection found live (an eighth: mostly old objects will be marked),
   * and the loop's own bit otherwise (a heap that is mostly young, a
   * program's first full collection). */
  gc_heap.mark_bits = (uint8_t)(avant_nursery_held * 8 > gc_heap.proof.old_live);
  if (gc_heap.keep_young) {
    /* The trace that leaves the young generation young (gc_collect):
     * nothing it marks is promoted, so no start bit is set in its loop.
     * If a young object names an old one, that old object's only path
     * may run through the young generation, which the proof does not
     * follow (avant_gc_proof.c): the mark stops and the collection
     * becomes an ordinary trace, marked again from the start. */
    gc_heap.mark_bits = 0;
    drain_keep();
    if (!gc_heap.keep_yold) {
      return;
    }
    gc_heap.ctr.sticky.keep_fell++;
    gc_heap.keep_young = 0;
    gc_heap.work_n = 0;
    gc_heap.live = 0;
    gc_heap.keep_young_live = 0;
    gc_sticky_keep_drop();
    gc_clear_side_marks();
    gc_line_epoch_advance();
    gc_mark_roots();
    return;
  }
  if (gc_heap.mark_bits) {
    drain_bits();
  } else {
    drain_plain();
  }
}

/* The sweep of one block after a full mark: a block with no live line
 * is released; a kept block has the start bits of every line it frees
 * cleared (32 granules a line: half a start word), so a hole's start
 * bits are clear before reuse (gc-design.md §2.3) and a dead object
 * whose header lies in a freed line is no longer a heap payload. A dead
 * object whose header lies in a live line keeps its bit (start bits are
 * not a liveness test). The inline allocation path still runs only in
 * fresh blocks (holes are W15b's). */
int gc_keep_if_live(Block *b) {
  uint64_t freed = 0;
  if (gc_block_live_lines(b) == 0) {
    /* AVANT_GC_STRESS=1: a block the full sweep releases is poisoned, as
     * an evacuated block is (the Valk stage, VALK-TM3, O650; before it
     * the full sweep poisoned nothing, so a stale reference to a mature
     * object that died here read its old bytes). */
    gc_poison(b->base, BLOCK_SIZE);
    return 0;
  }
  /* Word-wise (Stage 28 W15d, O94): the young sweep's pass over the whole
   * block clears the free lines' start bits and leaves the cursor on the
   * first hole, as the byte loop and gc_reset_block_bump did. */
  (void)gc_sweep_block_lines(b, 0, LINES_PER_BLOCK, &freed);
  if (gc_heap.set.stress) {
    /* And every line a kept block frees is poisoned, as the young sweep
     * poisons the young lines it frees (O650): the line marks are exact
     * (mark_lines marks every line an object touches), so a line no live
     * object marked holds nothing live. A line free before this
     * collection is poisoned again: it held nothing either. */
    int l;
    for (l = 0; l < LINES_PER_BLOCK; l++) {
      if (!line_live(b, l)) {
        gc_poison(b->base + (size_t)l * LINE_SIZE, LINE_SIZE);
      }
    }
  }
  return 1;
}

void gc_filter_blocks(Block **list, int (*keep)(Block *)) {
  Block *b = *list;
  Block *kept = NULL;
  while (b) {
    Block *next = b->next;
    if (keep && keep(b)) {
      b->next = kept;
      kept = b;
    } else {
      gc_cache_or_free_block(b);
    }
    b = next;
  }
  *list = kept;
}

