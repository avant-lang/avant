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
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

/* The collection driver and its policy (moved from
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

/* Young sizing.
 * The young budget starts at YOUNG_BYTES_DEFAULT (4 MiB; 8 MiB once) and
 * adapts within [floor, YOUNG_BYTES_CAP]
 * after every young collection the policy runs (young_size, below: the
 * floor follows the heap).
 * AVANT_YOUNG_MB fixes it (tests). Retired with this rule: the survival
 * probe (a second trace of the live young graph before each copy), the
 * sticky pause that raised the budget instead of copying, the dead-based
 * resize and the 192 MiB cap. (E's copy reserve, the last young
 * collection's survival times 1.5 charged as spent by heap-limit
 * decisions, went with E: S+'s copy needs no room.) */
/* The cap: 32 MiB, was 64. S+ evacuates up to one budget
 * of survivors per collection, so a budget of B can hold about 2 B
 * above the live data at a collection; at 64 MiB the compiler's
 * __emit-ll peak_heap rose to 311 MB once the budget grew without delay
 * (242 MB at 32 MiB). */
#ifndef YOUNG_BYTES_CAP
#define YOUNG_BYTES_CAP (32ull * 1024ull * 1024ull)
#endif
/* The floor's divisor: the budget never shrinks below
 * the heap in use / YOUNG_HEAP_DIV (nor below YOUNG_BYTES_DEFAULT), so a
 * program with a large heap is not collected every 4 MiB when a young
 * collection's fixed cost grows with the heap (the verifier's trace of
 * the whole heap at every young collection under AVANT_GC_VERIFY=1: the
 * compiler's dump of itself in test_fixed_point.av, 21 s at 4 MiB, 15 s
 * at 12). It costs at most 1/12 of the heap. */
#ifndef YOUNG_HEAP_DIV
#define YOUNG_HEAP_DIV 12
#endif
/* The survival that grows the budget: more than YOUNG_GROW_PCT percent
 * of a collection's young bytes, at two budget collections in a row
 * (young_size). 70 by measurement on the 64 LangArena programs (50, the
 * half the rule used before, 60, 65, 70, 75, 80 and 90): at 50 the
 * budget doubled to the cap in programs whose survival stays between
 * half and two thirds while their live set grows, and the budget then
 * held their peak (Graph::BFS / DFS / AStar 152 / 142 / 157 MB at 50,
 * 135 / 127 / 139 MB at 70; Calculator::Interpreter 61 -> 44 MB, its
 * budget 32 -> 16 MiB); 60 and 65 still grew it for the Graph rows
 * (157 / 150 / 156 MB); from 75 Etc::GameOfLife, whose grids survive at
 * about three quarters, stopped growing it and copied more (+2.4%
 * instructions, 45 -> 47 MB). The half stays the high-survival test of
 * the tenured phase and of dense blocks (young_high_prev). */
#ifndef YOUNG_GROW_PCT
#define YOUNG_GROW_PCT 70
#endif

/* The settings' one parsing rule (the knobs are
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

/* Retired knobs, and AVANT_GC_STACK_REWRITE with the conservative C-stack
 * scans: setting one is reported, and it has no effect. */
static const char *const retired_knobs[] = {
    "AVANT_NURSERY_DISCARD", "AVANT_GC_REM_FRONTIER", "AVANT_GC_YOUNG_SIZING",
    "AVANT_GC_COPY_RESERVE", "AVANT_GC_STACK_REWRITE"};

#ifdef AVANT_GC_TUNE
static void gc_tune_read(void);
#endif

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
  s->proof = env_bool("AVANT_GC_PROOF", 1);
  s->verify = env_bool("AVANT_GC_VERIFY", 0);
  s->stats = env_bool("AVANT_GC_STATS", 0);
  s->stress = env_bool("AVANT_GC_STRESS", 0);
  s->walk = s->stats | s->verify | s->stress;
  s->stall_ms = env_u64("AVANT_GC_VERIFY_STALL_MS", 10000, 0, 1ull << 40);
#ifdef AVANT_GC_TUNE
  gc_tune_read();
#endif
  for (i = 0; i < sizeof retired_knobs / sizeof retired_knobs[0]; i++) {
    const char *v = getenv(retired_knobs[i]);
    if (v && v[0]) {
      fprintf(stderr, "avant runtime: warning: %s is retired and has no effect\n",
              retired_knobs[i]);
    }
  }
}

#ifdef AVANT_GC_TUNE
GcTune gc_tune;

/* The development build's policy settings (GcTune, avant_gc_internal.h),
 * each defaulting to the release build's constant. */
static void gc_tune_read(void) {
  uint64_t kb = 1ull << 30;
  gc_tune.dflt = env_u64("AVANT_GC_TUNE_DEFAULT_KB", YOUNG_BYTES_DEFAULT / 1024, 64, kb) * 1024ull;
  gc_tune.start = env_u64("AVANT_GC_TUNE_START_KB", YOUNG_BYTES_START / 1024, 64, kb) * 1024ull;
  gc_tune.cap = env_u64("AVANT_GC_TUNE_CAP_KB", YOUNG_BYTES_CAP / 1024, 64, kb) * 1024ull;
  if (gc_tune.cap < gc_tune.dflt) {
    gc_tune.cap = gc_tune.dflt;
  }
  gc_tune.heap_div = env_u64("AVANT_GC_TUNE_HEAP_DIV", YOUNG_HEAP_DIV, 1, 1u << 20);
  gc_tune.high_pct = env_u64("AVANT_GC_TUNE_HIGH_PCT", YOUNG_GROW_PCT, 0, 100);
  gc_tune.low_pct = env_u64("AVANT_GC_TUNE_LOW_PCT", 10, 0, 100);
  gc_tune.streak = env_u64("AVANT_GC_TUNE_STREAK", 2, 1, 64);
  gc_tune.grow_pct = env_u64("AVANT_GC_TUNE_GROW_PCT", 200, 101, 1600);
  gc_tune.shrink_pct = env_u64("AVANT_GC_TUNE_SHRINK_PCT", 50, 1, 99);
  gc_tune.smooth = env_u64("AVANT_GC_TUNE_SMOOTH", 0, 0, 64);
  gc_tune.large_age = env_u64("AVANT_GC_TUNE_LARGE_AGE_KB", LARGE_AGE_BYTES / 1024, 0, kb) * 1024ull;
  gc_tune.byte_age = env_u64("AVANT_GC_TUNE_BYTE_AGE_KB", 0, 0, kb) * 1024ull;
  gc_tune.no_holes = env_u64("AVANT_GC_TUNE_NO_HOLES", 0, 0, 2);
  gc_tune.ret = env_u64("AVANT_GC_TUNE_RETURN", 1, 0, 1);
  gc_tune.keep_full = env_u64("AVANT_GC_TUNE_KEEP_FULL", 2, 0, 3);
  gc_tune.plain_div = env_u64("AVANT_GC_TUNE_PLAIN_DIV", PLAIN_LARGE_DIV, 1, 1u << 20);
  gc_tune.full_pct = env_u64("AVANT_GC_TUNE_FULL_PCT", (HEAP_FACTOR - 1) * 100, 1, 10000);
  gc_tune.mixed_age = env_u64("AVANT_GC_TUNE_MIXED_AGE", 0, 0, LINES_PER_BLOCK);
  gc_tune.trace = env_u64("AVANT_GC_TUNE_TRACE", 0, 0, 1);
  gc_tune.retain_young = env_u64("AVANT_GC_TUNE_RETAIN_YOUNG", 1, 0, 1);
  gc_tune.mixed_evac = env_u64("AVANT_GC_TUNE_MIXED_EVAC", MIXED_EVAC_LINES, 0, LINES_PER_BLOCK);
  gc_tune.alloc_pct = env_u64("AVANT_GC_TUNE_ALLOC_PCT", ALLOC_FULL_PCT, 0, 1u << 20);
  gc_tune.alloc_min = env_u64("AVANT_GC_TUNE_ALLOC_MIN_KB", ALLOC_FULL_MIN / 1024, 0, kb) * 1024ull;
  gc_tune.alloc_base = env_u64("AVANT_GC_TUNE_ALLOC_BASE", 0, 0, 1);
  gc_tune.alloc_grown = env_u64("AVANT_GC_TUNE_ALLOC_GROWN_PCT", ALLOC_FULL_GROWN_PCT, 0, 100);
  gc_tune.start_prom = env_u64("AVANT_GC_TUNE_START_PROM_KB", START_PROM_BYTES / 1024, 0, kb) * 1024ull;
  gc_tune.start_large = env_u64("AVANT_GC_TUNE_START_LARGE", 1, 0, 1);
  gc_tune.keep_large = env_u64("AVANT_GC_TUNE_KEEP_LARGE", FULL_KEEP_LARGE, 0, 1);
  gc_tune.keep_large_min = env_u64("AVANT_GC_TUNE_KEEP_LARGE_MIN_KB", FULL_KEEP_LARGE_MIN / 1024, 0, kb) * 1024ull;
  gc_tune.alloc_after = env_u64("AVANT_GC_TUNE_ALLOC_AFTER", 1, 0, 1);
  gc_tune.alloc_quiet = env_u64("AVANT_GC_TUNE_ALLOC_QUIET_DIV", ALLOC_FULL_QUIET_DIV, 0, 1u << 20);
}

void gc_tune_stamp(const void *block_base, uint64_t v) {
  uint64_t bi = (uint64_t)((uintptr_t)block_base - (uintptr_t)gc_heap.base) >> BLOCK_SHIFT;
  if (bi >= gc_tune.stamp_n) {
    uint64_t n = (gc_heap.committed >> BLOCK_SHIFT) + 64;
    if (n <= bi) {
      n = bi + 64;
    }
    gc_tune.stamp = gc_xrealloc(gc_tune.stamp, (size_t)n * sizeof(uint64_t));
    memset(gc_tune.stamp + gc_tune.stamp_n, 0, (size_t)(n - gc_tune.stamp_n) * sizeof(uint64_t));
    gc_tune.stamp_n = n;
  }
  gc_tune.stamp[bi] = v;
}

uint64_t gc_tune_stamp_of(const void *block_base) {
  uint64_t bi = (uint64_t)((uintptr_t)block_base - (uintptr_t)gc_heap.base) >> BLOCK_SHIFT;
  return bi < gc_tune.stamp_n ? gc_tune.stamp[bi] : 0;
}
#endif

/* The start phase ends: the budget goes to its default. */
static void young_start_end(void) {
  if (avant_young_bytes < TUNE(dflt, YOUNG_BYTES_DEFAULT)) {
    avant_young_bytes = TUNE(dflt, YOUNG_BYTES_DEFAULT);
    gc_heap.ctr.size.grows++;
  }
}

/* The start phase (YOUNG_BYTES_START): a young large object as large as
 * the budget ends it (gc_los_alloc). */
void gc_young_start_large(uint64_t bytes) {
  if (!gc_heap.set.young_fixed && bytes >= avant_young_bytes) {
    young_start_end();
  }
}

/* The start phase: a budget collection whose survivors pass an eighth of
 * the young bytes it saw raises the budget to max(2 * budget, 16 * surv),
 * the default at most, which ends the phase; the heap's floor raises it
 * too. It ends too once the young collections promoted START_PROM_BYTES
 * (the program's objects outlive two of them at this budget:
 * Compress::LZWEncode hunted holes for +20% instructions at 1 MiB), and
 * once the young large objects a collection leaves pass the budget (they
 * are not counted in it, and the program's buffers outlive it:
 * Compress::BWTEncode and LZWDecode promoted theirs by their byte age,
 * +88% and +94% peak). No high-survival or tenure decision is made in
 * the phase. */
static int young_start_size(uint64_t used, uint64_t surv, uint64_t h, int budget) {
  uint64_t dflt = TUNE(dflt, YOUNG_BYTES_DEFAULT);
  if (avant_young_bytes >= dflt) {
    return 0;
  }
  if (TUNE(start_prom, START_PROM_BYTES) &&
      gc_heap.ctr.prom.copied + gc_heap.ctr.prom.large + gc_heap.ctr.sticky.inplace > TUNE(start_prom, START_PROM_BYTES)) {
    young_start_end();
    return 0;
  }
  if (TUNE(start_large, 1) && gc_heap.young_large > avant_young_bytes) {
    young_start_end();
    return 0;
  }
  if (budget && surv * 8 > used) {
    uint64_t want = surv * 16;
    if (want < avant_young_bytes * 2) {
      want = avant_young_bytes * 2;
    }
    avant_young_bytes = want > dflt ? dflt : want;
    gc_heap.ctr.size.grows++;
  }
  if (h > avant_young_bytes) {
    avant_young_bytes = h > dflt ? dflt : h;
  }
  if (budget) {
    gc_heap.young_high_prev = 0;
#ifdef AVANT_GC_TUNE
    gc_tune.highs = 0;
#else
    gc_heap.young_grow_prev = 0;
#endif
  }
  return 1;
}

/* The young sizing rule, after a young collection that saw USED young
 * bytes and SURV surviving ones. The budget doubles, up to
 * YOUNG_BYTES_CAP, at the second consecutive young collection where more
 * than YOUNG_GROW_PCT percent of the young bytes survived, and halves back toward its floor
 * when less than a tenth survived. The floor is the larger of
 * YOUNG_BYTES_DEFAULT and the heap in use (mature blocks and large
 * objects) / YOUNG_HEAP_DIV; the budget is raised to it at once.
 * The doubling used to wait for a high-survival collection whose
 * pause per surviving byte rose against the previous one: a wall-clock
 * reading, so the budget's path, and with it every collection count,
 * changed from run to run. Growing at the first high-survival collection
 * was measured and rejected: it does not help the suite
 * (44.6 s against 42.3) and raises the compiler's peak_heap (245 ->
 * 318 MB). */
static void young_size(uint64_t used, uint64_t surv, int budget) {
  uint64_t floor = TUNE(dflt, YOUNG_BYTES_DEFAULT);
  uint64_t h = (gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes) / TUNE(heap_div, YOUNG_HEAP_DIV);
  int high;
#ifndef AVANT_GC_TUNE
  int grow;
#endif
  if (used == 0) {
    return;
  }
  if (young_start_size(used, surv, h, budget)) {
    return;
  }
  if (h > floor) {
    floor = h > TUNE(cap, YOUNG_BYTES_CAP) ? TUNE(cap, YOUNG_BYTES_CAP) : h;
  }
  high = surv * 2 > used;
#ifdef AVANT_GC_TUNE
  {
    /* The family: survival (smoothed over collections with weight
     * 1/smooth) above high_pct for streak budget collections in a row
     * grows the budget by grow_pct, below low_pct shrinks it by
     * shrink_pct. */
    uint64_t r = (uint64_t)((double)surv * 1e6 / (double)used);
    int th, tl;
    uint64_t highs;
    if (gc_tune.smooth > 1) {
      gc_tune.rate = gc_tune.rate ? (gc_tune.rate * (gc_tune.smooth - 1) + r) / gc_tune.smooth : r;
      th = gc_tune.rate > gc_tune.high_pct * 10000;
      tl = gc_tune.rate < gc_tune.low_pct * 10000;
    } else {
      th = surv * 100 > used * gc_tune.high_pct;
      tl = surv * 100 < used * gc_tune.low_pct;
    }
    highs = th ? gc_tune.highs + 1 : 0;
    if (th) {
      if (highs >= gc_tune.streak && avant_young_bytes < gc_tune.cap) {
        uint64_t g = avant_young_bytes * gc_tune.grow_pct / 100;
        avant_young_bytes = g > gc_tune.cap ? gc_tune.cap : g;
        gc_heap.ctr.size.grows++;
      }
    } else if (tl && avant_young_bytes > floor) {
      uint64_t k = avant_young_bytes * gc_tune.shrink_pct / 100;
      avant_young_bytes = k < floor ? floor : k;
      gc_heap.ctr.size.shrinks++;
    }
    if (budget) {
      gc_tune.highs = highs;
    }
  }
#else
  grow = surv * 100 > used * YOUNG_GROW_PCT;
  if (grow) {
    if (gc_heap.young_grow_prev && avant_young_bytes < YOUNG_BYTES_CAP) {
      avant_young_bytes = avant_young_bytes * 2 > YOUNG_BYTES_CAP ? YOUNG_BYTES_CAP
                                                                 : avant_young_bytes * 2;
      gc_heap.ctr.size.grows++;
    }
  } else if (surv * 10 < used && avant_young_bytes > floor) {
    avant_young_bytes = avant_young_bytes / 2 < floor ? floor : avant_young_bytes / 2;
    gc_heap.ctr.size.shrinks++;
  }
#endif
  if (avant_young_bytes < floor) {
    avant_young_bytes = floor;
  }
  /* The tenured phase (gc_sticky_retire, avant_gc_sticky.c), decided by
   * the collections the budget asks for only (the program's end it,
   * gc_collect). In the phase a young collection is a probe: the phase
   * goes on, its interval doubled up to TENURE_GAP_MAX budgets, while
   * nearly everything still survives. Out of it, the second such
   * collection in a row starts it. */
  if (budget && (gc_heap.tenure_on || (high && gc_heap.young_high_prev))) {
    int nearly = surv * TENURE_SURV_DEN > used * TENURE_SURV_NUM;
    if (gc_heap.tenure_on) {
      gc_heap.ctr.sticky.probes++;
      if (nearly) {
        gc_heap.tenure_gap = gc_heap.tenure_gap * 2 > TENURE_GAP_MAX ? TENURE_GAP_MAX : gc_heap.tenure_gap * 2;
        gc_heap.tenure_left = gc_heap.tenure_gap;
      } else {
        gc_heap.tenure_on = 0;
      }
    } else if (nearly && gc_sticky_tenure_ok()) {
      gc_heap.tenure_on = 1;
      gc_heap.tenure_gap = 1;
      gc_heap.tenure_left = 1;
    }
  }
  /* The streak of high-survival collections is the budget's: a collection
   * the program asks for (BUDGET 0) neither makes nor breaks it. Its
   * survival says what the program kept at that point, not what a budget
   * keeps (the collector benchmark's stable heap: 499 forced collections
   * of one new object each, each one surviving, left the streak set for
   * the chain build that follows, which then made one more young
   * collection, aged 8 MB in place and copied 5.6 KB more: about 100 M
   * instructions). */
  if (budget) {
    gc_heap.young_high_prev = (uint8_t)high;
#ifndef AVANT_GC_TUNE
    gc_heap.young_grow_prev = (uint8_t)grow;
#endif
  }
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

/* The fragmentation statistic: after a full collection's mark, the free
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

/* Under AVANT_HEAP_MAX_MB a live set the heap
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

/* The allowance of the full collection due by allocation
 * (ALLOC_FULL_PCT) starts again at a full collection and at a complete
 * one (a full collection the proof made young). */
static void alloc_full_restart(void) {
#ifdef AVANT_GC_TUNE
  uint64_t h = gc_tune.alloc_base ? gc_heap.live : gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes;
  uint64_t due = h / 100 * gc_tune.alloc_pct;
  uint64_t floor = gc_tune.alloc_min;
#else
  uint64_t due = (gc_heap.space[SPACE_MATURE].bytes + gc_heap.large_bytes) / 100 * ALLOC_FULL_PCT;
  uint64_t floor = ALLOC_FULL_MIN;
#endif
  gc_heap.last_full_at = avant_g_stats.allocated_bytes;
  gc_heap.alloc_full_due = due < floor ? floor : due;
}

static int region_refuses(void);

void gc_collect(GcKind kind, int allow_move, GcWhy why) {
  int full = kind == GC_FULL;
  /* A full collection that runs as a young one: the proof holds. */
  int complete = 0;
  int resume = 0;
  uint64_t t0;
  GcProofWhy proof = PROOF_INTACT;
  /* The inline path's allocations are counted before the policy's
   * counters are read (gc_alloc_sync). */
  gc_alloc_sync();
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
  /* The handshake (avant_gc_safepoint.c): the world
   * lock is released (rule 1) while this thread raises the request and
   * waits for every other registered thread to stop at a safepoint or be
   * native, then taken again. When another thread held the request, this
   * one stopped until that collection was done; a collection the policy
   * asked for (the budget, the trigger, a stress point) is then
   * reconsidered, an explicit or heap-limit one runs. */
  if (!gc_thread.collector &&
      (why == GC_WHY_STRESS || atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0) &&
      gc_stop_world_held()) {
    /* A stress collection with no other thread running, and any
     * collection of a program with one thread, keeps the world lock
     * (gc_stop_world_held): there is no thread to wait for, so the lock's
     * round trip buys nothing (about 70 instructions a young collection
     * on a heap that does not change). A threaded program's collections
     * outside the stress mode take the full handshake, as before. */
    resume = 1;
  } else if (!gc_thread.collector) {
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
  /* A collection the program asks for, or the heap limit's, ends the
   * tenured phase, whether it then traces or not: the program says its
   * work so far is dead (the collector benchmark's churn beside a live
   * set: the phase of the set's build retired the churn's first budget,
   * and two more full collections followed). */
  if (gc_heap.tenure_on && (why == GC_WHY_EXPLICIT || why == GC_WHY_LIMIT)) {
    gc_heap.tenure_on = 0;
  }
  t0 = now_ns();
  gc_thread.collecting = 1;
  avant_g_stats.collections++;
  /* Every thread's remembered-set buffer joins the set
   * before anything reads it. */
  gc_rem_merge_all();
  if (full && allow_move) {
    /* The complete collection (avant_gc_proof.c): a full collection, forced or due, first asks
     * whether any old object can have died since the last one. When none
     * can (no reference to an old object left an old object or the roots,
     * nothing became old), what is unreachable is young, a young
     * collection frees exactly that, and this collection is one: it is
     * complete all the same. Otherwise it traces, as before. (A due
     * collection seldom holds: the trigger counts what was promoted or
     * born old, which breaks the proof; the heap limit's can.) */
    proof = gc_proof_check();
    gc_heap.ctr.proof.asked++;
    if (why == GC_WHY_EXPLICIT) {
      gc_heap.ctr.proof.forced++;
    }
    if (proof == PROOF_INTACT && gc_heap.proof.grown != PROOF_INTACT) {
      /* The old objects are all reachable, and some object became old
       * since the last full collection: the sticky full collection marks
       * what is not old (avant_gc_proof.c), when the last full
       * collection's marks and line marks are only the old objects'. */
      int keepable = why == GC_WHY_EXPLICIT && !gc_heap.set.young_fixed && avant_nursery_held &&
                     avant_nursery_held * KEEP_YOUNG_DIV <= gc_heap.proof.old_live;
      /* Except a collection the program asks for whose young generation
       * the trace below would leave young: the sticky one promotes what it
       * marks, and a program that stores a fresh object before each forced
       * collection would make the next store remove the last reference to
       * an old object (the collector benchmark's stable heap alone: the
       * sticky collection, then a trace, for one trace). */
      if (gc_heap.proof.lines_saved && !gc_heap.proof.no_sticky && !keepable) {
        gc_heap.sticky = 1;
      } else {
        proof = (GcProofWhy)gc_heap.proof.grown;
      }
    } else if (proof == PROOF_INTACT && gc_heap.proof.lines_saved && !gc_heap.proof.no_sticky &&
               avant_nursery_held * PROOF_STICKY_YOUNG_DIV > gc_heap.proof.old_live &&
               avant_nursery_held <= PROOF_YOUNG_PER_OLD * gc_heap.proof.old_live) {
      /* It holds, and the young generation is large beside the old heap:
       * the sticky full collection marks each young survivor once and
       * promotes it, where the young collection would copy it, and copy or
       * promote it again at the next (PROOF_STICKY_YOUNG_DIV). */
      gc_heap.sticky = 1;
    }
    if (gc_heap.sticky) {
      /* (A trace: counted with the full collections, below.) */
    } else if (proof != PROOF_INTACT) {
      gc_heap.ctr.proof.fell[proof]++;
      /* A trace the program asked for, when what broke the proof is
       * undone by a full collection's end (not threads, no young
       * generation or the proof off) and the young generation is small
       * beside the old live bytes, leaves the young generation young:
       * a young collection first (its survivors age into survivor
       * blocks), then a mark through young objects that promotes none of
       * them (avant_gc_mark.c, gc_mark_roots). So the next forced
       * collection can hold again: the collector benchmark's stable heap
       * stores a fresh object before each forced collection, and a trace
       * that promoted it made the next store remove the last reference to
       * an old object, every time. */
      gc_heap.keep_young = (uint8_t)(why == GC_WHY_EXPLICIT && proof != PROOF_THREADS && proof != PROOF_NURSERY &&
                                     proof != PROOF_OFF && !gc_heap.set.young_fixed &&
                                     avant_nursery_held && avant_nursery_held * KEEP_YOUNG_DIV <= gc_heap.proof.old_live);
      /* A full collection due by allocation leaves it young too, in a
       * program of one thread: it runs where the program is, and what
       * it would promote is what is live there (a run's working set,
       * its buffers), which then dies old (ALLOC_FULL_PCT). Its mark
       * goes on through a young object that names an old one
       * (keep_full). */
#ifdef AVANT_GC_TUNE
      if (!gc_heap.keep_young &&
          (((gc_tune.keep_full & 1) && why == GC_WHY_TRIGGER) || ((gc_tune.keep_full & 2) && why == GC_WHY_HEAP_GOAL)) &&
#else
      if (!gc_heap.keep_young && why == GC_WHY_HEAP_GOAL &&
#endif
          proof != PROOF_THREADS && proof != PROOF_NURSERY && proof != PROOF_OFF && !gc_heap.set.young_fixed &&
          avant_nursery_held && atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0) {
        gc_heap.keep_young = 1;
        gc_heap.keep_full = 1;
      }
    } else if (avant_nursery_held > PROOF_YOUNG_PER_OLD * gc_heap.proof.old_live) {
      /* It holds, and the trace is the cheaper complete collection
       * (PROOF_YOUNG_PER_OLD's comment). */
      gc_heap.ctr.proof.fell[PROOF_YOUNG_HEAVY]++;
    } else {
      gc_heap.ctr.proof.held++;
      full = 0;
      complete = 1;
      if (!avant_nursery_held && !gc_heap.young_blocks_n && !gc_heap.young_large_n && !gc_heap.set.verify &&
          !gc_heap.set.stress) {
        /* It holds and nothing is young (no young block, no young large
         * object, nothing allocated since the last collection): there is
         * nothing to free, and a young collection would only walk the
         * roots (Binarytrees::Obj asks for one right after the collection
         * its hold's end made: 72 collections for 36). What a complete
         * collection restarts it restarts; the rest is left as it is.
         * The check modes run it all the same. */
        gc_heap.ctr.proof.idle++;
        gc_heap.mature_since_full = 0;
        gc_heap.promoted_since_full = 0;
        alloc_full_restart();
        avant_g_stats.collections--;
        gc_thread.collecting = 0;
        if (resume) {
          gc_resume_world();
        }
        return;
      }
    }
  }
  if (full) {
    count_full_why(why);
  }
  if (gc_heap.set.verify) {
    gc_quarantine_release();
    /* The eager comparison of the late start bits, and the eager
     * bits in place of the real ones while the checks below run: they
     * read every object as they did when each allocation set its bit,
     * and set none of the bits the collection itself must set. */
    gc_alloc_sync();
    gc_verify_starts_begin();
    gc_verify_begin(full);
    if (!full) {
      gc_verify_remembered();
    } else {
      gc_verify_lost();
    }
    /* The proof's oracle (avant_gc_verify.c): the verdict is taken
     * at every collection, the ordinary young ones too, and its
     * conclusion checked wherever it holds: before a complete collection
     * runs as a young one, the trace from the roots alone must reach
     * every old object. */
    if (!complete && !full) {
      proof = gc_proof_check();
    }
    /* The sticky full collection's proof holds for the old objects
     * (2: objects became old since, which the oracle then expects). */
    gc_verify_proof(full || complete,
                    proof != PROOF_INTACT ? 0 : gc_heap.proof.grown == PROOF_INTACT ? 1 : gc_heap.sticky ? 2 : 0);
    /* The side marks the sticky full collection would keep are only the
     * old objects' (avant_gc_verify.c). */
    gc_verify_marks_old();
    /* Every thread's outermost generated record entered through the
     * runtime (avant_gc_verify.c). */
    gc_verify_frames();
    gc_verify_starts_resume();
  }
  if (full && !gc_heap.sticky) {
    /* The log is emptied: this collection's end is what the next proof
     * starts from. (The sticky full collection marks from the logged
     * holders first, then empties it.) */
    gc_proof_full_begin();
  }
  gc_heap.young_clock += avant_nursery_held > gc_heap.young_carried
                              ? avant_nursery_held - gc_heap.young_carried
                              : 0;
  gc_heap.live = 0;
  gc_heap.work_n = 0;
  /* Every thread's allocation cursors and inline state (per thread; the
   * young cursor was cleared when the collection ended
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
    uint64_t here0 = gc_heap.ctr.sticky.here_bytes;
    uint64_t prom0 = gc_heap.ctr.prom.copied;
    uint64_t surv;
    avant_g_stats.young_collections++;
    if (complete) {
      /* The full trigger starts again, as after a trace: nothing became
       * old since the last one (the proof), so what it counted is not
       * mature growth to trace for; left standing, a due collection that
       * holds would fall due again at the next allocation. What this
       * collection promotes counts from here. (Memory goes back to the
       * OS by the young collection's own rule, not a full collection's:
       * a program that asks for a collection after each piece of work
       * would otherwise return and fault in again the memory the next
       * piece takes, at every one.) */
      gc_heap.mature_since_full = 0;
      gc_heap.promoted_since_full = 0;
    }
    gc_sticky_collect(gc_heap.set.young_fixed ? 1 : 0, complete);
    surv = (avant_g_stats.copied_bytes - copied0) + (gc_heap.ctr.sticky.inplace - inplace0) +
           (gc_heap.ctr.sticky.here_bytes - here0);
    /* An object this collection promoted, in place or by its copy, is old
     * from now on and no full collection marked it (a young large
     * object's promotion says so itself, promote_young_large). An aged
     * copy stays young. */
    if (gc_heap.ctr.sticky.inplace != inplace0 || gc_heap.ctr.prom.copied != prom0) {
      gc_proof_grow(PROOF_PROMOTED);
    }
    if (complete) {
      /* What a complete collection found live: the old objects, all of
       * them (the proof), and the young survivors. And the heap limit's
       * progress rule, as for the trace it stands for. */
      gc_heap.live += gc_heap.proof.old_live;
      if (why == GC_WHY_LIMIT && gc_heap.set.heap_max) {
        limit_progress_check();
      }
    }
    if (!gc_heap.set.young_fixed && !gc_heap.stress_now) {
      young_size(used, surv, why == GC_WHY_BUDGET);
    }
  } else {
    uint64_t keep_held = 0;
    if (gc_heap.keep_young) {
      /* The young collection of a trace that leaves the young generation
       * young: as a complete collection runs it (a sparse first survivor
       * ages, also from a hole of a recycled block), so that every young
       * object it leaves lies in a survivor block or is a young large
       * object. It makes no sizing decision. */
      avant_g_stats.young_collections++;
      gc_sticky_collect(0, 1);
      keep_held = avant_nursery_held;
      gc_heap.live = 0;
      gc_heap.work_n = 0;
      gc_heap.keep_young_live = 0;
      gc_heap.keep_yold = 0;
      if (!keep_held) {
        gc_heap.keep_young = 0;
      }
    }
    avant_g_stats.full_collections++;
    gc_heap.return_clock++;
    /* A full collection walks the mature blocks in order (its sweep, its
     * defragmentation, gc_sticky_full_end): the order in one list. */
    gc_mature_normalize();
    /* Non-moving for young objects: they are promoted where they lie
     * (defragmentation may move mature blocks, below). */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_MATURE);
    gc_sticky_full_begin();
    if (gc_heap.sticky) {
      /* The sticky full collection (avant_gc_proof.c): the last full
       * collection's side marks stay (the old objects'), its line marks
       * come back, and the mark starts from the roots and the logged
       * holders. The old objects are all live. */
      gc_proof_lines_restore();
      gc_mark_roots();
      gc_heap.ctr.sticky.stickies++;
      gc_heap.ctr.sticky.sticky_bytes += gc_heap.live;
      gc_heap.live += gc_heap.proof.old_live;
      gc_proof_full_begin();
    } else {
      /* The marks: one memset per chunk of the side bitmap; no header is
       * touched (no young mark outlives its collection). */
      gc_clear_side_marks();
      /* The line marks: the epoch advances (no walk zeroing them). */
      gc_line_epoch_advance();
      /* A trace the growth trigger asked for leaves the young buffers
       * young (FULL_KEEP_LARGE): it lands where the program is, and a
       * run's working buffer it would promote dies old and stays to the
       * next full collection. (One the program asks for makes what it
       * marks old, as it always did.) */
      gc_heap.keep_large = (uint8_t)(TUNE(keep_large, FULL_KEEP_LARGE) && why == GC_WHY_TRIGGER && !gc_heap.keep_young &&
                                     !gc_heap.set.young_fixed && !gc_heap.set.nursery_off && gc_heap.young_large_n);
      gc_heap.keep_large_k0 = gc_heap.keep_n;
      gc_mark_roots();
      if (gc_heap.keep_large) {
        /* Their holders join the remembered set before anything moves
         * (defragmentation rewrites the set's entries; the scrub then
         * drops what names no young object). */
        gc_rem_keep_large_holders();
        if (gc_heap.keep_young_live) {
          gc_heap.ctr.sticky.large_keeps++;
          gc_heap.ctr.sticky.large_keep_bytes += gc_heap.keep_young_live;
        }
      }
    }
    /* Late start bits: when the mark's loop set none (gc_mark_roots), the
     * runs of every block it left a live line in are built now
     * (gc_lazy_build_live); either way every object the mark reached has
     * its bit, and the inline runs of the cycle are forgotten before
     * anything is swept or moved. */
    gc_lazy_build_live();
    if (gc_heap.set.verify) {
      gc_verify_starts_end();
      if (gc_heap.sticky) {
        gc_verify_sticky_mark();
      }
    }
    gc_lazy_drop();
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
      if (allow_move && gc_heap.set.defrag && !gc_heap.sticky) {
        gc_defrag_evacuate(live_lines_this);
        gc_proof_anchors_forward();
      }
      if (gc_heap.set.verify && gc_heap.def_release.n) {
        gc_verify_defrag();
      }
      gc_defrag_release();
    }
    /* Dead and clean remembered entries leave the set. */
    gc_rem_full_scrub();
    gc_sweep_large();
    gc_filter_blocks(&gc_heap.space[SPACE_MATURE].list, gc_heap.keep_young ? gc_keep_young_or_live : gc_keep_if_live);
    /* A full collection promotes every object it marks (the mark clears
     * YOUNG) but the young buffers it leaves young (FULL_KEEP_LARGE,
     * which the young budget does not count, as after a young
     * collection), so no block object is young after it:
     * gc_sticky_full_end empties the young budget (avant_nursery_held).
     * (E's nursery survivors used to be summed here.) */
    gc_sticky_full_end();
    if (gc_heap.keep_young) {
      avant_nursery_held = keep_held;
      gc_heap.ctr.sticky.keeps++;
      gc_heap.ctr.sticky.keep_bytes += gc_heap.keep_young_live;
    }
    gc_heap.mature_since_full = 0;
    gc_heap.promoted_since_full = 0;
    /* What this cycle used beyond its live bytes joins the retention
     * history (GcHeap.cycle_peak); the next cycle's peak starts here. */
    gc_heap.retain_hist[gc_heap.retain_at++ % RETAIN_CYCLES] =
        gc_heap.cycle_peak > gc_heap.heap_bytes ? gc_heap.cycle_peak - gc_heap.heap_bytes : 0;
    gc_heap.cycle_peak = gc_heap.heap_bytes;
    /* Free memory beyond the retention target goes back to the OS. */
    gc_heap_return(gc_heap.live);
    if (why == GC_WHY_LIMIT && gc_heap.set.heap_max) {
      limit_progress_check();
    }
    /* What the next proof starts from: the roots as they are now,
     * an empty log. After everything this collection moves or frees. */
    gc_proof_full_end();
    if (gc_heap.keep_full) {
      gc_proof_break(PROOF_REF_LOST);
      gc_heap.keep_full = 0;
    }
    gc_heap.sticky = 0;
    gc_heap.keep_young = 0;
    gc_heap.keep_young_live = 0;
    gc_heap.keep_large = 0;
  }

  if (full) {
    /* The full trigger: max(4 MiB, (F - 1) * scanned live + plain large
     * live / PLAIN_LARGE_DIV), F = 2, uncapped (avant_gc_internal.h). */
    uint64_t plain = gc_heap.plain_large_live < gc_heap.live ? gc_heap.plain_large_live : gc_heap.live;
#ifdef AVANT_GC_TUNE
    uint64_t next = gc_tune.full_pct * (gc_heap.live - plain) / 100 + plain / gc_tune.plain_div;
#else
    uint64_t next = (uint64_t)(HEAP_FACTOR - 1) * (gc_heap.live - plain) + plain / PLAIN_LARGE_DIV;
#endif
    gc_heap.full_trigger = next < FULL_TRIGGER_MIN ? FULL_TRIGGER_MIN : next;
  }
#ifdef AVANT_GC_TUNE
  if (gc_tune.trace) {
    /* AVANT_GC_TUNE_TRACE=1: one line a collection on stderr. */
    static uint64_t t0;
    uint64_t t = now_ns();
    if (!t0) {
      t0 = t;
    }
    unsigned long long vm = 0, rss = 0;
    {
      FILE *f = fopen("/proc/self/statm", "r");
      if (f) {
        if (fscanf(f, "%llu %llu", &vm, &rss) != 2) {
          rss = 0;
        }
        fclose(f);
      }
    }
    fprintf(stderr, "avant gc trace: ms=%llu %s why=%d budget=%llu mature_since=%llu trigger=%llu live=%llu mature=%llu large=%llu young_large=%llu held=%llu free_res=%llu free_other=%llu clock=%llu rss=%llu spare=%llu\n",
            (unsigned long long)((t - t0) / 1000000), full ? "full" : "young", (int)why,
            (unsigned long long)avant_young_bytes, (unsigned long long)gc_heap.mature_since_full,
            (unsigned long long)gc_heap.full_trigger, (unsigned long long)gc_heap.live,
            (unsigned long long)gc_heap.space[SPACE_MATURE].bytes, (unsigned long long)gc_heap.large_bytes,
            (unsigned long long)gc_heap.young_large, (unsigned long long)avant_nursery_held,
            (unsigned long long)gc_heap.free_resident, (unsigned long long)gc_heap.free_resident_other,
            (unsigned long long)gc_heap.return_clock, rss * 4096ull,
            (unsigned long long)(gc_heap.los_spare ? gc_heap.los_spare->size : 0));
  }
#endif
  if (full || complete) {
    alloc_full_restart();
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
    /* A hold that spent the start phase's budget ends the phase. */
    if (gc_thread.hold && !gc_heap.set.young_fixed && avant_nursery_held >= avant_young_bytes) {
      young_start_end();
    }
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
   * a request off (above); runtime helpers root what
   * they hold (RC1) and allocate outside regions, so their allocations
   * are collection points like generated code's. */
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
    /* A full collection due by allocation (ALLOC_FULL_PCT), in place of
     * the young collection the budget asks for (the proof makes it that
     * young collection when no old object can have died). GREW: the
     * mature growth since the last budget collection (all of it after a
     * full collection). */
    uint64_t grew = gc_heap.mature_since_full >= gc_heap.alloc_mature_prev
                        ? gc_heap.mature_since_full - gc_heap.alloc_mature_prev
                        : gc_heap.mature_since_full;
    gc_heap.alloc_mature_prev = gc_heap.mature_since_full;
    if (avant_g_stats.allocated_bytes - gc_heap.last_full_at >= gc_heap.alloc_full_due &&
        gc_heap.mature_since_full * 100 >= gc_heap.full_trigger * TUNE(alloc_grown, ALLOC_FULL_GROWN_PCT) &&
        grew * TUNE(alloc_quiet, ALLOC_FULL_QUIET_DIV) <= avant_young_bytes &&
#ifdef AVANT_GC_TUNE
        gc_tune.alloc_pct && (!gc_tune.alloc_after || avant_g_stats.full_collections)
#else
        avant_g_stats.full_collections
#endif
    ) {
      gc_collect(GC_FULL, 1, GC_WHY_HEAP_GOAL);
      return;
    }
    /* The tenured phase (gc_sticky_retire): the budget is retired
     * instead of collected, until a probe is due; a young large object
     * makes this one a probe. */
    if (gc_heap.tenure_on && gc_heap.tenure_left) {
      if (!gc_sticky_tenure_ok()) {
        gc_heap.tenure_on = 0;
      } else if (gc_sticky_retire_ok()) {
        gc_heap.tenure_left--;
        gc_sticky_retire();
        return;
      }
    }
    /* (Under E a young copy not expected to fit under AVANT_HEAP_MAX_MB,
     * young_copy_fits, ran as a full collection; S+'s copy needs no room:
     * it stops at the limit and promotes the rest in place.) */
    gc_collect(GC_YOUNG, 1, GC_WHY_BUDGET);
  }
}

/* A stress point (AVANT_GC_STRESS=1). The caller holds gc_heap.world. Only
 * where a collection may run: not inside a defer region, not during a
 * collection, and only with a young generation to collect. Shared
 * programs too (the safepoint handshake stops the other threads; before
 * it, the stress mode ran in one-thread programs only). */
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
   * runs here. */
  if (gc_heap.mature_since_full >= gc_heap.full_trigger) {
    gc_collect(GC_FULL, 1, GC_WHY_TRIGGER);
  }
}

void gc_init_slow(void) {
  if (gc_heap.inited) {
    return;
  }
  gc_heap.inited = 1;
  /* A pointer buffer is walked word by word (EACH_REF_SLOT's list says
   * so; every other entry starts as "no reference word"). */
  gc_type_refs_update(AVANT_TYPE_PTRS);
  gc_settings_read(&gc_heap.set);
  /* No full collection is due by allocation before ALLOC_FULL_MIN. */
  gc_heap.alloc_full_due = TUNE(alloc_min, ALLOC_FULL_MIN);
  if (gc_heap.set.young_fixed) {
    avant_young_bytes = gc_heap.set.young_fixed;
  } else {
    avant_young_bytes = TUNE(start, YOUNG_BYTES_START);
  }
  /* The heap's address range: reserved once, here, so a refusal
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
  /* The proof holds from the program's start (no object is old, so the
   * barrier's slow call has nothing to log until the first full
   * collection ends); with no young generation no complete collection is
   * a young one. */
  if (gc_heap.set.nursery_off) {
    gc_proof_break(PROOF_NURSERY);
  }
  if (!gc_heap.set.proof) {
    gc_proof_break(PROOF_OFF);
  }
}

/* AVANT_GC_VERIFY at the program's exit: the whole
 * heap is verified once more, as at the start of a collection, after the
 * same handshake (every other thread stopped at a safepoint or native).
 * Not inside a collection or a no-GC region, where the heap is not in a
 * state a collection could start from. */
void gc_verify_exit_run(void (*check)(void)) {
  int resume = 0;
  if (gc_thread.collecting || gc_thread.nogc) {
    return;
  }
  gc_alloc_sync();
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

/* The heap-goal collection at the HTTP boundary (the request path's
 * allocations all pass through C helpers under defer). It is an ordinary
 * collection request: the
 * policy runs what is due (gc_maybe_collect: the young budget, which
 * counts the helpers' allocations, the full trigger, a pending
 * heap-limit collection), as at any allocation's slow path. Before, it
 * ran a non-moving full collection per allocation budget of its own
 * (the larger of the live bytes and 4 MB). Callers are at a boundary
 * where their C frames hold no heap pointers yet (the HTTP dispatch,
 * before it materializes the request). */
void avant_gc_heap_goal_collect(void) {
  gc_init();
  gc_alloc_sync();
  gc_safepoint();
  gc_lock(&gc_heap.world);
  gc_maybe_collect();
  pthread_mutex_unlock(&gc_heap.world);
}

/* The name of the function that holds ADDR, from the executable's own
 * symbol table: a static executable exports no symbol, so dladdr names
 * none, but its .symtab (kept unless the file was stripped) names every
 * function, an Avant function as av.MODULE/NAME. Writes the name to OUT
 * and returns 1, or returns 0. Read only on the way to gc_die. */
static int exe_function_name(void *addr, const Dl_info *info, char *out, size_t cap) {
  Elf64_Ehdr eh;
  Elf64_Shdr sym, str;
  int fd, found = 0;
  uint64_t key, i, n;
  if (!info->dli_fbase || cap < 2) {
    return 0;
  }
  fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr)) {
    close(fd);
    return 0;
  }
  /* A position-independent executable's symbols are offsets from its load
   * address; a fixed one's are addresses. */
  key = eh.e_type == ET_DYN ? (uint64_t)((uintptr_t)addr - (uintptr_t)info->dli_fbase) : (uint64_t)(uintptr_t)addr;
  for (i = 0; i < eh.e_shnum && !found; i++) {
    if (pread(fd, &sym, sizeof sym, (off_t)(eh.e_shoff + i * sizeof sym)) != (ssize_t)sizeof sym) {
      break;
    }
    if (sym.sh_type != SHT_SYMTAB || sym.sh_entsize != sizeof(Elf64_Sym) || sym.sh_link >= eh.e_shnum ||
        pread(fd, &str, sizeof str, (off_t)(eh.e_shoff + sym.sh_link * sizeof str)) != (ssize_t)sizeof str) {
      continue;
    }
    n = sym.sh_size / sizeof(Elf64_Sym);
    for (uint64_t k = 0; k < n; k++) {
      Elf64_Sym s;
      if (pread(fd, &s, sizeof s, (off_t)(sym.sh_offset + k * sizeof s)) != (ssize_t)sizeof s) {
        break;
      }
      if (ELF64_ST_TYPE(s.st_info) != STT_FUNC || key < s.st_value || key >= s.st_value + s.st_size ||
          s.st_name >= str.sh_size) {
        continue;
      }
      ssize_t got = pread(fd, out, cap - 1, (off_t)(str.sh_offset + s.st_name));
      if (got > 0) {
        out[got] = 0;
        found = out[0] != 0;
      }
      break;
    }
  }
  close(fd);
  return found;
}

/* The function a no-GC region was entered from, for a message WHAT: its
 * exported symbol, else its name in the executable's symbol table (an
 * Avant function shown as MODULE/NAME), else the call site's offset in
 * its image (addr2line resolves it). */
static __attribute__((noreturn)) void region_die_at(const char *what, void *site) {
  char msg[512];
  char name[256];
  Dl_info info;
  memset(&info, 0, sizeof info);
  if (site && dladdr(site, &info) && info.dli_sname) {
    snprintf(msg, sizeof msg, "%s (in %s)", what, info.dli_sname);
  } else if (site && info.dli_fbase && exe_function_name(site, &info, name, sizeof name)) {
    snprintf(msg, sizeof msg, "%s (in %s)", what, strncmp(name, "av.", 3) == 0 ? name + 3 : name);
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

/* An allocation inside a no-GC region proceeds past
 * AVANT_HEAP_MAX_MB (the full collection runs after the region),
 * unless the region alone has allocated more than the limit: no
 * collection after it could bring the heap back under the limit. */
void gc_region_limit_check(uint64_t bytes) {
  if (gc_heap.set.heap_max && gc_thread.nogc && gc_thread.region_bytes + bytes > gc_heap.set.heap_max) {
    region_die("heap limit in a no-GC region");
  }
  /* A hold (std.gc): the same rule over the hold's own
   * allocations (its inline ones included: the count since its entry). */
  if (gc_heap.set.heap_max && gc_thread.hold &&
      avant_g_stats.allocated_bytes - gc_thread.hold_base + bytes > gc_heap.set.heap_max) {
    region_die_at("heap limit in a gc.hold block: its own allocations passed AVANT_HEAP_MAX_MB", gc_thread.hold_site);
  }
}

/* RC4 (avant_gc.h): a collection attempted inside a no-GC
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
  gc_alloc_sync();
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
  gc_alloc_sync();
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

/* `gc.hold { }` (std.gc): GcThread.hold's comment.
 * The compiler brackets the block with these two and ends the hold on
 * every way out of it (the end of the block, return, break, continue,
 * a `?` that returns). Under AVANT_GC_STRESS=1 a hold is inactive, so a
 * stress run collects inside it as it would without it, and the hold
 * hides no missing root. */
/* Whether this thread is inside a hold, for the runtime's inline
 * allocation (avant_rt.h, avant_alloc_try_inline): with the young budget
 * spent, a hold's allocations collect nothing, and avant_alloc would only
 * take its locked path for them. */
__thread uint32_t avant_gc_hold_open;

void avant_gc_hold_begin(void) {
  gc_init();
  if (gc_heap.set.stress) {
    return;
  }
  if (gc_thread.hold++ == 0) {
    avant_gc_hold_open = 1;
    gc_alloc_sync();
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
  avant_gc_hold_open = 0;
  young = gc_thread.hold_young;
  gc_thread.hold_young = 0;
  gc_alloc_sync();
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
