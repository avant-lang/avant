#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

/* The collection driver and its policy (Stage 28 W12, moved from
 * avant_rt.c): initialization from the environment (the settings,
 * gc_settings_read; the knobs are listed in avant_gc.h), collect (young
 * or full) and gc_maybe_collect (when),
 * the adaptive nursery sizing and the full-collection trigger, and the
 * public collection entry points. */

/* Nonzero while every named safepoint must take its slow path: under
 * AVANT_GC_STRESS=1 (set once by gc_init_slow), or while a heap-limit
 * collection is pending (C32). */
__attribute__((visibility("hidden"))) uint32_t avant_gc_safepoint_force;

void gc_limit_collect_pending(void) {
  gc_heap.limit_pending = 1;
  gc_fast_clear();
  avant_gc_safepoint_force = 1;
}

uint64_t avant_young_bytes;

uint64_t avant_nursery_held;

/* Young sizing (Stage 28 W15a1, gc-design.md §5.2; O28 done in W14b1).
 * The young budget starts at YOUNG_BYTES_DEFAULT (8 MiB; Q3 chooses the
 * default later) and adapts within [YOUNG_BYTES_MIN, YOUNG_BYTES_CAP]
 * after every young collection the policy runs (young_size): it doubles
 * when more than half of the young bytes survived and the pause per
 * surviving byte rose against the previous young collection, and halves
 * back toward the default when less than a tenth survived.
 * AVANT_YOUNG_MB fixes it (tests). Retired with this rule: the survival
 * probe (a second trace of the live young graph before each copy), the
 * sticky pause that raised the budget instead of copying, the dead-based
 * resize and the 192 MiB cap. The copy reserve is the last young
 * collection's survival times RESERVE_FACTOR, charged as already spent
 * by heap-limit decisions outside a collection so a copy has its room;
 * inside a collection the reserve is being spent and is not charged. */
#define YOUNG_BYTES_MIN (4ull * 1024ull * 1024ull)
#define YOUNG_BYTES_CAP (64ull * 1024ull * 1024ull)
#define RESERVE_FACTOR 1.5

/* The settings' one parsing rule (Stage 28 W14b2, O40; the knobs are
 * listed in avant_gc.h). A knob that is unset or empty takes its
 * default. A boolean knob is on for exactly "1" and off for exactly "0";
 * a number is decimal digits only, within the knob's range. Anything
 * else is reported once on stderr at start-up, and the default applies. */
static void env_warn(const char *name, const char *v, const char *want) {
  fprintf(stderr, "avant runtime: warning: %s=%s is not %s; the default applies\n", name, v,
          want);
}

static int env_bool(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !v[0]) {
    return fallback;
  }
  if (v[0] == '1' && v[1] == 0) {
    return 1;
  }
  if (v[0] == '0' && v[1] == 0) {
    return 0;
  }
  env_warn(name, v, "0 or 1");
  return fallback;
}

static uint64_t env_u64(const char *name, uint64_t fallback, uint64_t lo, uint64_t hi) {
  const char *v = getenv(name);
  const char *p;
  uint64_t n = 0;
  if (!v || !v[0]) {
    return fallback;
  }
  for (p = v; *p; p++) {
    if (*p < '0' || *p > '9' || n > (UINT64_MAX - 9) / 10) {
      break;
    }
    n = n * 10 + (uint64_t)(*p - '0');
  }
  if (*p || n < lo || n > hi) {
    char want[64];
    snprintf(want, sizeof want, "a number from %llu to %llu", (unsigned long long)lo,
             (unsigned long long)hi);
    env_warn(name, v, want);
    return fallback;
  }
  return n;
}

/* Knobs retired by Stage 28 W14b2 (gc-design.md §1.7): setting one is
 * reported, and it has no effect. */
static const char *const retired_knobs[] = {
    "AVANT_NURSERY_DISCARD", "AVANT_GC_REM_FRONTIER", "AVANT_GC_YOUNG_SIZING",
    "AVANT_GC_COPY_RESERVE"};

static void gc_settings_read(GcSettings *s) {
  size_t i;
  /* Sizes in MiB stay below 2^44 so the byte count fits (1 << 20 GiB). */
  uint64_t mb_max = 1ull << 30;
  s->young_fixed = env_u64("AVANT_YOUNG_MB", 0, 0, mb_max) * 1024ull * 1024ull;
  s->heap_max = env_u64("AVANT_HEAP_MAX_MB", 0, 0, mb_max) * 1024ull * 1024ull;
  s->reserve_gb = env_u64("AVANT_HEAP_RESERVE_GB", HEAP_RESERVE_GB_DEFAULT, HEAP_RESERVE_GB_MIN,
                          1ull << 20);
  s->nursery_off = !env_bool("AVANT_NURSERY", 1);
  s->c_stack_pass = !s->nursery_off;
  s->c_stack_rewrite = s->c_stack_pass && env_bool("AVANT_GC_STACK_REWRITE", 1);
  s->defrag = env_bool("AVANT_GC_DEFRAG", 1);
  s->verify = env_bool("AVANT_GC_VERIFY", 0);
  s->stats = env_bool("AVANT_GC_STATS", 0);
  s->stress = env_bool("AVANT_GC_STRESS", 0);
  for (i = 0; i < sizeof retired_knobs / sizeof retired_knobs[0]; i++) {
    const char *v = getenv(retired_knobs[i]);
    if (v && v[0]) {
      fprintf(stderr, "avant runtime: warning: %s is retired and has no effect\n",
              retired_knobs[i]);
    }
  }
}

_Atomic uint32_t g_avant_str_gen;

/* §5.2's rule, after a young collection that saw USED young bytes and
 * SURV surviving ones in PAUSE_NS. */
static void young_size(uint64_t used, uint64_t surv, uint64_t pause_ns) {
  double per_byte;
  if (used == 0) {
    return;
  }
  per_byte = surv ? (double)pause_ns / (double)surv : 0.0;
  if (surv * 2 > used) {
    if (gc_heap.young_per_byte > 0.0 && per_byte > gc_heap.young_per_byte &&
        avant_young_bytes < YOUNG_BYTES_CAP) {
      avant_young_bytes = avant_young_bytes * 2 > YOUNG_BYTES_CAP ? YOUNG_BYTES_CAP
                                                                 : avant_young_bytes * 2;
      gc_heap.ctr.size.grows++;
    }
  } else if (surv * 10 < used && avant_young_bytes > YOUNG_BYTES_DEFAULT) {
    avant_young_bytes = avant_young_bytes / 2 < YOUNG_BYTES_DEFAULT ? YOUNG_BYTES_DEFAULT
                                                                   : avant_young_bytes / 2;
    gc_heap.ctr.size.shrinks++;
  }
  gc_heap.young_per_byte = per_byte;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A full collection's cause (ctr.why). The trigger counts as promotion
 * when young collections promoted at least half of the mature growth
 * since the last full collection, as mature allocation otherwise. */
static void count_full_why(GcWhy why) {
  GcWhyCounters *c = &gc_heap.ctr.why;
  switch (why) {
  case GC_WHY_TRIGGER:
    if (gc_heap.promoted_since_full * 2 >= gc_heap.mature_since_full &&
        gc_heap.promoted_since_full > 0) {
      c->promotion++;
    } else {
      c->mature++;
    }
    break;
  case GC_WHY_LIMIT:
    c->limit++;
    break;
  case GC_WHY_HEAP_GOAL:
    c->heap_goal++;
    break;
  default:
    c->explicit_++;
    break;
  }
}

/* M6 (gc-design.md §3.4): after a full collection's mark, the free
 * lines of every partly used block of LIST, and all its lines. */
static void frag_scan(Block *list, uint64_t *free_lines, uint64_t *lines) {
  Block *b;
  for (b = list; b; b = b->next) {
    int live = gc_block_live_lines(b);
    *lines += LINES_PER_BLOCK;
    if (live > 0 && live < LINES_PER_BLOCK) {
      *free_lines += (uint64_t)(LINES_PER_BLOCK - live);
    }
  }
}

static void frag_note(void) {
  GcFragCounters *c = &gc_heap.ctr.frag;
  uint64_t free_lines = 0;
  uint64_t lines = 0;
  frag_scan(gc_heap.space[SPACE_MATURE].list, &free_lines, &lines);
  frag_scan(gc_heap.space[SPACE_NURSERY].list, &free_lines, &lines);
  frag_scan(gc_heap.space[SPACE_FROM].list, &free_lines, &lines);
  c->fulls++;
  c->free_lines += free_lines;
  c->lines += lines;
  if (lines) {
    double f = (double)free_lines / (double)lines;
    uint64_t ppm = (uint64_t)(f * 1e6);
    c->num += f * (double)gc_heap.heap_bytes;
    c->den += (double)gc_heap.heap_bytes;
    if (ppm > c->max_ppm) {
      c->max_ppm = ppm;
    }
  }
}

/* The young collection of the candidate GC_CANDIDATE selects (the one
 * internal switch, avant_gc_internal.h; gc-design.md §3.4). TENURE: every
 * survivor is promoted at this collection (a fixed AVANT_YOUNG_MB); S
 * promotes every survivor at every young collection anyway. */
static void gc_young_collect(int tenure) {
#if GC_CANDIDATE == GC_CANDIDATE_E
  gc_young_copy_out(tenure);
#else
  (void)tenure;
  gc_sticky_collect();
#endif
}

/* C39 (Stage 28 W15a2): under AVANT_HEAP_MAX_MB a live set the heap
 * cannot hold with its copy room made every allocation run a heap-limit
 * full collection (limit_defer.av at 8 to 16 MB: a young collection does
 * not fit, young_copy_fits, and a full collection moves no young object,
 * so the young bytes stay and the next allocation asks again; 24 bytes
 * allocated per full collection), so the program neither died nor
 * finished. The rule: a full collection the heap limit caused after less
 * than one block (BLOCK_SIZE) was allocated since the previous one is a
 * strike, and two strikes running end the program with "heap limit",
 * the death a refused allocation gives: the collector is running a full
 * collection per block, which is what the limit means. A heap-limit full
 * collection after at least one block of allocation clears the count, so
 * a program near its limit that still makes progress (limit_defer.av at
 * 20 MB: about 98 KB per full collection, done in 3.4 s) goes on. */
static void limit_progress_check(void) {
  uint64_t since = avant_g_stats.allocated_bytes - gc_heap.limit_full_at;
  gc_heap.limit_full_at = avant_g_stats.allocated_bytes;
  if (since >= BLOCK_SIZE) {
    gc_heap.limit_strikes = 0;
    return;
  }
  if (++gc_heap.limit_strikes >= 2) {
    gc_die("heap limit");
  }
}

void gc_collect(GcKind kind, int allow_move, GcWhy why) {
  int full = kind == GC_FULL;
  uint64_t t0 = now_ns();
  if (gc_heap.collecting) {
    gc_die("GC reentered");
  }
  if (!full && !allow_move) {
    gc_die("a young collection always moves");
  }
  gc_heap.collecting = 1;
  atomic_fetch_add_explicit(&g_avant_str_gen, 1u, memory_order_release);
  avant_g_stats.collections++;
  if (full) {
    count_full_why(why);
  }
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
   * copy's rewrite (gc_rewrite_c_stack) and the full collection's
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
    /* One young collection: no probe, no pause. Survival, for sizing
     * and for the copy reserve, is what the copy moved and the pinned
     * objects it kept in place: the work a survivor costs. Young large
     * objects it reached are not counted: they neither move nor cost a
     * copy (counted, the reached grids of Etc::GameOfLife doubled the
     * budget to 32 MiB for no pause saved). A stress point (always
     * copying) and a fixed budget make no sizing decision, and a fixed
     * budget tenures every survivor as before. */
    uint64_t used = avant_nursery_held;
    uint64_t copied0 = avant_g_stats.copied_bytes;
    uint64_t pinned0 = gc_heap.ctr.prom.pinned;
#if GC_CANDIDATE == GC_CANDIDATE_S
    uint64_t inplace0 = gc_heap.ctr.sticky.inplace;
#endif
    uint64_t y0 = now_ns();
    uint64_t surv;
    avant_g_stats.young_collections++;
    gc_young_collect(gc_heap.set.young_fixed ? 1 : 0);
    surv = (avant_g_stats.copied_bytes - copied0) + (gc_heap.ctr.prom.pinned - pinned0);
#if GC_CANDIDATE == GC_CANDIDATE_S
    /* Under S survival is what the collection promoted in place
     * (sticky.inplace; copied_bytes and prom.pinned stay 0), and the copy
     * reserve stays 0: S needs no copy room. */
    surv += gc_heap.ctr.sticky.inplace - inplace0;
#else
    gc_heap.copy_reserve = (uint64_t)(RESERVE_FACTOR * (double)surv);
#endif
    if (!gc_heap.set.young_fixed && !gc_heap.stress_now) {
      young_size(used, surv, now_ns() - y0);
    }
  } else {
    Block *b;
    avant_g_stats.full_collections++;
    /* Non-moving. Evacuating the nursery here needs a second copy of
     * every live object while the from-space blocks are still charged
     * against the heap limit. */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_NURSERY);
    gc_clear_alloc_cur(SPACE_MATURE);
#if GC_CANDIDATE == GC_CANDIDATE_S
    gc_sticky_full_begin();
#endif
    /* The marks: one memset per chunk of the side bitmap; no header is
     * touched (no young mark outlives its collection, C36). */
    gc_clear_side_marks();
    /* The line marks: the epoch advances (no walk zeroing them). */
    gc_line_epoch_advance();
    gc_mark_roots();
    frag_note();
    {
      Block *db;
      uint64_t live_lines_this = 0;
      for (db = gc_heap.space[SPACE_MATURE].list; db; db = db->next) {
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
      if (allow_move && gc_heap.set.defrag) {
        gc_note_c_stack_blocks();
        gc_defrag_evacuate(live_lines_this);
      }
      if (gc_heap.set.verify && gc_heap.def_release.n) {
        gc_verify_defrag();
      }
      gc_defrag_release();
    }
    /* Dead and clean remembered entries leave the set (C33, O9). */
    gc_rem_full_scrub();
    gc_sweep_large();
    gc_filter_blocks(&gc_heap.space[SPACE_MATURE].list, gc_keep_if_live);
    gc_filter_blocks(&gc_heap.space[SPACE_NURSERY].list, gc_keep_if_live);
    gc_filter_blocks(&gc_heap.space[SPACE_FROM].list, gc_keep_if_live);
    gc_heap.mark_sum = 0;
    /* O28: the young bytes that survived, nursery blocks only. A
     * from-space block's live objects are pinned survivors promoted in
     * place (D205): mature, not young. */
    for (b = gc_heap.space[SPACE_NURSERY].list; b; b = b->next) {
      gc_foreach_block_object(b, gc_sum_marked_one);
    }
    /* The one young budget: the nursery's survivors and the young large
     * objects, which stay young across a full collection (gc_sweep_large
     * recounted them; before Stage 28 W15a1 a full collection promoted
     * every large object). */
    avant_nursery_held = gc_heap.mark_sum + gc_heap.young_large;
#if GC_CANDIDATE == GC_CANDIDATE_S
    gc_sticky_full_end();
#endif
    gc_heap.mature_since_full = 0;
    gc_heap.promoted_since_full = 0;
    gc_heap.last_full_at = avant_g_stats.allocated_bytes;
    /* Free memory beyond the retention target goes back to the OS. */
    gc_heap_return(gc_heap.live);
    if (why == GC_WHY_LIMIT && gc_heap.set.heap_max) {
      limit_progress_check();
    }
  }

  if (full) {
    /* gc-design.md §5.4: max(4 MiB, (F - 1) * live), F = 2, uncapped. */
    uint64_t next = (uint64_t)(HEAP_FACTOR - 1) * gc_heap.live;
    gc_heap.full_trigger = next < FULL_TRIGGER_MIN ? FULL_TRIGGER_MIN : next;
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
  {
    uint64_t dt = now_ns() - t0;
    if (full) {
      gc_heap.ctr.pause.full_ns += dt;
    } else {
      gc_heap.ctr.pause.young_ns += dt;
    }
    if (dt > gc_heap.ctr.pause.max_ns) {
      gc_heap.ctr.pause.max_ns = dt;
    }
  }
  gc_heap.collecting = 0;
}

/* Under AVANT_HEAP_MAX_MB, whether a young copy is expected to fit: the
 * copy reserve (the last young collection's survival times 1.5; before
 * the first one, every nursery byte) on top of the heap stays under the
 * limit. The survival probe and the sticky pause made this decision
 * before (they are retired, Stage 28 W15a1); a copy not expected to fit
 * runs as a full collection instead, which moves no young object and
 * needs no room. A copy that outgrows the estimate proceeds past the
 * limit and leaves a full collection pending (place). */
static int young_copy_fits(void) {
  uint64_t copy;
#if GC_CANDIDATE == GC_CANDIDATE_S
  /* S moves nothing: a young collection needs no room (its copy reserve
   * stays 0, gc_collect). */
  return 1;
#endif
  if (!gc_heap.set.heap_max) {
    return 1;
  }
  copy = avant_nursery_held > gc_heap.young_large ? avant_nursery_held - gc_heap.young_large : 0;
  if (avant_g_stats.young_collections > 0 && gc_heap.copy_reserve < copy) {
    copy = gc_heap.copy_reserve;
  }
  return gc_heap.heap_bytes + copy <= gc_heap.set.heap_max;
}

void gc_maybe_collect(void) {
  if (gc_heap.collecting || gc_heap.defer) {
    return;
  }
  if (gc_heap.limit_pending) {
    gc_heap.limit_pending = 0;
    avant_gc_safepoint_force = gc_heap.set.stress ? 1u : 0u;
    gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
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
      gc_collect(GC_FULL, 1, GC_WHY_TRIGGER);
    }
    return;
  }
  /* Nursery garbage is reclaimed by a young collection. A full
   * collection runs when mature space, including promotions and large
   * objects, has grown by gc_heap.full_trigger. Counting every nursery byte here
   * marked a live tree that a young collection should have discarded. */
  if (gc_heap.mature_since_full >= gc_heap.full_trigger) {
    gc_collect(GC_FULL, 1, GC_WHY_TRIGGER);
  } else if (avant_nursery_held >= avant_young_bytes) {
    if (young_copy_fits()) {
      gc_collect(GC_YOUNG, 1, GC_WHY_BUDGET);
    } else {
      gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
    }
  }
}

/* A stress point (AVANT_GC_STRESS=1). The caller holds gc_heap.world. Only
 * where a collection may run: not inside a defer region, not during a
 * collection, not in a shared program (no thread is stopped, D191), and
 * only with a young generation to collect. */
void gc_stress_point(void) {
  if (!gc_heap.set.stress || gc_heap.collecting || gc_heap.defer || gc_heap.set.nursery_off ||
      atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0 || !young_copy_fits()) {
    return;
  }
  gc_heap.stress_now = 1;
  gc_collect(GC_YOUNG, 1, GC_WHY_STRESS);
  gc_heap.stress_now = 0;
  /* The blocks a stress collection retains for the objects the C stack
   * names (gc_pin_c_stack) are mature growth; a stress point at a defer
   * entry is often the only collection point a helper-bound loop reaches
   * (its allocations run inside the region, where gc_maybe_collect
   * returns), so the full collection they trigger runs here. A full
   * collection moves no young object and keeps every mature block this
   * thread's stack names (C31). (Stage 28 W15a2.) */
  if (gc_heap.mature_since_full >= gc_heap.full_trigger) {
    gc_collect(GC_FULL, 1, GC_WHY_TRIGGER);
  }
}

/* A named safepoint's slow path (avant_gc.h; Stage 28 W15a1, gc-design.md
 * §5.1). ROOTS holds the helper's heap reference arguments; they are the
 * slots of a frame pushed for the collection, so a young collection
 * relocates them (interior addresses included) and a defragmenting full
 * one rewrites them, and the caller's macro reads them back. It is a
 * collection runtime C entered (gc_thread.c_entry, Stage 28 W15a2): any
 * other young object a word of this thread's C stack names is kept in
 * place (gc_pin_c_stack), never rewritten. A collection runs only
 * in an unshared program, outside every defer region, collection and
 * quiet leaf, on a registered thread: under AVANT_GC_STRESS=1 a young
 * collection that always copies, then whatever the policy asks for
 * (gc_maybe_collect: the young budget, the full trigger, a pending
 * heap-limit collection). */
#define GC_SAFEPOINT_MAX 3
__attribute__((visibility("hidden"))) void avant_gc_safepoint_slow(void **roots, uint32_t n) {
  void **slots[GC_SAFEPOINT_MAX];
  Frame f;
  uint32_t i;
  gc_init();
  if (n > GC_SAFEPOINT_MAX) {
    gc_die("too many safepoint roots");
  }
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0 || gc_heap.defer ||
      gc_heap.collecting || gc_thread.quiet || !gc_thread.reg.head) {
    return;
  }
  for (i = 0; i < n; i++) {
    slots[i] = &roots[i];
  }
  f.prev = gc_thread.frame;
  f.slots = slots;
  f.n = n;
  f.cap = n;
  gc_thread.frame = &f;
  /* A collection runtime C entered (Stage 28 W15a2): the young objects
   * words of this thread's C stack name are kept in place
   * (gc_pin_c_stack); the helper's arguments are this frame's slots and
   * are relocated precisely. */
  gc_thread.c_entry++;
  pthread_mutex_lock(&gc_heap.world);
  gc_heap.ctr.sp.calls++;
  if (gc_heap.set.stress) {
    gc_stress_point();
  }
  {
    uint64_t before = avant_g_stats.collections;
    gc_maybe_collect();
    if (avant_g_stats.collections != before) {
      gc_heap.ctr.sp.collections++;
    }
  }
  pthread_mutex_unlock(&gc_heap.world);
  gc_thread.c_entry--;
  gc_thread.frame = f.prev;
}

void gc_init_slow(void) {
  if (gc_heap.inited) {
    return;
  }
  gc_heap.inited = 1;
  gc_settings_read(&gc_heap.set);
  if (gc_heap.set.stress) {
    avant_gc_safepoint_force = 1;
  }
  if (gc_heap.set.young_fixed) {
    avant_young_bytes = gc_heap.set.young_fixed;
  } else {
    avant_young_bytes = YOUNG_BYTES_DEFAULT;
  }
  /* The heap's address range (W14a1): reserved once, here, so a refusal
   * is met at start-up (halving down to 1 GiB) and not at a block. */
  gc_heap_reserve();
  if (gc_heap.set.stats) {
    atexit(gc_print_stats);
  }
}

/* Q128's heap-goal collection at the HTTP boundary (the request path's
 * allocations all pass through C helpers under defer). Since Stage 28
 * W15a1 (gc-design.md §5.4) it is an ordinary collection request: the
 * policy runs what is due (gc_maybe_collect: the young budget, which
 * counts the helpers' allocations, the full trigger, a pending
 * heap-limit collection), as at any allocation's slow path. Before, it
 * ran a non-moving full collection per allocation budget of its own
 * (the larger of the live bytes and 4 MB). Callers are at a boundary
 * where their C frames hold no heap pointers yet (the HTTP dispatch,
 * before it materializes the request). */
void avant_gc_heap_goal_collect(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
  gc_maybe_collect();
  pthread_mutex_unlock(&gc_heap.world);
}

/* gc-design.md §5.4: an allocation inside a no-GC region proceeds past
 * AVANT_HEAP_MAX_MB (C32: the full collection runs after the region),
 * unless the region alone has allocated more than the limit: no
 * collection after it could bring the heap back under the limit. */
void gc_region_limit_check(uint64_t bytes) {
  if (gc_heap.set.heap_max && gc_heap.region_bytes + bytes > gc_heap.set.heap_max) {
    /* The helper is named by its symbol when the image exports one, else
     * by the call site's offset in its image (addr2line resolves it). */
    char msg[512];
    Dl_info info;
    memset(&info, 0, sizeof info);
    if (gc_heap.region_site && dladdr(gc_heap.region_site, &info) && info.dli_sname) {
      snprintf(msg, sizeof msg, "heap limit in a no-GC region (%s)", info.dli_sname);
    } else if (gc_heap.region_site && info.dli_fname && info.dli_fbase) {
      const char *base = strrchr(info.dli_fname, '/');
      snprintf(msg, sizeof msg, "heap limit in a no-GC region (%s+0x%lx)",
               base ? base + 1 : info.dli_fname,
               (unsigned long)((uintptr_t)gc_heap.region_site - (uintptr_t)info.dli_fbase));
    } else {
      snprintf(msg, sizeof msg, "heap limit in a no-GC region (called from %p)",
               gc_heap.region_site);
    }
    gc_die(msg);
  }
}

void avant_nursery_reclaim(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
  if (!gc_heap.set.nursery_off && !gc_heap.collecting && avant_nursery_held > 0) {
    gc_collect(young_copy_fits() ? GC_YOUNG : GC_FULL, 1, GC_WHY_EXPLICIT);
  }
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_gc_collect(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
  gc_collect(GC_FULL, 1, GC_WHY_EXPLICIT);
  pthread_mutex_unlock(&gc_heap.world);
}
