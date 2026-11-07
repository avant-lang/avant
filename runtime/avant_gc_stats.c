#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Statistics (Stage 28 W12, moved from avant_rt.c): AvantGcStats (an ABI
 * for generated code: field order fixed) and the runtime-internal counters
 * AVANT_GC_STATS=1 prints at exit, with the comments that define each
 * family. Counters are written where the events happen; they live here so
 * the report and its definitions are in one place. */

uint64_t g_interior_slots;

uint64_t g_peak_live;
/* Root bookkeeping call counts (Collector II Wave 0): every call to
 * avant_gc_root, avant_gc_reload, and avant_gc_enter, so a profile can
 * split call volume from per-call cost. Reported by AVANT_GC_STATS. */
uint64_t g_root_calls;
uint64_t g_reload_calls;
uint64_t g_enter_calls;
/* Remembered-set observability (Collector II Wave 2): scans counts every
 * remembered-set scan in gc_gen_young_copy (probe and real passes alike),
 * entries/fields sum the set size and field slots visited at each scan,
 * peak is the largest frontier seen at a scan start, adds counts
 * successful rem_add inserts, cleared counts frontier evictions.
 * Reported by AVANT_GC_STATS. Internal only: not part of AvantGcStats,
 * whose field order is an ABI for generated code. */
uint64_t g_rem_scans;
uint64_t g_rem_entries;
uint64_t g_rem_fields;
uint64_t g_rem_peak;
uint64_t g_rem_adds;
uint64_t g_rem_cleared;

/* Mature defragmentation observability (Collector II Wave 3, step 1):
 * per-block live-line histogram built from the line marks a full
 * collection already makes. Buckets by live lines in a 128-line block:
 * 0, 1-32, 33-64, 65-96, 97-127, 128. Wasted bytes count only partly
 * live blocks ((128 - live) * LINE_SIZE); empty blocks are freed whole
 * by the existing sweep and are not fragmentation waste. Counters are
 * cumulative over full collections and runtime-internal. */
uint64_t g_def_blocks;
uint64_t g_def_live_lines;
uint64_t g_def_wasted;
uint64_t g_def_hist[6];

uint64_t g_def_evac_blocks;
uint64_t g_def_evac_bytes;
uint64_t g_def_freed_blocks;
uint64_t g_def_skipped;
/* Survival and peak-heap observability (Collector II Wave 4, step 1):
 * probes counts adaptive survival probes, used/surv sum the nursery
 * bytes in use and the surviving bytes at each probe, min/max are the
 * extreme surviving-byte readings, and hist buckets the surv/used
 * ratio in percent (0, 1-10, 10-25, 25-50, 50-75, 75-100) so phase
 * oscillation is visible. yng_min/yng_max bound the adaptive young
 * threshold over the run. cpy_peak is the largest from-space plus
 * to-space object bytes held during one young copy. The pk_* fields
 * snapshot the heap composition (block bytes per space, large-object
 * bytes, cached block bytes) at the moment peak_heap was last set.
 * Reported by AVANT_GC_STATS. Internal only, like the rem/def counters. */
uint64_t g_srv_probes;
uint64_t g_srv_used;
uint64_t g_srv_surv;
uint64_t g_srv_min;
uint64_t g_srv_max;
uint64_t g_srv_hist[6];
uint64_t g_yng_min;
uint64_t g_yng_max;
uint64_t g_cpy_peak;
uint64_t g_pk_nursery;
uint64_t g_pk_mature;
uint64_t g_pk_from;
uint64_t g_pk_to;
uint64_t g_pk_large;
uint64_t g_pk_cache;
/* Large-object observability (Collector II Wave 5, step 1): allocs and
 * alloc_bytes count every gc_los_alloc by chunk bytes (the heap charge for
 * a large object); callocs counts fresh calloc calls, spare_hits counts
 * exact-size spare reuses, spare_cached counts dead young Bytes buffers
 * kept into the single spare slot, spare_dropped counts spares freed
 * unused, and freed counts large objects returned to free. scalar counts
 * Bytes allocations that are large only through SCALAR_LOS_MIN, big
 * counts allocations large through LOS_LIMIT. young/mature split the
 * birth space. hist buckets payload bytes: under 16 KiB, 16-64 KiB,
 * 64-256 KiB, 256 KiB-1 MiB, 1-4 MiB, at least 4 MiB. live samples the
 * marked large bytes at the liveness decision of each collection
 * (gc_reclaim_young_large for young, gc_sweep_large for full): sum over
 * samples, peak the largest single sample. promoted counts young large
 * objects tenured by promote_young_large. Reported by AVANT_GC_STATS.
 * Internal only, like the rem/def/srv counters. */
uint64_t g_los_allocs;
uint64_t g_los_alloc_bytes;
uint64_t g_los_callocs;
uint64_t g_los_spare_hits;
uint64_t g_los_spare_cached;
uint64_t g_los_spare_dropped;
uint64_t g_los_freed;
uint64_t g_los_scalar_allocs;
uint64_t g_los_scalar_bytes;
uint64_t g_los_big_allocs;
uint64_t g_los_big_bytes;
uint64_t g_los_young_allocs;
uint64_t g_los_young_bytes;
uint64_t g_los_mature_allocs;
uint64_t g_los_mature_bytes;
uint64_t g_los_hist[6];
uint64_t g_los_live_sum;
uint64_t g_los_live_peak;
uint64_t g_los_live_samples;
uint64_t g_los_promoted;
uint64_t g_los_promoted_bytes;

uint64_t g_sticky_grows;

AvantGcStats avant_g_stats;

static uint64_t g_los_now;
static uint64_t g_los_now_bytes;
static uint64_t g_los_spare_bytes;

void gc_print_stats(void) {
  {
    LargeIter it;
    AvantHeader *h;
    g_los_now = 0;
    g_los_now_bytes = 0;
    gc_large_iter_init(&it);
    while ((h = gc_large_next(&it))) {
      g_los_now++;
      g_los_now_bytes += (uint64_t)gc_large_bytes(h);
    }
    g_los_spare_bytes = g_los_spare ? (uint64_t)gc_large_bytes(g_los_spare) : 0;
  }
  fprintf(stderr,
          "avant gc: collections=%llu young=%llu full=%llu "
          "alloc=%llu copied=%llu live=%llu peak_live=%llu peak_heap=%llu interiors=%llu sm_walks=%llu sm_hits=%llu sm_sites=%u sm_left=%llu "
          "sticky=%llu young_mb=%llu held=%llu mature_since=%llu roots=%llu reloads=%llu enters=%llu inline_allocs=%llu "
          "rem_scans=%llu rem_entries=%llu rem_fields=%llu rem_peak=%llu rem_now=%llu rem_adds=%llu rem_cleared=%llu "
          "def_blocks=%llu def_live_lines=%llu def_wasted=%llu def_hist=%llu,%llu,%llu,%llu,%llu,%llu "
          "def_evac_blocks=%llu def_evac_bytes=%llu def_freed_blocks=%llu def_skipped=%llu "
          "srv_probes=%llu srv_used=%llu srv_surv=%llu srv_min=%llu srv_max=%llu srv_hist=%llu,%llu,%llu,%llu,%llu,%llu "
          "yng_min=%llu yng_max=%llu cpy_peak=%llu pk_nursery=%llu pk_mature=%llu pk_from=%llu pk_to=%llu pk_large=%llu pk_cache=%llu srv_reserve=%llu "
          "los_allocs=%llu los_bytes=%llu los_callocs=%llu los_hits=%llu los_cached=%llu los_dropped=%llu los_freed=%llu los_scalar=%llu los_scalar_bytes=%llu los_big=%llu los_big_bytes=%llu los_young=%llu los_young_bytes=%llu los_mature=%llu los_mature_bytes=%llu los_hist=%llu,%llu,%llu,%llu,%llu,%llu los_live_peak=%llu los_live_sum=%llu los_samples=%llu los_promoted=%llu los_promoted_bytes=%llu los_now=%llu los_now_bytes=%llu los_spare_bytes=%llu\n",
          (unsigned long long)g_stats.collections,
          (unsigned long long)g_stats.young_collections,
          (unsigned long long)g_stats.full_collections,
          (unsigned long long)g_stats.allocated_bytes,
          (unsigned long long)g_stats.copied_bytes,
          (unsigned long long)g_stats.live_bytes,
          (unsigned long long)g_peak_live,
          (unsigned long long)g_stats.peak_heap,
          (unsigned long long)g_interior_slots,
          /* sm_walks, sm_hits, sm_sites: the stackmap walker's counters,
           * always 0 since it was removed (Stage 28 W12r); the fields stay
           * so the report's format does not change. sm_left is the C-stack
           * pass's count (g_c_stack_left). */
          0ull,
          0ull,
          0u,
          (unsigned long long)g_c_stack_left,
          (unsigned long long)g_sticky_grows,
          (unsigned long long)(g_young_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_nursery_held,
          (unsigned long long)g_mature_since_full,
          (unsigned long long)g_root_calls,
          (unsigned long long)g_reload_calls,
          (unsigned long long)g_enter_calls,
          (unsigned long long)avant_inline_allocs,
          (unsigned long long)g_rem_scans,
          (unsigned long long)g_rem_entries,
          (unsigned long long)g_rem_fields,
          (unsigned long long)g_rem_peak,
          (unsigned long long)g_rem_n,
          (unsigned long long)g_rem_adds,
          (unsigned long long)g_rem_cleared,
          (unsigned long long)g_def_blocks,
          (unsigned long long)g_def_live_lines,
          (unsigned long long)g_def_wasted,
          (unsigned long long)g_def_hist[0],
          (unsigned long long)g_def_hist[1],
          (unsigned long long)g_def_hist[2],
          (unsigned long long)g_def_hist[3],
          (unsigned long long)g_def_hist[4],
          (unsigned long long)g_def_hist[5],
          (unsigned long long)g_def_evac_blocks,
          (unsigned long long)g_def_evac_bytes,
          (unsigned long long)g_def_freed_blocks,
          (unsigned long long)g_def_skipped,
          (unsigned long long)g_srv_probes,
          (unsigned long long)g_srv_used,
          (unsigned long long)g_srv_surv,
          (unsigned long long)g_srv_min,
          (unsigned long long)g_srv_max,
          (unsigned long long)g_srv_hist[0],
          (unsigned long long)g_srv_hist[1],
          (unsigned long long)g_srv_hist[2],
          (unsigned long long)g_srv_hist[3],
          (unsigned long long)g_srv_hist[4],
          (unsigned long long)g_srv_hist[5],
          (unsigned long long)g_yng_min,
          (unsigned long long)g_yng_max,
          (unsigned long long)g_cpy_peak,
          (unsigned long long)g_pk_nursery,
          (unsigned long long)g_pk_mature,
          (unsigned long long)g_pk_from,
          (unsigned long long)g_pk_to,
          (unsigned long long)g_pk_large,
          (unsigned long long)g_pk_cache,
          (unsigned long long)g_copy_reserve,
          (unsigned long long)g_los_allocs,
          (unsigned long long)g_los_alloc_bytes,
          (unsigned long long)g_los_callocs,
          (unsigned long long)g_los_spare_hits,
          (unsigned long long)g_los_spare_cached,
          (unsigned long long)g_los_spare_dropped,
          (unsigned long long)g_los_freed,
          (unsigned long long)g_los_scalar_allocs,
          (unsigned long long)g_los_scalar_bytes,
          (unsigned long long)g_los_big_allocs,
          (unsigned long long)g_los_big_bytes,
          (unsigned long long)g_los_young_allocs,
          (unsigned long long)g_los_young_bytes,
          (unsigned long long)g_los_mature_allocs,
          (unsigned long long)g_los_mature_bytes,
          (unsigned long long)g_los_hist[0],
          (unsigned long long)g_los_hist[1],
          (unsigned long long)g_los_hist[2],
          (unsigned long long)g_los_hist[3],
          (unsigned long long)g_los_hist[4],
          (unsigned long long)g_los_hist[5],
          (unsigned long long)g_los_live_peak,
          (unsigned long long)g_los_live_sum,
          (unsigned long long)g_los_live_samples,
          (unsigned long long)g_los_promoted,
          (unsigned long long)g_los_promoted_bytes,
          (unsigned long long)g_los_now,
          (unsigned long long)g_los_now_bytes,
          (unsigned long long)g_los_spare_bytes);
}

void avant_gc_stats(AvantGcStats *out) {
  if (out) {
    *out = g_stats;
  }
}
