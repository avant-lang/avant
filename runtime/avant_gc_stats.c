#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Statistics (Stage 28 W12, moved from avant_rt.c; per family since
 * W14b2): AvantGcStats (an ABI for generated code: field order fixed) and
 * the AVANT_GC_STATS=1 report at exit. The other counters are
 * gc_heap.ctr, one struct per family (avant_gc_internal.h defines each);
 * they are written where the events happen and printed here, one routine
 * per family. The report is one line whose format is fixed (tests and
 * records parse it): every routine appends its fields to one buffer in
 * the line's order, and the line is written once. */

AvantGcStats avant_g_stats;

/* The report line under construction. */
typedef struct StatsLine {
  char buf[8192];
  size_t n;
} StatsLine;

__attribute__((format(printf, 2, 3)))
static void line_add(StatsLine *l, const char *fmt, ...) {
  va_list ap;
  int k;
  if (l->n >= sizeof(l->buf)) {
    return;
  }
  va_start(ap, fmt);
  k = vsnprintf(l->buf + l->n, sizeof(l->buf) - l->n, fmt, ap);
  va_end(ap);
  if (k > 0) {
    l->n += (size_t)k;
    if (l->n > sizeof(l->buf)) {
      l->n = sizeof(l->buf);
    }
  }
}

static unsigned long long u(uint64_t v) {
  return (unsigned long long)v;
}

/* The heap's own segment: AvantGcStats, the peaks, the root family, the
 * young threshold and the inline path's count. (The fields that printed
 * 0 since their mechanisms went left the line in Stage 28 W16b, O106,
 * O110: sm_walks, sm_hits, sm_sites, the stackmap walker's, W12r;
 * sm_left, E's C-stack pass; cpy_peak, pk_from, pk_to, srv_reserve and
 * the keep family, E's spaces and kept objects, W15d; prom_pinned;
 * sp_calls, sp_collections, the named safepoints, W16a.) */
static void print_heap(StatsLine *l) {
  const GcCounters *c = &gc_heap.ctr;
  line_add(l,
           "avant gc: collections=%llu young=%llu full=%llu "
           "alloc=%llu copied=%llu live=%llu peak_live=%llu peak_heap=%llu interiors=%llu "
           "sticky=%llu young_mb=%llu held=%llu mature_since=%llu roots=%llu reloads=%llu enters=%llu inline_allocs=%llu ",
           u(avant_g_stats.collections), u(avant_g_stats.young_collections),
           u(avant_g_stats.full_collections), u(avant_g_stats.allocated_bytes),
           u(avant_g_stats.copied_bytes), u(avant_g_stats.live_bytes), u(c->peak_live),
           u(avant_g_stats.peak_heap), u(c->root.interiors),
           u(c->yng.sticky), u(avant_young_bytes / (1024ull * 1024ull)), u(avant_nursery_held),
           u(gc_heap.mature_since_full), u(c->root.roots), u(c->root.reloads), u(c->root.enters),
           u(avant_inline_allocs));
}

/* rem_now is the set's size at exit. */
static void print_rem(StatsLine *l) {
  const GcRemCounters *c = &gc_heap.ctr.rem;
  line_add(l,
           "rem_scans=%llu rem_entries=%llu rem_fields=%llu rem_peak=%llu rem_now=%llu rem_adds=%llu rem_cleared=%llu ",
           u(c->scans), u(c->entries), u(c->fields), u(c->peak), u(gc_heap.rem_n), u(c->adds),
           u(c->cleared));
}

static void print_def(StatsLine *l) {
  const GcDefCounters *c = &gc_heap.ctr.def;
  line_add(l,
           "def_blocks=%llu def_live_lines=%llu def_wasted=%llu def_hist=%llu,%llu,%llu,%llu,%llu,%llu "
           "def_evac_blocks=%llu def_evac_bytes=%llu def_freed_blocks=%llu def_skipped=%llu ",
           u(c->blocks), u(c->live_lines), u(c->wasted), u(c->hist[0]), u(c->hist[1]),
           u(c->hist[2]), u(c->hist[3]), u(c->hist[4]), u(c->hist[5]), u(c->evac_blocks),
           u(c->evac_bytes), u(c->freed_blocks), u(c->skipped));
}

static void print_srv(StatsLine *l) {
  const GcSrvCounters *c = &gc_heap.ctr.srv;
  line_add(l,
           "srv_probes=%llu srv_used=%llu srv_surv=%llu srv_min=%llu srv_max=%llu srv_hist=%llu,%llu,%llu,%llu,%llu,%llu ",
           u(c->probes), u(c->used), u(c->surv), u(c->min), u(c->max), u(c->hist[0]),
           u(c->hist[1]), u(c->hist[2]), u(c->hist[3]), u(c->hist[4]), u(c->hist[5]));
}

static void print_yng(StatsLine *l) {
  const GcYngCounters *c = &gc_heap.ctr.yng;
  line_add(l, "yng_min=%llu yng_max=%llu ", u(c->min), u(c->max));
}

static void print_pk(StatsLine *l) {
  const GcPkCounters *c = &gc_heap.ctr.pk;
  line_add(l, "pk_nursery=%llu pk_mature=%llu pk_large=%llu pk_cache=%llu ", u(c->nursery),
           u(c->mature), u(c->large), u(c->cache));
}

/* los_now, los_now_bytes and los_spare_bytes describe the space at exit:
 * a walk over every large object and the spare. */
static void print_los(StatsLine *l) {
  const GcLosCounters *c = &gc_heap.ctr.los;
  LargeIter it;
  AvantHeader *h;
  uint64_t now = 0;
  uint64_t now_bytes = 0;
  uint64_t spare_bytes = gc_heap.los_spare ? (uint64_t)gc_large_bytes(gc_heap.los_spare) : 0;
  gc_large_iter_init(&it);
  while ((h = gc_large_next(&it))) {
    now++;
    now_bytes += (uint64_t)gc_large_bytes(h);
  }
  line_add(l,
           "los_allocs=%llu los_bytes=%llu los_callocs=%llu los_hits=%llu los_cached=%llu los_dropped=%llu los_freed=%llu los_scalar=%llu los_scalar_bytes=%llu los_big=%llu los_big_bytes=%llu los_young=%llu los_young_bytes=%llu los_mature=%llu los_mature_bytes=%llu los_hist=%llu,%llu,%llu,%llu,%llu,%llu los_live_peak=%llu los_live_sum=%llu los_samples=%llu los_promoted=%llu los_promoted_bytes=%llu los_now=%llu los_now_bytes=%llu los_spare_bytes=%llu ",
           u(c->allocs), u(c->alloc_bytes), u(c->callocs), u(c->spare_hits), u(c->spare_cached),
           u(c->spare_dropped), u(c->freed), u(c->scalar_allocs), u(c->scalar_bytes),
           u(c->big_allocs), u(c->big_bytes), u(c->young_allocs), u(c->young_bytes),
           u(c->mature_allocs), u(c->mature_bytes), u(c->hist[0]), u(c->hist[1]), u(c->hist[2]),
           u(c->hist[3]), u(c->hist[4]), u(c->hist[5]), u(c->live_peak), u(c->live_sum),
           u(c->live_samples), u(c->promoted), u(c->promoted_bytes), u(now), u(now_bytes),
           u(spare_bytes));
}

/* The W15a1 families (gc-design.md §3.4 M5, M6), appended after the los
 * family: pause times in ns, full collections by cause, M6's
 * fragmentation (frag_ppm is §3.4 rule 5's aggregate, the free-line
 * fraction weighted by heap bytes over the full collections), and bytes
 * promoted by young collections. */
static void print_pause(StatsLine *l) {
  const GcPauseCounters *c = &gc_heap.ctr.pause;
  line_add(l, "pause_young_ns=%llu pause_full_ns=%llu pause_max_ns=%llu ", u(c->young_ns),
           u(c->full_ns), u(c->max_ns));
  /* The handshake (Stage 28 W17a): collections that waited for another
   * thread, and the wait's total and longest time. */
  line_add(l, "handshakes=%llu pause_hs_ns=%llu pause_hs_max_ns=%llu ", u(c->hs_n), u(c->hs_ns),
           u(c->hs_max_ns));
}

static void print_why(StatsLine *l) {
  const GcWhyCounters *c = &gc_heap.ctr.why;
  line_add(l,
           "full_promotion=%llu full_mature=%llu full_limit=%llu full_heap_goal=%llu "
           "full_explicit=%llu ",
           u(c->promotion), u(c->mature), u(c->limit), u(c->heap_goal), u(c->explicit_));
}

static void print_frag(StatsLine *l) {
  const GcFragCounters *c = &gc_heap.ctr.frag;
  uint64_t ppm = c->den > 0 ? (uint64_t)(c->num / c->den * 1e6) : 0;
  line_add(l, "frag_fulls=%llu frag_free_lines=%llu frag_lines=%llu frag_ppm=%llu frag_max_ppm=%llu ",
           u(c->fulls), u(c->free_lines), u(c->lines), u(ppm), u(c->max_ppm));
}

static void print_prom(StatsLine *l) {
  const GcPromCounters *c = &gc_heap.ctr.prom;
  line_add(l, "prom_bytes=%llu prom_copied=%llu prom_large=%llu ",
           u(c->copied + c->large + gc_heap.ctr.sticky.inplace), u(c->copied), u(c->large));
}

static void print_size(StatsLine *l) {
  const GcSizeCounters *c = &gc_heap.ctr.size;
  line_add(l, "young_grows=%llu young_shrinks=%llu ", u(c->grows), u(c->shrinks));
}

/* The sticky family (Stage 28 W15b, candidate S), last in
 * the line: bytes promoted in place by young collections, young blocks
 * swept, holes taken by the young allocator, lines the young sweeps
 * freed, medium objects placed in the overflow block, empty young blocks
 * given back to their chunks, and S+'s evacuated, kept-in-place and copy
 * blocks (Stage 28 W15c), the bytes it aged and their survivor blocks
 * (W15d). */
static void print_sticky(StatsLine *l) {
  const GcStickyCounters *c = &gc_heap.ctr.sticky;
  line_add(l,
           "prom_inplace=%llu young_blocks=%llu young_holes=%llu young_freed_lines=%llu "
           "young_overflow=%llu young_returned=%llu young_evac_blocks=%llu "
           "young_kept_blocks=%llu young_copy_blocks=%llu young_aged=%llu young_surv_blocks=%llu "
           "young_aged_holders=%llu young_aged_inplace_blocks=%llu\n",
           u(c->inplace), u(c->blocks), u(c->holes), u(c->freed_lines), u(c->overflow),
           u(c->returned), u(c->evac_blocks), u(c->kept_blocks), u(c->copy_blocks), u(c->aged),
           u(c->surv_blocks), u(c->aged_holders), u(c->aged_inplace_blocks));
}

void gc_print_stats(void) {
  StatsLine l;
  l.n = 0;
  print_heap(&l);
  print_rem(&l);
  print_def(&l);
  print_srv(&l);
  print_yng(&l);
  print_pk(&l);
  print_los(&l);
  print_pause(&l);
  print_why(&l);
  print_frag(&l);
  print_prom(&l);
  print_size(&l);
  print_sticky(&l);
  fwrite(l.buf, 1, l.n, stderr);
}

void avant_gc_stats(AvantGcStats *out) {
  if (out) {
    *out = avant_g_stats;
  }
}
