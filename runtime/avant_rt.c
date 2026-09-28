#define _DEFAULT_SOURCE

#include "avant_rt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Precise Immix-family collector.
 *
 * Geometry: 32 KiB blocks, 256-byte lines. Object-start bits identify
 * headers. Tracing is precise (type maps + a shadow stack of local slots).
 *
 * GenImmix: bump nursery (copying) + Immix mature. Pinned nursery objects
 * stay put; that collection recycles nursery lines instead of discarding
 * the space. Objects that escape to C should be pinned (D34).
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
#define YOUNG_BYTES (512u * 1024u * 1024u)
#define MAX_TYPES 1024
#define CACHE_BLOCKS 2

#define SPACE_MATURE 0
#define SPACE_NURSERY 1

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

static Block *g_bht[BLOCK_HASH];
static Block *g_mature;
static Block *g_nursery;
static Block *g_cache;
static int g_cache_n;

static Block *g_cur;
static uint8_t *g_bump;
static uint8_t *g_limit;

static Large *g_large;
static Large *g_large_ht[LARGE_HASH];
static __thread Frame *g_frame;

typedef struct ThreadReg {
  struct ThreadReg *next;
  Frame **head;
} ThreadReg;

static __thread ThreadReg g_self_reg;
static ThreadReg *g_threads;
static pthread_mutex_t g_world = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_threads_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_gc_defer;

typedef struct PinEnt {
  struct PinEnt *next;
  void *payload;
} PinEnt;

static PinEnt *g_pins;

static void **g_remset;
static uint32_t g_rem_n;
static uint32_t g_rem_cap;

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

void avant_gc_register_thread(void) {
  if (g_self_reg.head) {
    return;
  }
  g_self_reg.head = &g_frame;
  pthread_mutex_lock(&g_threads_mu);
  g_self_reg.next = g_threads;
  g_threads = &g_self_reg;
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

static void rem_add(void *obj) {
  uint32_t i;
  for (i = 0; i < g_rem_n; i++) {
    if (g_remset[i] == obj) {
      return;
    }
  }
  if (g_rem_n == g_rem_cap) {
    g_rem_cap = g_rem_cap ? g_rem_cap * 2 : 64;
    g_remset = xrealloc(g_remset, g_rem_cap * sizeof(void *));
  }
  g_remset[g_rem_n++] = obj;
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

int avant_is_heap(void *payload) {
  return is_heap_payload(payload, NULL, NULL);
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
  Large *L;
  for (L = g_large; L; L = L->next) {
    L->header.flags &= (uint16_t)~(AVANT_FLAG_MARKED | AVANT_FLAG_FORWARDED);
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
  if (!is_heap_payload(p, NULL, NULL)) {
    return;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_MARKED) {
    return;
  }
  h->flags |= AVANT_FLAG_MARKED;
  mark_object_lines(p);
  g_live += obj_total(h);
  work_push(p);
}

static void drain(void) {
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    scan_payload(p);
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

static void cache_or_free_block(Block *b) {
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
  b->space = space;
  b->search = 0;
  b->bump = b->base;
  b->limit = b->base + BLOCK_SIZE;
  block_register(b);
  if (space == SPACE_NURSERY) {
    b->next = g_nursery;
    g_nursery = b;
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
  clear_starts(b, off, need);
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

static int take_from_list(Block *list, size_t total) {
  Block *b;
  for (b = list; b; b = b->next) {
    if (try_bump(b, total)) {
      return 1;
    }
  }
  return 0;
}

static AvantHeader *place(size_t total, uint8_t space, int tried_gc);

static void collect(int full);

static AvantHeader *place(size_t total, uint8_t space, int tried_gc) {
  Block *list = space == SPACE_NURSERY ? g_nursery : g_mature;
  if (take_from_list(list, total)) {
    return bump_take(total);
  }
  Block *b = new_block(space);
  if (!b) {
    if (!tried_gc && !g_collecting) {
      collect(1);
      return place(total, space, 1);
    }
    die("heap limit");
  }
  use_block(b);
  if (!bump_fits(total)) {
    die("object larger than block");
  }
  return bump_take(total);
}

static void *mature_alloc(size_t total) {
  if (take_from_list(g_mature, total)) {
    return bump_take(total);
  }
  Block *b = new_block(SPACE_MATURE);
  if (!b) {
    die("out of memory");
  }
  use_block(b);
  if (!bump_fits(total)) {
    die("object larger than block");
  }
  return bump_take(total);
}

static void copy_from_slot(void **slot);

static void copy_payload_children(void *p) {
  AvantHeader *h = avant_header(p);
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_BYTES) {
    return;
  }
  if (id == AVANT_TYPE_PTRS) {
    size_t n = (size_t)h->size / sizeof(void *);
    size_t i;
    for (i = 0; i < n; i++) {
      copy_from_slot((void **)((uint8_t *)p + i * sizeof(void *)));
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
      copy_from_slot((void **)((uint8_t *)p + i * sizeof(void *)));
    }
  }
}

static void *copy_nursery(void *p) {
  Block *b = NULL;
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
  AvantHeader *nh = mature_alloc(total);
  memcpy(nh, h, total);
  nh->flags = (uint16_t)((h->flags & (AVANT_FLAG_PINNED | AVANT_FLAG_HEAP)) | AVANT_FLAG_MARKED);
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
  if (!is_heap_payload(p, NULL, NULL)) {
    return;
  }
  *slot = copy_nursery(p);
}

static void gen_young_copy(void) {
  fwd_reset();
  g_saw_pinned_nursery = 0;
  ThreadReg *t;
  uint32_t i;
  pthread_mutex_lock(&g_threads_mu);
  for (t = g_threads; t; t = t->next) {
    Frame *f;
    for (f = t->head ? *t->head : NULL; f; f = f->prev) {
      for (i = 0; i < f->n; i++) {
        copy_from_slot(f->slots[i]);
      }
    }
  }
  pthread_mutex_unlock(&g_threads_mu);
  for (i = 0; i < g_rem_n; i++) {
    void *obj = g_remset[i];
    if (is_heap_payload(obj, NULL, NULL)) {
      copy_payload_children(obj);
    }
  }
  while (g_work_n) {
    void *p = g_work[--g_work_n];
    copy_payload_children(p);
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

static void collect(int full) {
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
    g_stats.young_collections++;
    zero_line_marks(g_nursery);
    gen_young_copy();
    if (g_saw_pinned_nursery) {
      filter_blocks(&g_nursery, keep_if_live);
    } else {
      filter_blocks(&g_nursery, NULL);
    }
  } else {
    g_stats.full_collections++;
    clear_all_marks();
    zero_line_marks(g_mature);
    zero_line_marks(g_nursery);
    mark_roots();
    sweep_large();
    filter_blocks(&g_mature, keep_if_live);
    filter_blocks(&g_nursery, keep_if_live);
  }

  g_rem_n = 0;
  g_since_gc = 0;
  if (full) {
    g_last_full_at = g_stats.allocated_bytes;
  }
  g_stats.live_bytes = g_live;
  g_collecting = 0;
}

static void maybe_collect(void) {
  if (g_collecting || g_gc_defer) {
    return;
  }
  /*
   * myc-llvm can keep heap pointers in GPRs across CALL. Young copy
   * rewrites the shadow stack only, so those registers go stale (prod
   * Binarytrees/Matmul SIGSEGV). Full collection is non-moving.
   *
   * C helpers also hold heap pointers that mark_roots does not see
   * (concat args, hash tables mid-grow). avant_gc_defer_* blocks
   * collection for that window.
   */
  if (g_since_gc >= YOUNG_BYTES) {
    collect(1);
  }
}

static void *los_alloc(size_t payload, uint32_t type_id) {
  size_t chunk = align_up(sizeof(Large) + payload);
  if (g_heap_max && g_heap_bytes + chunk > g_heap_max) {
    if (!g_gc_defer) {
      collect(1);
    }
    if (g_heap_max && g_heap_bytes + chunk > g_heap_max) {
      die("heap limit");
    }
  }
  Large *L = calloc(1, chunk);
  if (!L) {
    die("out of memory");
  }
  L->header.size = (uint32_t)payload;
  L->header.type_id = (uint16_t)type_id;
  L->header.flags = AVANT_FLAG_HEAP;
  L->next = g_large;
  g_large = L;
  large_hash_add(L);
  g_heap_bytes += chunk;
  if (g_heap_bytes > g_stats.peak_heap) {
    g_stats.peak_heap = g_heap_bytes;
  }
  return payload_of(&L->header);
}

static void print_stats(void) {
  fprintf(stderr,
          "avant gc: collections=%llu young=%llu full=%llu "
          "alloc=%llu copied=%llu live=%llu peak_heap=%llu\n",
          (unsigned long long)g_stats.collections,
          (unsigned long long)g_stats.young_collections,
          (unsigned long long)g_stats.full_collections,
          (unsigned long long)g_stats.allocated_bytes,
          (unsigned long long)g_stats.copied_bytes,
          (unsigned long long)g_stats.live_bytes,
          (unsigned long long)g_stats.peak_heap);
}

static void init_gc(void) {
  if (g_inited) {
    return;
  }
  g_inited = 1;
  uint64_t mb = env_u64("AVANT_HEAP_MAX_MB", 0);
  g_heap_max = mb ? mb * 1024ull * 1024ull : 0;
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

void avant_gc_enter(void) {
  init_gc();
  avant_gc_register_thread();
  Frame *f = calloc(1, sizeof(Frame));
  if (!f) {
    die("out of memory");
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
  free(f->slots);
  free(f);
}

void avant_gc_root(void *slot) {
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

void avant_barrier(void *obj) {
  if (!obj || g_collecting) {
    return;
  }
  if (!is_heap_payload(obj, NULL, NULL)) {
    return;
  }
  if (is_old(obj)) {
    rem_add(obj);
  }
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
    pthread_mutex_unlock(&g_world);
    return;
  }
  PinEnt *e = (PinEnt *)malloc(sizeof(PinEnt));
  if (!e) {
    pthread_mutex_unlock(&g_world);
    die("out of memory");
  }
  h->flags |= AVANT_FLAG_PINNED;
  e->payload = payload;
  e->next = g_pins;
  g_pins = e;
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
   */
  if (g_gc_defer == 0) {
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

void *avant_alloc(uint64_t nbytes, uint32_t type_id) {
  init_gc();
  if (nbytes > UINT32_MAX) {
    die("object too large");
  }
  if (type_id > UINT16_MAX) {
    die("type id too large");
  }

  pthread_mutex_lock(&g_world);
  size_t payload = (size_t)nbytes;
  size_t total = align_up(sizeof(AvantHeader) + payload);
  maybe_collect();

  if (total > LOS_LIMIT) {
    void *obj = los_alloc(payload, type_id);
    memset(obj, 0, payload);
    g_stats.allocated_bytes += total;
    g_since_gc += total;
    pthread_mutex_unlock(&g_world);
    return obj;
  }

  uint8_t space = SPACE_NURSERY;
  AvantHeader *h = place(total, space, 0);
  h->size = (uint32_t)payload;
  h->type_id = (uint16_t)type_id;
  h->flags = AVANT_FLAG_HEAP;
  void *obj = payload_of(h);
  memset(obj, 0, payload);
  g_stats.allocated_bytes += total;
  g_since_gc += total;
  pthread_mutex_unlock(&g_world);
  return obj;
}

void avant_gc_stats(AvantGcStats *out) {
  if (out) {
    *out = g_stats;
  }
}

void avant_gc_collect(void) {
  init_gc();
  pthread_mutex_lock(&g_world);
  collect(1);
  pthread_mutex_unlock(&g_world);
}
