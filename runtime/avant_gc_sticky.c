#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The young generation: S+ (Stage 28 W15c, D233), candidate S with the
 * opportunistic young copy. Candidate E, the evacuating nursery, was
 * deleted as a separate young generation in Stage 28 W15d (D232); the
 * mechanisms of E this file uses stay: header forwarding (gc_set_forward,
 * avant_gc_internal.h), the copy (splus_copy) and the noted list (below, moved
 * from avant_gc_young.c, which held E's young copy).
 *
 * Candidate S, the in-place young generation (Stage 28 W15b,
 * gc-design.md §3.1, StickyImmix), is this file with GC_STICKY_COPY 0
 * (an internal constant, avant_gc_internal.h). A young collection under
 * S moves nothing. It traces from the roots
 * (frame slots, pins, runtime-held slots) and the remembered set, and
 * promotes each young object it reaches where it lies: YOUNG cleared,
 * its lines marked live at the current line epoch. A young object it
 * does not reach is garbage, reclaimed by the line sweep of the blocks
 * young allocation used, never visited. Mature objects are scanned one
 * level where R1' says (gc-design.md §1.3): when a root, a young
 * object's field or a remembered object's field names them; deeper ones
 * only when remembered. After the collection no object is young, so the
 * remembered set is emptied (§3.1.1 point 1).
 *
 * Nothing moves, so nothing is rewritten and nothing needs pinning: no
 * frame slot, C stack word or register is changed, and a young object a
 * C local names stays where it is. Liveness still needs every reference
 * to be found: generated code keeps its references in slots across
 * collecting calls (D165), and runtime C keeps what it holds across a
 * collecting call in handle scopes (RC1, avant_gc.h; Stage 28 W16a). */
/* The noted list: every header MARKED a young collection sets, cleared
 * when it ends (C36, gc-design.md §2.6), in the order the trace reached
 * them. An entry is the object's payload address with tag bits in its
 * three low bits (the Valk stage, VALK-T, O579; avant_gc_internal.h,
 * NOTED_*), so that the passes after the trace read the list and skip
 * what they have nothing to do for without reading its header:
 *   NOTED_SURV   a young block object this collection promoted
 *                (PROMOTED_NOW): the only entries the copy looks at;
 *   NOTED_FIX    its scan found a field naming such an object: the only
 *                entries the fix-up walks;
 *   NOTED_MOVED  the copy moved it: the entry is the copy's address, and
 *                the clearing passes over it (the copy's flags were
 *                written clean).
 * Returns the entry's index. */
static inline uint32_t note_marked(void *payload, uintptr_t tag) {
  if (__builtin_expect(gc_heap.noted_n == gc_heap.noted_cap, 0)) {
    gc_heap.noted_cap = gc_heap.noted_cap ? gc_heap.noted_cap * 2 : 256;
    gc_heap.noted = gc_xrealloc(gc_heap.noted, gc_heap.noted_cap * sizeof(void *));
  }
  gc_heap.noted[gc_heap.noted_n] = (void *)((uintptr_t)payload | tag);
  return gc_heap.noted_n++;
}

/* A large pointer buffer the trace marked (young or not) is also on the
 * carded list, in the order it was marked: gc_cards_settle makes the
 * cards of each exact after the collection, and reads this list instead
 * of every noted entry (the Valk stage, VALK-T, O579). */
static inline void note_carded(void *payload, const AvantHeader *h) {
  if (gc_carded(h)) {
    if (gc_heap.carded_n == gc_heap.carded_cap) {
      gc_heap.carded_cap = gc_heap.carded_cap ? gc_heap.carded_cap * 2 : 64;
      gc_heap.carded = gc_xrealloc(gc_heap.carded, gc_heap.carded_cap * sizeof(void *));
    }
    gc_heap.carded[gc_heap.carded_n++] = payload;
  }
}

static inline void noted_tag(uint32_t i, uintptr_t tag) {
  gc_heap.noted[i] = (void *)((uintptr_t)gc_heap.noted[i] | tag);
}

/* A holder whose scan found a movable object when its entry's index was
 * not at hand (a remembered object or a carded one scanned a second
 * time): the fix-up walks it after the list. Its entry carries NOTED_FIX
 * already whenever a first scan covered the same slots, so this is a
 * second walk of slots the first one rewrote; it is here so that the
 * fix-up never depends on that. */
static void fix_extra_add(void *payload) {
  if (gc_heap.fix_extra_n == gc_heap.fix_extra_cap) {
    gc_heap.fix_extra_cap = gc_heap.fix_extra_cap ? gc_heap.fix_extra_cap * 2 : 64;
    gc_heap.fix_extra = gc_xrealloc(gc_heap.fix_extra, gc_heap.fix_extra_cap * sizeof(void *));
  }
  gc_heap.fix_extra[gc_heap.fix_extra_n++] = payload;
}

/* The flags a young collection set are cleared where they are set: every
 * entry that was not moved (a moved object's copy was written with clean
 * flags, and its old header is never read again: its start bit goes,
 * splus_finish). Also the end of SURVIVED on every object this
 * collection promoted: promote_young leaves it so that S+'s copy can
 * tell a first survivor from an aged one (Stage 28 W15d); an object
 * still young keeps it (a young large object at its first survival).
 * An entry whose object holder aging moved (splus_age_holders: the keep
 * list does not know the entry) is told by its header's forward. */
void gc_clear_noted(void) {
  void **const noted = gc_heap.noted;
  /* The entries past the last one the copy did not move are all moved:
   * nothing of theirs is read (noted_hi, splus_evacuate). */
  const uint32_t n = gc_heap.noted_hi < gc_heap.noted_n ? gc_heap.noted_hi : gc_heap.noted_n;
  uint32_t i;
  for (i = 0; i < n; i++) {
    uintptr_t e = (uintptr_t)noted[i];
    AvantHeader *h;
    uint16_t f;
    if (e & NOTED_MOVED) {
      continue;
    }
    h = avant_header((void *)(e & ~NOTED_TAGS));
    f = h->flags;
    if (f & AVANT_FLAG_FORWARDED) {
      continue;
    }
    f = (uint16_t)(f & ~(AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW | AVANT_FLAG_CARD_SCAN));
    if (!(f & AVANT_FLAG_YOUNG)) {
      f = (uint16_t)(f & ~AVANT_FLAG_SURVIVED);
    }
    h->flags = f;
  }
  gc_heap.noted_hi = UINT32_MAX;
  gc_heap.noted_n = 0;
  gc_heap.fix_extra_n = 0;
  gc_heap.carded_n = 0;
}

/* What a traced reference tells its holder (trace_object's and
 * trace_field's result, or-ed over a holder's fields by scan_fields):
 * TR_YLARGE, it names a young large object, whose holder may then have
 * to stay remembered (the keep list); TR_MOVABLE, it names a block
 * object that was young when the collection began, which the copy may
 * move, so the holder's fields are the fix-up's to rewrite. */
#define TR_YLARGE 1
#define TR_MOVABLE 2

static int scan_fields(void *p, int from_mature);
static uint64_t scan_cards(void *p, int from_mature, int *r);
static int trace_object(void **slot, void *start, Block *b, AvantHeader *ol, int from_mature);

/* How many remembered objects trace_object is following inside one
 * another (only the collecting thread traces), and the most it follows
 * that way (its comment). */
#define TRACE_REM_DEPTH 64
static uint32_t trace_rem_depth;

/* The trace's state in locals (the Valk stage, VALK-T, O579), as the full
 * mark's MarkCtx: what a field's trace reads of gc_heap, loaded once by
 * the loop that scans (y_open), so the loop keeps it in registers. The
 * committed size, the side tables and the epoch do not change during a
 * young trace. base8 is gc_heap.base plus a header's size: a payload
 * address less it is its header's offset. The noted list's and the work
 * list's counts are written back when the loop leaves or calls a
 * function that reads them (y_close), and read again after (y_open). */
typedef struct YCtx {
  uintptr_t base8;
  uint64_t committed;
  uint8_t *lines;
  Block *blocks;
  uint8_t epoch;
  void **noted;
  uint32_t noted_n;
  uint32_t noted_cap;
  void **work;
  uint32_t work_n;
  uint32_t work_cap;
} YCtx;

static inline __attribute__((always_inline)) void y_open(YCtx *c) {
  c->base8 = (uintptr_t)gc_heap.base + sizeof(AvantHeader);
  c->committed = gc_heap.committed;
  c->lines = gc_heap.lines;
  c->blocks = gc_heap.blocks;
  c->epoch = gc_heap.line_epoch;
  c->noted = gc_heap.noted;
  c->noted_n = gc_heap.noted_n;
  c->noted_cap = gc_heap.noted_cap;
  c->work = gc_heap.work;
  c->work_n = gc_heap.work_n;
  c->work_cap = gc_heap.work_cap;
}

static inline __attribute__((always_inline)) void y_close(YCtx *c) {
  gc_heap.noted_n = c->noted_n;
  gc_heap.work_n = c->work_n;
}

/* LINE (an entry of the heap's line table) is made live by a promotion: the
 * mature growth for the full-collection trigger is the line, whole
 * (promote_young's comment). Out of the trace's loop: most promoted
 * objects lie in a line an earlier one made live. */
static __attribute__((noinline)) void y_line_live(uint8_t *line) {
  *line = gc_heap.line_epoch;
  gc_heap.promoted_since_full += LINE_SIZE;
  gc_heap.mature_since_full += LINE_SIZE;
  /* The block's count of the lines this collection made live in it
   * (Block.search; splus_evacuate takes an evacuated block's back). */
  gc_heap.blocks[(size_t)(line - gc_heap.lines) / LINES_PER_BLOCK].search++;
}

/* The lines after an object's first, L + 1 to E. */
static __attribute__((noinline)) void y_lines_rest(uint8_t *lines, size_t l, size_t e) {
  while (l++ < e) {
    if (lines[l] != gc_heap.line_epoch) {
      y_line_live(lines + l);
    }
  }
}

/* The lists grown by one entry's room, out of the loop. */
static __attribute__((noinline)) void y_noted_grow(void) {
  gc_heap.noted_cap = gc_heap.noted_cap ? gc_heap.noted_cap * 2 : 256;
  gc_heap.noted = gc_xrealloc(gc_heap.noted, gc_heap.noted_cap * sizeof(void *));
}

static __attribute__((noinline)) void y_work_grow(void) {
  gc_heap.work_cap = gc_heap.work_cap ? gc_heap.work_cap * 2 : 256;
  gc_heap.work = gc_xrealloc(gc_heap.work, gc_heap.work_cap * sizeof(void *));
}

/* Promote the young block object at START (header H, flags FL, its
 * header at offset OFF of the heap) in place: it is mature from now on,
 * MARKED and PROMOTED_NOW until the collection ends, noted (NOTED_SURV),
 * its lines marked live at the current epoch, and pushed for its fields'
 * trace unless it has none (the work list holds its entry's index, so
 * that the trace can tag the entry). Its bytes are the survival
 * young_size reads (sticky.inplace) and the collection's live bytes:
 * summed per block in Block.idle (S+'s survivor bytes) and added once
 * after the trace (gc_sticky_collect), not per object. The mature growth
 * it causes for the full-collection trigger (gc-design.md §5.4: in-place
 * promotions count) is the lines it newly makes live, whole: what the
 * heap loses to it until a full collection, since nothing compacts it (a
 * survivor alone in a line costs the line; counted by its bytes, a heap
 * of scattered survivors grew without a full collection: heap/churn.av
 * 9 MB under E, 53 MB). SURVIVED (an aged object's) ends with the
 * collection (gc_clear_noted): S+'s copy reads it to decide where the
 * copy goes. */
static inline __attribute__((always_inline)) void promote_young(YCtx *c, void *start, AvantHeader *h, uint16_t fl,
                                                                uintptr_t off) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  size_t l = off >> LINE_SHIFT;
  size_t e = (off + total - 1) >> LINE_SHIFT;
  uint32_t idx;
  h->flags = (uint16_t)((fl & ~AVANT_FLAG_YOUNG) | AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW);
  if (__builtin_expect(c->lines[l] != c->epoch, 0)) {
    y_line_live(c->lines + l);
  }
  if (__builtin_expect(l != e, 0)) {
    y_lines_rest(c->lines, l, e);
  }
#if GC_STICKY_COPY
  c->blocks[off >> BLOCK_SHIFT].idle += total;
#else
  gc_heap.ctr.sticky.inplace += total;
  gc_heap.live += total;
#endif
  if (__builtin_expect(c->noted_n == c->noted_cap, 0)) {
    y_noted_grow();
    c->noted = gc_heap.noted;
    c->noted_cap = gc_heap.noted_cap;
  }
  idx = c->noted_n++;
  c->noted[idx] = (void *)((uintptr_t)start | NOTED_SURV);
  if (h->type_id != AVANT_TYPE_BYTES) {
    if (__builtin_expect(c->work_n == c->work_cap, 0)) {
      y_work_grow();
      c->work = gc_heap.work;
      c->work_cap = gc_heap.work_cap;
    }
    c->work[c->work_n++] = (void *)(uintptr_t)idx;
  }
}

#if GC_STICKY_COPY
/* Variant S+ (Stage 28 W15c, gc-design.md §3.1, §3.3): the opportunistic
 * young copy. A young collection traces and promotes in place as S does,
 * and sums each young block's survivor bytes. After the trace a young
 * block that held no mature line when the collection began and whose
 * survivors' bytes fill at most SPLUS_SPARSE_LINES lines is evacuated:
 * its survivors (the noted list's NOTED_SURV entries in it) are copied
 * into fresh blocks (forwarding in the header, as defragmentation does)
 * and its line marks are cleared, so the young sweep frees it whole; every
 * reference to them is rewritten by one more pass over the references the
 * trace followed that can name one (splus_fix: frame slots, runtime-held
 * slots and the fields of the objects whose scan met a movable object,
 * NOTED_FIX; by the generational invariant no other field names a young
 * object); and the block's lines are freed by the young sweep. Other
 * survivors are promoted in
 * place, as under S. A block stays in place when any of its young
 * objects is named imprecisely: a pin or PINNED, or a registered slot
 * location inside it. (Until Stage 28 W16b also a word of the collecting
 * thread's C stack at a collection runtime C entered, W15a2's pin scan;
 * runtime C roots what it holds since W16a, RC1, and gc-design.md
 * section 4.3's proof retired the scan.) Nothing moves with the nursery
 * off (no young collection); in a shared program the copy runs as in an
 * unshared one since Stage 28 W17a (the handshake stops the other
 * threads; before, nothing moved there, D191). Copying is optional: a block refused at the
 * heap limit leaves the remaining survivors in place. */
#endif

/* One reference to the object at START (in block B, or the large object
 * OL), named by SLOT's word (NULL: named by a pin, a task root or a slot's
 * own location). FROM_MATURE: the reference is a field of a mature object
 * that is not remembered (R1': not followed unless remembered). Every
 * object the collection reaches is flagged MARKED ("traced this cycle")
 * and noted, so the flag ends with the collection (gc_clear_noted). A
 * young object is promoted at once and its fields are traced from the
 * work list; a young large object is marked and decided after the trace
 * (gc_reclaim_young_large: at its first survival it stays young, Stage 28
 * W15c). Returns TR_YLARGE or TR_MOVABLE (above), or 0. The general
 * form: roots come here through trace_ref, and a field's reference that
 * is not one of trace_field's in-line cases. */
static int trace_object(void **slot, void *start, Block *b, AvantHeader *ol, int from_mature) {
  AvantHeader *h = avant_header(start);
  uint16_t fl = h->flags;
  uint32_t idx;
  int r;
#if GC_STICKY_COPY
  /* S+: a young block object named other than by a slot (a pin, a
   * registered slot location inside it), or pinned, keeps its block in
   * place: splus_fix rewrites slots only. */
  if (gc_heap.splus_on && !ol && (fl & AVANT_FLAG_YOUNG) && (!slot || (fl & AVANT_FLAG_PINNED))) {
    b->kept = SPLUS_KEPT;
  }
#else
  (void)slot;
#endif
  if (fl & AVANT_FLAG_MARKED) {
    /* A carded object scanned so far only by its marked cards, now named
     * by a root or a young object's field (R1'(a), (b)): scanned whole,
     * as R1' says (Stage 28 W16t). */
    if (!from_mature && (fl & AVANT_FLAG_CARD_SCAN)) {
      h->flags &= (uint16_t)~AVANT_FLAG_CARD_SCAN;
      if (scan_fields(start, 1) & TR_MOVABLE) {
        fix_extra_add(start);
      }
    }
    if (ol) {
      return (fl & AVANT_FLAG_YOUNG) ? TR_YLARGE : 0;
    }
    return (fl & AVANT_FLAG_PROMOTED_NOW) ? TR_MOVABLE : 0;
  }
  if (fl & AVANT_FLAG_YOUNG) {
    if (ol) {
      h->flags = (uint16_t)(fl | AVANT_FLAG_MARKED);
      idx = note_marked(start, 0);
      note_carded(start, h);
      gc_heap.young_large_live += gc_large_bytes(ol);
      if (h->type_id != AVANT_TYPE_BYTES) {
        gc_work_push((void *)(uintptr_t)idx);
      }
      return TR_YLARGE;
    }
    {
      YCtx c;
      y_open(&c);
      promote_young(&c, start, h, fl, (uintptr_t)start - c.base8);
      y_close(&c);
    }
    return TR_MOVABLE;
  }
  if (from_mature && (fl & (AVANT_FLAG_REM | AVANT_FLAG_REMC)) == 0) {
    return 0;
  }
  /* A remembered object a mature object's field names is followed here,
   * and its fields name the next remembered object: on a chain of them
   * (a list of old nodes, each written since the last collection) this
   * function called itself once per node, and a chain of 100,000 ended
   * the program with a stack overflow (found in the Valk stage's VALK-T;
   * tests/fixtures/heap/rem_chain.av). Past TRACE_REM_DEPTH nested
   * objects the object is left to the remembered set's own scan
   * (trace_all, R1'(c)), which reaches every remembered object not yet
   * traced: such an object is met here only before or during that scan,
   * never after it. Below the depth the order of the trace is the one it
   * always had. */
  if (from_mature && trace_rem_depth >= TRACE_REM_DEPTH) {
    return 0;
  }
  h->flags = (uint16_t)(fl | AVANT_FLAG_MARKED);
  idx = note_marked(start, 0);
  if (ol) {
    note_carded(start, h);
    gc_heap.young_large_live += gc_large_bytes(ol);
  }
  if (from_mature) {
    trace_rem_depth++;
  }
  /* Followed only because it is remembered: a carded object's marked
   * cards are what the barrier recorded (Stage 28 W16t). */
  if (from_mature && (fl & AVANT_FLAG_REMC) && !(fl & AVANT_FLAG_CARD_ALL)) {
    h->flags |= AVANT_FLAG_CARD_SCAN;
    r = 0;
    (void)scan_cards(start, 1, &r);
  } else {
    r = scan_fields(start, 1);
  }
  if (from_mature) {
    trace_rem_depth--;
  }
  if (r & TR_MOVABLE) {
    noted_tag(idx, NOTED_FIX);
  }
  return 0;
}

/* A root's reference P (a frame slot's, a pin's, a task root's, a
 * runtime-held slot's, a frame slot's own location): it may name the
 * inside of an object, so the start bits resolve it. */
static int trace_ref(void **slot, void *p, int from_mature) {
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload_fast(p, &b, &ol);
  return start ? trace_object(slot, start, b, ol, from_mature) : 0;
}

/* A field's reference (SLOT is a word a type map names, or a carded
 * buffer's slot): null, an exact start or a static address by the field
 * rule (avant_gc_internal.h), so the range test alone classifies it (the
 * Valk stage, VALK-T, O585; until then the start-bit lookup of
 * gc_object_payload_fast), and the header right before it is read once:
 * the one lookup of a traced field (O579). The common answers are given
 * here, in line in the scan's loop over YCtx's locals: no heap object; a
 * block object already traced this cycle; a young block object, promoted
 * (trace_object's young arm: the block stays in place when the object is
 * PINNED); a mature block object a mature holder names that is not
 * remembered (R1': not followed). A large object and a mature block
 * object that is to be marked and scanned take trace_object, with the
 * loop's state written back around the call. */
static inline __attribute__((always_inline)) int trace_field(YCtx *c, void **slot, int from_mature) {
  void *p = *slot;
  uintptr_t off = (uintptr_t)p - c->base8;
  AvantHeader *h;
  uint16_t fl;
  int r;
  if (off >= c->committed) {
    return 0;
  }
  h = (AvantHeader *)((uint8_t *)p - sizeof(AvantHeader));
  fl = h->flags;
  if (__builtin_expect((fl & (AVANT_FLAG_LARGE | AVANT_FLAG_MARKED | AVANT_FLAG_YOUNG)) == AVANT_FLAG_YOUNG, 1)) {
#if GC_STICKY_COPY
    if (__builtin_expect((fl & AVANT_FLAG_PINNED) != 0, 0) && gc_heap.splus_on) {
      c->blocks[off >> BLOCK_SHIFT].kept = SPLUS_KEPT;
    }
#endif
    promote_young(c, p, h, fl, off);
    return TR_MOVABLE;
  }
  if (!(fl & AVANT_FLAG_LARGE)) {
    if (fl & AVANT_FLAG_MARKED) {
      return (fl & AVANT_FLAG_PROMOTED_NOW) ? TR_MOVABLE : 0;
    }
    if (from_mature && (fl & (AVANT_FLAG_REM | AVANT_FLAG_REMC)) == 0) {
      return 0;
    }
  }
  y_close(c);
  r = trace_object(slot, p, (fl & AVANT_FLAG_LARGE) ? NULL : &c->blocks[off >> BLOCK_SHIFT],
                   (fl & AVANT_FLAG_LARGE) ? h : NULL, from_mature);
  y_open(c);
  return r;
}

/* The slots of carded P's marked cards (Stage 28 W16t), traced as
 * scan_fields traces fields; returns their count (rem_fields counts the
 * remembered set's scans, as field_count does for whole objects) and
 * or-s what they named into *R. A young large object they name needs no
 * keep entry: gc_cards_settle keeps the card. */
static uint64_t scan_cards(void *p, int from_mature, int *r) {
  uint64_t n = 0;
  int rr = 0;
  YCtx c;
  y_open(&c);
  EACH_CARD_SLOT(p, slot, {
    rr |= trace_field(&c, slot, from_mature);
    n++;
  });
  y_close(&c);
  *r |= rr;
  return n;
}

/* A holder with a field naming a young large object joins the keep list
 * (gc_keep_add) unless it is itself a young large object (young, so
 * traced whenever it is reached): after the trace, every such holder
 * that is not young and still names a young object is remembered
 * (rem_rebuild), because the young large object may stay young across
 * this collection (Stage 28 W15c, O88). R is what the fields of P named. */
static inline void keep_holder(void *p, int r) {
  if ((r & TR_YLARGE) &&
      (avant_header(p)->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) != (AVANT_FLAG_YOUNG | AVANT_FLAG_LARGE)) {
    gc_keep_add(p);
  }
}

/* The fields of P; returns what they named (TR_*). The general form, a
 * function with its own copy of the trace's state: mature objects R1'
 * scans, remembered objects, and the work list's objects that are not
 * small records (trace_drain scans a small record's fields itself). */
static __attribute__((noinline)) int scan_fields(void *p, int from_mature) {
  int r = 0;
  YCtx c;
  if (avant_header(p)->type_id == AVANT_TYPE_BYTES) {
    return 0;
  }
  y_open(&c);
  EACH_REF_SLOT(p, slot, { r |= trace_field(&c, slot, from_mature); });
  y_close(&c);
  keep_holder(p, r);
  return r;
}

/* The work list: each entry is the noted index of a young object the
 * trace promoted (or a young large object it marked); its fields are
 * traced (R1'(b)), and when one names a movable object its noted entry
 * is tagged NOTED_FIX. A small record's reference words are read from
 * its type's list (gc_heap.refs) and traced in line over the loop's
 * locals; any other object goes through scan_fields. */
static void trace_drain(void) {
  YCtx c;
  y_open(&c);
  while (c.work_n) {
    uint32_t idx = (uint32_t)(uintptr_t)c.work[--c.work_n];
    void **p = (void **)((uintptr_t)c.noted[idx] & ~NOTED_TAGS);
    const AvantHeader *ph = avant_header(p);
    uint64_t d = gc_heap.refs[ph->type_id];
    unsigned k = (unsigned)(d & 0xffu);
    int r = 0;
    if (__builtin_expect(k > TYPE_REFS_MAX, 0)) {
      y_close(&c);
      r = scan_fields(p, 0);
      y_open(&c);
    } else if (k) {
      size_t nw = (size_t)ph->size / sizeof(void *);
      for (; k; k--) {
        d >>= 8;
        if ((size_t)(d & 0xffu) < nw) {
          r |= trace_field(&c, p + (size_t)(d & 0xffu), 0);
        }
      }
      if (__builtin_expect((r & TR_YLARGE) != 0, 0)) {
        keep_holder(p, r);
      }
    }
    if (r & TR_MOVABLE) {
      c.noted[idx] = (void *)((uintptr_t)c.noted[idx] | NOTED_FIX);
    }
  }
  y_close(&c);
}

static void trace_root_slot(void **slot) {
  (void)trace_ref(slot, *slot, 0);
}

/* Field slots scan_fields visits for one object (the rem_fields
 * counter, Q5). */
static uint64_t field_count(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_BYTES) {
    return 0;
  }
  if (id == AVANT_TYPE_PTRS) {
    return (uint64_t)((size_t)h->size / sizeof(void *));
  }
  if (id >= MAX_TYPES || !gc_heap.type_set[id]) {
    return 0;
  }
  if (gc_heap.types[id].stride) {
    return (uint64_t)gc_heap.types[id].nrefs *
           (uint64_t)((size_t)h->size / sizeof(void *) / gc_heap.types[id].stride);
  }
  return (uint64_t)gc_heap.types[id].nrefs;
}

/* The trace: frame slots (the value, and the location when it lies in a
 * young object, which keeps that object), the remembered set (each entry
 * marked and its fields traced as R1'(c) says), pins, the runtime-held
 * slots, then the work list (the young objects' fields, R1'(b)): each
 * entry is a noted index, and the object whose scan met a movable object
 * has its noted entry tagged NOTED_FIX. */
static void trace_all(void) {
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&gc_heap.threads_mu);
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, {
      void *loc = (void *)slot;
      Block *lb = NULL;
      AvantHeader *ll = NULL;
      void *ls = gc_object_payload(loc, &lb, &ll);
      if (ls && (avant_header(ls)->flags & AVANT_FLAG_YOUNG)) {
        trace_ref(NULL, ls, 0);
      }
#if GC_STICKY_COPY
      /* A slot location inside an object an earlier slot already
       * reached (promoted, so no longer YOUNG): its block must stay in
       * place all the same, since splus_fix rewrites what a slot holds,
       * never where the frame keeps it (Stage 28 W15d: before, such an
       * object moved and the frame's location named its old copy). A
       * candidate block holds only objects young when the collection
       * began. */
      else if (ls && gc_heap.splus_on && lb && !ll && lb->young_hi && lb->kept != SPLUS_KEPT) {
        lb->kept = SPLUS_KEPT;
      }
#endif
      trace_ref(slot, *slot, 0);
    });
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  {
    gc_heap.ctr.rem.scans++;
    gc_heap.ctr.rem.entries += gc_heap.rem_n;
    if (gc_heap.rem_n > gc_heap.ctr.rem.peak) {
      gc_heap.ctr.rem.peak = gc_heap.rem_n;
    }
    for (i = 0; i < gc_heap.rem_n; i++) {
      void *obj = gc_heap.remset[i];
      AvantHeader *ol = NULL;
      if (!gc_is_heap_payload(obj, NULL, &ol)) {
        continue;
      }
      {
        AvantHeader *oh = avant_header(obj);
        int carded = (oh->flags & AVANT_FLAG_REMC) && !(oh->flags & AVANT_FLAG_CARD_ALL);
        int fresh = !(oh->flags & AVANT_FLAG_MARKED);
        uint32_t idx = 0;
        int r = 0;
        if (fresh) {
          oh->flags |= AVANT_FLAG_MARKED;
          idx = note_marked(obj, 0);
          if (ol) {
            note_carded(obj, oh);
            gc_heap.young_large_live += gc_large_bytes(ol);
          }
          if (carded) {
            oh->flags |= AVANT_FLAG_CARD_SCAN;
          }
        }
        /* A carded entry: the slots of its marked cards (R1'(c) for a
         * carded object, Stage 28 W16t); every other entry: every field. */
        if (carded) {
          gc_heap.ctr.rem.fields += scan_cards(obj, 0, &r);
        } else {
          gc_heap.ctr.rem.fields += field_count(obj);
          r = scan_fields(obj, 0);
        }
        if (r & TR_MOVABLE) {
          if (fresh) {
            noted_tag(idx, NOTED_FIX);
          } else {
            fix_extra_add(obj);
          }
        }
      }
    }
  }
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    trace_ref(NULL, p->payload, 0);
  }
  EACH_TASK_ROOT(slot, trace_ref(NULL, *slot, 0));
  gc_each_runtime_root(trace_root_slot);
  trace_drain();
}

/* Every remembered entry leaves the set, REM cleared (gc-design.md
 * §3.1.1 point 1): after an S young collection no block object is young.
 * Run before any line is freed, while every entry's header is intact (no
 * allocation happens during the collection, and the set holds only
 * objects remembered since the last collection). rem_rebuild then
 * remembers again the holders that name a young large object staying
 * young (§3.1.1 point 1 as amended by Stage 28 W15c: the one case where
 * an object is still young after an S young collection). */
static void rem_empty(void) {
  uint32_t i;
  for (i = 0; i < gc_heap.rem_n; i++) {
    void *obj = gc_heap.remset[i];
    if (gc_is_heap_payload(obj, NULL, NULL)) {
      avant_header(obj)->flags &= (uint16_t)~(AVANT_FLAG_REM | AVANT_FLAG_REMC);
    }
  }
  gc_heap.ctr.rem.cleared += gc_heap.rem_n;
  gc_heap.rem_n = 0;
}

/* The holders the trace found naming a young large object (the keep
 * list, scan_fields): each one that is not young and still names a young
 * object after gc_reclaim_young_large is remembered, so the next young
 * collection traces the young large object through it. A holder this
 * collection reached only through a root (R1'(a)) is remembered too: the
 * root may be gone by the next collection. */
static void rem_rebuild(void) {
  gc_rem_keep_young_holders();
  gc_cards_settle();
}

/* The young lines LO..HI of block B after the trace: a line no survivor
 * marked at the current epoch is free, and its start bits are cleared
 * (32 granules, half a start word; gc-design.md §2.3, §3.1.1 point 2), so
 * its dead objects are no longer heap payloads and a later hole there
 * has clear start bits for the inline allocation path. A dead object
 * whose header lies in a live line keeps its start bit: nothing infers
 * liveness from start bits. */
/* The line sweep itself is gc_sweep_block_lines (avant_gc_heap.c, word
 * by word since Stage 28 W15c, O87; shared with the full sweep since
 * W15d, O94); here it is counted in the sticky family. */
static int sweep_lines(Block *b, int lo, int hi) {
  uint64_t freed = 0;
  int live = gc_sweep_block_lines(b, lo, hi, &freed);
  gc_heap.ctr.sticky.freed_lines += freed;
  gc_heap.ctr.sticky.blocks++;
  return live;
}

/* The block map (gc_heap.evac_map) grown to the committed heap, its new
 * entries clear. The young copy's map (S+), and the defragmenter's for
 * the blocks a full collection evacuates (avant_gc_defrag.c; the Valk
 * stage, VALK-T2, O603): each sets its blocks' entries when it has chosen
 * them and clears them before it ends, so the map is clear between
 * collections. */
void gc_evac_map_ensure(void) {
  uint64_t need = gc_heap.committed >> BLOCK_SHIFT;
  if (need > gc_heap.evac_map_n) {
    gc_heap.evac_map = gc_xrealloc(gc_heap.evac_map, (size_t)need);
    memset(gc_heap.evac_map + gc_heap.evac_map_n, 0, (size_t)(need - gc_heap.evac_map_n));
    gc_heap.evac_map_n = need;
  }
}

/* A young collection under S (gc_young_collect's S arm): the trace, the
 * young large objects, the remembered set emptied, then the sweep of the
 * young blocks. */
#if GC_STICKY_COPY
/* The new address of P when it lies in an object S+ moved (its exact
 * start or an interior address, resolved through the old copy's start
 * bit and forward), else P. Only addresses in an evacuated block (P's
 * block or, for a payload that starts a block, its header's) can. *AGED
 * is set when the copy is an aged one (still young, YOUNG_AGE 2). */
/* Block BI had objects moved out (the map covers the heap as committed
 * when the collection chose its blocks; blocks past it are new, never
 * evacuated). 1: the block is being evacuated (SPLUS_EVAC); 2: its
 * evacuation stopped, or holder aging took an object out of it
 * (SPLUS_PART). */
#define EVAC_MAP_EVAC 1
#define EVAC_MAP_PART 2
static inline int evac_at(uint64_t bi) {
  return bi < gc_heap.evac_map_n && gc_heap.evac_map[bi];
}

static inline void evac_map_set(const Block *b, uint8_t v) {
  uint64_t bi = (uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
  if (bi < gc_heap.evac_map_n) {
    gc_heap.evac_map[bi] = v;
  }
  if (v) {
    gc_heap.evac_any = 1;
  }
}

/* A root's word (a frame slot's, a runtime-held slot's, a task root's, a
 * keep entry): it may name the inside of an object. */
static void *splus_new_addr(void *p, int *aged) {
  uint8_t *a = (uint8_t *)p;
  uintptr_t off = (uintptr_t)a - (uintptr_t)gc_heap.base;
  uint64_t bi;
  Block *ob = NULL;
  AvantHeader *ol = NULL;
  void *start;
  void *to;
  /* The evacuated-block map (O93): P's block, or, for a payload that
   * starts a block, its header's (the block before). The first address
   * past the committed heap is inside the range on purpose (O601, the
   * Valk stage, VALK-T): it is the payload address of an object with no
   * payload whose header is the heap's last word, and then the block
   * before it is the one asked (until VALK-T `off >= committed` left such
   * a reference naming the old copy; avant_alloc gives every object one
   * word since W-INT1, so no object has that address today:
   * ffi/empty_end.av reaches the layout with that guard removed). */
  if (off > gc_heap.committed) {
    return p;
  }
  bi = (uint64_t)off >> BLOCK_SHIFT;
  if (!evac_at(bi) && !((off & (BLOCK_SIZE - 1)) == 0 && bi > 0 && evac_at(bi - 1))) {
    return p;
  }
  /* An exact start (nearly every reference): its header's start bit,
   * without the interior lookup. */
  if ((off & (ALIGN - 1)) == 0 && off >= sizeof(AvantHeader) && start_bit_at(a - sizeof(AvantHeader))) {
    const AvantHeader *h = (const AvantHeader *)(a - sizeof(AvantHeader));
    if (!(h->flags & AVANT_FLAG_FORWARDED)) {
      return p;
    }
    to = gc_forward_of(h);
    if (avant_header(to)->flags & AVANT_FLAG_YOUNG) {
      *aged = 1;
    }
    return to;
  }
  start = gc_object_payload(p, &ob, &ol);
  if (!start) {
    return p;
  }
  to = gc_forwarded(start);
  if (!to) {
    return p;
  }
  if (avant_header(to)->flags & AVANT_FLAG_YOUNG) {
    *aged = 1;
  }
  return (void *)((uint8_t *)to + (a - (uint8_t *)start));
}

/* The copy's destination when the current one is missing or cannot hold
 * the object: a fresh block (AGED: a survivor block, which joins
 * surv_blocks; else the copy block, the full one's free rest going to
 * part_blocks), or NULL when none can be had (the heap limit). Out of
 * the copy's loop: once per block filled. */
static __attribute__((noinline)) Block *splus_copy_room(int aged) {
  Block *d = aged ? gc_heap.splus_sdst : gc_heap.splus_dst;
  if (aged) {
    gc_heap.splus_sdst = NULL;
    d = gc_new_block(SPACE_MATURE);
    if (!d) {
      return NULL;
    }
    d->bump = d->base;
    d->limit = d->base + BLOCK_SIZE;
    d->kept = SPLUS_SURV;
    gc_sticky_push(&gc_heap.surv_blocks, &gc_heap.surv_blocks_n, &gc_heap.surv_blocks_cap, d);
    gc_heap.splus_sdst = d;
    gc_heap.ctr.sticky.surv_blocks++;
  } else {
    if (d && d->bump < d->base + BLOCK_SIZE) {
      gc_reset_block_bump(d);
      gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, d);
    }
    gc_heap.splus_dst = NULL;
    d = gc_new_block(SPACE_MATURE);
    if (!d) {
      return NULL;
    }
    d->bump = d->base;
    d->limit = d->base + BLOCK_SIZE;
    gc_heap.splus_dst = d;
    gc_heap.ctr.sticky.copy_blocks++;
  }
  return d;
}

/* Copy the young block object H into the copy block, promoted: its
 * header keeps its flags without YOUNG, SURVIVED and MARKED, its start bit
 * is set and its lines are marked live; the old header holds the forward.
 * AGED (YOUNG_AGE 2, a first survivor): into the survivor block instead,
 * still young (YOUNG | SURVIVED), its start bit set and its lines left
 * unmarked like any young object's; it is not mature growth. Returns the
 * copy's payload address, or NULL when no block can be had (the heap
 * limit): H stays in place. In line in the copy's loop (the Valk stage,
 * VALK-T, O579): the three smallest sizes (a header and one to three
 * words) are copied by fixed-size moves, with no call. The bytes moved
 * are added to *N_AGED or *N_PROM, which the caller gives to
 * splus_copied when its run of copies ends. */
static inline __attribute__((always_inline)) void *splus_copy(AvantHeader *h, int aged, uint64_t *n_aged,
                                                             uint64_t *n_prom) {
  size_t total = align_up(sizeof(AvantHeader) + (size_t)h->size);
  Block *d = aged ? gc_heap.splus_sdst : gc_heap.splus_dst;
  uint8_t *to;
  AvantHeader *nh;
  if (__builtin_expect(!d || d->bump + total > d->base + BLOCK_SIZE, 0)) {
    d = splus_copy_room(aged);
    if (!d) {
      return NULL;
    }
  }
  to = d->bump;
  d->bump = to + total;
  if (total == 24) {
    memcpy(to, h, 24);
  } else if (total == 16) {
    memcpy(to, h, 16);
  } else if (total == 32) {
    memcpy(to, h, 32);
  } else {
    memcpy(to, h, total);
  }
  nh = (AvantHeader *)to;
  set_start(d, (size_t)(to - d->base));
  if (aged) {
    nh->flags = (uint16_t)((nh->flags & ~(AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW)) | AVANT_FLAG_YOUNG |
                           AVANT_FLAG_SURVIVED);
    *n_aged += total;
  } else {
    nh->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED | AVANT_FLAG_MARKED | AVANT_FLAG_PROMOTED_NOW);
    mark_lines(d, (size_t)(to - d->base), total);
    *n_prom += total;
  }
  gc_set_forward(h, payload_of(nh));
  return payload_of(nh);
}

/* The bytes a run of copies moved, added to the counters once (the Valk
 * stage, VALK-T, O600; splus_copy added them at every object): N_AGED
 * into survivor blocks, still young (sticky.aged, the collection's
 * aged_now), N_PROM into mature space (prom.copied, and mature growth
 * for the full-collection trigger); both are copied bytes and live
 * bytes. */
static void splus_copied(uint64_t n_aged, uint64_t n_prom) {
  gc_heap.ctr.sticky.aged += n_aged;
  gc_heap.aged_now += n_aged;
  gc_heap.ctr.prom.copied += n_prom;
  gc_heap.promoted_since_full += n_prom;
  gc_heap.mature_since_full += n_prom;
  avant_g_stats.copied_bytes += n_aged + n_prom;
  gc_heap.live += n_aged + n_prom;
}

static void splus_fix_slot(void **slot) {
  int aged = 0;
  *slot = splus_new_addr(*slot, &aged);
}

/* A field's word after the copy (the Valk stage, VALK-T, O579 and O585):
 * by the field rule it is null, a static address or an exact start, so
 * its header's block is the one the map is asked about and, in a block
 * objects were moved out of, the header right before it says whether
 * this object moved (the header of an object the trace reached: every
 * field read here was traced). Written only when it changes. *AGED is
 * set when the copy is an aged one (still young, YOUNG_AGE 2). FixCtx is
 * what it reads of gc_heap, in the caller's locals (the map does not
 * change during the fix-up). */
typedef struct FixCtx {
  uintptr_t base8;
  const uint8_t *map;
  uint64_t map_n;
} FixCtx;

static inline __attribute__((always_inline)) void fix_open(FixCtx *c) {
  c->base8 = (uintptr_t)gc_heap.base + sizeof(AvantHeader);
  c->map = gc_heap.evac_map;
  c->map_n = gc_heap.evac_map_n;
}

static inline __attribute__((always_inline)) void fix_field(const FixCtx *c, void **slot, int *aged) {
  void *v = *slot;
  uint64_t bi = (uint64_t)((uintptr_t)v - c->base8) >> BLOCK_SHIFT;
  if (bi < c->map_n && c->map[bi]) {
    const AvantHeader *h = (const AvantHeader *)((uint8_t *)v - sizeof(AvantHeader));
    if (h->flags & AVANT_FLAG_FORWARDED) {
      void *to = gc_forward_of(h);
      *slot = to;
      if (avant_header(to)->flags & AVANT_FLAG_YOUNG) {
        *aged = 1;
      }
    }
  }
}

/* The fix-up of one holder P (its copy, when it moved): the slots of its
 * marked cards when the trace scanned it by them, else every field. One
 * that is not young and now names an aged copy (YOUNG_AGE 2) joins the
 * keep list, so the remembered set holds it after the collection
 * (gc_rem_keep_young_holders): by the generational invariant these are
 * the only holders of the aged copies. */
static void fix_holder(void *p) {
  AvantHeader *h = avant_header(p);
  int aged = 0;
  FixCtx c;
  if (h->type_id == AVANT_TYPE_BYTES) {
    return;
  }
  fix_open(&c);
  /* A carded object the trace scanned by its marked cards only: by the
   * card invariant no other slot names a young (movable) object. */
  if (h->flags & AVANT_FLAG_CARD_SCAN) {
    EACH_CARD_SLOT(p, slot, { fix_field(&c, slot, &aged); });
    return;
  }
  EACH_REF_SLOT(p, slot, { fix_field(&c, slot, &aged); });
  if (aged && !(h->flags & AVANT_FLAG_YOUNG)) {
    gc_keep_add(p);
  }
}

/* Every reference that can name a moved object, rewritten to the copy:
 * the frame slots of every registered thread, the runtime-held slots and
 * the task roots (each may name the inside of an object: splus_new_addr),
 * the fields of every object whose scan met a movable object (the noted
 * list's NOTED_FIX entries, in the list's order; a moved object's fields
 * are its copy's), and the keep list's holders. Until VALK-T it rewrote
 * every field of every object the trace scanned whenever anything at all
 * was copied (O579): an object none of whose fields named a block object
 * young at the collection's start names nothing that moved, and is not
 * read again. */
static void splus_fix(void) {
  ThreadReg *t;
  uint32_t i;
  void **noted;
  uint32_t noted_n;
  FixCtx c;
  fix_open(&c);
  for (t = gc_heap.threads; t; t = t->next) {
    EACH_FRAME_SLOT(gc_thread_frames(t), slot, { splus_fix_slot(slot); });
  }
  gc_each_runtime_root(splus_fix_slot);
  /* Task roots move with their records since Stage 28 W17a. */
  EACH_TASK_ROOT(slot, splus_fix_slot(slot));
  noted = gc_heap.noted;
  noted_n = gc_heap.noted_n;
  for (i = 0; i < noted_n; i++) {
    uintptr_t e = (uintptr_t)noted[i];
    void **p;
    const AvantHeader *ph;
    uint64_t d;
    unsigned k;
    if (!(e & NOTED_FIX)) {
      continue;
    }
    p = (void **)(e & ~NOTED_TAGS);
    if (!(e & NOTED_MOVED)) {
      /* Holder aging moved it (splus_age_holders knows no entry). */
      void *to = gc_forwarded(p);
      if (to) {
        p = (void **)to;
      }
    }
    /* A small record's fields here, by its type's list (a carded object
     * is a pointer buffer, never one); any other holder in fix_holder. */
    ph = avant_header(p);
    d = gc_heap.refs[ph->type_id];
    k = (unsigned)(d & 0xffu);
    if (__builtin_expect(k > TYPE_REFS_MAX, 0)) {
      fix_holder(p);
    } else {
      size_t nw = (size_t)ph->size / sizeof(void *);
      int aged = 0;
      for (; k; k--) {
        d >>= 8;
        if ((size_t)(d & 0xffu) < nw) {
          fix_field(&c, p + (size_t)(d & 0xffu), &aged);
        }
      }
      if (aged && !(ph->flags & AVANT_FLAG_YOUNG)) {
        gc_keep_add(p);
      }
    }
  }
  for (i = 0; i < gc_heap.fix_extra_n; i++) {
    void *p = gc_heap.fix_extra[i];
    void *to = gc_forwarded(p);
    fix_holder(to ? to : p);
  }
  for (i = 0; i < gc_heap.keep_n; i++) {
    int aged = 0;
    gc_heap.keep[i] = splus_new_addr(gc_heap.keep[i], &aged);
  }
}

/* After the trace: choose the evacuated blocks (a young block with no
 * mature line at the collection's start, SPLUS_PURE, not kept in place,
 * whose survivors fill at most SPLUS_SPARSE_LINES lines; each enters the
 * evacuated-block map), then copy their
 * survivors in the order the trace reached them (the noted list), so
 * objects the program reaches together end up together (the locality a
 * copying collector gives; copied in address order instead, Etc::GameOfLife
 * kept S's time), then clear each evacuated block's line marks: it held no
 * mature line, so its lines were all made live by this collection's
 * promotions, whose charge to the full trigger and bytes in place are
 * taken back (the copies charge their own bytes). A block one of whose
 * copies the heap limit refused (SPLUS_PART) keeps its lines live and its
 * remaining survivors in place. Then every reference to a moved object is
 * rewritten (splus_fix). Before the verifier and the sweep. */
/* Holder aging (Stage 28 W15d, with YOUNG_AGE 2 and S_LARGE_AGE 2): a
 * small first survivor that names a young large object (keep_holder put
 * it on the keep list) and lies in a young block S+ does not evacuate
 * (one with mature lines: young allocation took a hole of a recycled
 * block) would be promoted in place and, remembered, keep its buffer
 * alive into the buffer's second survival (the W15c finding, O88): it is
 * copied into a survivor block like an evacuated block's survivor, so it
 * dies young with its buffer. Its old copy stays in its line, which this
 * collection's promotion made live, until a full collection; the block
 * becomes SPLUS_PART (its lines stay, its forwarded start bits go,
 * splus_finish). Only a block not kept in place (SPLUS_KEPT: a young
 * object in it is named imprecisely) gives one up. */
static int splus_age_holders(void) {
  uint32_t i;
  int moved = 0;
  uint64_t n_aged = 0;
  uint64_t n_prom = 0;
  for (i = 0; i < gc_heap.keep_n; i++) {
    void *p = gc_heap.keep[i];
    AvantHeader *h = avant_header(p);
    Block *b = block_of(h);
    size_t total;
    if (!b || !b->young_hi || (b->kept != 0 && b->kept != SPLUS_PART) ||
        (h->flags & (AVANT_FLAG_FORWARDED | AVANT_FLAG_SURVIVED | AVANT_FLAG_LARGE | AVANT_FLAG_PINNED)) ||
        !(h->flags & AVANT_FLAG_PROMOTED_NOW)) {
      continue;
    }
    total = gc_obj_total(h);
    if (!splus_copy(h, 1, &n_aged, &n_prom)) {
      break;
    }
    b->kept = SPLUS_PART;
    evac_map_set(b, EVAC_MAP_PART);
    gc_heap.ctr.sticky.inplace -= total;
    gc_heap.live -= total;
    gc_heap.ctr.sticky.aged_holders++;
    moved = 1;
  }
  splus_copied(n_aged, n_prom);
  return moved;
}

static void splus_evacuate(void) {
  uint32_t i;
  int any = 0;
  /* O92 (Stage 28 W15d item 2): the blocks evacuated hold at most one
   * young budget of survivors (avant_young_bytes); the candidates past it
   * keep their survivors in place, promoted (S's rule). A collection whose
   * survivors outgrow the budget ran late, after a burst allocated where
   * no collection could run (a leaf, a no-GC region, a large live set
   * built at once: Calculator::Interpreter's 16.7 MB tree at a budget of
   * 8 MiB), and copying all of it doubled the peak (S 26.4 MB, S+ 42.8);
   * a collection on time copies everything as before (Etc::GameOfLife,
   * the compiler: unchanged). The copy's room is then bounded by what the
   * budget already allows, as E's copy reserve bounded its copy. */
  uint64_t sel = 0;
  /* The bytes promoted in place (the survival young_size reads, and the
   * live bytes): each young block's survivor bytes, which the trace
   * summed in Block.idle, added once here instead of at every object
   * (the Valk stage, VALK-T, O579); an evacuated block's are taken back
   * below. */
  uint64_t surv = 0;
  /* The map covers the committed heap (the test in line: the call is
   * for a heap that grew). */
  if ((gc_heap.committed >> BLOCK_SHIFT) > gc_heap.evac_map_n) {
    gc_evac_map_ensure();
  }
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    surv += b->idle;
    if (b->kept == SPLUS_KEPT) {
      gc_heap.ctr.sticky.kept_blocks++;
    } else if (gc_heap.set.stress) {
      /* The full stress mode (AVANT_GC_STRESS=1, Stage 28 W16a): every
       * young block nothing names imprecisely is evacuated, pure or
       * mixed, dense or sparse, past the O92 cap, so every young object
       * S+ can move is moved at every young collection; a function that
       * kept a reference outside a handle (RC1, RC2) then holds an old
       * copy, which the sweep fills with poison. */
      if (b->idle > 0) {
        b->kept = SPLUS_EVAC;
        evac_map_set(b, EVAC_MAP_EVAC);
        gc_heap.ctr.sticky.evac_blocks++;
        any = 1;
      } else {
        b->kept = 0;
      }
    } else if (b->kept == SPLUS_PURE_AGED &&
               (b->idle + LINE_SIZE - 1) / LINE_SIZE > SPLUS_AGED_SPARSE_LINES) {
      /* O97 (Stage 28 W15e): a survivor block whose aged survivors are
       * dense keeps them in place, promoted (no second copy). */
      b->kept = 0;
      gc_heap.ctr.sticky.aged_inplace_blocks++;
    } else if ((b->kept == SPLUS_PURE || b->kept == SPLUS_PURE_AGED) && b->idle > 0 &&
               (b->idle + LINE_SIZE - 1) / LINE_SIZE <= SPLUS_SPARSE_LINES &&
               sel + b->idle <= avant_young_bytes) {
      sel += b->idle;
      b->kept = SPLUS_EVAC;
      evac_map_set(b, EVAC_MAP_EVAC);
      gc_heap.ctr.sticky.evac_blocks++;
      any = 1;
    } else if (b->kept == SPLUS_PURE_AGED) {
      b->kept = SPLUS_PURE;
    }
  }
  gc_heap.ctr.sticky.inplace += surv;
  gc_heap.live += surv;
  if (!any) {
    if (gc_heap.splus_age && S_LARGE_AGE >= 2 && gc_heap.keep_n && splus_age_holders()) {
      splus_fix();
    }
    return;
  }
  {
    /* The loop's constants in locals (nothing in it grows the noted list
     * or the map). */
    void **const noted = gc_heap.noted;
    const uint32_t noted_n = gc_heap.noted_n;
    uint8_t *const map = gc_heap.evac_map;
    const uintptr_t base8 = (uintptr_t)gc_heap.base + sizeof(AvantHeader);
    const int age = gc_heap.splus_age;
    uint64_t n_aged = 0;
    uint64_t n_prom = 0;
    /* One past the last entry this pass does not move: the clearing
     * stops there (gc_clear_noted). */
    uint32_t hi = 0;
    for (i = 0; i < noted_n; i++) {
      uintptr_t e = (uintptr_t)noted[i];
      void *p;
      void *to;
      uint64_t bi;
      /* Only what this collection promoted, young when it began, may move
       * (NOTED_SURV): a mixed block (the stress mode) also holds mature
       * objects the trace marked (remembered ones, ones R1' scans), and
       * their entries are passed over here without a look at them. */
      if (!(e & NOTED_SURV)) {
        hi = i + 1;
        continue;
      }
      p = (void *)(e & ~NOTED_TAGS);
      bi = (uint64_t)((uintptr_t)p - base8) >> BLOCK_SHIFT;
      /* Its block is being evacuated (not one whose evacuation stopped). */
      if (map[bi] != EVAC_MAP_EVAC) {
        hi = i + 1;
        continue;
      }
      /* A first survivor ages (YOUNG_AGE 2): SURVIVED marks one that
       * survived a collection before (an aged copy). */
      to = splus_copy(avant_header(p), age && !(avant_header(p)->flags & AVANT_FLAG_SURVIVED), &n_aged, &n_prom);
      if (!to) {
        gc_heap.blocks[bi].kept = SPLUS_PART;
        map[bi] = EVAC_MAP_PART;
        hi = i + 1;
      } else {
        /* The entry is the copy from now on: the fix-up reads the copy's
         * fields, and nothing clears the old header's flags. */
        noted[i] = (void *)((uintptr_t)to | (e & NOTED_TAGS) | NOTED_MOVED);
      }
    }
    gc_heap.noted_hi = hi;
    splus_copied(n_aged, n_prom);
  }
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    if (b->kept == SPLUS_EVAC && gc_heap.set.stress && gc_heap.stress_mixed[i]) {
      /* A mixed block (stress): its mature lines stay live, the lines
       * this collection's promotions marked are free again. */
      uint64_t lines = (uint64_t)gc_block_live_lines(b);
      memcpy(b->line_mark, gc_heap.stress_lines + (size_t)i * LINES_PER_BLOCK, LINES_PER_BLOCK);
      lines = (lines - (uint64_t)gc_block_live_lines(b)) * LINE_SIZE;
      gc_heap.ctr.sticky.inplace -= b->idle;
      gc_heap.live -= b->idle;
      gc_heap.promoted_since_full -= lines;
      gc_heap.mature_since_full -= lines;
    } else if (b->kept == SPLUS_EVAC) {
      /* It held no live line when the collection began: its live lines
       * are the ones this collection made live, counted as they were
       * (Block.search, y_line_live; the Valk stage, VALK-T, O600: until
       * then its line marks were counted here). */
      uint64_t lines = (uint64_t)b->search * LINE_SIZE;
      if (gc_heap.set.verify && b->search != (uint16_t)gc_block_live_lines(b)) {
        gc_die("avant gc verify: an evacuated block's count of the lines the collection made live differs from its line marks");
      }
      memset(b->line_mark, 0, LINES_PER_BLOCK);
      gc_heap.ctr.sticky.inplace -= b->idle;
      gc_heap.live -= b->idle;
      gc_heap.promoted_since_full -= lines;
      gc_heap.mature_since_full -= lines;
    }
  }
  if (gc_heap.splus_age && S_LARGE_AGE >= 2 && gc_heap.keep_n) {
    (void)splus_age_holders();
  }
  if (avant_g_stats.copied_bytes != gc_heap.splus_copied0) {
    splus_fix();
  }
}

/* After the verifier: the moved objects' old copies lose their start bits
 * in the blocks that stay (no lookup may find a forwarded header; the
 * young sweep frees their lines unless a mature object keeps one live;
 * a block evacuated whole is freed by the sweep), and the copy block's
 * free rest joins part_blocks. */
static void splus_finish(void) {
  uint32_t i;
  Block *d = gc_heap.splus_dst;
  int copied = avant_g_stats.copied_bytes != gc_heap.splus_copied0;
  /* Only when a block entered the map (none did in a collection with no
   * survivor to move: no walk of the young blocks then). */
  for (i = 0; gc_heap.evac_any && i < gc_heap.young_blocks_n; i++) {
    Block *b = gc_heap.young_blocks[i];
    int w;
    int scan;
    if (b->kept != SPLUS_EVAC && b->kept != SPLUS_PART) {
      continue;
    }
    /* An evacuated block that held no mature line is freed whole by the
     * sweep (its line marks are clear), and a block taken again has its
     * start bits cleared: only a block that stays (one whose evacuation
     * stopped, one holder aging took an object from, a mixed block of the
     * stress mode) needs its moved objects' start bits cleared here (the
     * Valk stage, VALK-T, O600: until then every evacuated block's 64
     * start words were read, and every survivor's old header). */
    scan = copied && (b->kept == SPLUS_PART || (gc_heap.set.stress && gc_heap.stress_mixed[i]));
    /* A moved object was young: its header lies in the block's young
     * line range, so only that range's start words are read (two lines a
     * word; under the stress mode, which evacuates a mixed block at every
     * allocation, the range is often one hole). */
    for (w = b->young_lo / 2; scan && w < (b->young_hi + 1) / 2 && w < START_WORDS; w++) {
      uint64_t bits = b->start[w];
      while (bits) {
        int bit = __builtin_ctzll(bits);
        AvantHeader *h = (AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8);
        bits &= bits - 1;
        if (h->flags & AVANT_FLAG_FORWARDED) {
          b->start[w] &= ~(1ULL << bit);
        }
      }
    }
    /* The map is clear between collections (it is set when the blocks
     * are chosen, whether or not a copy then succeeds). */
    evac_map_set(b, 0);
  }
  gc_heap.evac_any = 0;
  /* The full stress mode (Stage 28 W16a) collects before every runtime C
   * allocation and copies a few objects each time: the copy block stays
   * the copy block across young collections until it is full (or a full
   * collection drops it, gc_sticky_full_begin), or every collection left a
   * fresh block holding one copied object, the rest of it holes young
   * allocation then took and the next collection evacuated (a block per
   * collection: limit_defer.av grew to 1 GB). Nothing else allocates in
   * it meanwhile: it is on no allocation list. */
  if (gc_heap.set.stress && d && d->bump < d->base + BLOCK_SIZE) {
    return;
  }
  if (d && d->bump < d->base + BLOCK_SIZE) {
    gc_reset_block_bump(d);
    gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, d);
  }
  gc_heap.splus_dst = NULL;
}
#endif

/* Block.kept in a young collection: the young sweep
 * left the block empty, and it leaves mature space (mature_drop_empty);
 * under S+ also SPLUS_KEPT and SPLUS_EVAC during a young collection. */
#define YOUNG_EMPTY 4

/* The EMPTY young blocks the sweep left without a live line (kept ==
 * YOUNG_EMPTY) leave the mature space, and its order is reversed: what
 * gc_filter_blocks(keep_unless_empty) did until the Valk stage's VALK-T
 * by walking every block of the heap (O577: 4,242 of a "nothing changed"
 * young collection's 13,576 instructions at 250 blocks, growing with the
 * heap). Without the walk: the space's blocks are two stacks (GcSpace's
 * comment) and a block the young sweep can empty held no mature line when
 * the collection began, so it was taken since the last young collection
 * began (a fresh block young allocation took, a survivor block of that
 * collection), and such blocks are at the tops of the two stacks. Both
 * are searched from the top, a block of each in turn, until the EMPTY
 * blocks are found: the search ends after about as many blocks as were
 * taken since that collection began, whatever the heap's size. The
 * blocks are then freed in the order the walk freed them (the space's
 * order: a freed block's place in the cache and in its chunk's free list
 * decides which block the next allocation takes), and the order is
 * reversed by flipping the direction, as the walk left the list
 * reversed. */
static void mature_drop_empty(uint32_t empty) {
  GcSpace *sp = &gc_heap.space[SPACE_MATURE];
  Block **pl = &sp->list;
  Block **pb = &sp->back;
  Block **dl;
  Block **db;
  uint32_t nl = 0;
  uint32_t nb = 0;
  uint32_t left = empty;
  uint32_t i;
  if (empty > gc_heap.dropped_cap) {
    gc_heap.dropped_cap = empty * 2;
    gc_heap.dropped = gc_xrealloc(gc_heap.dropped, (size_t)gc_heap.dropped_cap * 2 * sizeof(Block *));
  }
  dl = gc_heap.dropped;
  db = gc_heap.dropped + gc_heap.dropped_cap;
  while (left && (*pl || *pb)) {
    Block *b = *pl;
    if (b) {
      if (b->kept == YOUNG_EMPTY) {
        *pl = b->next;
        dl[nl++] = b;
        left--;
      } else {
        pl = &b->next;
      }
    }
    b = *pb;
    if (left && b) {
      if (b->kept == YOUNG_EMPTY) {
        *pb = b->next;
        db[nb++] = b;
        left--;
      } else {
        pb = &b->next;
      }
    }
  }
  if (left) {
    gc_die("a young block the sweep emptied is not in the mature space's lists");
  }
  /* In the space's order: the head stack's from its top down, then the
   * other's from its bottom up. */
  {
    Block **dh = sp->rev ? db : dl;
    Block **dt = sp->rev ? dl : db;
    uint32_t nh = sp->rev ? nb : nl;
    uint32_t nt = sp->rev ? nl : nb;
    for (i = 0; i < nh; i++) {
      dh[i]->kept = 0;
      gc_cache_or_free_block(dh[i]);
    }
    for (i = nt; i-- > 0;) {
      dt[i]->kept = 0;
      gc_cache_or_free_block(dt[i]);
    }
  }
  sp->rev ^= 1;
}

/* The young sweep of block B (entry I of the young blocks) under the
 * stress mode and under the verifier (the Valk stage, VALK-T2, O628): up
 * to B's allocation frontier. B's young range [young_lo, young_hi) is the
 * hull of the holes young allocation took in it since the last collection
 * (young_note_hole), and the frontier is the line after the last byte
 * allocated in the last of them: Block.bump, which every allocation path
 * keeps (bump_take, tl_bump, young_overflow, the inline sequence's store).
 * The cursor takes a block's holes in rising order, so every young object
 * of this cycle lies below the frontier, and the lines from it to young_hi
 * are the rest of the last hole: free when it was handed out (their start
 * bits clear, as every hole's are) and never allocated since. So only the
 * lines below the frontier are swept and, under the stress mode, poisoned
 * (until VALK-T2 a stress collection poisoned the 128 lines of a block of
 * which one had been allocated since the last collection: 4,400 of its
 * 12,233 instructions and 32 KB written); the lines past it are counted
 * free, as the sweep of the whole range counted them. Every line a young
 * object of this cycle lay in is poisoned as before; a line past the
 * frontier held none. A survivor block's range ends at its frontier
 * already (its bump is the block's end). Under AVANT_GC_VERIFY the lines
 * past the frontier are read, and a start bit or a live line there stops
 * the program; the verifier's sweep stops at the frontier too, so the
 * gate's verify pass checks the frontier on every program it runs.
 * Returns the block's live lines, as the sweep does. */
static __attribute__((noinline)) int young_sweep_checked(Block *b, uint32_t i, int stress, int verify) {
  int lo = b->young_lo;
  int hi = b->young_hi > LINES_PER_BLOCK ? LINES_PER_BLOCK : b->young_hi;
  int fr = (int)(((size_t)(b->bump - b->base) + LINE_SIZE - 1) >> LINE_SHIFT);
  int live;
  int l;
  if (fr > hi) {
    fr = hi;
  }
  if (fr < lo) {
    fr = lo;
  }
  if (verify) {
    for (l = fr; l < hi; l++) {
      /* A line's start bits are half a start word (32 granules). */
      if (line_live(b, l) || ((b->start[l >> 1] >> (32 * (l & 1))) & 0xffffffffULL)) {
        gc_die("avant gc verify: a young block has a start bit or a live line past its allocation frontier");
      }
    }
  }
#if GC_STICKY_COPY
  /* The block known to have no live line (gc_sticky_collect's comment):
   * its count alone. */
  if ((b->kept == SPLUS_EVAC && !(stress && gc_heap.stress_mixed[i])) || (b->kept == SPLUS_PURE && b->idle == 0)) {
    if (verify && gc_block_any_live(b)) {
      gc_die("avant gc verify: a young block taken for empty has a live line");
    }
    live = 0;
    if (hi > lo) {
      gc_heap.ctr.sticky.freed_lines += (uint64_t)(hi - lo);
    }
    gc_heap.ctr.sticky.blocks++;
  } else
#endif
  {
    live = sweep_lines(b, lo, fr);
    gc_heap.ctr.sticky.freed_lines += (uint64_t)(hi - fr);
  }
  if (stress) {
    /* Freed young lines are poisoned (Stage 28 W16a). */
    for (l = lo; l < fr; l++) {
      if (!line_live(b, l)) {
        gc_poison(b->base + (size_t)l * LINE_SIZE, LINE_SIZE);
      }
    }
  }
  return live;
}

void gc_sticky_collect(int tenure) {
  Block *b;
  uint32_t empty = 0;
  int ticked = 0;
  gc_heap.young_large_live = 0;
  gc_heap.keep_n = 0;
  gc_heap.work_n = 0;
  gc_heap.noted_hi = UINT32_MAX;
#if GC_STICKY_COPY
  /* In shared programs too since Stage 28 W17a: every other thread is
   * stopped at a safepoint (its references in root slots) or native (it
   * touches no unpinned heap object), so the copy may move what any
   * thread's roots name (until W17a nothing moved there, D191). */
  gc_heap.splus_on = 1;
  gc_heap.splus_copied0 = avant_g_stats.copied_bytes;
  gc_heap.splus_age = YOUNG_AGE >= 2 && !tenure;
  gc_heap.aged_now = 0;
  if (gc_heap.splus_on) {
    uint32_t i;
    if (gc_heap.set.stress && gc_heap.young_blocks_n > gc_heap.stress_lines_cap) {
      gc_heap.stress_lines_cap = gc_heap.young_blocks_n * 2;
      gc_heap.stress_lines = gc_xrealloc(gc_heap.stress_lines, (size_t)gc_heap.stress_lines_cap * LINES_PER_BLOCK);
      gc_heap.stress_mixed = gc_xrealloc(gc_heap.stress_mixed, (size_t)gc_heap.stress_lines_cap);
    }
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      Block *yb = gc_heap.young_blocks[i];
      /* A block taken whole since the last collection and a survivor
       * block have no live line (SPLUS_FRESH's comment): only a recycled
       * block's marks are read (the Valk stage, VALK-T, O600).
       * AVANT_GC_VERIFY reads them all and stops on a live one. */
      int known_pure = yb->kept == SPLUS_FRESH || yb->kept == SPLUS_WAS_SURV;
      int mature_lines = known_pure ? 0 : gc_block_any_live(yb);
      if (known_pure && gc_heap.set.verify && gc_block_any_live(yb)) {
        gc_die("avant gc verify: a fresh young block or a survivor block has a live line when a young collection begins");
      }
      yb->search = 0;
      if (gc_heap.set.stress) {
        /* The full stress mode (Stage 28 W16a): a block with mature
         * lines is evacuated too (splus_evacuate), its marks restored
         * from this copy afterwards. */
        memcpy(gc_heap.stress_lines + (size_t)i * LINES_PER_BLOCK, yb->line_mark, LINES_PER_BLOCK);
        gc_heap.stress_mixed[i] = (uint8_t)mature_lines;
      }
      yb->kept = mature_lines ? (gc_heap.set.stress ? SPLUS_MIXED : 0)
                              : yb->kept == SPLUS_WAS_SURV ? SPLUS_PURE_AGED : SPLUS_PURE;
      yb->idle = 0;
    }
  }
#endif
  trace_all();
#if GC_STICKY_COPY
  if (gc_heap.splus_on) {
    splus_evacuate();
  }
#endif
  if (gc_heap.set.verify) {
    gc_verify_sticky();
  }
#if GC_STICKY_COPY
  if (gc_heap.splus_on) {
    splus_finish();
  }
#endif
  /* The young large objects: an unreached one is freed; a reached one is
   * stays young at its first survival (SURVIVED, S_LARGE_AGE 2, the
   * default by measurement since Stage 28 W15d) and is promoted at its
   * second (O88, O76, O85), its holders that are not young remembered by
   * rem_rebuild; with S_LARGE_AGE 1, or a fixed AVANT_YOUNG_MB (TENURE),
   * it is promoted at once. */
  gc_heap.force_tenure = tenure || S_LARGE_AGE <= 1;
  /* The return clock ticks before the young large objects are freed
   * (Stage 28 W15e): a run freed now must carry the new stamp, or the
   * return below gives it back at once and its next use faults every
   * page in again (Binarytrees::Arena's 25 MB arena freed young: 13 k ->
   * 101 k page faults, +0.12 s; W15e record, item 2). */
  if (gc_heap.young_large_n) {
    gc_heap.return_clock++;
    ticked = 1;
  }
  gc_reclaim_young_large();
  gc_heap.force_tenure = 0;
  rem_empty();
  rem_rebuild();
  /* The young blocks (the young-blocks list, gc_heap.young_blocks): only
   * the line range young allocation used in each is swept. A block left
   * wholly free leaves mature space below (O89); one with free lines goes
   * to part_blocks, the young allocator's next holes. */
  {
    uint32_t i;
    /* The two check modes, read once (the loop makes calls). */
    const int stress = gc_heap.set.stress;
    const int verify = gc_heap.set.verify;
    for (i = 0; i < gc_heap.young_blocks_n; i++) {
      int live;
      b = gc_heap.young_blocks[i];
      if (__builtin_expect(stress | verify, 0)) {
        /* Under the stress mode and the verifier: up to the block's
         * allocation frontier (young_sweep_checked). The lines between
         * the frontier and the end of the young range are free and their
         * start bits clear either way, so the block is left as the sweep
         * of its whole range leaves it, and the count is the same. */
        live = young_sweep_checked(b, i, stress, verify);
      } else
#if GC_STICKY_COPY
      /* A block evacuated whole (its line marks were cleared) and a block
       * that held no live line and had no survivor have no live line: the
       * sweep of such a block is its count alone (the Valk stage, VALK-T,
       * O600: until then its marks were read to find that out, and its
       * start bits cleared, which a block taken again has cleared anyway).
       * AVANT_GC_VERIFY reads the marks and stops on a live one
       * (young_sweep_checked). */
      if (b->kept == SPLUS_EVAC || (b->kept == SPLUS_PURE && b->idle == 0)) {
        int hi = b->young_hi > LINES_PER_BLOCK ? LINES_PER_BLOCK : b->young_hi;
        live = 0;
        if (hi > b->young_lo) {
          gc_heap.ctr.sticky.freed_lines += (uint64_t)(hi - b->young_lo);
        }
        gc_heap.ctr.sticky.blocks++;
      } else
#endif
      {
        live = sweep_lines(b, b->young_lo, b->young_hi);
      }
      b->young_lo = 0;
      b->young_hi = 0;
      b->kept = 0;
      b->idle = 0;
      if (live == 0) {
        b->kept = YOUNG_EMPTY;
        empty++;
      } else if (live < LINES_PER_BLOCK) {
        /* AVANT_GC_VERIFY: the lines this sweep freed are not
         * allocatable until the next collection begins
         * (gc_quarantine_release), so a reference to an object that died
         * here still names freed memory there, where the lost-reference
         * check (avant_gc_verify.c) sees it. */
        if (verify) {
          gc_sticky_push(&gc_heap.verify_holes, &gc_heap.verify_holes_n, &gc_heap.verify_holes_cap, b);
        } else {
          gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
        }
      }
    }
    gc_heap.young_blocks_n = 0;
  }
#if GC_STICKY_COPY
  /* The survivor blocks (YOUNG_AGE 2) are young blocks until the next
   * collection: their used lines are its young range, and neither young
   * nor mature allocation takes a block with one (young_hi set). */
  {
    uint32_t i;
    for (i = 0; i < gc_heap.surv_blocks_n; i++) {
      b = gc_heap.surv_blocks[i];
      b->kept = SPLUS_WAS_SURV;
      b->idle = 0;
      b->young_lo = 0;
      b->young_hi = (uint8_t)((b->bump - b->base + LINE_SIZE - 1) / LINE_SIZE);
      b->bump = b->base + BLOCK_SIZE;
      b->limit = b->base + BLOCK_SIZE;
      b->search = LINES_PER_BLOCK;
      gc_sticky_push(&gc_heap.young_blocks, &gc_heap.young_blocks_n, &gc_heap.young_blocks_cap, b);
    }
    gc_heap.surv_blocks_n = 0;
    gc_heap.splus_sdst = NULL;
  }
#endif
  /* (Every thread's young cursor and overflow block were dropped when the
   * collection began, gc_alloc_stop_all.) */
  /* The empty young blocks go back to their chunks' free lists (O89):
   * before, they stayed in mature space until a full collection, so a
   * program that never runs one never returned them. They are unlinked
   * from the mature space's lists (mature_drop_empty: no walk of the
   * heap's blocks) only when this sweep left a block empty; the return
   * then runs on the hysteresis clock (gc_heap.return_clock), keeping
   * max(4 MiB, a quarter of the heap in use) of free memory resident. */
  if (empty) {
    /* The clock ticks first: the blocks freed now carry the new stamp, so
     * only memory that stayed free since an earlier tick goes back (a
     * block freed and reused every cycle stays resident: Binarytrees::Obj
     * refaulted its 90 MB every cycle with the tick after the frees). */
    if (!ticked) {
      gc_heap.return_clock++;
    }
    mature_drop_empty(empty);
    gc_heap.ctr.sticky.returned += empty;
    gc_heap_return(gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes);
  }
  gc_clear_noted();
  /* The one young budget: the aged survivors that stay young (as E
   * counted its to-space); the young large objects that stay young only
   * under W15d's age rule (LARGE_AGE_BYTES 0): an age in allocated bytes
   * needs the budget to measure new allocation (O98). */
#if LARGE_AGE_BYTES
  avant_nursery_held = gc_heap.aged_now;
#else
  avant_nursery_held = gc_heap.young_large + gc_heap.aged_now;
#endif
}

/* A full collection under S promotes every object it marks (the full
 * mark clears YOUNG): after it no object is young. Before its mark the
 * young blocks become ordinary mature blocks (the full mark and sweep
 * see every line, and defragmentation may evacuate them) and the young
 * allocator's lists are dropped; after its sweep, which released every
 * empty block, part_blocks is rebuilt from the blocks with free lines and
 * the young budget is empty. */
void gc_sticky_full_begin(void) {
  uint32_t i;
#if GC_STICKY_COPY
  /* A copy block the stress mode kept (splus_finish) becomes an ordinary
   * mature block: its free rest is free lines for the full sweep. */
  if (gc_heap.splus_dst) {
    gc_reset_block_bump(gc_heap.splus_dst);
    gc_heap.splus_dst = NULL;
  }
#endif
  for (i = 0; i < gc_heap.young_blocks_n; i++) {
    gc_heap.young_blocks[i]->young_lo = 0;
    gc_heap.young_blocks[i]->young_hi = 0;
    gc_heap.young_blocks[i]->kept = 0;
  }
  gc_heap.young_blocks_n = 0;
  gc_heap.part_blocks_n = 0;
  gc_heap.verify_holes_n = 0;
}

void gc_sticky_full_end(void) {
  Block *b;
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    if (gc_block_live_lines(b) < LINES_PER_BLOCK) {
      gc_sticky_push(&gc_heap.part_blocks, &gc_heap.part_blocks_n, &gc_heap.part_blocks_cap, b);
    }
  }
  avant_nursery_held = 0;
}
