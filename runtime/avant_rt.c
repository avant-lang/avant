#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_rt.h"
#include "avant_gc_internal.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct FwdEnt {
  void *from;
  void *to;
} FwdEnt;

int g_inited;
static int g_collecting;
int g_saw_pinned_nursery;
static int g_nursery_off;
static int g_discard_from;

Block *g_bht[BLOCK_HASH];
static Block *g_mature;
static Block *g_nursery;
static Block *g_from;
static Block *g_to;
static Block *g_cache;
static int g_cache_n;

static Block *g_cur;
static uint8_t *g_limit;
/* Inline nursery fast path (Collector II Wave 1): generated code bumps
 * avant_gc_bump directly when avant_gc_fast_ok is set, bounded by
 * avant_gc_limit (the block limit capped by the young threshold) and
 * writing the start bit through avant_gc_starts/avant_gc_base. The C
 * cursor g_bump is the same storage, so both sides share one cursor.
 * Only the runtime publishes or clears the fast state; see
 * gc_fast_refresh below. */
__thread uint8_t *avant_gc_bump;
__thread uint8_t *avant_gc_limit;
__thread uint8_t *avant_gc_base;
__thread uint64_t *avant_gc_starts;
__thread void *avant_gc_block;
__thread uint32_t avant_gc_fast_ok;
static Block *g_fast_block;
static void gc_fast_clear(void);

/* Per-space bump cursor. A full block used to send the next allocation
 * back through every older block (take_from_list + find_hole). Workers
 * allocate in mature space, and that walk dominated Matmul::T16.
 * g_alloc_fresh means the cursor is a new block: when it fills, allocate
 * another instead of rescanning full blocks. A full collection clears
 * the cursor so the next allocation reuses holes. */
static Block *g_alloc_cur[4];
static uint8_t g_alloc_fresh[4];

static Large *g_large;
Large *g_large_ht[LARGE_HASH];
/* The address range [g_large_lo, g_large_hi) covers every large chunk
 * ever put on g_large (los_alloc). It only grows: a chunk that
 * is freed leaves it as wide as it was. So an address outside it is
 * inside no object on g_large, and object_payload skips its walk of the
 * list, which for such an address could only miss. That walk ran for
 * every young-copy slot naming a static object in the binary (a string
 * literal), and dominated compiling the compiler (Stage 28 W11d). */
static uint8_t *g_large_lo = (uint8_t *)UINTPTR_MAX;
static uint8_t *g_large_hi;
/* One dead scalar large buffer, kept for the next same-sized alloc. */
static Large *g_los_spare;
/* Bytes in young scalar large objects. They do not move. A young
 * collection frees the ones the mark does not reach. */
static uint64_t g_young_large;
static int g_young_large_n;
/* Set when a young large object survived a collection. Further
 * same-sized allocs are live rows, not a replaced buffer, so they
 * must not collect on every malloc. */
static int g_los_eager_hold;
static __thread Frame *g_frame;
static __thread int g_gc_quiet;
static __thread Frame *g_frame_free;
static __thread int g_alloc_mature;

__thread ThreadReg g_self_reg;
static ThreadReg *g_threads;
/* Becomes 1 when a second thread registers, and stays 1. A single
 * mutator collects only at its own allocating call, so an interior
 * pointer used before the next call does not need a root. Another
 * thread can collect at any instruction. */
atomic_int g_avant_shared;
static pthread_mutex_t g_world = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_gc_defer;
int g_stack_scan;
static uint64_t g_interior_slots;

static PinEnt *g_pins;

static void **g_remset;
static uint32_t g_rem_n;
static uint32_t g_rem_cap;
/* Workers call avant_barrier without the world lock (a fresh array
 * header). The remembered-set realloc is shared. */
static pthread_mutex_t g_rem_mu = PTHREAD_MUTEX_INITIALIZER;
/* Mature objects that still point at a young survivor after this collection. */
static void **g_keep;
static uint32_t g_keep_n;
static uint32_t g_keep_cap;
/* Objects flagged MARKED by the survival probe. Clearing them by walking
 * every nursery object retraced the dead tree. */
static void **g_noted;
static uint32_t g_noted_n;
static uint32_t g_noted_cap;
uint64_t avant_young_bytes;

/* 0 when AVANT_YOUNG_MB pinned the threshold (tests). */
static int g_young_adapt;
uint64_t avant_nursery_held;

static uint64_t g_mature_since_full;
static uint64_t g_peak_live;
/* Root bookkeeping call counts (Collector II Wave 0): every call to
 * avant_gc_root, avant_gc_reload, and avant_gc_enter, so a profile can
 * split call volume from per-call cost. Reported by AVANT_GC_STATS. */
static uint64_t g_root_calls;
static uint64_t g_reload_calls;
static uint64_t g_enter_calls;
/* Remembered-set observability (Collector II Wave 2): scans counts every
 * remembered-set scan in gen_young_copy (probe and real passes alike),
 * entries/fields sum the set size and field slots visited at each scan,
 * peak is the largest frontier seen at a scan start, adds counts
 * successful rem_add inserts, cleared counts frontier evictions.
 * Reported by AVANT_GC_STATS. Internal only: not part of AvantGcStats,
 * whose field order is an ABI for generated code. */
static uint64_t g_rem_scans;
static uint64_t g_rem_entries;
static uint64_t g_rem_fields;
static uint64_t g_rem_peak;
static uint64_t g_rem_adds;
static uint64_t g_rem_cleared;
/* Frontier clearing (Collector II Wave 2): AVANT_GC_REM_FRONTIER=0
 * disables it. Dirty is tracked during the remembered scan itself, per
 * set index, so no second field walk is needed. Hysteresis (evict after
 * N clean scans) and probation (a second list scanned every K young
 * collections) were measured against immediate clearing on
 * bench/graph_frontier.av and lost (0.20s vs 0.19s at AVANT_YOUNG_MB=1,
 * 0.16s vs 0.15s adaptive), so only immediate clearing is kept. */
static int g_rem_mode = 1;
static uint8_t *g_rem_dirty;
/* Mature defragmentation observability (Collector II Wave 3, step 1):
 * per-block live-line histogram built from the line marks a full
 * collection already makes. Buckets by live lines in a 128-line block:
 * 0, 1-32, 33-64, 65-96, 97-127, 128. Wasted bytes count only partly
 * live blocks ((128 - live) * LINE_SIZE); empty blocks are freed whole
 * by the existing sweep and are not fragmentation waste. Counters are
 * cumulative over full collections and runtime-internal. */
static uint64_t g_def_blocks;
static uint64_t g_def_live_lines;
static uint64_t g_def_wasted;
static uint64_t g_def_hist[6];
/* Evacuation (step 2): AVANT_GC_DEFRAG=0 disables it, histogram stays
 * on. Candidates are mature blocks at or below 64 of 128 live lines
 * with no pinned object and no word of the collecting thread's stack
 * naming them (C31, note_c_stack_blocks); per full collection the live lines evacuated
 * stay at or below 25% of that mark's mature live lines, worst first. */
static int g_defrag_mode = 1;
static uint64_t g_def_evac_blocks;
static uint64_t g_def_evac_bytes;
static uint64_t g_def_freed_blocks;
static uint64_t g_def_skipped;
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
static uint64_t g_srv_probes;
static uint64_t g_srv_used;
static uint64_t g_srv_surv;
static uint64_t g_srv_min;
static uint64_t g_srv_max;
static uint64_t g_srv_hist[6];
static uint64_t g_yng_min;
static uint64_t g_yng_max;
static uint64_t g_cpy_peak;
static uint64_t g_pk_nursery;
static uint64_t g_pk_mature;
static uint64_t g_pk_from;
static uint64_t g_pk_to;
static uint64_t g_pk_large;
static uint64_t g_pk_cache;
/* Large-object observability (Collector II Wave 5, step 1): allocs and
 * alloc_bytes count every los_alloc by chunk bytes (the heap charge for
 * a large object); callocs counts fresh calloc calls, spare_hits counts
 * exact-size spare reuses, spare_cached counts dead young Bytes buffers
 * kept into the single spare slot, spare_dropped counts spares freed
 * unused, and freed counts large objects returned to free. scalar counts
 * Bytes allocations that are large only through SCALAR_LOS_MIN, big
 * counts allocations large through LOS_LIMIT. young/mature split the
 * birth space. hist buckets payload bytes: under 16 KiB, 16-64 KiB,
 * 64-256 KiB, 256 KiB-1 MiB, 1-4 MiB, at least 4 MiB. live samples the
 * marked large bytes at the liveness decision of each collection
 * (reclaim_young_large for young, sweep_large for full): sum over
 * samples, peak the largest single sample. promoted counts young large
 * objects tenured by promote_young_large. Reported by AVANT_GC_STATS.
 * Internal only, like the rem/def/srv counters. */
static uint64_t g_los_allocs;
static uint64_t g_los_alloc_bytes;
static uint64_t g_los_callocs;
static uint64_t g_los_spare_hits;
static uint64_t g_los_spare_cached;
static uint64_t g_los_spare_dropped;
static uint64_t g_los_freed;
static uint64_t g_los_scalar_allocs;
static uint64_t g_los_scalar_bytes;
static uint64_t g_los_big_allocs;
static uint64_t g_los_big_bytes;
static uint64_t g_los_young_allocs;
static uint64_t g_los_young_bytes;
static uint64_t g_los_mature_allocs;
static uint64_t g_los_mature_bytes;
static uint64_t g_los_hist[6];
static uint64_t g_los_live_sum;
static uint64_t g_los_live_peak;
static uint64_t g_los_live_samples;
static uint64_t g_los_promoted;
static uint64_t g_los_promoted_bytes;
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
static int g_reserve_mode = 1;
static uint64_t g_copy_reserve;
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
static uint64_t g_to_bytes;
static uint64_t g_sticky_grows;
static int g_probe;
static int g_force_tenure;
static int g_from_mature;
static uint64_t g_probe_nursery_bytes;
/* A depth-20 object tree is about 96 MiB. The nursery grows to hold one
 * burst and collects when that burst has died. 192 MiB is the cap. */
#define YOUNG_BYTES_MAX (192ull * 1024ull * 1024ull)
#define STICKY_MIN (8ull * 1024ull * 1024ull)

static void **g_work;
static uint32_t g_work_n;
static uint32_t g_work_cap;

TypeMap g_types[MAX_TYPES];
uint8_t g_type_set[MAX_TYPES];

/* Whether payload word W of an object with header H is a reference slot
 * by its type map (the verifier's inverse question). */
static int type_word_is_ref(const AvantHeader *h, size_t w) {
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_PTRS) {
    return 1;
  }
  if (id == AVANT_TYPE_BYTES || id >= MAX_TYPES || !g_type_set[id]) {
    return 0;
  }
  const TypeMap *m = &g_types[id];
  if (m->stride) {
    w %= m->stride;
  }
  if (w / 64 >= m->nchunks) {
    return 0;
  }
  return (m->words[w / 64] >> (w % 64)) & 1u;
}

static FwdEnt *g_fwd;
static size_t g_fwd_cap;
static size_t g_fwd_used;

uint64_t avant_since_gc;

static uint64_t g_heap_bytes;
static uint64_t g_heap_max;
uint64_t g_live;
static uint64_t g_last_full_at;

AvantGcStats avant_g_stats;

/* Nursery allocations taken by generated code without a call. */
uint64_t avant_inline_allocs;

static void die(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
}

void avant_gc_register_thread(void) {
  if (g_self_reg.head) {
    return;
  }
  g_self_reg.head = &g_frame;
  {
    pthread_attr_t attr;
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
        g_self_reg.stack_lo = addr;
        g_self_reg.stack_hi = (uint8_t *)addr + sz;
      }
      pthread_attr_destroy(&attr);
    }
  }
  pthread_mutex_lock(&g_threads_mu);
  g_self_reg.next = g_threads;
  g_threads = &g_self_reg;
  if (g_self_reg.next) {
    atomic_store_explicit(&g_avant_shared, 1, memory_order_release);
  }
  pthread_mutex_unlock(&g_threads_mu);
}

void avant_gc_unregister_thread(void) {
  pthread_mutex_lock(&g_threads_mu);
  ThreadReg **pp = &g_threads;
  while (*pp) {
    if (*pp == &g_self_reg) {
      *pp = g_self_reg.next;
      break;
    }
    pp = &(*pp)->next;
  }
  g_self_reg.head = NULL;
  g_self_reg.next = NULL;
  pthread_mutex_unlock(&g_threads_mu);
}

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

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) {
    die("out of memory");
  }
  return q;
}

void work_push(void *p) {
  if (g_work_n == g_work_cap) {
    g_work_cap = g_work_cap ? g_work_cap * 2 : 256;
    g_work = xrealloc(g_work, g_work_cap * sizeof(void *));
  }
  g_work[g_work_n++] = p;
}

static void rem_add(void *obj) {
  AvantHeader *h = avant_header(obj);
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  if (h->flags & AVANT_FLAG_REM) {
    if (locked) {
      pthread_mutex_unlock(&g_rem_mu);
    }
    return;
  }
  h->flags |= AVANT_FLAG_REM;
  if (g_rem_n == g_rem_cap) {
    g_rem_cap = g_rem_cap ? g_rem_cap * 2 : 64;
    g_remset = xrealloc(g_remset, g_rem_cap * sizeof(void *));
    g_rem_dirty = xrealloc(g_rem_dirty, g_rem_cap * sizeof(uint8_t));
  }
  g_rem_dirty[g_rem_n] = 0;
  g_remset[g_rem_n++] = obj;
  g_rem_adds++;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

static void rem_forget(void) {
  uint32_t i;
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    if (is_heap_payload(obj, NULL, NULL)) {
      avant_header(obj)->flags &= (uint16_t)~AVANT_FLAG_REM;
    }
  }
  g_rem_n = 0;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

static void fwd_reset(void) {
  if (!g_fwd_cap) {
    g_fwd_cap = 1024;
    g_fwd = calloc(g_fwd_cap, sizeof(FwdEnt));
    if (!g_fwd) {
      die("out of memory");
    }
  } else {
    memset(g_fwd, 0, g_fwd_cap * sizeof(FwdEnt));
  }
  g_fwd_used = 0;
}

static void fwd_grow(void) {
  size_t old_cap = g_fwd_cap;
  FwdEnt *old = g_fwd;
  g_fwd_cap *= 2;
  g_fwd = calloc(g_fwd_cap, sizeof(FwdEnt));
  if (!g_fwd) {
    die("out of memory");
  }
  size_t i;
  for (i = 0; i < old_cap; i++) {
    if (!old[i].from) {
      continue;
    }
    size_t j = hash_ptr(old[i].from) & (g_fwd_cap - 1);
    while (g_fwd[j].from) {
      j = (j + 1) & (g_fwd_cap - 1);
    }
    g_fwd[j] = old[i];
  }
  free(old);
}

void fwd_put(void *from, void *to) {
  if (g_fwd_used * 10 > g_fwd_cap * 7) {
    fwd_grow();
  }
  size_t j = hash_ptr(from) & (g_fwd_cap - 1);
  while (g_fwd[j].from && g_fwd[j].from != from) {
    j = (j + 1) & (g_fwd_cap - 1);
  }
  if (!g_fwd[j].from) {
    g_fwd_used++;
  }
  g_fwd[j].from = from;
  g_fwd[j].to = to;
}

void *fwd_get(void *from) {
  if (!g_fwd_cap || !from) {
    return NULL;
  }
  size_t j = hash_ptr(from) & (g_fwd_cap - 1);
  for (;;) {
    if (!g_fwd[j].from) {
      return NULL;
    }
    if (g_fwd[j].from == from) {
      return g_fwd[j].to;
    }
    j = (j + 1) & (g_fwd_cap - 1);
  }
}

static void block_register(Block *b) {
  size_t k = block_hash(b->base);
  b->hash_next = g_bht[k];
  g_bht[k] = b;
}

static void block_unregister(Block *b) {
  size_t k = block_hash(b->base);
  Block **pp = &g_bht[k];
  while (*pp) {
    if (*pp == b) {
      *pp = b->hash_next;
      b->hash_next = NULL;
      return;
    }
    pp = &(*pp)->hash_next;
  }
}

static int block_live_lines(Block *b) {
  int n = 0;
  int i;
  for (i = 0; i < LINES_PER_BLOCK; i++) {
    if (b->line_mark[i]) {
      n++;
    }
  }
  return n;
}

size_t obj_total(const AvantHeader *h) {
  return align_up(sizeof(AvantHeader) + (size_t)h->size);
}

static void large_hash_add(Large *L) {
  size_t k = large_key(payload_of(&L->header));
  L->hash_next = g_large_ht[k];
  g_large_ht[k] = L;
}

static void large_hash_del(Large *L) {
  size_t k = large_key(payload_of(&L->header));
  Large **pp = &g_large_ht[k];
  while (*pp) {
    if (*pp == L) {
      *pp = L->hash_next;
      L->hash_next = NULL;
      return;
    }
    pp = &(*pp)->hash_next;
  }
}

static size_t large_chunk(const Large *L) {
  return align_up(sizeof(Large) + (size_t)L->header.size);
}

static void los_spare_drop(void) {
  size_t chunk;
  if (!g_los_spare) {
    return;
  }
  chunk = large_chunk(g_los_spare);
  free(g_los_spare);
  g_los_spare = NULL;
  g_heap_bytes -= chunk;
  g_los_spare_dropped++;
}

static void unlink_large(Large *L) {
  Large **pp = &g_large;
  large_hash_del(L);
  while (*pp) {
    if (*pp == L) {
      *pp = L->next;
      break;
    }
    pp = &(*pp)->next;
  }
  L->next = NULL;
}

/* keep_bytes leaves the allocation charged, for the spare slot. */
static void account_large_gone(Large *L, int keep_bytes) {
  size_t chunk = large_chunk(L);
  if (L->header.flags & AVANT_FLAG_YOUNG) {
    if (g_young_large >= chunk) {
      g_young_large -= chunk;
    }
    if (g_young_large_n > 0) {
      g_young_large_n--;
    }
  } else if (g_mature_since_full >= chunk) {
    g_mature_since_full -= chunk;
  }
  if (!keep_bytes) {
    g_heap_bytes -= chunk;
  }
}

static void release_large(Large *L) {
  int cache = (L->header.flags & AVANT_FLAG_YOUNG) &&
              L->header.type_id == AVANT_TYPE_BYTES &&
              (L->header.flags & AVANT_FLAG_PINNED) == 0 &&
              g_los_spare == NULL;
  unlink_large(L);
  account_large_gone(L, cache);
  if (cache) {
    L->hash_next = NULL;
    g_los_spare = L;
    g_los_spare_cached++;
  } else {
    free(L);
    g_los_freed++;
  }
}

static void free_large(Large *L) {
  release_large(L);
}

static void los_note_live(void) {
  Large *L;
  uint64_t live = 0;
  for (L = g_large; L; L = L->next) {
    if (L->header.flags & AVANT_FLAG_MARKED) {
      live += (uint64_t)large_chunk(L);
    }
  }
  g_los_live_samples++;
  g_los_live_sum += live;
  if (live > g_los_live_peak) {
    g_los_live_peak = live;
  }
}

static void promote_young_large(Large *L) {
  size_t chunk = large_chunk(L);
  g_los_promoted++;
  g_los_promoted_bytes += (uint64_t)chunk;
  L->header.flags = (uint16_t)(L->header.flags &
                                (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_LOS_KEEP));
  if (g_young_large >= chunk) {
    g_young_large -= chunk;
  }
  if (g_young_large_n > 0) {
    g_young_large_n--;
  }
  g_mature_since_full += chunk;
}

/* Unmarked young large objects are unreachable and are freed. A marked
 * one is promoted when a heap object points at it, or when the stack
 * is holding a working set at least as big as the young limit. A
 * stack-only buffer stays young, so the next collection can free it
 * once that local is gone. */
static void reclaim_young_large(void) {
  Large *L;
  size_t stay_bytes = 0;
  los_note_live();
  for (L = g_large; L; L = L->next) {
    if ((L->header.flags & AVANT_FLAG_YOUNG) == 0) {
      continue;
    }
    if ((L->header.flags & AVANT_FLAG_MARKED) == 0) {
      continue;
    }
    if (L->header.flags & (AVANT_FLAG_PINNED | AVANT_FLAG_LOS_KEEP)) {
      continue;
    }
    stay_bytes += large_chunk(L);
  }
  int promote_stay = stay_bytes >= g_young_bytes;
  L = g_large;
  while (L) {
    Large *next = L->next;
    if (L->header.flags & AVANT_FLAG_YOUNG) {
      int marked = (L->header.flags & AVANT_FLAG_MARKED) != 0;
      int pinned = (L->header.flags & AVANT_FLAG_PINNED) != 0;
      int keep = (L->header.flags & AVANT_FLAG_LOS_KEEP) != 0;
      if (!marked && !pinned) {
        release_large(L);
      } else if (pinned || keep || promote_stay) {
        promote_young_large(L);
      } else {
        L->header.flags = (uint16_t)(L->header.flags & (uint16_t)~AVANT_FLAG_LOS_KEEP);
      }
    }
    L = next;
  }
}

int is_heap_payload(void *p, Block **ob, Large **ol) {
  if (!p || (uintptr_t)p % ALIGN) {
    return 0;
  }
  AvantHeader *h = avant_header(p);
  Block *b = block_of(h);
  if (b) {
    size_t off = (size_t)((uint8_t *)h - b->base);
    if (off % ALIGN || !has_start(b, off)) {
      return 0;
    }
    if (ob) {
      *ob = b;
    }
    if (ol) {
      *ol = NULL;
    }
    return 1;
  }
  Large *L = large_of(p);
  if (L) {
    if (ob) {
      *ob = NULL;
    }
    if (ol) {
      *ol = L;
    }
    return 1;
  }
  return 0;
}

/*
 * Object start, or the payload of the object that contains p.
 * myc FIELD/ADDR can root an interior slot; young copy must still
 * move the object and keep the same offset into it.
 */
static void *object_payload(void *p, Block **ob, Large **ol) {
  if (is_heap_payload(p, ob, ol)) {
    return p;
  }
  if (!p) {
    return NULL;
  }
  Block *b = block_of(p);
  if (b) {
    uint8_t *addr = (uint8_t *)p;
    if (addr < b->base || addr >= b->base + BLOCK_SIZE) {
      return NULL;
    }
    size_t off = (size_t)(addr - b->base);
    size_t bit = off / 8;
    for (;;) {
      size_t cand = bit * 8;
      if (has_start(b, cand)) {
        AvantHeader *h = (AvantHeader *)(b->base + cand);
        size_t total = obj_total(h);
        uint8_t *hs = (uint8_t *)h;
        if (addr >= hs && addr < hs + total) {
          if (ob) {
            *ob = b;
          }
          if (ol) {
            *ol = NULL;
          }
          return payload_of(h);
        }
        return NULL;
      }
      if (bit == 0) {
        return NULL;
      }
      bit--;
    }
  }
  Large *L;
  uint8_t *addr = (uint8_t *)p;
  if (addr < g_large_lo || addr >= g_large_hi) {
    return NULL; /* in no large chunk: the walk below would miss */
  }
  for (L = g_large; L; L = L->next) {
    uint8_t *hs = (uint8_t *)&L->header;
    size_t total = obj_total(&L->header);
    if (addr >= hs && addr < hs + total) {
      if (ob) {
        *ob = NULL;
      }
      if (ol) {
        *ol = L;
      }
      return payload_of(&L->header);
    }
  }
  return NULL;
}

int avant_is_heap(void *payload) {
  return is_heap_payload(payload, NULL, NULL);
}

_Atomic uint32_t g_avant_str_gen;

/* 1 when a young collection can move this buffer. Mature and large
 * objects stay put, so a parallel numeric loop can keep the address. */
int32_t avant_is_young(void *payload) {
  AvantHeader *h;
  if (!payload || ((uintptr_t)payload & (ALIGN - 1)) != 0) {
    return 0;
  }
  h = avant_header(payload);
  /* A large object never moves, even when it is still in the young set. */
  if (h->flags & AVANT_FLAG_LARGE) {
    return 0;
  }
  if ((h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_HEAP)) ==
      (AVANT_FLAG_YOUNG | AVANT_FLAG_HEAP)) {
    return 1;
  }
  return 0;
}

static int in_nursery(void *p) {
  Block *b = NULL;
  if (!is_heap_payload(p, &b, NULL)) {
    return 0;
  }
  return b && b->space == SPACE_NURSERY;
}

static int is_old(void *p) {
  return !in_nursery(p);
}

static void foreach_block_object(Block *b, void (*fn)(AvantHeader *, Block *)) {
  int w;
  for (w = 0; w < START_WORDS; w++) {
    uint64_t bits = b->start[w];
    while (bits) {
      int bit = __builtin_ctzll(bits);
      size_t off = ((size_t)w * 64 + (size_t)bit) * 8;
      fn((AvantHeader *)(b->base + off), b);
      bits &= bits - 1;
    }
  }
}

static void clear_mark_one(AvantHeader *h, Block *b) {
  (void)b;
  h->flags &= (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
}

static void clear_all_marks(void) {
  Block *b;
  for (b = g_mature; b; b = b->next) {
    foreach_block_object(b, clear_mark_one);
  }
  for (b = g_nursery; b; b = b->next) {
    foreach_block_object(b, clear_mark_one);
  }
  for (b = g_from; b; b = b->next) {
    foreach_block_object(b, clear_mark_one);
  }
  Large *L;
  for (L = g_large; L; L = L->next) {
    L->header.flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_LOS_KEEP);
  }
}

static void zero_line_marks(Block *list) {
  Block *b;
  for (b = list; b; b = b->next) {
    memset(b->line_mark, 0, sizeof(b->line_mark));
  }
}

void mark_object_lines(void *p) {
  Block *b = NULL;
  if (!is_heap_payload(p, &b, NULL) || !b) {
    return;
  }
  AvantHeader *h = avant_header(p);
  mark_lines(b, (size_t)((uint8_t *)h - b->base), obj_total(h));
}

static void scan_payload(void *p);

static void mark(void *p) {
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  mark_object_lines(start);
  g_live += obj_total(h);
  work_push(start);
}

static void drain(void) {
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    scan_payload(p);
  }
}

static void mark_tls_slot(void **slot) {
  if (slot) {
    mark(*slot);
  }
}

static void scan_payload(void *p) {
  EACH_REF_SLOT(p, slot, { mark(*slot); });
}

static void mark_roots(void) {
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        mark(*f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
  for (PinEnt *p = g_pins; p; p = p->next) {
    mark(p->payload);
  }
  avant_hash_walk_tls(mark_tls_slot);
  drain();
}

static int find_hole(Block *b, size_t need) {
  int i = b->search;
  while (i < LINES_PER_BLOCK) {
    if (b->line_mark[i]) {
      i++;
      continue;
    }
    int j = i;
    while (j < LINES_PER_BLOCK && !b->line_mark[j]) {
      j++;
    }
    size_t hole = (size_t)(j - i) * LINE_SIZE;
    if (hole >= need) {
      b->bump = b->base + (size_t)i * LINE_SIZE;
      b->limit = b->base + (size_t)j * LINE_SIZE;
      return 1;
    }
    i = j;
  }
  return 0;
}

static void reset_block_bump(Block *b) {
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base;
  if (!find_hole(b, ALIGN)) {
    b->bump = b->base + BLOCK_SIZE;
    b->limit = b->base + BLOCK_SIZE;
    b->search = LINES_PER_BLOCK;
  }
}

static void clear_alloc_cur(uint8_t space);

static void cache_or_free_block(Block *b) {
  if (g_alloc_cur[SPACE_NURSERY] == b) {
    clear_alloc_cur(SPACE_NURSERY);
  }
  if (g_alloc_cur[SPACE_MATURE] == b) {
    clear_alloc_cur(SPACE_MATURE);
  }
  if (g_alloc_cur[SPACE_TO] == b) {
    clear_alloc_cur(SPACE_TO);
  }
  if (g_cur == b) {
    g_cur = NULL;
    g_bump = NULL;
    g_limit = NULL;
  }
  if (g_fast_block == b) {
    gc_fast_clear();
  }
  block_unregister(b);
  if (g_cache_n < CACHE_BLOCKS) {
    memset(b->line_mark, 0, sizeof(b->line_mark));
    memset(b->start, 0, sizeof(b->start));
    b->next = g_cache;
    g_cache = b;
    g_cache_n++;
    return;
  }
  free(b->base);
  free(b);
  g_heap_bytes -= BLOCK_SIZE;
}

static uint64_t space_block_bytes(Block *b) {
  uint64_t n = 0;
  for (; b; b = b->next) {
    n += BLOCK_SIZE;
  }
  return n;
}

/* Wave 4 step 1: when the heap high-water mark moves, snapshot what
 * the heap is made of, so a peak can be decomposed after the fact. */
static void note_peak(void) {
  Large *L;
  uint64_t large = 0;
  if (g_heap_bytes <= g_stats.peak_heap) {
    return;
  }
  g_stats.peak_heap = g_heap_bytes;
  g_pk_nursery = space_block_bytes(g_nursery);
  g_pk_mature = space_block_bytes(g_mature);
  g_pk_from = space_block_bytes(g_from);
  g_pk_to = space_block_bytes(g_to);
  g_pk_cache = (uint64_t)g_cache_n * BLOCK_SIZE;
  for (L = g_large; L; L = L->next) {
    large += align_up(sizeof(Large) + L->header.size);
  }
  g_pk_large = large;
}

/* Wave 4 step 2: outside a collection the copy reserve counts as
 * spent, so fresh block growth stops early enough that the next copy
 * has its destination room under the heap limit. The charge is soft:
 * it makes allocation collect earlier, never fail, so the retry after
 * a collection (g_reserve_retry) and any allocation inside a
 * collection (where the reserve is what is being spent) are charged
 * only the true heap bytes. Cached blocks are already owned and are
 * never refused by this test. */
static int g_reserve_retry;
static int heap_block_fits(void) {
  uint64_t spent;
  if (!g_heap_max) {
    return 1;
  }
  spent = g_heap_bytes + BLOCK_SIZE;
  if (!g_collecting && !g_reserve_retry && g_reserve_mode) {
    spent += g_copy_reserve;
  }
  return spent <= g_heap_max;
}

static Block *new_block(uint8_t space) {
  if (!heap_block_fits() && g_cache == NULL) {
    return NULL;
  }
  Block *b = NULL;
  if (g_cache) {
    b = g_cache;
    g_cache = b->next;
    g_cache_n--;
    memset(b->line_mark, 0, sizeof(b->line_mark));
    memset(b->start, 0, sizeof(b->start));
    b->next = NULL;
    b->hash_next = NULL;
  } else {
    if (!heap_block_fits()) {
      return NULL;
    }
    b = calloc(1, sizeof(Block));
    if (!b) {
      die("out of memory");
    }
    void *mem = NULL;
    if (posix_memalign(&mem, BLOCK_SIZE, BLOCK_SIZE) != 0) {
      die("out of memory");
    }
    b->base = mem;
    g_heap_bytes += BLOCK_SIZE;
    note_peak();
  }
  /* Block memory is not pre-zeroed. Every allocation zeroes its own
   * payload in init_object, so a fresh or recycled block costs no
   * whole-block clearing pass ahead of the mutator's own writes. */
  b->space = space;
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base + BLOCK_SIZE;
  block_register(b);
  if (space == SPACE_NURSERY) {
    b->next = g_nursery;
    g_nursery = b;
  } else if (space == SPACE_TO) {
    b->next = g_to;
    g_to = b;
  } else {
    b->next = g_mature;
    g_mature = b;
  }
  return b;
}

static void use_block(Block *b) {
  g_cur = b;
  g_bump = b->bump;
  g_limit = b->limit;
  if (b->space == SPACE_NURSERY) {
    /* Keep the inline fast path's mirror on the block the cursor is
     * actually in. A slow allocation that switches blocks would
     * otherwise leave generated code writing start bits through the
     * previous block's base. A recycled block clears the fast path:
     * its start bits need the clearing only bump_take does. */
    if (!g_collecting && b == g_alloc_cur[SPACE_NURSERY] &&
        g_alloc_fresh[SPACE_NURSERY]) {
      uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
      uint8_t *cap = g_bump + rem;
      g_fast_block = b;
      avant_gc_base = b->base;
      avant_gc_starts = b->start;
      avant_gc_block = b;
      avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
    } else {
      gc_fast_clear();
    }
  }
}

static int bump_fits(size_t need) {
  return g_cur && g_bump && g_bump + need <= g_limit;
}

static void *bump_take(size_t need) {
  Block *b = g_cur;
  size_t off = (size_t)(g_bump - b->base);
  /* A fresh block is zeroed and only bumps forward. Clearing start bits
   * is for a hole in a recycled block. */
  if (!g_alloc_fresh[b->space]) {
    clear_starts(b, off, need);
  }
  set_start(b, off);
  void *h = g_bump;
  g_bump += need;
  b->bump = g_bump;
  if (b == g_fast_block) {
    uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
    uint8_t *cap = g_bump + rem;
    avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
  }
  return h;
}

static void gc_fast_clear(void) {
  avant_gc_fast_ok = 0;
  avant_gc_limit = NULL;
  g_fast_block = NULL;
}

/* Publish the inline fast path when every C fast-path condition holds
 * and the current nursery block is fresh (a recycled block needs the
 * start-bit clearing only bump_take does). Anything else clears it, so
 * generated code only ever checks fast_ok, the shared flag, and the
 * cursor against the limit. Called at slow allocation entries; state
 * transitions clear eagerly below. */
static void gc_fast_refresh(void) {
  Block *cur = g_alloc_cur[SPACE_NURSERY];
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 &&
      !g_gc_defer && !g_collecting && !g_nursery_off && !g_alloc_mature &&
      g_mature_since_full < FULL_BYTES && cur && g_cur == cur &&
      g_alloc_fresh[SPACE_NURSERY] && g_bump) {
    uint64_t rem = g_young_bytes > g_nursery_held ? g_young_bytes - g_nursery_held : 0;
    uint8_t *cap = g_bump + rem;
    g_fast_block = cur;
    avant_gc_base = cur->base;
    avant_gc_starts = cur->start;
    avant_gc_block = cur;
    avant_gc_limit = g_limit && cap > g_limit ? g_limit : cap;
    avant_gc_fast_ok = 1;
  } else {
    gc_fast_clear();
  }
}

static int try_bump(Block *b, size_t total) {
  use_block(b);
  if (bump_fits(total)) {
    return 1;
  }
  b->search = (uint16_t)((b->limit - b->base) / LINE_SIZE);
  if (!find_hole(b, total)) {
    return 0;
  }
  use_block(b);
  return bump_fits(total);
}

static void clear_alloc_cur(uint8_t space) {
  g_alloc_cur[space] = NULL;
  g_alloc_fresh[space] = 0;
}

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

static void collect(int full);

static AvantHeader *place(size_t total, uint8_t space, int tried_gc) {
  Block *cur = g_alloc_cur[space];
  if (cur && try_bump(cur, total)) {
    return bump_take(total);
  }
  /* A fresh block has no holes behind it. Scanning the list again
   * walks every full block. Recycle only after a collection cleared
   * the fresh bit, and then only forward from the cursor. */
  if (!g_alloc_fresh[space]) {
    Block *list = g_mature;
    if (space == SPACE_NURSERY) {
      list = g_nursery;
    } else if (space == SPACE_TO) {
      list = g_to;
    }
    Block *b = cur ? cur->next : list;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        g_alloc_cur[space] = b;
        return bump_take(total);
      }
    }
  }
  Block *b = new_block(space);
  if (!b) {
    if (!tried_gc && !g_collecting) {
      /* The reserve charge is soft (see heap_block_fits): collect,
       * then retry charged only the true heap bytes, so a reserve
       * refusal can never become a spurious heap-limit death. */
      collect(1);
      g_reserve_retry = 1;
      {
        AvantHeader *retry = place(total, space, 1);
        g_reserve_retry = 0;
        return retry;
      }
    }
    die("heap limit");
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
  use_block(b);
  if (!bump_fits(total)) {
    die("object larger than block");
  }
  return bump_take(total);
}

/* Like place into the nursery, but never collects. */
static AvantHeader *place_bump(size_t total) {
  uint8_t space = SPACE_NURSERY;
  Block *cur;
  Block *b;
  if (g_nursery_off || g_alloc_mature || total > LOS_LIMIT) {
    return NULL;
  }
  cur = g_alloc_cur[space];
  /* Steady state: the global cursor already mirrors the nursery block.
   * Take from it directly instead of re-syncing through try_bump. */
  if (cur && g_cur == cur && g_bump && g_bump + total <= g_limit) {
    return (AvantHeader *)bump_take(total);
  }
  if (cur && try_bump(cur, total)) {
    return (AvantHeader *)bump_take(total);
  }
  if (!g_alloc_fresh[space]) {
    b = cur ? cur->next : g_nursery;
    for (; b; b = b->next) {
      if (try_bump(b, total)) {
        g_alloc_cur[space] = b;
        return (AvantHeader *)bump_take(total);
      }
    }
  }
  b = new_block(space);
  if (!b) {
    return NULL;
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
  use_block(b);
  if (!bump_fits(total)) {
    return NULL;
  }
  return (AvantHeader *)bump_take(total);
}

static void *mature_alloc(size_t total) {
  /* Young copy promotes into mature space while g_collecting is set.
   * tried_gc stays 1 so a failed block does not re-enter collect. */
  g_mature_since_full += total;
  return place(total, SPACE_MATURE, 1);
}

static AvantHeader *to_alloc(size_t total) {
  AvantHeader *h = place(total, SPACE_TO, 1);
  g_to_bytes += total;
  return h;
}

static void keep_add(void *obj) {
  if (g_keep_n == g_keep_cap) {
    g_keep_cap = g_keep_cap ? g_keep_cap * 2 : 64;
    g_keep = xrealloc(g_keep, g_keep_cap * sizeof(void *));
  }
  g_keep[g_keep_n++] = obj;
}

static void note_marked(void *payload) {
  if (g_noted_n == g_noted_cap) {
    g_noted_cap = g_noted_cap ? g_noted_cap * 2 : 256;
    g_noted = xrealloc(g_noted, g_noted_cap * sizeof(void *));
  }
  g_noted[g_noted_n++] = payload;
}

static void clear_noted(void) {
  uint32_t i;
  for (i = 0; i < g_noted_n; i++) {
    avant_header(g_noted[i])->flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
  }
  g_noted_n = 0;
}

static void probe_note(void *start) {
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  note_marked(start);
  g_probe_nursery_bytes += obj_total(h);
  work_push(start);
}

/* The slot now names a young object. If that slot lives in a mature
 * object, the next young collection has to find it from the remembered
 * set: the mature object is not walked from the roots. */
static void remember_young_edge(void **slot, void *payload) {
  Block *pb = NULL;
  Block *ob = NULL;
  Large *ol = NULL;
  void *owner;
  if (!is_heap_payload(payload, &pb, NULL) || !pb) {
    return;
  }
  if (pb->space != SPACE_NURSERY && pb->space != SPACE_TO) {
    return;
  }
  owner = object_payload((void *)slot, &ob, &ol);
  if (!owner) {
    return;
  }
  /* A pinned nursery owner is not copied: it stays in its block and is
   * promoted in place when this collection ends (promote_pinned_one),
   * so its young edge needs the remembered set like a mature owner's. */
  if (ob && (ob->space == SPACE_NURSERY || ob->space == SPACE_TO) &&
      (avant_header(owner)->flags & AVANT_FLAG_PINNED) == 0) {
    return;
  }
  keep_add(owner);
}

static int g_from_heap_slot;
/* Set while a remembered object is being scanned: copy_from_slot then
 * records whether any direct field names a young object afterwards. */
static int g_rem_track;
static int g_rem_track_dirty;

static int names_young(void *p) {
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(p, &b, &ol);
  if (!start) {
    return 0;
  }
  if (ol) {
    return (avant_header(start)->flags & AVANT_FLAG_YOUNG) != 0;
  }
  return b && (b->space == SPACE_NURSERY || b->space == SPACE_TO);
}
static void trace_mature(void *p, int from_mature);

static void copy_payload_children(void *p) {
  if (avant_header(p)->type_id == AVANT_TYPE_BYTES) {
    return;
  }
  int saved = g_from_heap_slot;
  g_from_heap_slot = 1;
  EACH_REF_SLOT(p, slot, { copy_from_slot(slot); });
  g_from_heap_slot = saved;
}

/* A remembered object is an old-to-young edge, not a root. Scanning
 * every remembered buffer keeps a dead array's nodes alive: the
 * calculator replaces that array each iteration, the buffer is a large
 * object, and the young collection was tenuring the whole history.
 * Scan a mature or large object when a root or a young object points
 * at it. From inside a mature object, scan a remembered child or a
 * young large child. A mature tree that is not remembered is not walked. */
static void note_los_keep(AvantHeader *h, int from_mature) {
  if ((h->flags & AVANT_FLAG_YOUNG) == 0) {
    return;
  }
  if (from_mature || g_from_heap_slot) {
    h->flags |= AVANT_FLAG_LOS_KEEP;
  }
}

static void trace_mature(void *p, int from_mature) {
  AvantHeader *h = avant_header(p);
  int saved;
  if (h->flags & AVANT_FLAG_MARKED) {
    note_los_keep(h, from_mature);
    return;
  }
  /* A mature parent's young child is usually in the nursery and is
   * copied before this function runs. A young large object is not in a
   * block, so it arrives here. Leaving it unmarked lets the young
   * collection free a string the parent array still holds. */
  if (from_mature && (h->flags & AVANT_FLAG_REM) == 0 && (h->flags & AVANT_FLAG_YOUNG) == 0) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  note_los_keep(h, from_mature);
  if (g_probe) {
    note_marked(p);
  }
  saved = g_from_mature;
  g_from_mature = 1;
  /* Nested mature scans do not dirty the remembered parent: only its
   * direct fields count, and the outer copy_from_slot judges that slot
   * itself once this returns. */
  {
    int track_saved = g_rem_track;
    g_rem_track = 0;
    copy_payload_children(p);
    g_rem_track = track_saved;
  }
  g_from_mature = saved;
}

void *copy_nursery(void *p) {
  Block *b = NULL;
  if (g_probe) {
    die("probe copied");
  }
  if (!is_heap_payload(p, &b, NULL) || !b || b->space != SPACE_NURSERY) {
    return p;
  }
  void *to = fwd_get(p);
  if (to) {
    return to;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_PINNED) {
    g_saw_pinned_nursery = 1;
    if (!(h->flags & AVANT_FLAG_MARKED)) {
      h->flags |= AVANT_FLAG_MARKED;
      mark_object_lines(p);
      g_live += obj_total(h);
      work_push(p);
    }
    return p;
  }
  size_t total = obj_total(h);
  /* Tenure only after a previous collection, or when the caller has
   * already decided this graph is long-lived. A first survivor stays
   * in the nursery (SPACE_TO) so a tree that dies next cycle is not
   * promoted. */
  int tenure = g_force_tenure || (h->flags & AVANT_FLAG_SURVIVED);
  AvantHeader *nh = tenure ? (AvantHeader *)mature_alloc(total) : to_alloc(total);
  memcpy(nh, h, total);
  nh->flags = (uint16_t)(h->flags &
                         (AVANT_FLAG_PINNED | AVANT_FLAG_HEAP | AVANT_FLAG_STRMETA));
  nh->flags |= AVANT_FLAG_MARKED;
  if (!tenure) {
    nh->flags |= (uint16_t)(AVANT_FLAG_SURVIVED | AVANT_FLAG_YOUNG);
  }
  void *np = payload_of(nh);
  fwd_put(p, np);
  g_stats.copied_bytes += total;
  g_live += total;
  mark_object_lines(np);
  work_push(np);
  return np;
}

void copy_from_slot(void **slot) {
  void *p = *slot;
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  if (start != p && !g_probe) {
    g_interior_slots++;
  }
  if (b && b->space == SPACE_NURSERY) {
    if (g_probe) {
      /* A nursery child is a young edge by definition; the probe must
       * record it, or the frontier would evict this parent as clean. */
      if (g_rem_track) {
        g_rem_track_dirty = 1;
      }
      probe_note(start);
      return;
    }
    ptrdiff_t delta = (uint8_t *)p - (uint8_t *)start;
    void *np = copy_nursery(start);
    *slot = (uint8_t *)np + delta;
    remember_young_edge(slot, np);
    if (g_rem_track && names_young(*slot)) {
      g_rem_track_dirty = 1;
    }
    return;
  }
  /* Mature and large objects stay put. Follow one when this slot is a
   * root or a field of a young object. A field of a mature object is
   * followed only when that child is remembered. A live object in a
   * from-space block is a pinned survivor promoted in place
   * (promote_pinned_one): it is mature, and its young children are
   * reached the same way (C29). */
  if (ol || (b && (b->space == SPACE_MATURE || b->space == SPACE_FROM))) {
    trace_mature(start, g_from_mature);
  }
  if (g_rem_track && names_young(*slot)) {
    g_rem_track_dirty = 1;
  }
}

static void relocate_root_location(void ***slot_loc) {
  void *addr = (void *)*slot_loc;
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(addr, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return;
  }
  if (start != addr) {
    g_interior_slots++;
  }
  ptrdiff_t delta = (uint8_t *)addr - (uint8_t *)start;
  void *np = copy_nursery(start);
  *slot_loc = (void **)((uint8_t *)np + delta);
}

/* Frontier decision for one live, marked hot entry whose scan result
 * is dirty (a direct field names a young object). Returns 1 to keep the
 * entry. On 0 the caller drops it with REM already clear; the barrier
 * re-adds it on the next young store. Caller holds g_rem_mu when the
 * set is shared. */
static int rem_frontier_decide(void *obj, int dirty) {
  if (g_rem_mode == 0 || dirty) {
    return 1;
  }
  avant_header(obj)->flags &= (uint16_t)~AVANT_FLAG_REM;
  g_rem_cleared++;
  return 0;
}

static void rem_scrub(void) {
  uint32_t i;
  uint32_t w = 0;
  int locked = rem_locked();
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    AvantHeader *h;
    if (!is_heap_payload(obj, NULL, NULL)) {
      continue;
    }
    h = avant_header(obj);
    if (h->flags & AVANT_FLAG_MARKED) {
      int dirty = g_rem_dirty ? g_rem_dirty[i] : 1;
      if (!rem_frontier_decide(obj, dirty)) {
        continue;
      }
      g_remset[w] = obj;
      if (g_rem_dirty) {
        g_rem_dirty[w] = (uint8_t)dirty;
      }
      w++;
    } else if ((h->flags & AVANT_FLAG_PINNED) == 0) {
      Large *L = large_of(obj);
      /* A dead expression buffer is a large object. Leaving it on the
       * large list makes every missed pointer walk that list. */
      if (L) {
        free_large(L);
      } else {
        h->flags &= (uint16_t)~AVANT_FLAG_REM;
      }
    } else {
      h->flags &= (uint16_t)~AVANT_FLAG_REM;
    }
  }
  g_rem_n = w;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

/* Field slots copy_payload_children would visit for one object. */
static uint64_t rem_field_count(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_BYTES) {
    return 0;
  }
  if (id == AVANT_TYPE_PTRS) {
    return (uint64_t)((size_t)h->size / sizeof(void *));
  }
  if (id >= MAX_TYPES || !g_type_set[id]) {
    return 0;
  }
  if (g_types[id].stride) {
    return (uint64_t)g_types[id].nrefs *
           (uint64_t)((size_t)h->size / sizeof(void *) / g_types[id].stride);
  }
  return (uint64_t)g_types[id].nrefs;
}

static void gen_young_copy(void) {
  if (!g_probe) {
    fwd_reset();
  }
  g_saw_pinned_nursery = 0;
  g_from_mature = 0;
  g_work_n = 0;
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        if (!g_probe) {
          relocate_root_location(&f->slots[i]);
        }
        copy_from_slot(f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
  /* Remembered mature objects are roots for the young generation.
   * Their nursery children must be copied and their fields updated
   * even when no root or young object reaches them this cycle: a
   * mature tree interior that tenured together with its children is
   * otherwise never scanned again, and its fields keep pointing at
   * from-space addresses that later collections reuse. */
  {
    uint32_t ri;
    int locked = rem_locked();
    if (locked) {
      pthread_mutex_lock(&g_rem_mu);
    }
    g_rem_scans++;
    g_rem_entries += g_rem_n;
    if (g_rem_n > g_rem_peak) {
      g_rem_peak = g_rem_n;
    }
    for (ri = 0; ri < g_rem_n; ri++) {
      void *obj = g_remset[ri];
      if (is_heap_payload(obj, NULL, NULL)) {
        /* A remembered object is a root this cycle: mark it so the
         * probe pass's rem_scrub keeps it in the set. */
        avant_header(obj)->flags |= AVANT_FLAG_MARKED;
        g_rem_fields += rem_field_count(obj);
        g_rem_track = 1;
        g_rem_track_dirty = 0;
        copy_payload_children(obj);
        g_rem_track = 0;
        if (g_rem_dirty) {
          g_rem_dirty[ri] = (uint8_t)g_rem_track_dirty;
        }
      }
    }
    if (locked) {
      pthread_mutex_unlock(&g_rem_mu);
    }
  }
  if (!g_probe) {
    copy_from_c_stack();
  }
  copy_from_stackmaps();
  /*
   * D34: pinned objects are mark roots. Full collect already walks
   * g_pins; young copy must too. C globals (argv, interned bytes)
   * hold those pointers and would otherwise see a freed nursery.
   */
  for (PinEnt *p = g_pins; p; p = p->next) {
    copy_from_slot(&p->payload);
  }
  avant_hash_walk_tls(copy_from_slot);
  /* Remembered objects are reached from the roots above. The set is
   * not itself a root list: a dead buffer would keep its nodes alive. */
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    copy_payload_children(p);
  }
  if (g_probe) {
    return;
  }
  /*
   * Mapped stackmap slots were roots above. Extra LLVM spills of the
   * same pointer bits are not in gc-live; rewrite them now that every
   * copied object is in the forwarding table.
   */
  rewrite_from_stackmaps();
  rewrite_c_stack();
  count_c_stack_nursery();
}

static int g_block_has_pin;

/*
 * C29 (Stage 28 W8c). A pinned nursery object survives a young copy in
 * place, and its block stays as SPACE_FROM. It is promoted in place
 * here: YOUNG and SURVIVED are cleared, so from now on it is a mature
 * object that happens to live in a from-space block. That restores
 * the generational invariant for it: the barrier (inline and
 * avant_barrier) remembers it when a young child is stored into it,
 * copy_from_slot follows it like a mature object (a root, a young
 * parent, or a remembered parent reaches it), and remember_young_edge
 * keeps it remembered while its fields name young objects. Before,
 * it kept YOUNG (so no barrier) while no later young collection
 * looked inside it (copy_nursery skips non-nursery blocks), and its
 * young children were discarded with their blocks.
 */
static void promote_pinned_one(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    g_block_has_pin = 1;
    h->flags &= (uint16_t)~(AVANT_FLAG_YOUNG | AVANT_FLAG_SURVIVED);
  }
}

static void note_pinned_in_block(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_PINNED) {
    g_block_has_pin = 1;
  }
}

static int block_has_pinned(Block *b) {
  g_block_has_pin = 0;
  foreach_block_object(b, note_pinned_in_block);
  return g_block_has_pin;
}

static int block_keep_pinned(Block *b) {
  g_block_has_pin = 0;
  foreach_block_object(b, promote_pinned_one);
  return g_block_has_pin;
}

/*
 * After young copy, free unpinned nursery blocks. Pinned objects keep
 * their address (D34); those blocks stay on g_from as SPACE_FROM.
 *
 * AVANT_NURSERY_DISCARD=0 parks every nursery block instead. That was
 * the Stage 17 safety net before stackmap leftover rewrite; it is not
 * the product path.
 */
static void park_nursery_from(void) {
  Block *b = g_nursery;
  g_nursery = NULL;
  while (b) {
    Block *next = b->next;
    if (g_saw_pinned_nursery) {
      (void)block_keep_pinned(b);
    }
    b->space = SPACE_FROM;
    b->next = g_from;
    g_from = b;
    b = next;
  }
}

static void discard_nursery_from(void) {
  Block *b = g_nursery;
  g_nursery = NULL;
  /* Pins are roots, so a young copy already saw every pinned nursery
   * object. Walking every dead object to recheck the flag was most of
   * a calculator collection: the tree is garbage and nothing is pinned. */
  if (!g_saw_pinned_nursery) {
    while (b) {
      Block *next = b->next;
      cache_or_free_block(b);
      b = next;
    }
    return;
  }
  while (b) {
    Block *next = b->next;
    if (block_keep_pinned(b)) {
      b->space = SPACE_FROM;
      b->next = g_from;
      g_from = b;
    } else {
      cache_or_free_block(b);
    }
    b = next;
  }
}

static int keep_if_live(Block *b) {
  if (block_live_lines(b) == 0) {
    return 0;
  }
  reset_block_bump(b);
  return 1;
}

static void filter_blocks(Block **list, int (*keep)(Block *)) {
  Block *b = *list;
  Block *kept = NULL;
  while (b) {
    Block *next = b->next;
    if (keep && keep(b)) {
      b->next = kept;
      kept = b;
    } else {
      cache_or_free_block(b);
    }
    b = next;
  }
  *list = kept;
}

static void sweep_large(void) {
  Large **pp = &g_large;
  los_note_live();
  while (*pp) {
    Large *L = *pp;
    if (L->header.flags & AVANT_FLAG_MARKED) {
      pp = &L->next;
    } else {
      size_t chunk = align_up(sizeof(Large) + (size_t)L->header.size);
      large_hash_del(L);
      *pp = L->next;
      free(L);
      g_heap_bytes -= chunk;
      g_los_freed++;
    }
  }
}

static void clear_mark_flag_one(AvantHeader *h, Block *b) {
  (void)b;
  h->flags &= (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
}

static void clear_mark_flags(void) {
  Block *b;
  Large *L;
  for (b = g_mature; b; b = b->next) {
    foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_nursery; b; b = b->next) {
    foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_from; b; b = b->next) {
    foreach_block_object(b, clear_mark_flag_one);
  }
  for (b = g_to; b; b = b->next) {
    foreach_block_object(b, clear_mark_flag_one);
  }
  for (L = g_large; L; L = L->next) {
    L->header.flags &=
        (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED | AVANT_FLAG_LOS_KEEP);
  }
}

static uint64_t g_mark_sum;

static void sum_marked_one(AvantHeader *h, Block *b) {
  (void)b;
  if (h->flags & AVANT_FLAG_MARKED) {
    g_mark_sum += obj_total(h);
  }
}

static void adopt_to_space(void) {
  Block *head = g_to;
  Block *b = g_to;
  g_to = NULL;
  while (b) {
    Block *next = b->next;
    b->space = SPACE_NURSERY;
    b->next = g_nursery;
    g_nursery = b;
    b = next;
  }
  clear_alloc_cur(SPACE_TO);
  clear_alloc_cur(SPACE_NURSERY);
  if (head && head->bump < head->base + BLOCK_SIZE) {
    g_alloc_cur[SPACE_NURSERY] = head;
    g_alloc_fresh[SPACE_NURSERY] = 1;
    use_block(head);
  }
}

static void rem_commit_kept(void) {
  uint32_t n = g_keep_n;
  void **kept = g_keep;
  uint32_t i;
  uint32_t w = 0;
  int locked = rem_locked();
  g_keep = NULL;
  g_keep_n = 0;
  g_keep_cap = 0;
  /* A remembered object with a nursery child stays remembered: the
   * scan just proved that edge. A live member with no nursery child
   * leaves the hot set under the frontier policy (the barrier re-adds
   * it on the next young store); only members whose memory is gone
   * leave unconditionally, as before. */
  if (locked) {
    pthread_mutex_lock(&g_rem_mu);
  }
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    if (is_heap_payload(obj, NULL, NULL)) {
      int dirty = g_rem_dirty ? g_rem_dirty[i] : 1;
      if (!rem_frontier_decide(obj, dirty)) {
        continue;
      }
      g_remset[w] = obj;
      if (g_rem_dirty) {
        g_rem_dirty[w] = (uint8_t)dirty;
      }
      w++;
    }
  }
  g_rem_n = w;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
  for (i = 0; i < n; i++) {
    if (is_heap_payload(kept[i], NULL, NULL)) {
      rem_add(kept[i]);
    }
  }
  free(kept);
}

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

static int g_verify;

static int verify_ptr(void *p, const char *where, void *holder, long off) {
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(p, &b, &ol);
  if (!start || !b || b->space != SPACE_NURSERY) {
    return 0;
  }
  void *to = fwd_get(start);
  if (to && to != start) {
    AvantHeader *ch = avant_header(start);
    fprintf(stderr,
            "avant gc verify: stale %s reference holder=%p off=%ld "
            "points at from-space %p (forwarded to %p) coll=%llu "
            "child_flags=0x%x\n",
            where, holder, off, start, to,
            (unsigned long long)g_stats.collections, ch->flags);
    return 1;
  }
  /* C29: a reachable reference to a nursery object the copy neither
   * moved nor kept in place. Its block is discarded after this check,
   * so the reference is about to dangle (a live holder the young copy
   * did not trace, or a missing barrier). */
  if (!to && (avant_header(start)->flags & AVANT_FLAG_PINNED) == 0) {
    fprintf(stderr,
            "avant gc verify: lost %s reference holder=%p off=%ld "
            "names nursery object %p the young copy did not keep coll=%llu "
            "child_flags=0x%x\n",
            where, holder, off, start, (unsigned long long)g_stats.collections,
            avant_header(start)->flags);
    return 1;
  }
  return 0;
}

static void verify_trace(void *p) {
  Block *b = NULL;
  Large *ol = NULL;
  void *start = object_payload(p, &b, &ol);
  if (!start) {
    return;
  }
  AvantHeader *h = avant_header(start);
  if (h->flags & AVANT_FLAG_FORWARDED) {
    return;
  }
  h->flags |= AVANT_FLAG_FORWARDED;
  work_push(start);
}

static void verify_tls_slot(void **slot) {
  verify_trace(*slot);
}

/* Independent reachability, ignoring the collector's own tracing
 * gates: a holder counts as live when a root reaches it through any
 * field. This is what keeps the checker sensitive to the F1 class
 * (a live holder the young copy itself never traced) while staying
 * silent about dead mature objects, whose fields legitimately name
 * copied objects. FORWARDED is free here: clear_mark_flags resets
 * it at the end of the collection. */
static void verify_mark_reachable(void) {
  ThreadReg *t;
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_trace(*f->slots[i]);
      }
    }
  }
  for (PinEnt *p = g_pins; p; p = p->next) {
    verify_trace(p->payload);
  }
  {
    uint32_t ri;
    for (ri = 0; ri < g_rem_n; ri++) {
      verify_trace(g_remset[ri]);
    }
  }
  avant_hash_walk_tls(verify_tls_slot);
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    EACH_REF_SLOT(p, slot, { verify_trace(*slot); });
  }
}

/* A word the type map does not call a reference, in an object the
 * verifier reached, that is exactly the start of an unpinned object in
 * from-space (moved or about to be discarded): the program stored a
 * reference where the collector does not look (a wrong or missing type
 * map, audit-abi 8.1). Heap strings (STRMETA) hold text and are
 * skipped; a scalar equal to such an address by chance is the only
 * false positive. */
static uint64_t g_verify_untraced;

static void verify_untraced_words(AvantHeader *h) {
  size_t n, i;
  void **p;
  if (h->flags & AVANT_FLAG_STRMETA) {
    return;
  }
  if (h->type_id == AVANT_TYPE_PTRS) {
    return;
  }
  p = (void **)payload_of(h);
  n = (size_t)h->size / sizeof(void *);
  for (i = 0; i < n; i++) {
    void *w = p[i];
    Block *cb = NULL;
    void *to;
    if (!w || ((uintptr_t)w & (ALIGN - 1)) != 0) {
      continue;
    }
    if (!is_heap_payload(w, &cb, NULL) || !cb || cb->space != SPACE_NURSERY) {
      continue;
    }
    if (avant_header(w)->flags & AVANT_FLAG_PINNED) {
      continue;
    }
    if (type_word_is_ref(h, i)) {
      continue;
    }
    to = fwd_get(w);
    g_verify_untraced++;
    fprintf(stderr,
            "avant gc verify: untraced reference holder=%p type=%u size=%u "
            "word=%lu names young object %p (%s %p) coll=%llu\n",
            (void *)p, (unsigned)h->type_id, (unsigned)h->size, (unsigned long)i, w,
            to && to != w ? "forwarded to" : "not copied, from-space", to,
            (unsigned long long)g_stats.collections);
  }
}

static void verify_obj_fields(AvantHeader *h, Block *b) {
  if (!(h->flags & AVANT_FLAG_FORWARDED)) {
    return;
  }
  void *p = payload_of(h);
  verify_untraced_words(h);
  EACH_REF_SLOT(p, slot, {
    if (verify_ptr(*slot, "field", p, (long)((uint8_t *)slot - (uint8_t *)p))) {
      fprintf(stderr, "avant gc verify: holder %p flags=0x%x space=%d\n", p, h->flags,
              b ? (int)b->space : -1);
    }
  });
}

/* Pinned nursery objects stay in their block; their fields are checked
 * like a to-space object's (C29). */
static void verify_pinned_fields(AvantHeader *h, Block *b) {
  if (h->flags & AVANT_FLAG_PINNED) {
    verify_obj_fields(h, b);
  }
}

static uint64_t g_verify_left_seen;

static void verify_young_copy(void) {
  ThreadReg *t;
  Block *b;
  if (g_sm_leftover > g_verify_left_seen) {
    fprintf(stderr,
            "avant gc verify: %llu stack words still name from-space "
            "after rewrite (cumulative %llu)\n",
            (unsigned long long)(g_sm_leftover - g_verify_left_seen),
            (unsigned long long)g_sm_leftover);
    g_verify_left_seen = g_sm_leftover;
  }
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      uint32_t i;
      for (i = 0; i < f->n; i++) {
        verify_ptr(*f->slots[i], "root", (void *)f->slots[i], (long)i);
      }
    }
  }
  verify_mark_reachable();
  for (b = g_to; b; b = b->next) {
    foreach_block_object(b, verify_obj_fields);
  }
  for (b = g_mature; b; b = b->next) {
    foreach_block_object(b, verify_obj_fields);
  }
  for (b = g_from; b; b = b->next) {
    foreach_block_object(b, verify_obj_fields);
  }
  if (g_saw_pinned_nursery) {
    for (b = g_nursery; b; b = b->next) {
      foreach_block_object(b, verify_pinned_fields);
    }
  }
  {
    Large *L;
    for (L = g_large; L; L = L->next) {
      verify_obj_fields(&L->header, NULL);
    }
  }
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

static void block_set_add(BlockSet *s, Block *b) {
  if (s->n && s->v[s->n - 1] == b) {
    return;
  }
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->v = (Block **)xrealloc(s->v, s->cap * sizeof(Block *));
  }
  s->v[s->n++] = b;
}

static void block_set_seal(BlockSet *s) {
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

static int block_set_has(const BlockSet *s, const Block *b) {
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

/* C31: defragmentation moves mature objects and rewrites only the
 * precise references to them (root slots, pins, runtime TLS slots, the
 * remembered set, heap fields). A runtime C frame on the collecting
 * thread can hold a reference across a full collection that the
 * collector cannot rewrite: with AVANT_NURSERY=0 the outermost
 * avant_gc_defer_enter collects while the helper holds its arguments
 * (avant_str_concat's operands: the caller rooted them, so they are
 * marked, but the helper's own copies are C values), and a helper that
 * calls avant_alloc outside defer holds its arguments across the full
 * collection maybe_collect may run. Such an object must not move. So a
 * defragmenting collection first notes every mature block that a word
 * of the collecting thread's stack names (exact start, interior, or a
 * scalar that happens to look like one; a false positive only skips a
 * candidate), from this function's frame to the stack top, and those
 * blocks are not candidates. collect() first saves its callers'
 * callee-saved registers into its own frame, so a reference a caller
 * keeps in one is on the scanned stack. Other threads' C frames are
 * D191's (Part 2). */
static BlockSet g_cstack_blocks;
static BlockSet g_def_release;

__attribute__((noinline)) static void note_c_stack_blocks(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = g_self_reg.stack_hi;
  uint8_t *lo = g_self_reg.stack_lo;
  g_cstack_blocks.n = 0;
  if (!hi || !p || p >= hi || (lo && p < lo)) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= hi; p += ALIGN) {
    uint8_t *w = *(uint8_t **)p;
    /* The word's block, and its header's: a payload pointer just past
     * an object that ends a block lies in the next one. */
    Block *b = block_of(w);
    Block *hb = w ? block_of(w - sizeof(AvantHeader)) : NULL;
    if (b && b->space == SPACE_MATURE) {
      block_set_add(&g_cstack_blocks, b);
    }
    if (hb && hb != b && hb->space == SPACE_MATURE) {
      block_set_add(&g_cstack_blocks, hb);
    }
  }
  block_set_seal(&g_cstack_blocks);
}

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
  void *start = object_payload(w, &b, &ol);
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
  if (h->flags & AVANT_FLAG_MARKED) {
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
 * collection after marking (line marks and MARKED flags are the live
 * set) and before the sweep. Live objects in candidate blocks are
 * copied into fresh destination blocks, every root, heap field,
 * remembered entry, and stackmap spill naming them is rewritten
 * through the forwarding table, and the emptied blocks are freed
 * whole. Destination blocks are allocated before anything moves; if
 * one cannot be allocated under the heap limit, nothing moves and the
 * ordinary non-moving collection completes (g_def_skipped counts it),
 * so defragmentation can never make a collection fail. The evacuated
 * blocks are freed by defrag_release. Not inlined: its locals name
 * objects in the evacuated blocks, and they must not sit in collect's
 * frame, which verify_defrag scans as part of the C stack. */
__attribute__((noinline)) static void defrag_evacuate(uint64_t live_lines_this) {
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
    int live = block_live_lines(b);
    DefCand *c;
    if (live <= 0 || live > 64 || block_has_pinned(b) || block_set_has(&g_cstack_blocks, b)) {
      continue;
    }
    if (ncand == ccap) {
      ccap = ccap ? ccap * 2 : 32;
      cand = (DefCand *)xrealloc(cand, ccap * sizeof(DefCand));
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
          if (h->flags & AVANT_FLAG_MARKED) {
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
  dest = (Block **)xrealloc(NULL, dest_cap_blocks * sizeof(Block *));
  for (i = 0; i < dest_cap_blocks; i++) {
    Block *nb = new_block(SPACE_MATURE);
    if (!nb) {
      size_t j;
      for (j = 0; j < ndest; j++) {
        mature_unlink(dest[j]);
        cache_or_free_block(dest[j]);
      }
      free(dest);
      free(cand);
      g_def_skipped++;
      return;
    }
    dest[ndest++] = nb;
  }
  fwd_reset();
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
          if (!(h->flags & AVANT_FLAG_MARKED)) {
            bits &= bits - 1;
            continue;
          }
          total = obj_total(h);
          if (bump + total > db->base + BLOCK_SIZE) {
            di++;
            if (di >= ndest) {
              Block *nb = new_block(SPACE_MATURE);
              if (!nb) {
                /* Heap limit mid-evacuation: stop here. Objects
                 * already copied are forwarded and fixed up below;
                 * this block keeps the rest and is not freed. */
                stopped = 1;
                break;
              }
              if (ndest == dest_cap_blocks) {
                dest_cap_blocks = dest_cap_blocks * 2 + 4;
                dest = (Block **)xrealloc(dest, dest_cap_blocks * sizeof(Block *));
              }
              dest[ndest++] = nb;
            }
            db = dest[di];
            bump = db->base;
          }
          nh = (AvantHeader *)bump;
          memcpy(nh, h, total);
          set_start(db, (size_t)(bump - db->base));
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
    foreach_block_object(b, defrag_fix_one);
  }
  for (b = g_nursery; b; b = b->next) {
    foreach_block_object(b, defrag_fix_one);
  }
  for (b = g_from; b; b = b->next) {
    foreach_block_object(b, defrag_fix_one);
  }
  {
    Large *L;
    for (L = g_large; L; L = L->next) {
      if (L->header.flags & AVANT_FLAG_MARKED) {
        defrag_scan_payload(payload_of(&L->header));
      }
    }
  }
  rewrite_from_stackmaps();
  /* The evacuated blocks are freed by defrag_release, after collect()
   * has let the verifier look at them from its own frame. */
  g_def_release.n = 0;
  for (i = 0; i < ncand; i++) {
    block_set_add(&g_def_release, cand[i].b);
  }
  block_set_seal(&g_def_release);
  free(dest);
  free(cand);
}

static void defrag_release(void) {
  size_t i;
  for (i = 0; i < g_def_release.n; i++) {
    mature_unlink(g_def_release.v[i]);
    cache_or_free_block(g_def_release.v[i]);
    g_def_freed_blocks++;
  }
  g_def_release.n = 0;
}

/* AVANT_GC_VERIFY after defragmentation (C31): no reference the
 * program or the runtime can still use names a block about to be
 * freed: root slots, pins, the remembered set, the fields of marked
 * objects, and the words of the collecting thread's stack from this
 * function's frame up (the C frames of the runtime helper that
 * collected and of the generated code that called it). */
static void verify_defrag_word(void *w, const char *where, void *at) {
  Block *b = block_of(w);
  Block *hb = w ? block_of((uint8_t *)w - sizeof(AvantHeader)) : NULL;
  void *start;
  if (!(b && block_set_has(&g_def_release, b)) && !(hb && block_set_has(&g_def_release, hb))) {
    return;
  }
  start = object_payload(w, NULL, NULL);
  fprintf(stderr,
          "avant gc verify: %s %p names %p in a mature block defragmentation "
          "is freeing (object %p moved to %p) coll=%llu\n",
          where, at, w, start, start ? fwd_get(start) : NULL,
          (unsigned long long)g_stats.collections);
}

static void verify_defrag_fields(AvantHeader *h, Block *b) {
  void *p;
  if (!(h->flags & AVANT_FLAG_MARKED) || (b && block_set_has(&g_def_release, b))) {
    return;
  }
  p = payload_of(h);
  EACH_REF_SLOT(p, slot, { verify_defrag_word(*slot, "field", (void *)slot); });
}

static void verify_defrag_tls(void **slot) {
  verify_defrag_word(*slot, "runtime TLS slot", (void *)slot);
}

__attribute__((noinline)) static void verify_defrag(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = g_self_reg.stack_hi;
  uint8_t *lo = g_self_reg.stack_lo;
  ThreadReg *t;
  Block *b;
  Large *L;
  uint32_t i;
  if (hi && p && p < hi && !(lo && p < lo)) {
    p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
    for (; p + sizeof(void *) <= hi; p += ALIGN) {
      verify_defrag_word(*(void **)p, "C stack word at", (void *)p);
    }
  }
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        verify_defrag_word(*f->slots[i], "root slot", (void *)f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
  for (PinEnt *pe = g_pins; pe; pe = pe->next) {
    verify_defrag_word(pe->payload, "pin", (void *)pe);
  }
  /* A remembered entry may name a dead object (full collections do not
   * scrub the set, and the young copy skips an entry whose block is
   * gone); only a live one that moved must have been rewritten. */
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    Block *rb = block_of(obj);
    if (rb && block_set_has(&g_def_release, rb) && is_heap_payload(obj, NULL, NULL) &&
        (avant_header(obj)->flags & AVANT_FLAG_MARKED)) {
      verify_defrag_word(obj, "remembered entry", (void *)&g_remset[i]);
    }
  }
  avant_hash_walk_tls(verify_defrag_tls);
  for (b = g_mature; b; b = b->next) {
    foreach_block_object(b, verify_defrag_fields);
  }
  for (b = g_nursery; b; b = b->next) {
    foreach_block_object(b, verify_defrag_fields);
  }
  for (b = g_from; b; b = b->next) {
    foreach_block_object(b, verify_defrag_fields);
  }
  for (L = g_large; L; L = L->next) {
    verify_defrag_fields(&L->header, NULL);
  }
}

static void young_copy_out(int tenure) {
  g_force_tenure = tenure;
  g_to_bytes = 0;
  g_keep_n = 0;
  zero_line_marks(g_nursery);
  gen_young_copy();
  if (g_verify) {
    verify_young_copy();
  }
  clear_alloc_cur(SPACE_NURSERY);
  if (g_discard_from) {
    discard_nursery_from();
  } else {
    park_nursery_from();
  }
  adopt_to_space();
  rem_commit_kept();
  if (g_nursery_held + g_to_bytes > g_cpy_peak) {
    g_cpy_peak = g_nursery_held + g_to_bytes;
  }
  g_nursery_held = g_to_bytes;
  g_to_bytes = 0;
  g_force_tenure = 0;
  reclaim_young_large();
  clear_mark_flags();
}

static void collect(int full) {
  int pause = 0;
  uint64_t used = 0;
  uint64_t surv = 0;
  if (g_collecting) {
    die("GC reentered");
  }
  g_collecting = 1;
  atomic_fetch_add_explicit(&g_avant_str_gen, 1u, memory_order_release);
  g_stats.collections++;
  g_live = 0;
  g_work_n = 0;
  gc_fast_clear();
  g_cur = NULL;
  g_bump = NULL;
  g_limit = NULL;

  /* Spill the callers' callee-saved GPRs into collect's frame so the
   * C-stack walks see a reference a caller keeps in one: the young
   * copy's rewrite (rewrite_c_stack) and the full collection's
   * defragmentation exclusion (note_c_stack_blocks, C31). Without
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
      young_copy_out(1);
    } else {
      used = g_nursery_held;
      if (used >= STICKY_MIN) {
        g_probe = 1;
        g_probe_nursery_bytes = 0;
        gen_young_copy();
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
        rem_scrub();
        reclaim_young_large();
        clear_noted();
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
        young_copy_out(tenure);
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
    Large *L;
    g_stats.full_collections++;
    /* Non-moving. Evacuating the nursery here needs a second copy of
     * every live object while the from-space blocks are still charged
     * against the heap limit. */
    los_spare_drop();
    clear_alloc_cur(SPACE_NURSERY);
    clear_alloc_cur(SPACE_MATURE);
    clear_all_marks();
    zero_line_marks(g_mature);
    zero_line_marks(g_nursery);
    zero_line_marks(g_from);
    mark_roots();
    {
      Block *db;
      uint64_t live_lines_this = 0;
      for (db = g_mature; db; db = db->next) {
        int live = block_live_lines(db);
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
        note_c_stack_blocks();
      }
      defrag_evacuate(live_lines_this);
      if (g_verify && g_def_release.n) {
        verify_defrag();
      }
      defrag_release();
    }
    sweep_large();
    filter_blocks(&g_mature, keep_if_live);
    filter_blocks(&g_nursery, keep_if_live);
    filter_blocks(&g_from, keep_if_live);
    g_mark_sum = 0;
    for (b = g_nursery; b; b = b->next) {
      foreach_block_object(b, sum_marked_one);
    }
    for (b = g_from; b; b = b->next) {
      foreach_block_object(b, sum_marked_one);
    }
    g_nursery_held = g_mark_sum;
    g_mature_since_full = 0;
    g_young_large = 0;
    g_young_large_n = 0;
    g_los_eager_hold = 0;
    for (L = g_large; L; L = L->next) {
      L->header.flags =
          (uint16_t)(L->header.flags & (uint16_t)~AVANT_FLAG_YOUNG);
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

static void maybe_collect(void) {
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
   * C helpers also hold heap pointers that mark_roots does not see
   * (concat args, hash tables mid-grow). avant_gc_defer_* blocks
   * collection for that window.
   */
  if (g_nursery_off) {
    if (g_since_gc >= g_full_trigger) {
      collect(1);
    }
    return;
  }
  /* Nursery garbage is reclaimed by a young collection. A full
   * collection runs when mature space, including promotions and large
   * objects, has grown by g_full_trigger. Counting every nursery byte here
   * marked a live tree that a young collection should have discarded. */
  if (g_mature_since_full >= g_full_trigger) {
    collect(1);
  } else if (g_nursery_held >= g_young_bytes) {
    collect(0);
  }
}

static void *los_alloc(size_t payload, uint32_t type_id) {
  size_t chunk = align_up(sizeof(Large) + payload);
  int young = !g_nursery_off && !g_alloc_mature && type_id == AVANT_TYPE_BYTES;
  Large *L;
  if (g_los_spare && g_los_spare->header.size != (uint32_t)payload) {
    los_spare_drop();
  }
  if (g_heap_max && !g_los_spare && g_heap_bytes + chunk > g_heap_max) {
    if (!g_gc_defer) {
      collect(1);
    }
    if (g_heap_max && g_heap_bytes + chunk > g_heap_max) {
      die("heap limit");
    }
  }
  g_los_allocs++;
  g_los_alloc_bytes += (uint64_t)chunk;
  {
    int bucket = payload < 16384 ? 0 : payload < 65536 ? 1 : payload < 262144 ? 2 : payload < 1048576 ? 3 : payload < 4194304 ? 4 : 5;
    g_los_hist[bucket]++;
    if (type_id == AVANT_TYPE_BYTES && align_up(sizeof(AvantHeader) + payload) <= LOS_LIMIT) {
      g_los_scalar_allocs++;
      g_los_scalar_bytes += (uint64_t)chunk;
    } else {
      g_los_big_allocs++;
      g_los_big_bytes += (uint64_t)chunk;
    }
    if (young) {
      g_los_young_allocs++;
      g_los_young_bytes += (uint64_t)chunk;
    } else {
      g_los_mature_allocs++;
      g_los_mature_bytes += (uint64_t)chunk;
    }
  }
  if (g_los_spare && g_los_spare->header.size == (uint32_t)payload) {
    L = g_los_spare;
    g_los_spare = NULL;
    memset(payload_of(&L->header), 0, payload);
    g_los_spare_hits++;
  } else {
    L = calloc(1, chunk);
    if (!L) {
      die("out of memory");
    }
    g_heap_bytes += chunk;
    g_los_callocs++;
  }
  L->header.size = (uint32_t)payload;
  L->header.type_id = (uint16_t)type_id;
  L->header.flags = (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_LARGE);
  if (young) {
    L->header.flags |= AVANT_FLAG_YOUNG;
    g_young_large += chunk;
    g_young_large_n++;
  } else {
    g_mature_since_full += chunk;
  }
  L->next = g_large;
  g_large = L;
  /* The chunk holds the header and payload (chunk is at least
   * sizeof(Large) + payload, aligned; a reused spare has this size). */
  if ((uint8_t *)L < g_large_lo) {
    g_large_lo = (uint8_t *)L;
  }
  if ((uint8_t *)L + chunk > g_large_hi) {
    g_large_hi = (uint8_t *)L + chunk;
  }
  large_hash_add(L);
  note_peak();
  return payload_of(&L->header);
}

/* Block memory is never pre-zeroed, so every allocation zeroes its
 * own payload here. Callers pass zeroed=1 only for memory already
 * cleared by its allocator (the large-object path). */
static void init_object(AvantHeader *h, size_t payload, uint32_t type_id, int young, int zeroed) {
  h->size = (uint32_t)payload;
  h->type_id = (uint16_t)type_id;
  h->flags = young ? (uint16_t)(AVANT_FLAG_HEAP | AVANT_FLAG_YOUNG) : (uint16_t)AVANT_FLAG_HEAP;
  if (!zeroed) {
    memset(payload_of(h), 0, payload);
  }
}

static void account_alloc(size_t total, int nursery) {
  g_stats.allocated_bytes += total;
  g_since_gc += total;
  if (nursery) {
    g_nursery_held += total;
  } else {
    g_mature_since_full += total;
  }
  note_peak();
}

static uint64_t g_los_now;
static uint64_t g_los_now_bytes;
static uint64_t g_los_spare_bytes;

static void print_stats(void) {
  {
    Large *L;
    g_los_now = 0;
    g_los_now_bytes = 0;
    for (L = g_large; L; L = L->next) {
      g_los_now++;
      g_los_now_bytes += (uint64_t)large_chunk(L);
    }
    g_los_spare_bytes = g_los_spare ? (uint64_t)large_chunk(g_los_spare) : 0;
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
          (unsigned long long)g_sm_walks,
          (unsigned long long)g_sm_hits,
          g_sm_nsites,
          (unsigned long long)g_sm_leftover,
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
    const char *v = getenv("AVANT_NURSERY_STACK");
    /* Off unless set to 1. Conservative rewrite of LLVM spill slots
     * smashes packed i32s; pin-only misses spills. Precise roots are
     * the shadow stack after myc-llvm store-before-gc_root. */
    g_stack_scan = v && v[0] == '1' && v[1] == 0;
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
    const char *v = getenv("AVANT_NURSERY_STACKMAP");
    if (v && v[0] == '0' && v[1] == 0) {
      g_stackmap_walk = 0;
    } else if (v && v[0] == '1' && v[1] == 0) {
      g_stackmap_walk = 1;
    } else {
      /* On with the copying nursery so leftover LLVM spills in myc
       * frames can be rewritten after young copy. Set to 0 to disable. */
      g_stackmap_walk = !g_nursery_off;
    }
  }
  if (g_stackmap_walk) {
    sm_init();
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
  if (getenv("AVANT_GC_STATS")) {
    atexit(print_stats);
  }
}

void avant_type_map(uint32_t type_id, uint64_t word_bits) {
  gc_init();
  if (type_id == AVANT_TYPE_PTRS) {
    return;
  }
  if (type_id >= MAX_TYPES) {
    die("type id too large");
  }
  TypeMap *m = &g_types[type_id];
  if (m->words && m->words != &m->first) {
    free(m->words);
  }
  m->first = word_bits;
  m->words = &m->first;
  m->nchunks = 1;
  m->stride = 0;
  m->nrefs = (uint32_t)__builtin_popcountll(word_bits);
  g_type_set[type_id] = 1;
}

/* Stage 28 W1b. Registers bits for payload words word_base ..
 * word_base + 63 of type_id (word_base a multiple of 64; later chunks
 * extend the bitmap, so a map may be any length) with an element stride
 * in words (0: one record; > 0: a buffer of repeated elements, every
 * bit below stride). All calls for one id carry the same stride. */
void avant_type_map_at(uint32_t type_id, uint32_t stride_words, uint32_t word_base,
                       uint64_t bits) {
  gc_init();
  if (type_id == AVANT_TYPE_PTRS || type_id == AVANT_TYPE_BYTES) {
    return;
  }
  if (type_id >= MAX_TYPES) {
    die("type id too large");
  }
  if (word_base % 64 != 0) {
    die("type map chunk is not 64-word aligned");
  }
  TypeMap *m = &g_types[type_id];
  uint32_t chunk = word_base / 64;
  if (!g_type_set[type_id]) {
    m->first = 0;
    m->words = &m->first;
    m->nchunks = 1;
    m->stride = stride_words;
    m->nrefs = 0;
    g_type_set[type_id] = 1;
  } else if (m->stride != stride_words) {
    die("type map stride changed");
  }
  if (chunk >= m->nchunks) {
    uint64_t *nw = calloc((size_t)chunk + 1, sizeof(uint64_t));
    if (!nw) {
      die("out of memory");
    }
    memcpy(nw, m->words, (size_t)m->nchunks * sizeof(uint64_t));
    if (m->words != &m->first) {
      free(m->words);
    }
    m->words = nw;
    m->nchunks = chunk + 1;
  }
  if (stride_words) {
    uint32_t i;
    for (i = 0; i < 64; i++) {
      if (((bits >> i) & 1u) && word_base + i >= stride_words) {
        die("type map bit past the element stride");
      }
    }
  }
  m->nrefs -= (uint32_t)__builtin_popcountll(m->words[chunk]);
  m->words[chunk] = bits;
  m->nrefs += (uint32_t)__builtin_popcountll(bits);
}

/* A call used to calloc a frame and free it on leave. Hot recursion
 * (Brainfuck) and numeric methods spent the call on that pair. Frames
 * are kept on a thread-local freelist; the slot array is reused too. */
void avant_gc_enter(void) {
  g_enter_calls++;
  gc_init();
  avant_gc_register_thread();
  Frame *f = g_frame_free;
  if (f) {
    g_frame_free = f->prev;
    f->n = 0;
  } else {
    f = calloc(1, sizeof(Frame));
    if (!f) {
      die("out of memory");
    }
  }
  f->prev = g_frame;
  g_frame = f;
}

void avant_gc_leave(void) {
  Frame *f = g_frame;
  if (!f) {
    die("gc leave without enter");
  }
  g_frame = f->prev;
  f->n = 0;
  f->prev = g_frame_free;
  g_frame_free = f;
}

void avant_gc_quiet_enter(void) {
  g_gc_quiet++;
}

void avant_gc_quiet_leave(void) {
  if (g_gc_quiet > 0) {
    g_gc_quiet--;
  }
}

void avant_gc_root(void *slot) {
  g_root_calls++;
  if (g_gc_quiet) {
    return;
  }
  if (!g_frame) {
    die("gc root without enter");
  }
  Frame *f = g_frame;
  uint32_t i;
  /*
   * Codegen emits CALL :avant_gc_root at the first use site of a temp.
   * When that site is inside a loop (Array.push/pop), the same stack
   * slot was appended every iteration and the frame list grew without
   * bound (prod Words RSS). Same address is already a root.
   */
  for (i = 0; i < f->n; i++) {
    if (f->slots[i] == (void **)slot) {
      return;
    }
  }
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->slots = xrealloc(f->slots, f->cap * sizeof(void **));
  }
  f->slots[f->n++] = (void **)slot;
}

void *avant_gc_root_begin(uint32_t count) {
  g_root_calls += count;
  if (!g_frame) {
    die("gc root begin without enter");
  }
  Frame *f = g_frame;
  if (count > f->cap) {
    uint32_t cap = f->cap ? f->cap : 8;
    while (cap < count) {
      cap *= 2;
    }
    f->slots = xrealloc(f->slots, cap * sizeof(void **));
    f->cap = cap;
  }
  f->n = g_gc_quiet ? 0 : count;
  return f->slots;
}

__attribute__((noinline))
void *avant_gc_reload(void *slot) {
  g_reload_calls++;
  /*
   * Stage 17: myc-llvm stores this return into the rooted alloca after
   * a collecting CALL. The load is opaque to LLVM isel, so a pre-call
   * copy in an unrooted spill cannot replace it.
   */
  void *p = *(void *volatile *)slot;
  *(void *volatile *)slot = p;
  return p;
}

void avant_barrier(void *obj) {
  Block *b = NULL;
  AvantHeader *h;
  if (!obj || g_collecting) {
    return;
  }
  /* The compiler passes the object being stored into. A nursery object
   * is scanned from the roots, so the hot path is the flag in the header
   * and not a block-table lookup. */
  if (((uintptr_t)obj & (ALIGN - 1)) == 0) {
    h = avant_header(obj);
    if (h->flags & AVANT_FLAG_HEAP) {
      /* Already scanned from the roots, or already in the remembered set. */
      if (h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_REM)) {
        return;
      }
    }
  }
  if (!is_heap_payload(obj, &b, NULL)) {
    return;
  }
  if (b && (b->space == SPACE_NURSERY || b->space == SPACE_TO)) {
    return;
  }
  rem_add(obj);
}

/* Stage 28 W1b. The barrier for a store through an address that may be
 * inside a heap object (a struct method's self, a struct field reached
 * through such a self): remembers the object containing addr, like
 * avant_barrier(object). An address on this thread's stack, or outside
 * the heap, is a no-op. Never collects. */
void avant_barrier_at(void *addr) {
  Block *b = NULL;
  void *start;
  if (!addr || g_collecting) {
    return;
  }
  if (g_self_reg.stack_lo && (uint8_t *)addr >= (uint8_t *)g_self_reg.stack_lo &&
      (uint8_t *)addr < (uint8_t *)g_self_reg.stack_hi) {
    return;
  }
  start = object_payload(addr, &b, NULL);
  if (!start) {
    return;
  }
  avant_barrier(start);
}

void avant_pin(void *payload) {
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&g_world);
  if (!is_heap_payload(payload, NULL, NULL)) {
    pthread_mutex_unlock(&g_world);
    return;
  }
  AvantHeader *h = avant_header(payload);
  if (h->flags & AVANT_FLAG_PINNED) {
    for (PinEnt *p = g_pins; p; p = p->next) {
      if (p->payload == payload) {
        p->count += 1;
        pthread_mutex_unlock(&g_world);
        return;
      }
    }
  }
  PinEnt *e = (PinEnt *)malloc(sizeof(PinEnt));
  if (!e) {
    pthread_mutex_unlock(&g_world);
    die("out of memory");
  }
  h->flags |= AVANT_FLAG_PINNED;
  e->payload = payload;
  e->count = 1;
  e->next = g_pins;
  g_pins = e;
  pthread_mutex_unlock(&g_world);
}

int avant_pin_count(void) {
  int n = 0;
  pthread_mutex_lock(&g_world);
  for (PinEnt *p = g_pins; p; p = p->next) {
    n += 1;
  }
  pthread_mutex_unlock(&g_world);
  return n;
}

void avant_unpin(void *payload) {
  PinEnt **pp;
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&g_world);
  if (!is_heap_payload(payload, NULL, NULL)) {
    pthread_mutex_unlock(&g_world);
    return;
  }
  AvantHeader *h = avant_header(payload);
  if ((h->flags & AVANT_FLAG_PINNED) == 0) {
    pthread_mutex_unlock(&g_world);
    return;
  }
  pp = &g_pins;
  while (*pp) {
    if ((*pp)->payload == payload) {
      (*pp)->count -= 1;
      if ((*pp)->count > 0) {
        pthread_mutex_unlock(&g_world);
        return;
      }
      PinEnt *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  h->flags = (uint16_t)(h->flags & ~AVANT_FLAG_PINNED);
  pthread_mutex_unlock(&g_world);
}

void avant_gc_defer_enter(void) {
  gc_init();
  gc_fast_clear();
  pthread_mutex_lock(&g_world);
  /*
   * Helpers wrap avant_alloc in defer so C-held pointers are not swept.
   * If every allocation in a loop is inside that window, maybe_collect
   * in avant_alloc never fires (prod Base64 RSS 3.7 GB). Collect *before*
   * taking defer, while the caller has already stored the previous result
   * in a rooted local. Nested enter (hash grow, buf push inside json_gen)
   * must not collect: the outer helper still holds unmarked pointers.
   * Do not collect on leave — a helper that returns a heap pointer has
   * not yet STOREd it.
   *
   * Young copy must not run here: myc spilled heap args into roots and
   * then copied them into C registers. A moving collect would update the
   * slots and leave the C args dangling. Full collect stays non-moving
   * (AVANT_NURSERY=0), so it is still safe at this boundary.
   */
  if (g_gc_defer == 0 && g_nursery_off) {
    maybe_collect();
  }
  g_gc_defer += 1;
  pthread_mutex_unlock(&g_world);
}

void avant_gc_defer_leave(void) {
  pthread_mutex_lock(&g_world);
  if (g_gc_defer > 0) {
    g_gc_defer -= 1;
  }
  pthread_mutex_unlock(&g_world);
}

/* Q128 heap-goal collection for workloads whose allocations all pass
 * through C helpers under defer (the HTTP request path), which never
 * reach maybe_collect undeferred. Callers must be at a boundary where
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
    collect(1);
    g_defrag_mode = defrag_saved;
  }
  pthread_mutex_unlock(&g_world);
}

void avant_alloc_in_mature(int32_t on) {
  if (on) {
    gc_fast_clear();
  }
  g_alloc_mature = on ? 1 : 0;
}

void avant_world_lock(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
}

void avant_world_unlock(void) {
  pthread_mutex_unlock(&g_world);
}

static int alloc_is_large(uint32_t type_id, size_t payload, size_t total) {
  if (total > LOS_LIMIT) {
    return 1;
  }
  return type_id == AVANT_TYPE_BYTES && payload >= SCALAR_LOS_MIN;
}

void *avant_alloc(uint64_t nbytes, uint32_t type_id) {
  gc_init();
  gc_fast_refresh();
  if (nbytes > UINT32_MAX) {
    die("object too large");
  }
  if (type_id > UINT16_MAX) {
    die("type id too large");
  }

  size_t payload = (size_t)nbytes;
  size_t total = align_up(sizeof(AvantHeader) + payload);
  int large = alloc_is_large(type_id, payload, total);
  /* Single mutator, under the young threshold: bump and return.
   * Collection stays on the locked path, with the caller's roots
   * already spilled. This is the same contract as avant_bump_alloc.
   * Medium numeric buffers take the large path so their address stays. */
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0 && !g_gc_defer &&
      !g_collecting && !g_nursery_off && !g_alloc_mature && !large &&
      g_mature_since_full < FULL_BYTES && g_nursery_held + total < g_young_bytes) {
    AvantHeader *fast = place_bump(total);
    if (fast) {
      init_object(fast, payload, type_id, 1, 0);
      account_alloc(total, 1);
      return payload_of(fast);
    }
  }

  pthread_mutex_lock(&g_world);
  maybe_collect();

  if (large) {
    /* One replaced scalar buffer is reclaimed into the spare slot.
     * A survived buffer sets the hold so a run of live rows does not
     * collect on every allocation. The byte threshold still runs. */
    if (!g_nursery_off && !g_alloc_mature && !g_gc_defer && g_young_large_n > 0 &&
        (g_young_large >= g_young_bytes || (!g_los_eager_hold && g_young_large_n == 1))) {
      uint64_t mature_before = g_mature_since_full;
      collect(0);
      if (g_young_large_n > 0 || g_mature_since_full > mature_before) {
        g_los_eager_hold = 1;
      } else {
        g_los_eager_hold = 0;
      }
    }
    void *obj = los_alloc(payload, type_id);
    g_stats.allocated_bytes += total;
    g_since_gc += total;
    pthread_mutex_unlock(&g_world);
    return obj;
  }

  uint8_t space = SPACE_NURSERY;
  if (g_nursery_off || g_alloc_mature) {
    space = SPACE_MATURE;
  }
  AvantHeader *h = place(total, space, 0);
  init_object(h, payload, type_id, space == SPACE_NURSERY, 0);
  account_alloc(total, space == SPACE_NURSERY);
  void *obj = payload_of(h);
  pthread_mutex_unlock(&g_world);
  return obj;
}

void avant_gc_stats(AvantGcStats *out) {
  if (out) {
    *out = g_stats;
  }
}

void *avant_bump_alloc(uint64_t nbytes, uint32_t type_id) {
  size_t payload;
  size_t total;
  AvantHeader *h;
  if (atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0) {
    return NULL;
  }
  if (nbytes > UINT32_MAX || type_id > UINT16_MAX) {
    return NULL;
  }
  gc_init();
  gc_fast_refresh();
  payload = (size_t)nbytes;
  total = align_up(sizeof(AvantHeader) + payload);
  h = place_bump(total);
  if (!h) {
    return NULL;
  }
  init_object(h, payload, type_id, 1, 0);
  account_alloc(total, 1);
  return payload_of(h);
}

void avant_nursery_reclaim(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
  if (!g_nursery_off && !g_collecting && g_nursery_held > 0) {
    collect(0);
  }
  pthread_mutex_unlock(&g_world);
}

void avant_gc_collect(void) {
  gc_init();
  pthread_mutex_lock(&g_world);
  collect(1);
  pthread_mutex_unlock(&g_world);
}
