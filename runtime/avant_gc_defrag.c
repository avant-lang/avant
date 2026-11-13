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

/* Whether block B holds a pinned object: a candidate holding one is not
 * evacuated (D34). Moved here with E's deletion (Stage 28 W15d) from
 * avant_gc_young.c, its only user being this file; a walk of B's start
 * bits. */
static int block_has_pinned(const Block *b) {
  int w;
  for (w = 0; w < START_WORDS; w++) {
    uint64_t bits = b->start[w];
    while (bits) {
      int bit = __builtin_ctzll(bits);
      const AvantHeader *h = (const AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8);
      bits &= bits - 1;
      if (h->flags & AVANT_FLAG_PINNED) {
        return 1;
      }
    }
  }
  return 0;
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
    int w;
    for (w = 0; w < START_WORDS; w++) {
      uint64_t bits = b->start[w];
      while (bits) {
        int bit = __builtin_ctzll(bits);
        const AvantHeader *h = (const AvantHeader *)(b->base + ((size_t)w * 64 + (size_t)bit) * 8);
        bits &= bits - 1;
        if (full_marked(h)) {
          size_t total = gc_obj_total(h);
          if (used + total > BLOCK_SIZE) {
            blocks++;
            used = 0;
          }
          used += total;
        }
      }
    }
  }
  return blocks;
}

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

static void defrag_scan_payload(void *p) {
  EACH_REF_SLOT(p, slot, { defrag_rewrite_slot(slot); });
}

static void defrag_fix_one(AvantHeader *h, Block *b) {
  (void)b;
  /* A moved object's fields are fixed in its copy (marked too). */
  if (full_marked(h) && !(h->flags & AVANT_FLAG_FORWARDED)) {
    defrag_scan_payload(payload_of(h));
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
    if (live <= 0 || live > 64 || block_has_pinned(b)) {
      continue;
    }
    /* The last evacuation's last destination (see the top of the file). */
    if (b == gc_heap.def_last && !gc_heap.set.stress) {
      continue;
    }
    if (ncand == ccap) {
      ccap = ccap ? ccap * 2 : 32;
      cand = (DefCand *)gc_xrealloc(cand, ccap * sizeof(DefCand));
    }
    c = &cand[ncand++];
    c->b = b;
    c->live = live;
    c->bytes = 0;
    {
      int w;
      for (w = 0; w < START_WORDS; w++) {
        uint64_t bits = b->start[w];
        while (bits) {
          int bit = __builtin_ctzll(bits);
          size_t off = ((size_t)w * 64 + (size_t)bit) * 8;
          AvantHeader *h = (AvantHeader *)(b->base + off);
          if (full_marked(h)) {
            c->bytes += gc_obj_total(h);
          }
          bits &= bits - 1;
        }
      }
    }
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
      int w;
      for (w = 0; w < START_WORDS && !stopped; w++) {
        uint64_t bits = cb->start[w];
        while (bits) {
          int bit = __builtin_ctzll(bits);
          size_t off = ((size_t)w * 64 + (size_t)bit) * 8;
          AvantHeader *h = (AvantHeader *)(cb->base + off);
          size_t total;
          AvantHeader *nh;
          if (!full_marked(h)) {
            bits &= bits - 1;
            continue;
          }
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
  {
    ThreadReg *t;
    uint32_t fi;
    pthread_mutex_lock(&gc_heap.threads_mu);
    for (t = gc_heap.threads; t; t = t->next) {
      Frame *f;
      for (f = t->head ? *t->head : NULL; f; f = f->prev) {
        for (fi = 0; fi < f->n; fi++) {
          defrag_rewrite_slot(f->slots[fi]);
        }
      }
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
  for (b = gc_heap.space[SPACE_MATURE].list; b; b = b->next) {
    gc_foreach_block_object(b, defrag_fix_one);
  }
  {
    LargeIter it;
    AvantHeader *lh;
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      if (full_marked(lh)) {
        defrag_scan_payload(payload_of(lh));
      }
    }
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
