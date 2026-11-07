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
 * every precise reference is rewritten; blocks the collecting thread's C
 * stack names are not candidates (C31, avant_gc_stack.c). Also the sorted
 * block sets the full collection uses for both lists. */

/* Evacuation (step 2): AVANT_GC_DEFRAG=0 disables it, histogram stays
 * on. Candidates are mature blocks at or below 64 of 128 live lines
 * with no pinned object and no word of the collecting thread's stack
 * naming them (C31, gc_note_c_stack_blocks); per full collection the live lines evacuated
 * stay at or below 25% of that mark's mature live lines, worst first. */
int g_defrag_mode = 1;

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

BlockSet g_def_release;

static int def_cand_cmp(const void *a, const void *b) {
  const DefCand *x = (const DefCand *)a;
  const DefCand *y = (const DefCand *)b;
  if (x->live != y->live) {
    return x->live - y->live;
  }
  return (x->b > y->b) - (x->b < y->b);
}

static void defrag_rewrite_slot(void **slot) {
  void *w = *slot;
  Block *b = NULL;
  Large *ol = NULL;
  void *start = gc_object_payload(w, &b, &ol);
  void *to;
  if (!start) {
    return;
  }
  to = fwd_get(start);
  if (to && to != start) {
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
  if (full_marked(h)) {
    defrag_scan_payload(payload_of(h));
  }
}

static void mature_unlink(Block *b) {
  Block **pp = &g_mature;
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
 * through the forwarding table, and the emptied blocks are freed
 * whole. Destination blocks are allocated before anything moves; if
 * one cannot be allocated under the heap limit, nothing moves and the
 * ordinary non-moving collection completes (g_def_skipped counts it),
 * so defragmentation can never make a collection fail. The evacuated
 * blocks are freed by gc_defrag_release. Not inlined: its locals name
 * objects in the evacuated blocks, and they must not sit in collect's
 * frame, which gc_verify_defrag scans as part of the C stack. */
__attribute__((noinline)) void gc_defrag_evacuate(uint64_t live_lines_this) {
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
  if (!g_defrag_mode || live_lines_this == 0) {
    return;
  }
  for (b = g_mature; b; b = b->next) {
    int live = gc_block_live_lines(b);
    DefCand *c;
    if (live <= 0 || live > 64 || gc_block_has_pinned(b) || gc_block_set_has(&g_cstack_blocks, b)) {
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
            c->bytes += obj_total(h);
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
      g_def_skipped++;
      return;
    }
    dest[ndest++] = nb;
  }
  gc_fwd_reset();
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
          total = obj_total(h);
          if (bump + total > db->base + BLOCK_SIZE) {
            di++;
            if (di >= ndest) {
              Block *nb = gc_new_block(SPACE_MATURE);
              if (!nb) {
                /* Heap limit mid-evacuation: stop here. Objects
                 * already copied are forwarded and fixed up below;
                 * this block keeps the rest and is not freed. */
                stopped = 1;
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
          fwd_put(payload_of(h), payload_of(nh));
          g_def_evac_bytes += total;
          bump += total;
          db->bump = bump;
          bits &= bits - 1;
        }
      }
      if (!stopped) {
        evac_done = ci + 1;
        g_def_evac_blocks++;
      }
    }
    ncand = evac_done;
    if (stopped) {
      g_def_skipped++;
    }
  }
  {
    ThreadReg *t;
    uint32_t fi;
    pthread_mutex_lock(&g_threads_mu);
    for (t = g_threads; t; t = t->next) {
      Frame *f;
      for (f = t->head ? *t->head : NULL; f; f = f->prev) {
        for (fi = 0; fi < f->n; fi++) {
          defrag_rewrite_slot(f->slots[fi]);
        }
      }
    }
    pthread_mutex_unlock(&g_threads_mu);
  }
  {
    PinEnt *p;
    for (p = g_pins; p; p = p->next) {
      defrag_rewrite_slot(&p->payload);
    }
  }
  avant_hash_walk_tls(defrag_rewrite_tls);
  {
    uint32_t ri;
    int locked = rem_locked();
    if (locked) {
      pthread_mutex_lock(&g_rem_mu);
    }
    for (ri = 0; ri < g_rem_n; ri++) {
      defrag_rewrite_slot((void **)&g_remset[ri]);
    }
    if (locked) {
      pthread_mutex_unlock(&g_rem_mu);
    }
  }
  for (b = g_mature; b; b = b->next) {
    gc_foreach_block_object(b, defrag_fix_one);
  }
  for (b = g_nursery; b; b = b->next) {
    gc_foreach_block_object(b, defrag_fix_one);
  }
  for (b = g_from; b; b = b->next) {
    gc_foreach_block_object(b, defrag_fix_one);
  }
  {
    Large *L;
    for (L = g_large; L; L = L->next) {
      if (full_marked(&L->header)) {
        defrag_scan_payload(payload_of(&L->header));
      }
    }
  }
  /* The evacuated blocks are freed by gc_defrag_release, after gc_collect()
   * has let the verifier look at them from its own frame. */
  g_def_release.n = 0;
  for (i = 0; i < ncand; i++) {
    gc_block_set_add(&g_def_release, cand[i].b);
  }
  gc_block_set_seal(&g_def_release);
  free(dest);
  free(cand);
}

void gc_defrag_release(void) {
  size_t i;
  for (i = 0; i < g_def_release.n; i++) {
    mature_unlink(g_def_release.v[i]);
    gc_cache_or_free_block(g_def_release.v[i]);
    g_def_freed_blocks++;
  }
  g_def_release.n = 0;
}
