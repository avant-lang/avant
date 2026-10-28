#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_rt.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Precise Immix-family collector.
 *
 * Geometry: 32 KiB blocks, 256-byte lines. Object-start bits identify
 * headers. Tracing is precise (type maps + a shadow stack of local slots).
 *
 * GenImmix: bump nursery (copying) + Immix mature. Pinned nursery objects
 * stay put (those blocks remain `SPACE_FROM` so their address is stable,
 * D34). After young copy, unpinned from-space is discarded. Set
 * `AVANT_NURSERY_DISCARD=0` to park all nursery blocks instead (leftover
 * pointer diagnosis). Objects that escape to C should be pinned.
 *
 * Stage 6 spawn uses 1:1 OS threads (D35). Frames are thread-local;
 * allocation and collection take a world lock.
 */

#define ALIGN 8
#define LINE_SIZE 256
#define LINES_PER_BLOCK 128
#define BLOCK_SIZE (LINE_SIZE * LINES_PER_BLOCK)
#define START_WORDS 64
#define BLOCK_HASH 4096
#define LARGE_HASH 4096
#define LOS_LIMIT 8192
/* Numeric buffers at or above this size are young large objects.
 * They do not move, so a parallel loop can keep the address in a
 * register, and a young collection still frees them when unmarked.
 * 900 doubles is 7200 bytes: under LOS_LIMIT, but large enough that
 * copying the row is the slow path. */
#define SCALAR_LOS_MIN 6144
/* 1 MiB retraced one depth-20 tree 93 times (3.85s, copied bytes
 * about equal to allocated bytes). 16 MiB is the measured default.
 * The full threshold stays 32 MiB, so young collection still runs.
 * AVANT_YOUNG_MB overrides this. */
#define YOUNG_BYTES_DEFAULT (16u * 1024u * 1024u)
#define FULL_BYTES (32u * 1024u * 1024u)
#define MAX_TYPES 1024
#define CACHE_BLOCKS 2

#define SPACE_MATURE 0
#define SPACE_NURSERY 1
#define SPACE_FROM 2
/* Survivors of a low-survival young collection are copied here, then the
 * block is retagged as nursery. Mature space is for objects that have
 * already survived a collection. */
#define SPACE_TO 3

typedef struct Block {
  struct Block *hash_next;
  struct Block *next;
  uint8_t *base;
  uint8_t space;
  uint16_t search;
  uint8_t *bump;
  uint8_t *limit;
  uint8_t line_mark[LINES_PER_BLOCK];
  uint64_t start[START_WORDS];
} Block;

typedef struct Large {
  struct Large *next;
  struct Large *hash_next;
  AvantHeader header;
} Large;

typedef struct Frame {
  struct Frame *prev;
  void ***slots;
  uint32_t n;
  uint32_t cap;
} Frame;

typedef struct FwdEnt {
  void *from;
  void *to;
} FwdEnt;

static int g_inited;
static int g_collecting;
static int g_saw_pinned_nursery;
static int g_nursery_off;
static int g_discard_from;
static int g_stackmap_walk;

static Block *g_bht[BLOCK_HASH];
static Block *g_mature;
static Block *g_nursery;
static Block *g_from;
static Block *g_to;
static Block *g_cache;
static int g_cache_n;

static Block *g_cur;
static uint8_t *g_bump;
static uint8_t *g_limit;
/* Per-space bump cursor. A full block used to send the next allocation
 * back through every older block (take_from_list + find_hole). Workers
 * allocate in mature space, and that walk dominated Matmul::T16.
 * g_alloc_fresh means the cursor is a new block: when it fills, allocate
 * another instead of rescanning full blocks. A full collection clears
 * the cursor so the next allocation reuses holes. */
static Block *g_alloc_cur[4];
static uint8_t g_alloc_fresh[4];

static Large *g_large;
static Large *g_large_ht[LARGE_HASH];
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

typedef struct ThreadReg {
  struct ThreadReg *next;
  Frame **head;
  void *stack_lo;
  void *stack_hi;
} ThreadReg;

static __thread ThreadReg g_self_reg;
static ThreadReg *g_threads;
/* Becomes 1 when a second thread registers, and stays 1. A single
 * mutator collects only at its own allocating call, so an interior
 * pointer used before the next call does not need a root. Another
 * thread can collect at any instruction. */
atomic_int g_avant_shared;
static pthread_mutex_t g_world = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_gc_defer;
static int g_stack_scan;
static uint64_t g_interior_slots;
static uint64_t g_sm_walks;
static uint64_t g_sm_hits;
static uint64_t g_sm_leftover;
static uint32_t g_sm_nsites;

typedef struct PinEnt {
  struct PinEnt *next;
  void *payload;
  int count;
} PinEnt;

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
static uint64_t g_young_bytes;
/* 0 when AVANT_YOUNG_MB pinned the threshold (tests). */
static int g_young_adapt;
static uint64_t g_nursery_held;
static uint64_t g_mature_since_full;
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

static uint64_t g_type_bits[MAX_TYPES];
static uint8_t g_type_set[MAX_TYPES];

static FwdEnt *g_fwd;
static size_t g_fwd_cap;
static size_t g_fwd_used;

static uint64_t g_since_gc;
static uint64_t g_heap_bytes;
static uint64_t g_heap_max;
static uint64_t g_live;
static uint64_t g_last_full_at;

static AvantGcStats g_stats;

static size_t align_up(size_t n) {
  return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

static void die(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
}

void avant_trap_bounds(int32_t index, int32_t size) {
  fprintf(stderr, "avant runtime: index %d out of bounds (size %d)\n", index, size);
  exit(1);
}

void avant_trap_div_zero(void) {
  fprintf(stderr, "avant runtime: division by zero\n");
  exit(1);
}

void avant_trap_overflow(void) {
  fprintf(stderr, "avant runtime: integer overflow\n");
  exit(1);
}

int64_t avant_checked_add_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_sub_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_mul_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_add_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_sub_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_mul_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_div_i64(int64_t a, int64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  if (a == INT64_MIN && b == -1) {
    avant_trap_overflow();
  }
  return a / b;
}

int64_t avant_checked_rem_i64(int64_t a, int64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  if (a == INT64_MIN && b == -1) {
    return 0;
  }
  return a % b;
}

uint64_t avant_checked_div_u64(uint64_t a, uint64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  return a / b;
}

uint64_t avant_checked_rem_u64(uint64_t a, uint64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  return a % b;
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

static void work_push(void *p) {
  if (g_work_n == g_work_cap) {
    g_work_cap = g_work_cap ? g_work_cap * 2 : 256;
    g_work = xrealloc(g_work, g_work_cap * sizeof(void *));
  }
  g_work[g_work_n++] = p;
}

#define AVANT_FLAG_REM 16u
/* Young large object reached from a heap slot. It must be promoted:
 * a mature object can hold it, and that edge is not in the remembered
 * set (a large payload has no block). A buffer reached only from the
 * stack stays young so the next collection can free it. */
#define AVANT_FLAG_LOS_KEEP 128u

static int rem_locked(void) {
  return atomic_load_explicit(&g_avant_shared, memory_order_acquire) != 0;
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
  }
  g_remset[g_rem_n++] = obj;
  if (locked) {
    pthread_mutex_unlock(&g_rem_mu);
  }
}

static int is_heap_payload(void *p, Block **ob, Large **ol);

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

static size_t hash_ptr(void *p) {
  uintptr_t x = (uintptr_t)p;
  x ^= x >> 16;
  x *= (uintptr_t)0x9e3779b97f4a7c15ULL;
  return (size_t)x;
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

static void fwd_put(void *from, void *to) {
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

static void *fwd_get(void *from) {
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

static size_t block_hash(const void *p) {
  return ((uintptr_t)p / BLOCK_SIZE) % BLOCK_HASH;
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

static Block *block_of(const void *p) {
  size_t k = block_hash(p);
  Block *b;
  for (b = g_bht[k]; b; b = b->hash_next) {
    if (p >= (void *)b->base && p < (void *)(b->base + BLOCK_SIZE)) {
      return b;
    }
  }
  return NULL;
}

static void set_start(Block *b, size_t off) {
  size_t bit = off / 8;
  b->start[bit / 64] |= 1ULL << (bit % 64);
}

static void clear_starts(Block *b, size_t off, size_t n) {
  size_t i;
  for (i = 0; i < n; i += 8) {
    size_t bit = (off + i) / 8;
    b->start[bit / 64] &= ~(1ULL << (bit % 64));
  }
}

static int has_start(Block *b, size_t off) {
  size_t bit = off / 8;
  return (int)((b->start[bit / 64] >> (bit % 64)) & 1ULL);
}

static void mark_lines(Block *b, size_t off, size_t total) {
  size_t start = off / LINE_SIZE;
  size_t end = (off + total - 1) / LINE_SIZE;
  size_t i;
  for (i = start; i <= end && i < LINES_PER_BLOCK; i++) {
    b->line_mark[i] = 1;
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

static size_t obj_total(const AvantHeader *h) {
  return align_up(sizeof(AvantHeader) + (size_t)h->size);
}

static void *payload_of(AvantHeader *h) {
  return (void *)(h + 1);
}

static size_t large_key(void *payload) {
  return hash_ptr(payload) & (size_t)(LARGE_HASH - 1);
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
  } else {
    free(L);
  }
}

static void free_large(Large *L) {
  release_large(L);
}

static void promote_young_large(Large *L) {
  size_t chunk = large_chunk(L);
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

static Large *large_of(void *payload) {
  Large *L;
  if (!payload) {
    return NULL;
  }
  for (L = g_large_ht[large_key(payload)]; L; L = L->hash_next) {
    if (payload == payload_of(&L->header)) {
      return L;
    }
  }
  return NULL;
}

static int is_heap_payload(void *p, Block **ob, Large **ol) {
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

static void mark_object_lines(void *p) {
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

static void scan_word(void *p, size_t off) {
  if (off + sizeof(void *) > avant_header(p)->size) {
    return;
  }
  void *child = *(void **)((uint8_t *)p + off);
  mark(child);
}

static void scan_payload(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_BYTES) {
    return;
  }
  if (id == AVANT_TYPE_PTRS) {
    size_t n = (size_t)h->size / sizeof(void *);
    size_t i;
    for (i = 0; i < n; i++) {
      scan_word(p, i * sizeof(void *));
    }
    return;
  }
  if (id >= MAX_TYPES || !g_type_set[id]) {
    return;
  }
  uint64_t bits = g_type_bits[id];
  size_t i;
  for (i = 0; i < 64; i++) {
    if (bits & (1ULL << i)) {
      scan_word(p, i * sizeof(void *));
    }
  }
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

static Block *new_block(uint8_t space) {
  if (g_heap_max && g_heap_bytes + BLOCK_SIZE > g_heap_max && g_cache == NULL) {
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
    if (g_heap_max && g_heap_bytes + BLOCK_SIZE > g_heap_max) {
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
    if (g_heap_bytes > g_stats.peak_heap) {
      g_stats.peak_heap = g_heap_bytes;
    }
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
  return h;
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
      collect(1);
      return place(total, space, 1);
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
  if (ob && (ob->space == SPACE_NURSERY || ob->space == SPACE_TO)) {
    return;
  }
  keep_add(owner);
}

static void copy_from_slot(void **slot);
static int g_from_heap_slot;
static void trace_mature(void *p, int from_mature);

static void copy_payload_children(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  int saved = g_from_heap_slot;
  g_from_heap_slot = 1;
  if (id == AVANT_TYPE_BYTES) {
    g_from_heap_slot = saved;
    return;
  }
  if (id == AVANT_TYPE_PTRS) {
    size_t n = (size_t)h->size / sizeof(void *);
    size_t i;
    for (i = 0; i < n; i++) {
      copy_from_slot((void **)((uint8_t *)p + i * sizeof(void *)));
    }
    g_from_heap_slot = saved;
    return;
  }
  if (id >= MAX_TYPES || !g_type_set[id]) {
    g_from_heap_slot = saved;
    return;
  }
  uint64_t bits = g_type_bits[id];
  size_t i;
  for (i = 0; i < 64; i++) {
    if (bits & (1ULL << i)) {
      copy_from_slot((void **)((uint8_t *)p + i * sizeof(void *)));
    }
  }
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
  copy_payload_children(p);
  g_from_mature = saved;
}

static void *copy_nursery(void *p) {
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

static void copy_from_slot(void **slot) {
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
      probe_note(start);
      return;
    }
    ptrdiff_t delta = (uint8_t *)p - (uint8_t *)start;
    void *np = copy_nursery(start);
    *slot = (uint8_t *)np + delta;
    remember_young_edge(slot, np);
    return;
  }
  /* Mature and large objects stay put. Follow one when this slot is a
   * root or a field of a young object. A field of a mature object is
   * followed only when that child is remembered. */
  if (ol || (b && b->space == SPACE_MATURE)) {
    trace_mature(start, g_from_mature);
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

/*
 * LLVM spill slots hold heap pointers that avant_gc_root never sees.
 * After precise copy, from-space object-starts on the live C stack are
 * rewritten to to-space when the object was already forwarded. Words
 * that only appear here are pinned in place (D34-shaped) so we do not
 * treat a packed i32 pair as a root that gets copied. Do not walk
 * interiors: that smashed compile-compiler.
 */
static void pin_nursery_in_place(void *p) {
  if (fwd_get(p)) {
    return;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_PINNED) {
    copy_nursery(p);
    return;
  }
  fwd_put(p, p);
  g_saw_pinned_nursery = 1;
  if (!(h->flags & AVANT_FLAG_MARKED)) {
    h->flags |= AVANT_FLAG_MARKED;
    mark_object_lines(p);
    g_live += obj_total(h);
    work_push(p);
  }
}

static void copy_from_c_stack(void) {
  uint8_t *sp;
  uint8_t *hi;
  uint8_t *p;
  uint8_t *lo;
  if (!g_stack_scan) {
    return;
  }
  sp = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
  if (!hi || !sp || sp >= (uint8_t *)hi) {
    return;
  }
  if (lo && sp < (uint8_t *)lo) {
    return;
  }
  p = (uint8_t *)((uintptr_t)sp & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= (uint8_t *)hi; p += ALIGN) {
    void *w = *(void **)p;
    Block *b = NULL;
    if (!is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      void *to = fwd_get(w);
      if (to) {
        *(void **)p = to;
      } else {
        pin_nursery_in_place(w);
      }
    }
  }
}

extern char __LLVM_StackMaps[] __attribute__((weak));

#define SM_DIRECT 2
#define SM_INDIRECT 3
#define DWARF_X64_RBP 6
#define DWARF_X64_RSP 7

typedef struct SmSite {
  uintptr_t site;
  const uint8_t *rec;
  uint32_t fsize;
} SmSite;

static const uint8_t *g_sm_sec;
static uintptr_t g_load_bias;
static SmSite *g_sm_sites;

static uint16_t sm_u16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

static uint32_t sm_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint64_t sm_u64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static int32_t sm_i32(const uint8_t *p) {
  int32_t v;
  memcpy(&v, p, 4);
  return v;
}

static unsigned sm_record_size(const uint8_t *rec) {
  uint16_t nloc = sm_u16(rec + 14);
  unsigned loc_end = ((16u + 12u * nloc) + 7u) & ~7u;
  unsigned nlo_off = loc_end + 2u;
  uint16_t nlive = sm_u16(rec + nlo_off);
  unsigned rec_end = nlo_off + 2u + 4u * nlive;
  return (rec_end + 7u) & ~7u;
}

static void sm_apply_record(const uint8_t *rec, void *rbp, void *rsp) {
  uint16_t nloc = sm_u16(rec + 14);
  const uint8_t *loc = rec + 16;
  uint16_t i;
  for (i = 0; i < nloc; i++, loc += 12) {
    uint8_t kind = loc[0];
    uint16_t size = sm_u16(loc + 2);
    uint16_t reg = sm_u16(loc + 4);
    int32_t off = sm_i32(loc + 8);
    uint8_t *base;
    if (size != sizeof(void *)) {
      continue;
    }
    if (kind != SM_INDIRECT && kind != SM_DIRECT) {
      continue;
    }
    if (reg == DWARF_X64_RBP) {
      base = (uint8_t *)rbp;
    } else if (reg == DWARF_X64_RSP && rsp) {
      base = (uint8_t *)rsp;
    } else {
      continue;
    }
    copy_from_slot((void **)(base + off));
  }
}

static int sm_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
  int *seen = (int *)data;
  (void)size;
  if (*seen) {
    return 1;
  }
  g_load_bias = (uintptr_t)info->dlpi_addr;
  *seen = 1;
  return 1;
}

/*
 * LLVM emits __LLVM_StackMaps as a local section label. The runtime's
 * weak extern stays UND in dynsym, so the pointer is always NULL.
 * Find .llvm_stackmaps via ELF section headers of /proc/self/exe.
 */
static const uint8_t *sm_section_from_exe(void) {
  int fd;
  Elf64_Ehdr eh;
  Elf64_Shdr *sh;
  char *names;
  const uint8_t *sec = NULL;
  size_t bytes;
  uint32_t i;
  uint32_t strndx;

  fd = open("/proc/self/exe", O_RDONLY);
  if (fd < 0) {
    return NULL;
  }
  if (read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh) ||
      memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_shentsize != sizeof(Elf64_Shdr) ||
      eh.e_shoff == 0 || eh.e_shnum == 0) {
    close(fd);
    return NULL;
  }
  bytes = (size_t)eh.e_shnum * sizeof(Elf64_Shdr);
  sh = malloc(bytes);
  if (!sh) {
    close(fd);
    return NULL;
  }
  if (lseek(fd, (off_t)eh.e_shoff, SEEK_SET) != (off_t)eh.e_shoff ||
      (size_t)read(fd, sh, bytes) != bytes) {
    free(sh);
    close(fd);
    return NULL;
  }
  strndx = eh.e_shstrndx;
  if (strndx >= eh.e_shnum) {
    free(sh);
    close(fd);
    return NULL;
  }
  names = malloc(sh[strndx].sh_size + 1);
  if (!names) {
    free(sh);
    close(fd);
    return NULL;
  }
  if (lseek(fd, (off_t)sh[strndx].sh_offset, SEEK_SET) !=
          (off_t)sh[strndx].sh_offset ||
      (size_t)read(fd, names, sh[strndx].sh_size) != sh[strndx].sh_size) {
    free(names);
    free(sh);
    close(fd);
    return NULL;
  }
  names[sh[strndx].sh_size] = 0;
  for (i = 0; i < eh.e_shnum; i++) {
    uint32_t no = sh[i].sh_name;
    if (no >= sh[strndx].sh_size) {
      continue;
    }
    if (strcmp(names + no, ".llvm_stackmaps") != 0) {
      continue;
    }
    sec = (const uint8_t *)(g_load_bias + (uintptr_t)sh[i].sh_addr);
    break;
  }
  free(names);
  free(sh);
  close(fd);
  return sec;
}

static int sm_site_cmp(const void *a, const void *b) {
  const SmSite *x = (const SmSite *)a;
  const SmSite *y = (const SmSite *)b;
  if (x->site < y->site) {
    return -1;
  }
  if (x->site > y->site) {
    return 1;
  }
  return 0;
}

static void sm_index_records(void) {
  const uint8_t *sec = g_sm_sec;
  uint32_t nfn, nconst, nrec, fi, ri;
  const uint8_t *fnp;
  const uint8_t *rec;
  uint32_t cap;

  if (!sec || sec[0] != 3) {
    return;
  }
  nfn = sm_u32(sec + 4);
  nconst = sm_u32(sec + 8);
  nrec = sm_u32(sec + 12);
  if (nrec == 0 || nrec > 10000000u) {
    return;
  }
  fnp = sec + 16;
  rec = fnp + (unsigned)nfn * 24u + (unsigned)nconst * 8u;
  cap = nrec;
  g_sm_sites = malloc((size_t)cap * sizeof(SmSite));
  if (!g_sm_sites) {
    return;
  }
  ri = 0;
  for (fi = 0; fi < nfn; fi++) {
    uint64_t faddr = sm_u64(fnp + (unsigned)fi * 24u);
    uint64_t fcount = sm_u64(fnp + (unsigned)fi * 24u + 16u);
    uint64_t k;
    uintptr_t fbase = (uintptr_t)faddr;
    /*
     * R_X86_64_RELATIVE in .llvm_stackmaps (TEXTREL) rewrites faddr to
     * the runtime VA. If the loader left the ELF vaddr, add the bias.
     */
    if (g_load_bias && fbase < g_load_bias) {
      fbase += g_load_bias;
    }
    uint64_t fsize = sm_u64(fnp + (unsigned)fi * 24u + 8u);
    uint32_t fsize32 = 0;
    if (fsize && fsize != UINT64_MAX && fsize <= (1u << 20)) {
      fsize32 = (uint32_t)fsize;
    }
    for (k = 0; k < fcount && ri < nrec; k++, ri++) {
      uint32_t inst_off = sm_u32(rec + 8);
      /* LLVM records the return address (next insn after CALL). */
      g_sm_sites[g_sm_nsites].site = fbase + inst_off;
      g_sm_sites[g_sm_nsites].rec = rec;
      g_sm_sites[g_sm_nsites].fsize = fsize32;
      g_sm_nsites++;
      rec += sm_record_size(rec);
    }
  }
  if (g_sm_nsites) {
    qsort(g_sm_sites, g_sm_nsites, sizeof(SmSite), sm_site_cmp);
  }
}

static void sm_init(void) {
  int seen = 0;
  if (g_sm_sec || !g_stackmap_walk) {
    return;
  }
  dl_iterate_phdr(sm_phdr_cb, &seen);
  if (__LLVM_StackMaps && (unsigned char)__LLVM_StackMaps[0] == 3) {
    g_sm_sec = (const uint8_t *)__LLVM_StackMaps;
  } else {
    g_sm_sec = sm_section_from_exe();
  }
  sm_index_records();
}

static SmSite *sm_find_site(void *ret) {
  uintptr_t retu = (uintptr_t)ret;
  uint32_t lo = 0;
  uint32_t hi;
  if (!g_sm_nsites) {
    return NULL;
  }
  hi = g_sm_nsites;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (g_sm_sites[mid].site < retu) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= g_sm_nsites || g_sm_sites[lo].site != retu) {
    return NULL;
  }
  return &g_sm_sites[lo];
}

static int sm_lookup_apply(void *ret, void *caller_rbp, void *callee_rbp) {
  SmSite *site;
  const uint8_t *rec;
  void *rsp;
  site = sm_find_site(ret);
  if (!site) {
    return 0;
  }
  rec = site->rec;
  /*
   * Callee prologue: push rbp; mov rsp, rbp. Caller's RSP at the CALL
   * (before pushing the return address, after any stack args) is
   * callee_rbp + 16. LLVM stack size is one number per function, so
   * rbp - fsize is wrong at callsites that push extra stack arguments.
   */
  rsp = callee_rbp ? (uint8_t *)callee_rbp + 16 : NULL;
  sm_apply_record(rec, caller_rbp, rsp);
  g_sm_hits++;
  return 1;
}

static void rewrite_if_forwarded(void **slot) {
  void *w = *slot;
  void *to = fwd_get(w);
  if (to && to != w) {
    *slot = to;
  }
}

/*
 * After the work queue drains, every copied object is in the forwarding
 * table. Stackmap apply ran earlier as a *root* walk (copy_from_slot).
 * LLVM still leaves extra as(0) spills of the same pointer bits that
 * were never in gc-live. Rewrite those words in the matched myc frame
 * (LLVM stack size from the callsite RSP) so discard does not alias.
 * Exact fwd_get only: packed i32s that are not a copied payload stay.
 */
static void sm_rewrite_frame(void *rsp, uint32_t fsize) {
  uint8_t *p;
  uint8_t *end;
  uint8_t *hi;
  if (!rsp || fsize < sizeof(void *)) {
    return;
  }
  p = (uint8_t *)rsp;
  end = p + fsize;
  hi = g_self_reg.stack_hi;
  if (hi && end > hi) {
    end = hi;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= end; p += ALIGN) {
    rewrite_if_forwarded((void **)p);
  }
}

static int sm_lookup_rewrite(void *ret, void *caller_rbp, void *callee_rbp) {
  SmSite *site;
  void *rsp;
  (void)caller_rbp;
  site = sm_find_site(ret);
  if (!site) {
    return 0;
  }
  rsp = callee_rbp ? (uint8_t *)callee_rbp + 16 : NULL;
  sm_rewrite_frame(rsp, site->fsize);
  return 1;
}

static void walk_c_frames(int roots) {
  uint8_t *fp;
  uint8_t *hi;
  uint8_t *lo;
  uint32_t n;
  if (!g_stackmap_walk || !g_sm_nsites) {
    return;
  }
  fp = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
  if (!fp || !hi) {
    return;
  }
  for (n = 0; n < 256 && fp; n++) {
    uint8_t *next;
    void *ret;
    if (lo && fp < lo) {
      break;
    }
    if (fp >= hi) {
      break;
    }
    if (((uintptr_t)fp & 7u) != 0) {
      break;
    }
    next = *(uint8_t **)fp;
    ret = *((void **)fp + 1);
    if (ret) {
      if (roots) {
        g_sm_walks++;
        sm_lookup_apply(ret, next, fp);
      } else {
        sm_lookup_rewrite(ret, next, fp);
      }
    }
    if (!next || next <= fp) {
      break;
    }
    fp = next;
  }
}

static void copy_from_stackmaps(void) {
  /*
   * On with AVANT_NURSERY=1 unless AVANT_NURSERY_STACKMAP=0. Callsite
   * RSP is callee_rbp+16 (not LLVM stack size). Mapped slots are roots
   * (copy_from_slot). Leftover spills in the same frame are rewritten
   * after the work queue (rewrite_from_stackmaps).
   */
  walk_c_frames(1);
}

static void rewrite_from_stackmaps(void) {
  walk_c_frames(0);
}

static void count_c_stack_nursery(void) {
  uint8_t *p;
  uint8_t *hi;
  uint8_t *lo;
  if (!g_stackmap_walk) {
    return;
  }
  p = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
  if (!hi || !p) {
    return;
  }
  if (lo && p < lo) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= hi; p += ALIGN) {
    void *w = *(void **)p;
    Block *b = NULL;
    if (!is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      void *to = fwd_get(w);
      /* Copied to to-space, but this stack word still names from-space. */
      if (to && to != w) {
        g_sm_leftover++;
      }
    }
  }
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
      g_remset[w++] = obj;
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
  count_c_stack_nursery();
}

static int g_block_has_pin;

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
    if (block_has_pinned(b)) {
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
  g_keep = NULL;
  g_keep_n = 0;
  g_keep_cap = 0;
  rem_forget();
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
  if (next <= g_young_bytes) {
    return 0;
  }
  g_young_bytes = next;
  g_sticky_grows++;
  return 1;
}

static void young_copy_out(int tenure) {
  g_force_tenure = tenure;
  g_to_bytes = 0;
  g_keep_n = 0;
  zero_line_marks(g_nursery);
  gen_young_copy();
  clear_alloc_cur(SPACE_NURSERY);
  if (g_discard_from) {
    discard_nursery_from();
  } else {
    park_nursery_from();
  }
  adopt_to_space();
  rem_commit_kept();
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
  g_stats.collections++;
  g_live = 0;
  g_work_n = 0;
  g_cur = NULL;
  g_bump = NULL;
  g_limit = NULL;

  if (!full) {
#if defined(__x86_64__)
    /* Spill myc callee-saved GPRs into collect's frame so the C-stack
     * walk can rewrite or pin them. Without this, a heap pointer can
     * sit in rbx across avant_alloc and miss the shadow stack. */
    __asm__ volatile ("" ::: "rbx", "r12", "r13", "r14", "r15", "memory");
#endif
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

  g_since_gc = 0;
  g_stats.live_bytes = g_live;
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
    if (g_since_gc >= FULL_BYTES) {
      collect(1);
    }
    return;
  }
  /* Nursery garbage is reclaimed by a young collection. A full
   * collection runs when mature space, including promotions and large
   * objects, has grown by FULL_BYTES. Counting every nursery byte here
   * marked a live tree that a young collection should have discarded. */
  if (g_mature_since_full >= FULL_BYTES) {
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
  if (g_los_spare && g_los_spare->header.size == (uint32_t)payload) {
    L = g_los_spare;
    g_los_spare = NULL;
    memset(payload_of(&L->header), 0, payload);
  } else {
    L = calloc(1, chunk);
    if (!L) {
      die("out of memory");
    }
    g_heap_bytes += chunk;
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
  large_hash_add(L);
  if (g_heap_bytes > g_stats.peak_heap) {
    g_stats.peak_heap = g_heap_bytes;
  }
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
  if (g_heap_bytes > g_stats.peak_heap) {
    g_stats.peak_heap = g_heap_bytes;
  }
}

static void print_stats(void) {
  fprintf(stderr,
          "avant gc: collections=%llu young=%llu full=%llu "
          "alloc=%llu copied=%llu live=%llu peak_heap=%llu interiors=%llu sm_walks=%llu sm_hits=%llu sm_sites=%u sm_left=%llu "
          "sticky=%llu young_mb=%llu held=%llu mature_since=%llu\n",
          (unsigned long long)g_stats.collections,
          (unsigned long long)g_stats.young_collections,
          (unsigned long long)g_stats.full_collections,
          (unsigned long long)g_stats.allocated_bytes,
          (unsigned long long)g_stats.copied_bytes,
          (unsigned long long)g_stats.live_bytes,
          (unsigned long long)g_stats.peak_heap,
          (unsigned long long)g_interior_slots,
          (unsigned long long)g_sm_walks,
          (unsigned long long)g_sm_hits,
          g_sm_nsites,
          (unsigned long long)g_sm_leftover,
          (unsigned long long)g_sticky_grows,
          (unsigned long long)(g_young_bytes / (1024ull * 1024ull)),
          (unsigned long long)g_nursery_held,
          (unsigned long long)g_mature_since_full);
}

static void init_gc(void) {
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
  if (getenv("AVANT_GC_STATS")) {
    atexit(print_stats);
  }
}

void avant_type_map(uint32_t type_id, uint64_t word_bits) {
  init_gc();
  if (type_id == AVANT_TYPE_PTRS) {
    return;
  }
  if (type_id >= MAX_TYPES) {
    die("type id too large");
  }
  g_type_bits[type_id] = word_bits;
  g_type_set[type_id] = 1;
}

/* A call used to calloc a frame and free it on leave. Hot recursion
 * (Brainfuck) and numeric methods spent the call on that pair. Frames
 * are kept on a thread-local freelist; the slot array is reused too. */
void avant_gc_enter(void) {
  init_gc();
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

__attribute__((noinline))
void *avant_gc_reload(void *slot) {
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
  init_gc();
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

void avant_alloc_in_mature(int32_t on) {
  g_alloc_mature = on ? 1 : 0;
}

void avant_world_lock(void) {
  init_gc();
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
  init_gc();
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
  init_gc();
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
  init_gc();
  pthread_mutex_lock(&g_world);
  if (!g_nursery_off && !g_collecting && g_nursery_held > 0) {
    collect(0);
  }
  pthread_mutex_unlock(&g_world);
}

void avant_gc_collect(void) {
  init_gc();
  pthread_mutex_lock(&g_world);
  collect(1);
  pthread_mutex_unlock(&g_world);
}
