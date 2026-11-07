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

void gc_limit_collect_pending(void) {
  gc_heap.limit_pending = 1;
  gc_fast_clear();
}

uint64_t avant_young_bytes;

uint64_t avant_nursery_held;

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
#define YOUNG_SURVIVAL_FACTOR 1.5
#define YOUNG_SIZING_FLOOR (4ull * 1024ull * 1024ull)
#define YOUNG_SIZING_SLACK (1ull * 1024ull * 1024ull)

/* A depth-20 object tree is about 96 MiB. The nursery grows to hold one
 * burst and collects when that burst has died. 192 MiB is the cap. */
#define YOUNG_BYTES_MAX (192ull * 1024ull * 1024ull)
#define STICKY_MIN (8ull * 1024ull * 1024ull)

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
  if (gc_heap.set.heap_max && gc_heap.heap_bytes + avant_young_bytes > gc_heap.set.heap_max) {
    return 0;
  }
  if (gc_heap.set.heap_max && gc_heap.set.copy_reserve &&
      gc_heap.heap_bytes + avant_young_bytes + gc_heap.copy_reserve > gc_heap.set.heap_max) {
    /* Pausing would leave the nursery in place with no copy room
     * under the limit; copy instead (Wave 4 reserve, Q-W4-2). */
    return 0;
  }
  dead = used > surv ? used - surv : 0;
  if (dead >= STICKY_MIN && dead >= surv) {
    next = used + (dead - surv) + (64ull * 1024ull);
  } else {
    next = avant_young_bytes * 2;
    if (next < used * 2) {
      next = used * 2;
    }
  }
  if (next > YOUNG_BYTES_MAX) {
    next = YOUNG_BYTES_MAX;
  }
  if (gc_heap.set.young_sizing) {
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
  if (next <= avant_young_bytes) {
    return 0;
  }
  avant_young_bytes = next;
  gc_heap.ctr.yng.sticky++;
  return 1;
}

void gc_collect(int full) {
  int pause = 0;
  uint64_t used = 0;
  uint64_t surv = 0;
  if (gc_heap.collecting) {
    gc_die("GC reentered");
  }
  gc_heap.collecting = 1;
  atomic_fetch_add_explicit(&g_avant_str_gen, 1u, memory_order_release);
  avant_g_stats.collections++;
  if (gc_heap.set.verify) {
    gc_verify_no_stale_marks();
    if (!full) {
      gc_verify_remembered();
    }
  }
  gc_heap.live = 0;
  gc_heap.work_n = 0;
  gc_fast_clear();
  gc_heap.cur = NULL;
  avant_gc_bump = NULL;
  gc_heap.limit = NULL;

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
    if (!gc_heap.set.young_adapt) {
      avant_g_stats.young_collections++;
      gc_young_copy_out(1);
    } else if (gc_heap.stress_now) {
      /* A stress point copies: no survival probe, no pause, no sizing
       * decision, so every stress point moves what it reaches. */
      avant_g_stats.young_collections++;
      gc_young_copy_out(0);
    } else {
      used = avant_nursery_held;
      if (used >= STICKY_MIN) {
        gc_heap.probe = 1;
        gc_heap.probe_nursery_bytes = 0;
        gc_gen_young_copy();
        gc_heap.probe = 0;
        surv = gc_heap.probe_nursery_bytes;
        gc_heap.ctr.srv.probes++;
        gc_heap.ctr.srv.used += used;
        gc_heap.ctr.srv.surv += surv;
        if (gc_heap.ctr.srv.probes == 1 || surv < gc_heap.ctr.srv.min) {
          gc_heap.ctr.srv.min = surv;
        }
        if (surv > gc_heap.ctr.srv.max) {
          gc_heap.ctr.srv.max = surv;
        }
        {
          uint64_t pct = used ? (surv * 100) / used : 0;
          int bucket = pct == 0 ? 0 : pct <= 10 ? 1 : pct <= 25 ? 2 : pct <= 50 ? 3 : pct <= 75 ? 4 : 5;
          gc_heap.ctr.srv.hist[bucket]++;
        }
        if (gc_heap.set.copy_reserve) {
          gc_heap.copy_reserve =
              (uint64_t)(YOUNG_SURVIVAL_FACTOR * (double)surv);
        }
        gc_rem_scrub();
        gc_reclaim_young_large();
        gc_clear_noted();
        gc_heap.work_n = 0;
        if (surv * 2 > used) {
          pause = young_pause(used, surv);
        }
      }
      if (!pause) {
        int tenure = 0;
        avant_g_stats.young_collections++;
        if (used >= STICKY_MIN && surv * 2 > used) {
          /* Cap reached, or the heap cannot grow. The graph is live.
           * Tenure it so the next cycle does not trace it again. */
          tenure = 1;
        }
        gc_young_copy_out(tenure);
        if (gc_heap.set.young_adapt && used >= STICKY_MIN && surv * 2 <= used &&
            used > surv && (used - surv) >= STICKY_MIN) {
          uint64_t dead = used - surv;
          uint64_t next = dead + (64ull * 1024ull);
          if (next > YOUNG_BYTES_MAX) {
            next = YOUNG_BYTES_MAX;
          }
          if (next >= STICKY_MIN) {
            avant_young_bytes = next;
          }
        }
      }
    }
  } else {
    Block *b;
    LargeIter it;
    AvantHeader *lh;
    avant_g_stats.full_collections++;
    /* Non-moving. Evacuating the nursery here needs a second copy of
     * every live object while the from-space blocks are still charged
     * against the heap limit. */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_NURSERY);
    gc_clear_alloc_cur(SPACE_MATURE);
    /* The marks: one memset per chunk of the side bitmap; no header is
     * touched (no young mark outlives its collection, C36). */
    gc_clear_side_marks();
    gc_zero_line_marks(gc_heap.mature);
    gc_zero_line_marks(gc_heap.nursery);
    gc_zero_line_marks(gc_heap.from);
    gc_mark_roots();
    {
      Block *db;
      uint64_t live_lines_this = 0;
      for (db = gc_heap.mature; db; db = db->next) {
        int live = gc_block_live_lines(db);
        int bucket = live == 0 ? 0 : live <= 32 ? 1 : live <= 64 ? 2 : live <= 96 ? 3 : live < LINES_PER_BLOCK ? 4 : 5;
        gc_heap.ctr.def.blocks++;
        gc_heap.ctr.def.live_lines += (uint64_t)live;
        live_lines_this += (uint64_t)live;
        gc_heap.ctr.def.hist[bucket]++;
        if (live > 0 && live < LINES_PER_BLOCK) {
          gc_heap.ctr.def.wasted += (uint64_t)(LINES_PER_BLOCK - live) * LINE_SIZE;
        }
      }
      if (gc_heap.set.defrag) {
        gc_note_c_stack_blocks();
      }
      gc_defrag_evacuate(live_lines_this);
      if (gc_heap.set.verify && gc_heap.def_release.n) {
        gc_verify_defrag();
      }
      gc_defrag_release();
    }
    /* Dead and clean remembered entries leave the set (C33, O9). */
    gc_rem_full_scrub();
    gc_sweep_large();
    gc_filter_blocks(&gc_heap.mature, gc_keep_if_live);
    gc_filter_blocks(&gc_heap.nursery, gc_keep_if_live);
    gc_filter_blocks(&gc_heap.from, gc_keep_if_live);
    gc_heap.mark_sum = 0;
    /* O28: the young bytes that survived, nursery blocks only. A
     * from-space block's live objects are pinned survivors promoted in
     * place (D205): mature, not young. */
    for (b = gc_heap.nursery; b; b = b->next) {
      gc_foreach_block_object(b, gc_sum_marked_one);
    }
    avant_nursery_held = gc_heap.mark_sum;
    gc_heap.mature_since_full = 0;
    gc_heap.young_large = 0;
    gc_heap.young_large_n = 0;
    gc_heap.los_eager_hold = 0;
    gc_large_iter_init(&it);
    while ((lh = gc_large_next(&it))) {
      lh->flags = (uint16_t)(lh->flags & (uint16_t)~AVANT_FLAG_YOUNG);
    }
    gc_heap.last_full_at = avant_g_stats.allocated_bytes;
    /* Free memory beyond the retention target goes back to the OS. */
    gc_heap_return(gc_heap.live);
  }

  if (full) {
    uint64_t next = gc_heap.live * 2 + (4ull * 1024ull * 1024ull);
    if (next < (4ull * 1024ull * 1024ull)) {
      next = 4ull * 1024ull * 1024ull;
    }
    if (next > FULL_BYTES) {
      next = FULL_BYTES;
    }
    gc_heap.full_trigger = next;
  }
  if (gc_heap.ctr.yng.min == 0 || avant_young_bytes < gc_heap.ctr.yng.min) {
    gc_heap.ctr.yng.min = avant_young_bytes;
  }
  if (avant_young_bytes > gc_heap.ctr.yng.max) {
    gc_heap.ctr.yng.max = avant_young_bytes;
  }
  avant_since_gc = 0;
  avant_g_stats.live_bytes = gc_heap.live;
  if (gc_heap.live > gc_heap.ctr.peak_live) {
    gc_heap.ctr.peak_live = gc_heap.live;
  }
  gc_heap.collecting = 0;
}

void gc_maybe_collect(void) {
  if (gc_heap.collecting || gc_heap.defer) {
    return;
  }
  if (gc_heap.limit_pending) {
    gc_heap.limit_pending = 0;
    gc_collect(1);
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
  if (gc_heap.set.nursery_off) {
    if (avant_since_gc >= gc_heap.full_trigger) {
      gc_collect(1);
    }
    return;
  }
  /* Nursery garbage is reclaimed by a young collection. A full
   * collection runs when mature space, including promotions and large
   * objects, has grown by gc_heap.full_trigger. Counting every nursery byte here
   * marked a live tree that a young collection should have discarded. */
  if (gc_heap.mature_since_full >= gc_heap.full_trigger) {
    gc_collect(1);
  } else if (avant_nursery_held >= avant_young_bytes) {
    gc_collect(0);
  }
}

/* A stress point (AVANT_GC_STRESS=1). The caller holds gc_heap.world. Only
 * where a collection may run: not inside a defer region, not during a
 * collection, not in a shared program (no thread is stopped, D191), and
 * only with a young generation to collect. */
void gc_stress_point(void) {
  if (!gc_heap.set.stress || gc_heap.collecting || gc_heap.defer || gc_heap.set.nursery_off ||
      atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    return;
  }
  gc_heap.stress_now = 1;
  gc_collect(0);
  gc_heap.stress_now = 0;
}

void gc_init_slow(void) {
  if (gc_heap.inited) {
    return;
  }
  gc_heap.inited = 1;
  {
    uint64_t ymb = env_u64("AVANT_YOUNG_MB", 0);
    if (ymb) {
      avant_young_bytes = ymb * 1024ull * 1024ull;
      gc_heap.set.young_adapt = 0;
    } else {
      avant_young_bytes = YOUNG_BYTES_DEFAULT;
      gc_heap.set.young_adapt = 1;
    }
  }
  uint64_t mb = env_u64("AVANT_HEAP_MAX_MB", 0);
  gc_heap.set.heap_max = mb ? mb * 1024ull * 1024ull : 0;
  {
    const char *v = getenv("AVANT_NURSERY");
    /* Copying nursery is the product default. Set to 0 for Stage 7
     * full-collect (non-moving) diagnosis. */
    if (v && v[0] == '0' && v[1] == 0) {
      gc_heap.set.nursery_off = 1;
    } else {
      gc_heap.set.nursery_off = 0;
    }
  }
  {
    const char *v = getenv("AVANT_NURSERY_DISCARD");
    /* Product path discards unpinned from-space after young copy.
     * Set to 0 to park (diagnostic). */
    if (v && v[0] == '0' && v[1] == 0) {
      gc_heap.set.discard_from = 0;
    } else {
      gc_heap.set.discard_from = 1;
    }
  }
  {
    /* The conservative C-stack pass after a young copy (avant_gc_stack.c,
     * W8c, W11): on with the copying nursery. It rewrites forwarded
     * words left on the collecting thread's stack and counts any that
     * still name from-space (sm_left). AVANT_GC_STACK_REWRITE=0 turns
     * the rewrite off and keeps the count. Read once, here. */
    const char *v = getenv("AVANT_GC_STACK_REWRITE");
    gc_heap.set.c_stack_pass = !gc_heap.set.nursery_off;
    gc_heap.set.c_stack_rewrite = gc_heap.set.c_stack_pass && !(v && v[0] == '0' && v[1] == 0);
  }
  {
    const char *v = getenv("AVANT_GC_REM_FRONTIER");
    gc_heap.set.rem_frontier = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_DEFRAG");
    gc_heap.set.defrag = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_YOUNG_SIZING");
    gc_heap.set.young_sizing = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  {
    const char *v = getenv("AVANT_GC_COPY_RESERVE");
    gc_heap.set.copy_reserve = (v && v[0] == '0' && v[1] == 0) ? 0 : 1;
  }
  gc_heap.set.verify = getenv("AVANT_GC_VERIFY") != NULL;
  {
    const char *v = getenv("AVANT_GC_STRESS");
    gc_heap.set.stress = v && v[0] == '1' && v[1] == 0;
  }
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
  pthread_mutex_lock(&gc_heap.world);
  if (!gc_heap.set.nursery_off && !gc_heap.collecting && !gc_heap.defer &&
      (gc_heap.mature_since_full >= gc_heap.full_trigger ||
       avant_g_stats.allocated_bytes - gc_heap.last_full_at >=
           (gc_heap.live > (4ull * 1024ull * 1024ull)
                ? gc_heap.live
                : (4ull * 1024ull * 1024ull)))) {
    int defrag_saved = gc_heap.set.defrag;
    gc_heap.set.defrag = 0;
    gc_collect(1);
    gc_heap.set.defrag = defrag_saved;
  }
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_nursery_reclaim(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
  if (!gc_heap.set.nursery_off && !gc_heap.collecting && avant_nursery_held > 0) {
    gc_collect(0);
  }
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_gc_collect(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
  gc_collect(1);
  pthread_mutex_unlock(&gc_heap.world);
}
