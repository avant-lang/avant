#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mature defragmentation (Stage 28 W12, moved from avant_rt.c): during a
 * full collection, sparse mature blocks are evacuated into fresh ones and
 * every precise reference is rewritten. Also the sorted block set of the
 * blocks it frees. */

/* Evacuation (step 2; gc_heap.set.defrag, AVANT_GC_DEFRAG=0 turns it
 * off, the histogram stays on). Candidates are mature blocks at or below
 * 64 of 128 live lines with no pinned object (until Stage 28 W16b also
 * none a word of the collecting thread's stack named, C31's exclusion,
 * D213: runtime C roots what it holds since W16a, RC1);
 * per full collection the live lines evacuated stay at or below 25% of
 * that mark's mature live lines, worst first.
 *
 * No evacuation without a gain (Stage 29 W-FIX1c, O202). Moving anything
 * at all costs a pass over every live object to rewrite references
 * (gc_defrag_evacuate's fix-up), so the chosen blocks are evacuated only
 * when that frees blocks net, the blocks freed less the destination
 * blocks their live objects fill (def_dest_blocks): at least one, and at
 * least one for every DEFRAG_GAIN_DIV blocks' worth of live lines the
 * mark found, since the pass costs in proportion to them (a heap of up to
 * 128 live blocks, 4 MiB, moves for one block; one of 8 MiB needs two).
 * What is given up is bounded: sparse blocks that would have freed less
 * than 1/128 of the live heap stay, and their free lines stay
 * allocatable. And the block the last evacuation filled last
 * (gc_heap.def_last), sparse by construction, is not a candidate:
 * evacuated with the next sparse block it would be copied again at every
 * full collection for one block's gain.
 * Before, a heap that did not change had its one sparse block (the last
 * destination) evacuated into a fresh block at every full collection:
 * the same few hundred bytes moved, no block gained, and 41% of the
 * collection spent in the fix-up (500,000 objects, W-BENCH-GC F4).
 * Under AVANT_GC_STRESS=1 neither rule applies: the stress mode moves
 * what can move, as before. */

/* The live objects of block B are its start bits that are also marked,
 * read a word (64 granules) at a time: an object's mark bit is at its
 * header's granule, as its start bit is, so the block's slice of
 * gc_heap.marks lines up with Block.start. A dead object (its start bit
 * stays while its line is live) costs nothing (the Valk stage, VALK-T2,
 * O603: until then every walk of a block here read every start bit's
 * object and asked its mark). */
static inline const uint64_t *block_marks(const Block *b) {
  return &gc_heap.marks[((uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT) * START_WORDS];
}

/* The bytes of block B's live objects in *BYTES; 0 when one of them is
 * pinned: a candidate holding a pinned object is not evacuated (D34). A
 * pinned object is live (avant_pin flags an object it puts on the pin
 * list, a root; avant_unpin clears the flag), so asking the marked objects
 * alone is asking every object, as block_has_pinned did until VALK-T2. */
static int block_live_bytes(const Block *b, uint64_t *bytes) {
  const uint64_t *marks = block_marks(b);
  uint64_t n = 0;
  int w;
  for (w = 0; w < START_WORDS; w++) {
    uint64_t bits = b->start[w] & marks[w];
    while (bits) {
      const AvantHeader *h = (const AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)__builtin_ctzll(bits)) * 8);
      bits &= bits - 1;
      if (h->flags & AVANT_FLAG_PINNED) {
        return 0;
      }
      n += gc_obj_total(h);
    }
  }
  *bytes = n;
  return 1;
}

typedef struct DefCand {
  Block *b;
  int live;
  uint64_t bytes;
} DefCand;

static int block_ptr_cmp(const void *a, const void *b) {
  const Block *x = *(Block *const *)a;
  const Block *y = *(Block *const *)b;
  return (x > y) - (x < y);
}

void gc_block_set_add(BlockSet *s, Block *b) {
  if (s->n && s->v[s->n - 1] == b) {
    return;
  }
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->v = (Block **)gc_xrealloc(s->v, s->cap * sizeof(Block *));
  }
  s->v[s->n++] = b;
}

void gc_block_set_seal(BlockSet *s) {
  size_t i, j = 0;
  if (s->n < 2) {
    return;
  }
  qsort(s->v, s->n, sizeof(Block *), block_ptr_cmp);
  for (i = 0; i < s->n; i++) {
    if (j == 0 || s->v[j - 1] != s->v[i]) {
      s->v[j++] = s->v[i];
    }
  }
  s->n = j;
}

int gc_block_set_has(const BlockSet *s, const Block *b) {
  size_t lo = 0, hi = s->n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (s->v[mid] < b) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < s->n && s->v[lo] == b;
}

static int def_cand_cmp(const void *a, const void *b) {
  const DefCand *x = (const DefCand *)a;
  const DefCand *y = (const DefCand *)b;
  if (x->live != y->live) {
    return x->live - y->live;
  }
  return (x->b > y->b) - (x->b < y->b);
}

/* The destination blocks the marked objects of CAND[0..N) fill when they
 * are copied in order, as gc_defrag_evacuate's copy packs them (an object
 * that does not fit the rest of a block starts the next one). */
static size_t def_dest_blocks(const DefCand *cand, size_t n) {
  size_t blocks = 0;
  size_t used = BLOCK_SIZE;
  size_t i;
  for (i = 0; i < n; i++) {
    const Block *b = cand[i].b;
    const uint64_t *marks = block_marks(b);
    int w;
    for (w = 0; w < START_WORDS; w++) {
      uint64_t bits = b->start[w] & marks[w];
      while (bits) {
        int bit = __builtin_ctzll(bits);
        const AvantHeader *h = (const AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8);
        size_t total = gc_obj_total(h);
        bits &= bits - 1;
        if (used + total > BLOCK_SIZE) {
          blocks++;
          used = 0;
        }
        used += total;
      }
    }
  }
  return blocks;
}

/* A root's word (a frame slot's, a pin's, a runtime-held slot's, a task
 * root's, a remembered entry): it may name the inside of an object, so
 * the start bits resolve it. */
static void defrag_rewrite_slot(void **slot) {
  void *w = *slot;
  Block *b = NULL;
  AvantHeader *ol = NULL;
  void *start = gc_object_payload_fast(w, &b, &ol);
  void *to;
  if (!start) {
    return;
  }
  to = gc_forwarded(start);
  if (to) {
    *slot = (void *)((uint8_t *)to + ((uint8_t *)w - (uint8_t *)start));
  }
}

static void defrag_rewrite_tls(void **slot) {
  defrag_rewrite_slot(slot);
}

/* The fix-up of the heap's fields (the Valk stage, VALK-T2, O603), in the
 * shape VALK-T built for the young fix-up (fix_field, fix_holder,
 * avant_gc_sticky.c). Until VALK-T2 every reference of every live object
 * was asked the object lookup (gc_object_payload_fast: the alignment, the
 * range, the start bit, the chunk's kind, the block in use) and then its
 * header's forward, through a call per object and a second mark test.
 *
 * The evacuated blocks are in the block map (gc_heap.evac_map, one byte
 * per block, the young copy's map: clear between young collections, and a
 * full collection is not one): DEFRAG_MAP_EVAC for a block evacuated
 * whole, DEFRAG_MAP_PART for the block an evacuation stopped in at the
 * heap limit. A field's word is null, a static address or an exact start
 * (the field rule, avant_gc_internal.h), so its header's block is the one
 * the map is asked about, and only in a block objects were moved out of is
 * the header read (a field of a marked object names a marked object: in a
 * block evacuated whole it moved; in the stopped block its header says).
 * A reference whose block did not move costs the map's test. The map
 * covers the heap as committed when the blocks were chosen; a block past
 * it is a destination block's new chunk, never evacuated. */
#define DEFRAG_MAP_EVAC 1
#define DEFRAG_MAP_PART 2

typedef struct DefCtx {
  uintptr_t base8;
  const uint8_t *map;
  uint64_t map_n;
} DefCtx;

static inline __attribute__((always_inline)) void defrag_fix_field(const DefCtx *c, void **slot) {
  void *v = *slot;
  uint64_t bi = (uint64_t)((uintptr_t)v - c->base8) >> BLOCK_SHIFT;
  if (__builtin_expect(bi < c->map_n && c->map[bi], 0)) {
    const AvantHeader *h = (const AvantHeader *)((uint8_t *)v - sizeof(AvantHeader));
    if (h->flags & AVANT_FLAG_FORWARDED) {
      *slot = gc_forward_of(h);
    }
  }
}

/* The fields of an object that is not a small record (a pointer buffer,
 * an element buffer, a record with more than seven reference words), by
 * its bitmap; out of the block walk's loop. */
static __attribute__((noinline)) void defrag_fix_walk(const DefCtx *c, void *p) {
  EACH_REF_SLOT_WALK(p, slot, { defrag_fix_field(c, slot); });
}

/* The fields of the live object at P (a large object's, here): a small
 * record's by its type's list (gc_heap.refs); any other object's by its
 * bitmap. */
static void defrag_fix_object(const DefCtx *c, void **p) {
  const AvantHeader *h = avant_header(p);
  uint64_t d = gc_heap.refs[h->type_id];
  unsigned k = (unsigned)(d & 0xffu);
  if (k > TYPE_REFS_MAX) {
    defrag_fix_walk(c, p);
  } else if (k) {
    size_t nw = (size_t)h->size / sizeof(void *);
    do {
      d >>= 8;
      if ((size_t)(d & 0xffu) < nw) {
        defrag_fix_field(c, p + (size_t)(d & 0xffu));
      }
    } while (--k);
  }
}

/* The fix-up of block B's live objects (block_marks' comment: a dead
 * object costs nothing, a live one no call). A record with one reference
 * word, the commonest holder (a list's node, a box, an array's header),
 * has its field fixed with no loop; one with two to seven by its type's
 * list; an object walked by its bitmap (a pointer buffer, an element
 * buffer, a large record) takes defrag_fix_walk. A function of its own,
 * the map and the base in its locals, so that the loop keeps them in
 * registers (in line in gc_defrag_evacuate it spilled four values around
 * every object: 43 instructions an object). Not for the block an
 * evacuation stopped in (defrag_fix_block_part). */
static __attribute__((noinline)) void defrag_fix_block(const Block *b, uintptr_t base8, const uint8_t *map,
                                                       uint64_t map_n) {
  const DefCtx c = {base8, map, map_n};
  const uint64_t *refs = gc_heap.refs;
  const uint64_t *marks = block_marks(b);
  const uint64_t *starts = b->start;
  uint8_t *at = b->base + sizeof(AvantHeader);
  int w;
  for (w = 0; w < START_WORDS; w++, at += 64 * 8) {
    uint64_t bits = starts[w] & marks[w];
    while (bits) {
      void **p = (void **)(at + (size_t)__builtin_ctzll(bits) * 8);
      const AvantHeader *h = avant_header(p);
      uint64_t d = refs[h->type_id];
      unsigned k = (unsigned)(d & 0xffu);
      bits &= bits - 1;
      if (__builtin_expect(k == 1, 1)) {
        /* The list's second byte is the word's index; the rest is zero. */
        size_t x = (size_t)(d >> 8);
        if (x < (size_t)h->size / sizeof(void *)) {
          defrag_fix_field(&c, p + x);
        }
      } else if (k > TYPE_REFS_MAX) {
        defrag_fix_walk(&c, p);
      } else if (k) {
        size_t nw = (size_t)h->size / sizeof(void *);
        do {
          d >>= 8;
          if ((size_t)(d & 0xffu) < nw) {
            defrag_fix_field(&c, p + (size_t)(d & 0xffu));
          }
        } while (--k);
      }
    }
  }
}

/* The same for the block an evacuation stopped in at the heap limit: the
 * objects that moved out of it are told by their headers (their fields
 * are fixed in their copies), the others are fixed here. One block at
 * most in a collection. */
static void defrag_fix_block_part(const Block *b, const DefCtx *c) {
  const uint64_t *marks = block_marks(b);
  int w;
  for (w = 0; w < START_WORDS; w++) {
    uint64_t bits = b->start[w] & marks[w];
    while (bits) {
      void **p = (void **)(b->base + ((size_t)w * 64 + (size_t)__builtin_ctzll(bits)) * 8 + sizeof(AvantHeader));
      bits &= bits - 1;
      if (!(avant_header(p)->flags & AVANT_FLAG_FORWARDED)) {
        defrag_fix_object(c, p);
      }
    }
  }
}

static void mature_unlink(Block *b) {
  Block **pp = &gc_heap.space[SPACE_MATURE].list;
  while (*pp) {
    if (*pp == b) {
      *pp = b->next;
      b->next = NULL;
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Copying defragmentation of the mature space, run inside a full
 * collection after marking (line marks and the side mark bits are the
 * live set) and before the sweep. Live objects in candidate blocks are
 * copied into fresh destination blocks, every root, heap field,
 * and remembered entry naming them is rewritten
 * through the forward in the moved object's header (gc_set_forward,
 * Stage 28 W15a2; before, a side table), and the emptied blocks are freed
 * whole. Destination blocks are allocated before anything moves; if
 * one cannot be allocated under the heap limit, nothing moves and the
 * ordinary non-moving collection completes (gc_heap.ctr.def.skipped counts it),
 * so defragmentation can never make a collection fail. The evacuated
 * blocks are freed by gc_defrag_release. */
void gc_defrag_evacuate(uint64_t live_lines_this) {
  DefCand *cand = NULL;
  size_t ncand = 0;
  size_t ccap = 0;
  size_t i;
  Block *b;
  uint64_t budget;
  uint64_t evac_lines = 0;
  uint64_t evac_bytes = 0;
  Block **dest = NULL;
  size_t ndest = 0;
  size_t dest_cap_blocks;
  Block *stopped_block = NULL;
  if (live_lines_this == 0) {
    return;
  }
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    int live = gc_block_live_lines(b);
    DefCand *c;
    uint64_t bytes = 0;
    if (live <= 0 || live > 64) {
      continue;
    }
    /* The last evacuation's last destination (see the top of the file). */
    if (b == gc_heap.def_last && !gc_heap.set.stress) {
      continue;
    }
    /* Its live objects' bytes, in one walk with the pinned test. */
    if (!block_live_bytes(b, &bytes)) {
      continue;
    }
    if (ncand == ccap) {
      ccap = ccap ? ccap * 2 : 32;
      cand = (DefCand *)gc_xrealloc(cand, ccap * sizeof(DefCand));
    }
    c = &cand[ncand++];
    c->b = b;
    c->live = live;
    c->bytes = bytes;
  }
  if (ncand == 0) {
    free(cand);
    return;
  }
  qsort(cand, ncand, sizeof(DefCand), def_cand_cmp);
  budget = live_lines_this / 4;
  i = 0;
  while (i < ncand && evac_lines + (uint64_t)cand[i].live <= budget) {
    evac_lines += (uint64_t)cand[i].live;
    evac_bytes += cand[i].bytes;
    i++;
  }
  ncand = i;
  if (ncand == 0) {
    free(cand);
    return;
  }
  /* No evacuation without a gain (the top of the file). */
  if (!gc_heap.set.stress) {
    size_t dest_n = def_dest_blocks(cand, ncand);
    uint64_t need = (live_lines_this + (uint64_t)LINES_PER_BLOCK * DEFRAG_GAIN_DIV - 1) /
                    ((uint64_t)LINES_PER_BLOCK * DEFRAG_GAIN_DIV);
    if (ncand <= dest_n || (uint64_t)(ncand - dest_n) < need) {
      free(cand);
      return;
    }
  }
  dest_cap_blocks = (size_t)(evac_bytes / BLOCK_SIZE) + 2;
  dest = (Block **)gc_xrealloc(NULL, dest_cap_blocks * sizeof(Block *));
  for (i = 0; i < dest_cap_blocks; i++) {
    Block *nb = gc_new_block(SPACE_MATURE);
    if (!nb) {
      size_t j;
      for (j = 0; j < ndest; j++) {
        mature_unlink(dest[j]);
        gc_cache_or_free_block(dest[j]);
      }
      free(dest);
      free(cand);
      gc_heap.ctr.def.skipped++;
      return;
    }
    dest[ndest++] = nb;
  }
  {
    Block *db = dest[0];
    size_t di = 0;
    uint8_t *bump = db->base;
    size_t ci;
    size_t evac_done = 0;
    int stopped = 0;
    for (ci = 0; ci < ncand && !stopped; ci++) {
      Block *cb = cand[ci].b;
      const uint64_t *cmarks = block_marks(cb);
      int w;
      for (w = 0; w < START_WORDS && !stopped; w++) {
        /* The block's live objects (block_marks). */
        uint64_t bits = cb->start[w] & cmarks[w];
        while (bits) {
          int bit = __builtin_ctzll(bits);
          size_t off = ((size_t)w * 64 + (size_t)bit) * 8;
          AvantHeader *h = (AvantHeader *)(cb->base + off);
          size_t total;
          AvantHeader *nh;
          total = gc_obj_total(h);
          if (bump + total > db->base + BLOCK_SIZE) {
            di++;
            if (di >= ndest) {
              Block *nb = gc_new_block(SPACE_MATURE);
              if (!nb) {
                /* Heap limit mid-evacuation: stop here. Objects
                 * already copied are forwarded and fixed up below;
                 * this block keeps the rest and is not freed. */
                stopped = 1;
                stopped_block = cb;
                break;
              }
              if (ndest == dest_cap_blocks) {
                dest_cap_blocks = dest_cap_blocks * 2 + 4;
                dest = (Block **)gc_xrealloc(dest, dest_cap_blocks * sizeof(Block *));
              }
              dest[ndest++] = nb;
            }
            db = dest[di];
            bump = db->base;
          }
          nh = (AvantHeader *)bump;
          memcpy(nh, h, total);
          set_start(db, (size_t)(bump - db->base));
          full_set_mark(nh);
          mark_lines(db, (size_t)(bump - db->base), total);
          gc_set_forward(h, payload_of(nh));
          gc_heap.ctr.def.evac_bytes += total;
          bump += total;
          db->bump = bump;
          bits &= bits - 1;
        }
      }
      if (!stopped) {
        evac_done = ci + 1;
        gc_heap.ctr.def.evac_blocks++;
      }
    }
    ncand = evac_done;
    if (stopped) {
      gc_heap.ctr.def.skipped++;
    }
    /* The block filled last: not a candidate of the next evacuation. */
    gc_heap.def_last = db;
  }
  /* The blocks objects moved out of enter the block map (the fix-up's
   * test of every field): the ones evacuated whole and the one the
   * evacuation stopped in. */
  gc_evac_map_ensure();
  for (i = 0; i < ncand; i++) {
    gc_heap.evac_map[(uint64_t)(cand[i].b->base - gc_heap.base) >> BLOCK_SHIFT] = DEFRAG_MAP_EVAC;
  }
  if (stopped_block) {
    gc_heap.evac_map[(uint64_t)(stopped_block->base - gc_heap.base) >> BLOCK_SHIFT] = DEFRAG_MAP_PART;
  }
  {
    ThreadReg *t;
    pthread_mutex_lock(&gc_heap.threads_mu);
    for (t = gc_heap.threads; t; t = t->next) {
      EACH_FRAME_SLOT(gc_thread_frames(t), slot, { defrag_rewrite_slot(slot); });
    }
    pthread_mutex_unlock(&gc_heap.threads_mu);
  }
  {
    PinEnt *p;
    for (p = gc_heap.pins; p; p = p->next) {
      defrag_rewrite_slot(&p->payload);
    }
  }
  gc_each_runtime_root(defrag_rewrite_tls);
  /* Task roots: their records may move since Stage 28 W17a. */
  EACH_TASK_ROOT(slot, defrag_rewrite_slot(slot));
  {
    uint32_t ri;
    for (ri = 0; ri < gc_heap.rem_n; ri++) {
      defrag_rewrite_slot((void **)&gc_heap.remset[ri]);
    }
  }
  {
    DefCtx c;
    LargeIter it;
    AvantHeader *lh;
    c.base8 = (uintptr_t)gc_heap.base + sizeof(AvantHeader);
    c.map = gc_heap.evac_map;
    c.map_n = gc_heap.evac_map_n;
    for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
      /* A block evacuated whole holds only moved objects, whose fields
       * are fixed in their copies (marked too, in the destination
       * blocks): it is passed over. */
      uint64_t bi = (uint64_t)(b->base - gc_heap.base) >> BLOCK_SHIFT;
      uint8_t m = bi < c.map_n ? c.map[bi] : 0;
      if (m == DEFRAG_MAP_EVAC) {
        continue;
      }
      if (m) {
        defrag_fix_block_part(b, &c);
      } else {
        defrag_fix_block(b, c.base8, c.map, c.map_n);
      }
    }
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      if (full_marked(lh)) {
        defrag_fix_object(&c, (void **)payload_of(lh));
      }
    }
  }
  /* The map is clear between collections (the young copy's rule). */
  for (i = 0; i < ncand; i++) {
    gc_heap.evac_map[(uint64_t)(cand[i].b->base - gc_heap.base) >> BLOCK_SHIFT] = 0;
  }
  if (stopped_block) {
    gc_heap.evac_map[(uint64_t)(stopped_block->base - gc_heap.base) >> BLOCK_SHIFT] = 0;
  }
  /* A block whose evacuation stopped stays: its moved objects hold
   * forwards in their headers, so their start bits go (every reference
   * to them is rewritten above), and no lookup or walk takes one for an
   * object again. */
  if (stopped_block) {
    int w;
    for (w = 0; w < START_WORDS; w++) {
      uint64_t bits = stopped_block->start[w];
      while (bits) {
        int bit = __builtin_ctzll(bits);
        AvantHeader *h = (AvantHeader *)(stopped_block->base + ((size_t)w * 64 + (size_t)bit) * 8);
        if (h->flags & AVANT_FLAG_FORWARDED) {
          stopped_block->start[w] &= ~(1ULL << bit);
        }
        bits &= bits - 1;
      }
    }
  }
  /* The evacuated blocks are freed by gc_defrag_release, after gc_collect()
   * has let the verifier look at them from its own frame. */
  gc_heap.def_release.n = 0;
  for (i = 0; i < ncand; i++) {
    gc_block_set_add(&gc_heap.def_release, cand[i].b);
  }
  gc_block_set_seal(&gc_heap.def_release);
  free(dest);
  free(cand);
}

void gc_defrag_release(void) {
  size_t i;
  for (i = 0; i < gc_heap.def_release.n; i++) {
    mature_unlink(gc_heap.def_release.v[i]);
    /* AVANT_GC_STRESS=1: an evacuated block is poisoned (Stage 28 W16a). */
    gc_poison(gc_heap.def_release.v[i]->base, BLOCK_SIZE);
    gc_cache_or_free_block(gc_heap.def_release.v[i]);
    gc_heap.ctr.def.freed_blocks++;
  }
  gc_heap.def_release.n = 0;
}
