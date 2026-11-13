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
 * collection of a heap that is already mature dirties no header. */
static inline void mark_object(void *start, AvantHeader *h, Block *b) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  if (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED)) {
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
static inline __attribute__((always_inline)) int mark_field(MarkCtx *c, void *v) {
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
    if (fl & (AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED)) {
      h->flags = (uint16_t)(fl & ~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED));
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
static __attribute__((noinline)) void scan_walk(void *p) {
  MarkCtx c;
  mark_open(&c);
  EACH_REF_SLOT_WALK(p, slot, {
    void *v = *slot;
    if (mark_field(&c, v)) {
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
static void drain(void) {
  MarkCtx c;
  mark_open(&c);
  while (c.n) {
    void **p = (void **)c.work[--c.n];
    for (;;) {
      const AvantHeader *ph = avant_header(p);
      uint64_t d = gc_heap.refs[ph->type_id];
      unsigned k = (unsigned)(d & 0xffu);
      void *next = NULL;
      if (__builtin_expect(k > TYPE_REFS_MAX, 0)) {
        mark_close(&c);
        scan_walk(p);
        mark_open(&c);
        break;
      }
      if (k) {
        size_t nw = (size_t)ph->size / sizeof(void *);
        for (; k > 1; k--) {
          d >>= 8;
          if ((size_t)(d & 0xffu) < nw) {
            void *v = p[d & 0xffu];
            if (mark_field(&c, v)) {
              mark_push(&c, v);
            }
          }
        }
        d >>= 8;
        if ((size_t)(d & 0xffu) < nw) {
          void *v = p[d & 0xffu];
          if (mark_field(&c, v)) {
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

static void mark_tls_slot(void **slot) {
  if (slot) {
    mark(*slot);
  }
}

void gc_mark_roots(void) {
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        mark(*f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    mark(p->payload);
  }
  EACH_TASK_ROOT(slot, mark(*slot));
  gc_each_runtime_root(mark_tls_slot);
  drain();
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
    return 0;
  }
  /* Word-wise (Stage 28 W15d, O94): the young sweep's pass over the whole
   * block clears the free lines' start bits and leaves the cursor on the
   * first hole, as the byte loop and gc_reset_block_bump did. */
  (void)gc_sweep_block_lines(b, 0, LINES_PER_BLOCK, &freed);
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

