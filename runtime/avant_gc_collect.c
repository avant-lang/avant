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

void gc_limit_collect_pending(void) {
  gc_heap.limit_pending = 1;
  gc_fast_clear();
}

uint64_t avant_young_bytes;

uint64_t avant_nursery_held;

/* Young sizing (Stage 28 W15a1, gc-design.md §5.2; O28 done in W14b1).
 * The young budget starts at YOUNG_BYTES_DEFAULT (4 MiB since Stage 28
 * W15d, Q3; 8 MiB before) and adapts within [floor, YOUNG_BYTES_CAP]
 * after every young collection the policy runs (young_size, below: the
 * floor follows the heap since Stage 28 W15e).
 * AVANT_YOUNG_MB fixes it (tests). Retired with this rule: the survival
 * probe (a second trace of the live young graph before each copy), the
 * sticky pause that raised the budget instead of copying, the dead-based
 * resize and the 192 MiB cap. (E's copy reserve, the last young
 * collection's survival times 1.5 charged as spent by heap-limit
 * decisions, went with E in Stage 28 W15d: S+'s copy needs no room.) */
/* The cap (Stage 28 W15e): 32 MiB, was 64. S+ evacuates up to one budget
 * of survivors per collection (O92), so a budget of B can hold about 2 B
 * above the live data at a collection; at 64 MiB the compiler's
 * __emit-ll peak_heap rose to 311 MB once the budget grew without delay
 * (242 MB at 32 MiB; W15e record, vsuite/c2h16, c2h16c32). */
#ifndef YOUNG_BYTES_CAP
#define YOUNG_BYTES_CAP (32ull * 1024ull * 1024ull)
#endif
/* The floor's divisor (Stage 28 W15e): the budget never shrinks below
 * the heap in use / YOUNG_HEAP_DIV (nor below YOUNG_BYTES_DEFAULT), so a
 * program with a large heap is not collected every 4 MiB when a young
 * collection's fixed cost grows with the heap (the verifier's trace of
 * the whole heap at every young collection under AVANT_GC_VERIFY=1: the
 * compiler's dump of itself in test_fixed_point.av, 21 s at 4 MiB, 15 s
 * at 12; W15e record, vsuite/). It costs at most 1/12 of the heap. */
#ifndef YOUNG_HEAP_DIV
#define YOUNG_HEAP_DIV 12
#endif

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

/* Knobs retired by Stage 28 W14b2 (gc-design.md §1.7), and
 * AVANT_GC_STACK_REWRITE by W16b (§4.3): setting one is reported, and it
 * has no effect. */
static const char *const retired_knobs[] = {
    "AVANT_NURSERY_DISCARD", "AVANT_GC_REM_FRONTIER", "AVANT_GC_YOUNG_SIZING",
    "AVANT_GC_COPY_RESERVE", "AVANT_GC_STACK_REWRITE"};

static void gc_settings_read(GcSettings *s) {
  size_t i;
  /* Sizes in MiB stay below 2^44 so the byte count fits (1 << 20 GiB). */
  uint64_t mb_max = 1ull << 30;
  s->young_fixed = env_u64("AVANT_YOUNG_MB", 0, 0, mb_max) * 1024ull * 1024ull;
  s->heap_max = env_u64("AVANT_HEAP_MAX_MB", 0, 0, mb_max) * 1024ull * 1024ull;
  s->reserve_gb = env_u64("AVANT_HEAP_RESERVE_GB", HEAP_RESERVE_GB_DEFAULT, HEAP_RESERVE_GB_MIN,
                          1ull << 20);
  s->nursery_off = !env_bool("AVANT_NURSERY", 1);
  s->defrag = env_bool("AVANT_GC_DEFRAG", 1);
  s->verify = env_bool("AVANT_GC_VERIFY", 0);
  s->stats = env_bool("AVANT_GC_STATS", 0);
  s->stress = env_bool("AVANT_GC_STRESS", 0);
  s->stall_ms = env_u64("AVANT_GC_VERIFY_STALL_MS", 10000, 0, 1ull << 40);
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
 * SURV surviving ones (Stage 28 W15e, O96). The budget doubles, up to
 * YOUNG_BYTES_CAP, at the second consecutive young collection where more
 * than half the young bytes survived, and halves back toward its floor
 * when less than a tenth survived. The floor is the larger of
 * YOUNG_BYTES_DEFAULT and the heap in use (mature blocks and large
 * objects) / YOUNG_HEAP_DIV; the budget is raised to it at once.
 * Before W15e the doubling waited for a high-survival collection whose
 * pause per surviving byte rose against the previous one: a wall-clock
 * reading, so the budget's path, and with it every collection count,
 * changed from run to run. Growing at the first high-survival collection
 * (O96 as filed) was measured and rejected: it does not help the suite
 * (44.6 s against 42.3) and raises the compiler's peak_heap (245 ->
 * 318 MB; W15e record, vsuite/g50s2). */
static void young_size(uint64_t used, uint64_t surv) {
  uint64_t floor = YOUNG_BYTES_DEFAULT;
  uint64_t h = (gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes) / YOUNG_HEAP_DIV;
  int high;
  if (used == 0) {
    return;
  }
  if (h > floor) {
    floor = h > YOUNG_BYTES_CAP ? YOUNG_BYTES_CAP : h;
  }
  high = surv * 2 > used;
  if (high) {
    if (gc_heap.young_high_prev && avant_young_bytes < YOUNG_BYTES_CAP) {
      avant_young_bytes = avant_young_bytes * 2 > YOUNG_BYTES_CAP ? YOUNG_BYTES_CAP
                                                                 : avant_young_bytes * 2;
      gc_heap.ctr.size.grows++;
    }
  } else if (surv * 10 < used && avant_young_bytes > floor) {
    avant_young_bytes = avant_young_bytes / 2 < floor ? floor : avant_young_bytes / 2;
    gc_heap.ctr.size.shrinks++;
  }
  if (avant_young_bytes < floor) {
    avant_young_bytes = floor;
  }
  gc_heap.young_high_prev = (uint8_t)high;
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

/* C39 (Stage 28 W15a2): under AVANT_HEAP_MAX_MB a live set the heap
 * cannot hold with its copy room made every allocation run a heap-limit
 * full collection (limit_defer.av at 8 to 16 MB under E: a young
 * collection did not fit its copy room, and a full collection moves no
 * young object, so the young bytes stayed and the next allocation asked
 * again; 24 bytes allocated per full collection), so the program neither
 * died nor finished. The rule: a full collection the heap limit caused after less
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

static int region_refuses(void);

void gc_collect(GcKind kind, int allow_move, GcWhy why) {
  int full = kind == GC_FULL;
  int resume = 0;
  uint64_t t0;
  if (gc_thread.collecting) {
    gc_die("GC reentered");
  }
  /* RC4: only the heap limit's last resort (place, avant_gc_alloc.c, when
   * even an over-limit block cannot be had) reaches here inside a no-GC
   * region; under AVANT_GC_VERIFY=1 that dies naming the region. */
  if (gc_thread.nogc && gc_heap.set.verify) {
    (void)region_refuses();
  }
  if (!full && !allow_move) {
    gc_die("a young collection always moves");
  }
  /* The handshake (Stage 28 W17a, gc-design.md section 4.4): the world
   * lock is released (rule 1) while this thread raises the request and
   * waits for every other registered thread to stop at a safepoint or be
   * native, then taken again. When another thread held the request, this
   * one stopped until that collection was done; a collection the policy
   * asked for (the budget, the trigger, a stress point) is then
   * reconsidered, an explicit or heap-limit one runs. */
  if (!gc_thread.collector) {
    for (;;) {
      int mine;
      pthread_mutex_unlock(&gc_heap.world);
      mine = gc_stop_world();
      pthread_mutex_lock(&gc_heap.world);
      if (mine) {
        break;
      }
      if (why == GC_WHY_STRESS || (why == GC_WHY_BUDGET && avant_nursery_held < avant_young_bytes) ||
          (why == GC_WHY_TRIGGER && !gc_heap.set.nursery_off && gc_heap.mature_since_full < gc_heap.full_trigger) ||
          (why == GC_WHY_TRIGGER && gc_heap.set.nursery_off && avant_since_gc < gc_heap.full_trigger)) {
        return;
      }
    }
    resume = 1;
  }
  t0 = now_ns();
  gc_thread.collecting = 1;
  atomic_fetch_add_explicit(&g_avant_str_gen, 1u, memory_order_release);
  avant_g_stats.collections++;
  /* Every thread's remembered-set buffer joins the set (Stage 28 W17b)
   * before anything reads it. */
  gc_rem_merge_all();
  if (full) {
    count_full_why(why);
  }
  if (gc_heap.set.verify) {
    gc_quarantine_release();
    gc_verify_begin(full);
    if (!full) {
      gc_verify_remembered();
    } else {
      gc_verify_lost();
    }
  }
  gc_heap.young_clock += avant_nursery_held > gc_heap.young_carried
                              ? avant_nursery_held - gc_heap.young_carried
                              : 0;
  gc_heap.live = 0;
  gc_heap.work_n = 0;
  /* Every thread's allocation cursors and inline state (Stage 28 W17b:
   * per thread; the young cursor was cleared when the collection ended
   * before, which no allocation in between could tell apart). */
  gc_alloc_stop_all();

  if (!full) {
    /* One young collection: no probe, no pause. Survival, for sizing,
     * is what the collection copied and what it promoted in place
     * (sticky.inplace): the work a survivor costs. Young large objects
     * it reached are not counted: they neither move nor cost a copy
     * (counted, the reached grids of Etc::GameOfLife doubled the budget
     * to 32 MiB for no pause saved). A stress point and a fixed budget
     * make no sizing decision, and a fixed budget tenures every survivor
     * as before. */
    uint64_t used = avant_nursery_held;
    uint64_t copied0 = avant_g_stats.copied_bytes;
    uint64_t inplace0 = gc_heap.ctr.sticky.inplace;
    uint64_t surv;
    avant_g_stats.young_collections++;
    gc_sticky_collect(gc_heap.set.young_fixed ? 1 : 0);
    surv = (avant_g_stats.copied_bytes - copied0) + (gc_heap.ctr.sticky.inplace - inplace0);
    if (!gc_heap.set.young_fixed && !gc_heap.stress_now) {
      young_size(used, surv);
    }
  } else {
    avant_g_stats.full_collections++;
    gc_heap.return_clock++;
    /* Non-moving for young objects: they are promoted where they lie
     * (defragmentation may move mature blocks, below). */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_MATURE);
    gc_sticky_full_begin();
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
    /* A full collection promotes every object it marks (the mark clears
     * YOUNG), so no object is young after it: gc_sticky_full_end empties
     * the young budget (avant_nursery_held). (Before Stage 28 W15d E's
     * nursery survivors were summed here, O28.) */
    gc_sticky_full_end();
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
  gc_heap.young_carried = avant_nursery_held;
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
  if (gc_heap.quarantine) {
    gc_quarantine_seal();
  }
  gc_thread.collecting = 0;
  if (resume) {
    gc_resume_world();
  }
}

void gc_maybe_collect(void) {
  /* A hold (std.gc) defers what falls due to its end, as a region does
   * to its first collection point after it. */
  if (gc_thread.collecting || gc_thread.nogc || gc_thread.hold) {
    return;
  }
  if (gc_heap.explicit_pending) {
    /* avant_gc_collect asked inside a no-GC region (RC4). */
    gc_heap.explicit_pending = 0;
    gc_collect(GC_FULL, 1, GC_WHY_EXPLICIT);
    return;
  }
  if (gc_heap.limit_pending) {
    gc_heap.limit_pending = 0;
    gc_collect(GC_FULL, 1, GC_WHY_LIMIT);
    return;
  }
  /* Only a no-GC region (RC4, avant_gc.h) or a running collection holds
   * a request off (above); since Stage 28 W16a runtime helpers root what
   * they hold (RC1) and allocate outside regions, so their allocations
   * are collection points like generated code's (O20, O46). */
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
    /* (Under E a young copy not expected to fit under AVANT_HEAP_MAX_MB,
     * young_copy_fits, ran as a full collection; S+'s copy needs no room:
     * it stops at the limit and promotes the rest in place.) */
    gc_collect(GC_YOUNG, 1, GC_WHY_BUDGET);
  }
}

/* A stress point (AVANT_GC_STRESS=1). The caller holds gc_heap.world. Only
 * where a collection may run: not inside a defer region, not during a
 * collection, and only with a young generation to collect. Shared
 * programs too since Stage 28 W17a (the handshake stops the other
 * threads; before, D191 kept the stress mode to one thread). */
void gc_stress_point(void) {
  if (!gc_heap.set.stress || gc_thread.collecting || gc_thread.nogc || gc_heap.set.nursery_off) {
    return;
  }
  gc_heap.stress_now = 1;
  gc_collect(GC_YOUNG, 1, GC_WHY_STRESS);
  gc_heap.stress_now = 0;
  /* The survivors a stress collection promotes are mature growth; a
   * stress point at a defer entry is often the only collection point a
   * helper-bound loop reaches (its allocations run inside the region,
   * where gc_maybe_collect returns), so the full collection they trigger
   * runs here. (Stage 28 W15a2.) */
  if (gc_heap.mature_since_full >= gc_heap.full_trigger) {
    gc_collect(GC_FULL, 1, GC_WHY_TRIGGER);
  }
}

void gc_init_slow(void) {
  if (gc_heap.inited) {
    return;
  }
  gc_heap.inited = 1;
  gc_settings_read(&gc_heap.set);
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
  /* After the statistics' handler, so it runs first (atexit is last in,
   * first out) and the report counts the exit's verification. */
  if (gc_heap.set.verify) {
    gc_verify_init();
  }
}

/* AVANT_GC_VERIFY at the program's exit (Stage 28 W18M item 7): the whole
 * heap is verified once more, as at the start of a collection, after the
 * same handshake (every other thread stopped at a safepoint or native).
 * Not inside a collection or a no-GC region, where the heap is not in a
 * state a collection could start from. */
void gc_verify_exit_run(void (*check)(void)) {
  int resume = 0;
  if (gc_thread.collecting || gc_thread.nogc) {
    return;
  }
  gc_lock(&gc_heap.world);
  if (!gc_thread.collector) {
    for (;;) {
      int mine;
      pthread_mutex_unlock(&gc_heap.world);
      mine = gc_stop_world();
      pthread_mutex_lock(&gc_heap.world);
      if (mine) {
        break;
      }
    }
    resume = 1;
  }
  gc_thread.collecting = 1;
  gc_rem_merge_all();
  check();
  gc_thread.collecting = 0;
  if (resume) {
    gc_resume_world();
  }
  pthread_mutex_unlock(&gc_heap.world);
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
  gc_safepoint();
  gc_lock(&gc_heap.world);
  gc_maybe_collect();
  pthread_mutex_unlock(&gc_heap.world);
}

/* The function a no-GC region was entered from, for a message WHAT: its
 * symbol when the image exports one, else the call site's offset in its
 * image (addr2line resolves it). */
static __attribute__((noreturn)) void region_die_at(const char *what, void *site) {
  char msg[512];
  Dl_info info;
  memset(&info, 0, sizeof info);
  if (site && dladdr(site, &info) && info.dli_sname) {
    snprintf(msg, sizeof msg, "%s (%s)", what, info.dli_sname);
  } else if (site && info.dli_fname && info.dli_fbase) {
    const char *base = strrchr(info.dli_fname, '/');
    snprintf(msg, sizeof msg, "%s (%s+0x%lx)", what, base ? base + 1 : info.dli_fname,
             (unsigned long)((uintptr_t)site - (uintptr_t)info.dli_fbase));
  } else {
    snprintf(msg, sizeof msg, "%s (called from %p)", what, site);
  }
  gc_die(msg);
}

static __attribute__((noreturn)) void region_die(const char *what) {
  region_die_at(what, gc_thread.region_site);
}

/* gc-design.md §5.4: an allocation inside a no-GC region proceeds past
 * AVANT_HEAP_MAX_MB (C32: the full collection runs after the region),
 * unless the region alone has allocated more than the limit: no
 * collection after it could bring the heap back under the limit. */
void gc_region_limit_check(uint64_t bytes) {
  if (gc_heap.set.heap_max && gc_thread.nogc && gc_thread.region_bytes + bytes > gc_heap.set.heap_max) {
    region_die("heap limit in a no-GC region");
  }
  /* A hold (std.gc, Stage 29 W1c2): the same rule over the hold's own
   * allocations (its inline ones included: the count since its entry). */
  if (gc_heap.set.heap_max && gc_thread.hold &&
      avant_g_stats.allocated_bytes - gc_thread.hold_base + bytes > gc_heap.set.heap_max) {
    region_die_at("heap limit in a no-GC region", gc_thread.hold_site);
  }
}

/* RC4 (avant_gc.h, Stage 28 W16a): a collection attempted inside a no-GC
 * region. Under AVANT_GC_VERIFY=1 it dies naming the region's function;
 * otherwise an explicit request is deferred to the first collection
 * point after the region (returns 1: the caller does not collect). */
static int region_refuses(void) {
  if (!gc_thread.nogc) {
    return 0;
  }
  if (gc_heap.set.verify) {
    region_die("collection inside a no-GC region");
  }
  return 1;
}

void avant_gc_collect_young(void) {
  gc_init();
  /* `gc.collect()` inside a hold (std.gc): deferred to the hold's end. */
  if (gc_thread.hold) {
    gc_thread.hold_young = 1;
    return;
  }
  gc_safepoint();
  gc_lock(&gc_heap.world);
  /* A young collection asked for inside a no-GC region is dropped (the
   * budget still triggers one after it). */
  if (region_refuses()) {
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  if (!gc_heap.set.nursery_off && !gc_thread.collecting && avant_nursery_held > 0) {
    gc_collect(GC_YOUNG, 1, GC_WHY_EXPLICIT);
  }
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_gc_collect(void) {
  gc_init();
  gc_safepoint();
  gc_lock(&gc_heap.world);
  if (gc_thread.hold && !gc_thread.nogc) {
    /* Inside a hold (std.gc): at the hold's end. */
    gc_heap.explicit_pending = 1;
    gc_fast_clear();
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  if (region_refuses()) {
    gc_heap.explicit_pending = 1;
    gc_fast_clear();
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  gc_collect(GC_FULL, 1, GC_WHY_EXPLICIT);
  pthread_mutex_unlock(&gc_heap.world);
}

/* `gc.hold { }` (std.gc, Stage 29 W1c2, D305): GcThread.hold's comment.
 * The compiler brackets the block with these two and ends the hold on
 * every way out of it (the end of the block, return, break, continue,
 * a `?` that returns). Under AVANT_GC_STRESS=1 a hold is inactive, so a
 * stress run collects inside it as it would without it, and the hold
 * hides no missing root. */
void avant_gc_hold_begin(void) {
  gc_init();
  if (gc_heap.set.stress) {
    return;
  }
  if (gc_thread.hold++ == 0) {
    gc_thread.hold_base = avant_g_stats.allocated_bytes;
    gc_thread.hold_site = __builtin_return_address(0);
    gc_thread.hold_young = 0;
  }
}

/* The outermost end: a collection point that runs what fell due inside,
 * a young collection `gc.collect()` asked for, then the policy's
 * (gc_maybe_collect: a pending explicit or heap-limit full collection,
 * the full trigger, the spent young budget). */
void avant_gc_hold_end(void) {
  int young;
  int shared;
  if (gc_thread.hold <= 0) {
    return;
  }
  if (--gc_thread.hold > 0) {
    return;
  }
  young = gc_thread.hold_young;
  gc_thread.hold_young = 0;
  gc_safepoint();
  gc_lock(&gc_heap.world);
  shared = atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0;
  if (shared) {
    gc_tl_settle(&gc_thread);
    gc_fast_clear();
  }
  if (!gc_thread.collecting && !gc_thread.nogc) {
    if (young && !gc_heap.explicit_pending && !gc_heap.limit_pending && !gc_heap.set.nursery_off &&
        avant_nursery_held > 0) {
      gc_collect(GC_YOUNG, 1, GC_WHY_EXPLICIT);
    }
    gc_maybe_collect();
  }
  pthread_mutex_unlock(&gc_heap.world);
}
