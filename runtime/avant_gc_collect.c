#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The collection driver and its policy (Stage 28 W12, moved from
 * avant_rt.c): initialization from the environment (every AVANT_GC_* and
 * AVANT_NURSERY* knob), collect (young or full) and gc_maybe_collect (when),
 * the adaptive nursery sizing and the full-collection trigger, and the
 * public collection entry points. */

int g_inited;
int g_collecting;

int g_nursery_off;

uint64_t avant_young_bytes;

/* 0 when AVANT_YOUNG_MB pinned the threshold (tests). */
static int g_young_adapt;
uint64_t avant_nursery_held;

uint64_t g_mature_since_full;

/* Survival-capped growth and copy reserve (Collector II Wave 4, step 2,
 * as amended by Q-W4-6): sticky growth of the young threshold is capped
 * at YOUNG_SURVIVAL_FACTOR times the probe's measured survival, plus
 * slack, because the uncapped growth to 42 MiB on trees was the peak
 * heap driver while a typical probe survives only a few MiB. The
 * dead-based shrink after low-survival copies stays as it was: sizing
 * the nursery from survival alone was measured at factors 2, 1.5, and
 * 1.25 and lost at every one (wall up 26-36% on trees, copied bytes
 * doubled, the peak itself worse at the two smaller factors), because
 * a smaller nursery tenures partly-built trees into mature space. The
 * reserve is the same survival-scaled amount, charged as already spent
 * by heap-limit decisions outside a collection so a copy always has
 * its room; inside a collection the reserve is being spent and is not
 * charged. Kill switches: AVANT_GC_YOUNG_SIZING=0 (the growth cap),
 * AVANT_GC_COPY_RESERVE=0. */
static int g_sizing_mode = 1;
int g_reserve_mode = 1;
uint64_t g_copy_reserve;
#define YOUNG_SURVIVAL_FACTOR 1.5
#define YOUNG_SIZING_FLOOR (4ull * 1024ull * 1024ull)
#define YOUNG_SIZING_SLACK (1ull * 1024ull * 1024ull)
/* Mature growth that triggers the next full collection. Starts at
 * FULL_BYTES and adapts after every full collection to twice the
 * live set plus slack, so a program with a small live set and heavy
 * mature churn (a parsed document per iteration) does not grow the
 * heap to 32 MB before reclaiming, while a program whose live set is
 * large keeps today's spacing. */
static uint64_t g_full_trigger = FULL_BYTES;

/* A depth-20 object tree is about 96 MiB. The nursery grows to hold one
 * burst and collects when that burst has died. 192 MiB is the cap. */
#define YOUNG_BYTES_MAX (192ull * 1024ull * 1024ull)
#define STICKY_MIN (8ull * 1024ull * 1024ull)

uint64_t g_live;
static uint64_t g_last_full_at;

static uint64_t env_u64(const char *name, uint64_t fallback) {
  const char *s = getenv(name);
  if (!s || !s[0]) {
    return fallback;
  }
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (end == s) {
    return fallback;
  }
  return (uint64_t)v;
}

_Atomic uint32_t g_avant_str_gen;

/* Return 1 if the nursery should be left in place and the threshold raised.
 * A live object tree larger than the nursery used to be promoted and then
 * retraced. Waiting until that tree is dead costs a mark and no copy. */
static int young_pause(uint64_t used, uint64_t surv) {
  uint64_t dead;
  uint64_t next;
  if (g_heap_max && g_heap_bytes + g_young_bytes > g_heap_max) {
    return 0;
  }
  if (g_heap_max && g_reserve_mode &&
      g_heap_bytes + g_young_bytes + g_copy_reserve > g_heap_max) {
    /* Pausing would leave the nursery in place with no copy room
     * under the limit; copy instead (Wave 4 reserve, Q-W4-2). */
    return 0;
  }
  dead = used > surv ? used - surv : 0;
  if (dead >= STICKY_MIN && dead >= surv) {
    next = used + (dead - surv) + (64ull * 1024ull);
  } else {
    next = g_young_bytes * 2;
    if (next < used * 2) {
      next = used * 2;
    }
  }
  if (next > YOUNG_BYTES_MAX) {
    next = YOUNG_BYTES_MAX;
  }
  if (g_sizing_mode) {
    /* Survival cap (Wave 4 step 2, Q-W4-6): growth may not outrun what
     * the probe just saw survive by more than the factor and slack.
     * The floor keeps a minimum working nursery when survival reads
     * near zero; a cap at or below the current threshold simply means
     * no growth this time, so the copy proceeds instead of pausing. */
    uint64_t cap =
        (uint64_t)(YOUNG_SURVIVAL_FACTOR * (double)surv) + YOUNG_SIZING_SLACK;
    if (cap < YOUNG_SIZING_FLOOR) {
      cap = YOUNG_SIZING_FLOOR;
    }
    if (next > cap) {
      next = cap;
    }
  }
  if (next <= g_young_bytes) {
    return 0;
  }
  g_young_bytes = next;
  g_sticky_grows++;
  return 1;
}

void gc_collect(int full) {
  int pause = 0;
  uint64_t used = 0;
  uint64_t surv = 0;
  if (g_collecting) {
    gc_die("GC reentered");
  }
  g_collecting = 1;
  atomic_fetch_add_explicit(&g_avant_str_gen, 1u, memory_order_release);
  g_stats.collections++;
  if (g_verify) {
    gc_verify_no_stale_marks();
  }
  g_live = 0;
  g_work_n = 0;
  gc_fast_clear();
  g_cur = NULL;
  g_bump = NULL;
  g_limit = NULL;

  /* Spill the callers' callee-saved GPRs into collect's frame so the
   * C-stack walks see a reference a caller keeps in one: the young
   * copy's rewrite (rewrite_c_stack) and the full collection's
   * defragmentation exclusion (gc_note_c_stack_blocks, C31). Without
   * this, a heap pointer can sit in rbx across avant_alloc and miss
   * the shadow stack. Before the branch, so shrink-wrapping cannot move
   * the saves into the young path only. */
#if defined(__x86_64__)
  __asm__ volatile ("" ::: "rbx", "r12", "r13", "r14", "r15", "memory");
#else
  __builtin_unwind_init();
#endif
  if (!full) {
    if (!g_young_adapt) {
      g_stats.young_collections++;
      gc_young_copy_out(1);
    } else {
      used = g_nursery_held;
      if (used >= STICKY_MIN) {
        g_probe = 1;
        g_probe_nursery_bytes = 0;
        gc_gen_young_copy();
        g_probe = 0;
        surv = g_probe_nursery_bytes;
        g_srv_probes++;
        g_srv_used += used;
        g_srv_surv += surv;
        if (g_srv_probes == 1 || surv < g_srv_min) {
          g_srv_min = surv;
        }
        if (surv > g_srv_max) {
          g_srv_max = surv;
        }
        {
          uint64_t pct = used ? (surv * 100) / used : 0;
          int bucket = pct == 0 ? 0 : pct <= 10 ? 1 : pct <= 25 ? 2 : pct <= 50 ? 3 : pct <= 75 ? 4 : 5;
          g_srv_hist[bucket]++;
        }
        if (g_reserve_mode) {
          g_copy_reserve =
              (uint64_t)(YOUNG_SURVIVAL_FACTOR * (double)surv);
        }
        gc_rem_scrub();
        gc_reclaim_young_large();
        gc_clear_noted();
        g_work_n = 0;
        if (surv * 2 > used) {
          pause = young_pause(used, surv);
        }
      }
      if (!pause) {
        int tenure = 0;
        g_stats.young_collections++;
        if (used >= STICKY_MIN && surv * 2 > used) {
          /* Cap reached, or the heap cannot grow. The graph is live.
           * Tenure it so the next cycle does not trace it again. */
          tenure = 1;
        }
        gc_young_copy_out(tenure);
        if (g_young_adapt && used >= STICKY_MIN && surv * 2 <= used &&
            used > surv && (used - surv) >= STICKY_MIN) {
          uint64_t dead = used - surv;
          uint64_t next = dead + (64ull * 1024ull);
          if (next > YOUNG_BYTES_MAX) {
            next = YOUNG_BYTES_MAX;
          }
          if (next >= STICKY_MIN) {
            g_young_bytes = next;
          }
        }
      }
    }
  } else {
    Block *b;
    LargeIter it;
    AvantHeader *lh;
    g_stats.full_collections++;
    /* Non-moving. Evacuating the nursery here needs a second copy of
     * every live object while the from-space blocks are still charged
     * against the heap limit. */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_NURSERY);
    gc_clear_alloc_cur(SPACE_MATURE);
    /* The marks: one memset per chunk of the side bitmap; no header is
     * touched (no young mark outlives its collection, C36). */
    gc_clear_side_marks();
    gc_zero_line_marks(g_mature);
    gc_zero_line_marks(g_nursery);
    gc_zero_line_marks(g_from);
    gc_mark_roots();
    {
      Block *db;
      uint64_t live_lines_this = 0;
      for (db = g_mature; db; db = db->next) {
        int live = gc_block_live_lines(db);
        int bucket = live == 0 ? 0 : live <= 32 ? 1 : live <= 64 ? 2 : live <= 96 ? 3 : live < LINES_PER_BLOCK ? 4 : 5;
        g_def_blocks++;
        g_def_live_lines += (uint64_t)live;
        live_lines_this += (uint64_t)live;
        g_def_hist[bucket]++;
        if (live > 0 && live < LINES_PER_BLOCK) {
          g_def_wasted += (uint64_t)(LINES_PER_BLOCK - live) * LINE_SIZE;
        }
      }
      if (g_defrag_mode) {
        gc_note_c_stack_blocks();
      }
      gc_defrag_evacuate(live_lines_this);
      if (g_verify && g_def_release.n) {
        gc_verify_defrag();
      }
      gc_defrag_release();
    }
    gc_sweep_large();
    gc_filter_blocks(&g_mature, gc_keep_if_live);
    gc_filter_blocks(&g_nursery, gc_keep_if_live);
    gc_filter_blocks(&g_from, gc_keep_if_live);
    g_mark_sum = 0;
    for (b = g_nursery; b; b = b->next) {
      gc_foreach_block_object(b, gc_sum_marked_one);
    }
    for (b = g_from; b; b = b->next) {
      gc_foreach_block_object(b, gc_sum_marked_one);
    }
    g_nursery_held = g_mark_sum;
    g_mature_since_full = 0;
    g_young_large = 0;
    g_young_large_n = 0;
    g_los_eager_hold = 0;
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      lh->flags = (uint16_t)(lh->flags & (uint16_t)~AVANT_FLAG_YOUNG);
    }
    g_last_full_at = g_stats.allocated_bytes;
  }

  if (full) {
    uint64_t next = g_live * 2 + (4ull * 1024ull * 1024ull);
    if (next < (4ull * 1024ull * 1024ull)) {
      next = 4ull * 1024ull * 1024ull;
    }
    if (next > FULL_BYTES) {
      next = FULL_BYTES;
    }
    g_full_trigger = next;
  }
  if (g_yng_min == 0 || g_young_bytes < g_yng_min) {
    g_yng_min = g_young_bytes;
  }
  if (g_young_bytes > g_yng_max) {
    g_yng_max = g_young_bytes;
  }
  g_since_gc = 0;
  g_stats.live_bytes = g_live;
  if (g_live > g_peak_live) {
    g_peak_live = g_live;
  }
  g_collecting = 0;
}

void gc_maybe_collect(void) {
  if (g_collecting || g_gc_defer) {
    return;
  }
  /*
   * Stage 17: myc-llvm spills live pointer SSA into avant_gc_root
   * slots around CALL, so young copy can rewrite them. Full collection
   * remains non-moving. Default is still full collect only; set
   * AVANT_NURSERY=1 to enable young copy. AVANT_NURSERY=0 is explicit
   * Stage 7 behaviour.
   *
   * C helpers also hold heap pointers that gc_mark_roots does not see
   * (concat args, hash tables mid-grow). avant_gc_defer_* blocks
   * collection for that window.
   */
  if (g_nursery_off) {
    if (g_since_gc >= g_full_trigger) {
      gc_collect(1);
    }
    return;
  }
  /* Nursery garbage is reclaimed by a young collection. A full
   * collection runs when mature space, including promotions and large
   * objects, has grown by g_full_trigger. Counting every nursery byte here
   * marked a live tree that a young collection should have discarded. */
  if (g_mature_since_full >= g_full_trigger) {
    gc_collect(1);
  } else if (g_nursery_held >= g_young_bytes) {
    gc_collect(0);
  }
}

void gc_init_slow(void) {
  if (g_inited) {
    return;
  }
  g_inited = 1;
  {
    uint64_t ymb = env_u64("AVANT_YOUNG_MB", 0);
    if (ymb) {
      g_young_bytes = ymb * 1024ull * 1024ull;
      g_young_adapt = 0;
    } else {
      g_young_bytes = YOUNG_BYTES_DEFAULT;
      g_young_adapt = 1;
    }
  }
  uint64_t mb = env_u64("AVANT_HEAP_MAX_MB", 0);
  g_heap_max = mb ? mb * 1024ull * 1024ull : 0;
  {
    const char *v = getenv("AVANT_NURSERY");
    /* Copying nursery is the product default. Set to 0 for Stage 7
     * full-collect (non-moving) diagnosis. */
    if (v && v[0] == '0' && v[1] == 0) {
      g_nursery_off = 1;
    } else {
      g_nursery_off = 0;
    }
  }
  {
    const char *v = getenv("AVANT_NURSERY_DISCARD");
    /* Product path discards unpinned from-space after young copy.
     * Set to 0 to park (diagnostic). */
    if (v && v[0] == '0' && v[1] == 0) {
      g_discard_from = 0;
    } else {
      g_discard_from = 1;
    }
  }
  {
    /* The conservative C-stack pass after a young copy (avant_gc_stack.c,
     * W8c, W11): on with the copying nursery. It rewrites forwarded
     * words left on the collecting thread's stack and counts any that
     * still name from-space (sm_left). AVANT_GC_STACK_REWRITE=0 turns
     * the rewrite off and keeps the count. Read once, here. */
    const char *v = getenv("AVANT_GC_STACK_REWRITE");
    g_c_stack_pass = !g_nursery_off;
    g_c_stack_rewrite = g_c_stack_pass && !(v && v[0] == '0' && v[1] == 0);
  }
  {
    const char *v = getenv("AVANT_GC_REM_FRONTIER");
    g_rem_mode = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_DEFRAG");
    g_defrag_mode = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_YOUNG_SIZING");
    g_sizing_mode = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_COPY_RESERVE");
    g_reserve_mode = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  g_verify = getenv("AVANT_GC_VERIFY") != NULL;
  /* The heap's address range (W14a1): reserved once, here, so a refusal
   * is met at start-up (halving down to 1 GiB) and not at a block. */
  gc_heap_reserve();
  if (getenv("AVANT_GC_STATS")) {
    atexit(gc_print_stats);
  }
}

/* Q128 heap-goal collection for workloads whose allocations all pass
 * through C helpers under defer (the HTTP request path), which never
 * reach gc_maybe_collect undeferred. Callers must be at a boundary where
 * their C frames hold no heap pointers yet (the HTTP dispatch calls
 * this before materializing the request), so the full, non-moving
 * collection strands nothing; defragmentation stays off for the same
 * reason (it moves mature objects). The trigger is an allocation
 * budget since the last full collection — the larger of the live
 * bytes it saw and 4 MB — plus the mature-growth trigger, so the
 * boundary costs one marking per budget of allocation. */
void avant_gc_heap_goal_collect(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
  if (!g_nursery_off && !g_collecting && !g_gc_defer &&
      (g_mature_since_full >= g_full_trigger ||
       g_stats.allocated_bytes - g_last_full_at >=
           (g_live > (4ull * 1024ull * 1024ull)
                ? g_live
                : (4ull * 1024ull * 1024ull)))) {
    int defrag_saved = g_defrag_mode;
    g_defrag_mode = 0;
    gc_collect(1);
    g_defrag_mode = defrag_saved;
  }
  pthread_mutex_unlock(&g_world);
}

void avant_nursery_reclaim(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
  if (!g_nursery_off && !g_collecting && g_nursery_held > 0) {
    gc_collect(0);
  }
  pthread_mutex_unlock(&g_world);
}

void avant_gc_collect(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
  gc_collect(1);
  pthread_mutex_unlock(&g_world);
}
