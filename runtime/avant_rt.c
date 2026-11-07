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
int g_collecting;

int g_nursery_off;

Block *g_bht[BLOCK_HASH];
Block *g_mature;
Block *g_nursery;
Block *g_from;
Block *g_to;
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
Block *g_alloc_cur[4];
uint8_t g_alloc_fresh[4];

static __thread Frame *g_frame;
static __thread int g_gc_quiet;
static __thread Frame *g_frame_free;
__thread int g_alloc_mature;

__thread ThreadReg g_self_reg;
ThreadReg *g_threads;
/* Becomes 1 when a second thread registers, and stays 1. A single
 * mutator collects only at its own allocating call, so an interior
 * pointer used before the next call does not need a root. Another
 * thread can collect at any instruction. */
atomic_int g_avant_shared;
static pthread_mutex_t g_world = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
int g_gc_defer;

PinEnt *g_pins;

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
static int g_reserve_mode = 1;
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

void **g_work;
uint32_t g_work_n;
static uint32_t g_work_cap;

TypeMap g_types[MAX_TYPES];
uint8_t g_type_set[MAX_TYPES];

/* Whether payload word W of an object with header H is a reference slot
 * by its type map (the verifier's inverse question). */
int gc_type_word_is_ref(const AvantHeader *h, size_t w) {
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

uint64_t g_heap_bytes;
uint64_t g_heap_max;
uint64_t g_live;
static uint64_t g_last_full_at;

/* Nursery allocations taken by generated code without a call. */
uint64_t avant_inline_allocs;

void gc_die(const char *msg) {
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

void *gc_xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) {
    gc_die("out of memory");
  }
  return q;
}

void work_push(void *p) {
  if (g_work_n == g_work_cap) {
    g_work_cap = g_work_cap ? g_work_cap * 2 : 256;
    g_work = gc_xrealloc(g_work, g_work_cap * sizeof(void *));
  }
  g_work[g_work_n++] = p;
}

void gc_fwd_reset(void) {
  if (!g_fwd_cap) {
    g_fwd_cap = 1024;
    g_fwd = calloc(g_fwd_cap, sizeof(FwdEnt));
    if (!g_fwd) {
      gc_die("out of memory");
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
    gc_die("out of memory");
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

int gc_block_live_lines(Block *b) {
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
void *gc_object_payload(void *p, Block **ob, Large **ol) {
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

void gc_foreach_block_object(Block *b, void (*fn)(AvantHeader *, Block *)) {
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

void gc_zero_line_marks(Block *list) {
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

void gc_reset_block_bump(Block *b) {
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base;
  if (!find_hole(b, ALIGN)) {
    b->bump = b->base + BLOCK_SIZE;
    b->limit = b->base + BLOCK_SIZE;
    b->search = LINES_PER_BLOCK;
  }
}

void gc_cache_or_free_block(Block *b) {
  if (g_alloc_cur[SPACE_NURSERY] == b) {
    gc_clear_alloc_cur(SPACE_NURSERY);
  }
  if (g_alloc_cur[SPACE_MATURE] == b) {
    gc_clear_alloc_cur(SPACE_MATURE);
  }
  if (g_alloc_cur[SPACE_TO] == b) {
    gc_clear_alloc_cur(SPACE_TO);
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
void gc_note_peak(void) {
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

Block *gc_new_block(uint8_t space) {
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
      gc_die("out of memory");
    }
    void *mem = NULL;
    if (posix_memalign(&mem, BLOCK_SIZE, BLOCK_SIZE) != 0) {
      gc_die("out of memory");
    }
    b->base = mem;
    g_heap_bytes += BLOCK_SIZE;
    gc_note_peak();
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

void gc_use_block(Block *b) {
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
  gc_use_block(b);
  if (bump_fits(total)) {
    return 1;
  }
  b->search = (uint16_t)((b->limit - b->base) / LINE_SIZE);
  if (!find_hole(b, total)) {
    return 0;
  }
  gc_use_block(b);
  return bump_fits(total);
}

void gc_clear_alloc_cur(uint8_t space) {
  g_alloc_cur[space] = NULL;
  g_alloc_fresh[space] = 0;
}

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

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
  Block *b = gc_new_block(space);
  if (!b) {
    if (!tried_gc && !g_collecting) {
      /* The reserve charge is soft (see heap_block_fits): collect,
       * then retry charged only the true heap bytes, so a reserve
       * refusal can never become a spurious heap-limit death. */
      gc_collect(1);
      g_reserve_retry = 1;
      {
        AvantHeader *retry = place(total, space, 1);
        g_reserve_retry = 0;
        return retry;
      }
    }
    gc_die("heap limit");
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
  gc_use_block(b);
  if (!bump_fits(total)) {
    gc_die("object larger than block");
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
  b = gc_new_block(space);
  if (!b) {
    return NULL;
  }
  g_alloc_cur[space] = b;
  g_alloc_fresh[space] = 1;
  gc_use_block(b);
  if (!bump_fits(total)) {
    return NULL;
  }
  return (AvantHeader *)bump_take(total);
}

void *gc_mature_alloc(size_t total) {
  /* Young copy promotes into mature space while g_collecting is set.
   * tried_gc stays 1 so a failed block does not re-enter collect. */
  g_mature_since_full += total;
  return place(total, SPACE_MATURE, 1);
}

AvantHeader *gc_to_alloc(size_t total) {
  AvantHeader *h = place(total, SPACE_TO, 1);
  g_to_bytes += total;
  return h;
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
    Large *L;
    g_stats.full_collections++;
    /* Non-moving. Evacuating the nursery here needs a second copy of
     * every live object while the from-space blocks are still charged
     * against the heap limit. */
    gc_los_spare_drop();
    gc_clear_alloc_cur(SPACE_NURSERY);
    gc_clear_alloc_cur(SPACE_MATURE);
    gc_clear_all_marks();
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
  gc_note_peak();
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
    atexit(gc_print_stats);
  }
}

void avant_type_map(uint32_t type_id, uint64_t word_bits) {
  gc_init();
  if (type_id == AVANT_TYPE_PTRS) {
    return;
  }
  if (type_id >= MAX_TYPES) {
    gc_die("type id too large");
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
    gc_die("type id too large");
  }
  if (word_base % 64 != 0) {
    gc_die("type map chunk is not 64-word aligned");
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
    gc_die("type map stride changed");
  }
  if (chunk >= m->nchunks) {
    uint64_t *nw = calloc((size_t)chunk + 1, sizeof(uint64_t));
    if (!nw) {
      gc_die("out of memory");
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
        gc_die("type map bit past the element stride");
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
      gc_die("out of memory");
    }
  }
  f->prev = g_frame;
  g_frame = f;
}

void avant_gc_leave(void) {
  Frame *f = g_frame;
  if (!f) {
    gc_die("gc leave without enter");
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
    gc_die("gc root without enter");
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
    f->slots = gc_xrealloc(f->slots, f->cap * sizeof(void **));
  }
  f->slots[f->n++] = (void **)slot;
}

void *avant_gc_root_begin(uint32_t count) {
  g_root_calls += count;
  if (!g_frame) {
    gc_die("gc root begin without enter");
  }
  Frame *f = g_frame;
  if (count > f->cap) {
    uint32_t cap = f->cap ? f->cap : 8;
    while (cap < count) {
      cap *= 2;
    }
    f->slots = gc_xrealloc(f->slots, cap * sizeof(void **));
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
    gc_die("out of memory");
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
    gc_collect(1);
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
    gc_die("object too large");
  }
  if (type_id > UINT16_MAX) {
    gc_die("type id too large");
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
      gc_collect(0);
      if (g_young_large_n > 0 || g_mature_since_full > mature_before) {
        g_los_eager_hold = 1;
      } else {
        g_los_eager_hold = 0;
      }
    }
    void *obj = gc_los_alloc(payload, type_id);
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
