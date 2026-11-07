#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The heap's shape (Stage 28 W12; the reservation, chunks and side
 * tables since W14a1): the reserved range and its block supply, the
 * spaces' block lists, object starts and line marks, object lookup (is
 * a word a heap payload, which object holds an address), the forwarding
 * table, the mark work list, and the type maps every tracer reads
 * through EACH_REF_SLOT. Also the collector's two error helpers. */

typedef struct FwdEnt {
  void *from;
  void *to;
} FwdEnt;

/* The reservation (gc-design.md §2.1, avant_gc_internal.h). g_heap_base
 * is 4 MiB aligned; g_heap_reserved bytes from it are reserved, the
 * first g_heap_committed of them committed (whole chunks, in order). The
 * side tables are reserved for the whole range and committed by the
 * slice of each chunk. */
uint8_t *g_heap_base;
uint64_t g_heap_committed;
uint64_t g_heap_reserved;
Block *g_blocks;
uint64_t *g_starts;
uint8_t *g_lines;
GcChunk *g_chunks;
static Block *g_free_blocks;

Block *g_mature;
Block *g_nursery;
Block *g_from;
Block *g_to;

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

void gc_die(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
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

/* Reserve LEN bytes of address space, PROT_NONE and MAP_NORESERVE: no
 * memory and (under vm.overcommit_memory=2) no commit charge until a
 * part is made read-write. */
static void *reserve(uint64_t len) {
  void *p = mmap(NULL, (size_t)len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static void unreserve(void *p, uint64_t len) {
  if (p) {
    munmap(p, (size_t)len);
  }
}

/* One attempt at a reservation of BYTES (a multiple of CHUNK_SIZE):
 * the data range, 4 MiB aligned (over-reserved by one chunk, the excess
 * returned), and the three side tables and the chunk map at their
 * ratios (records 1/512, start bits 1/64, line marks 1/256, the map 8
 * bytes per chunk). All or nothing. */
static int reserve_all(uint64_t bytes) {
  uint64_t nblocks = bytes >> BLOCK_SHIFT;
  uint64_t nchunks = bytes >> CHUNK_SHIFT;
  uint64_t rec_len = nblocks * sizeof(Block);
  uint64_t st_len = nblocks * START_WORDS * sizeof(uint64_t);
  uint64_t ln_len = nblocks * LINES_PER_BLOCK;
  uint64_t ch_len = (nchunks * sizeof(GcChunk) + 4095) & ~(uint64_t)4095;
  uint8_t *raw = reserve(bytes + CHUNK_SIZE);
  void *rec = raw ? reserve(rec_len) : NULL;
  void *st = rec ? reserve(st_len) : NULL;
  void *ln = st ? reserve(ln_len) : NULL;
  void *ch = ln ? reserve(ch_len) : NULL;
  if (!ch) {
    unreserve(raw, bytes + CHUNK_SIZE);
    unreserve(rec, rec_len);
    unreserve(st, st_len);
    unreserve(ln, ln_len);
    unreserve(ch, ch_len);
    return 0;
  }
  {
    uint8_t *base = (uint8_t *)(((uintptr_t)raw + CHUNK_SIZE - 1) & ~(uintptr_t)(CHUNK_SIZE - 1));
    uint64_t head = (uint64_t)(base - raw);
    if (head) {
      munmap(raw, (size_t)head);
    }
    if (CHUNK_SIZE - head) {
      munmap(base + bytes, (size_t)(CHUNK_SIZE - head));
    }
    g_heap_base = base;
  }
  g_blocks = rec;
  g_starts = st;
  g_lines = ln;
  g_chunks = ch;
  g_heap_reserved = bytes;
  g_heap_committed = 0;
  return 1;
}

/* The reservation is AVANT_HEAP_RESERVE_GB GiB (default 64); when the
 * system refuses it (ulimit -v, a container's address-space limit), the
 * size is halved until it succeeds, down to 1 GiB. It is the heap's
 * ceiling for the process's life (gc_block_take). Called once, by
 * gc_init_slow. */
void gc_heap_reserve(void) {
  uint64_t gb;
  if (g_heap_base) {
    return;
  }
  gb = HEAP_RESERVE_GB_DEFAULT;
  {
    const char *v = getenv("AVANT_HEAP_RESERVE_GB");
    if (v && *v) {
      char *end = NULL;
      unsigned long long n = strtoull(v, &end, 10);
      if (end && *end == 0 && n >= HEAP_RESERVE_GB_MIN && n <= (1ull << 20)) {
        gb = (uint64_t)n;
      }
    }
  }
  for (;;) {
    if (reserve_all(gb << 30)) {
      return;
    }
    if (gb <= HEAP_RESERVE_GB_MIN) {
      gc_die("cannot reserve the heap's address range (1 GiB minimum)");
    }
    gb /= 2;
  }
}

static int commit(void *p, uint64_t len) {
  return mprotect(p, (size_t)len, PROT_READ | PROT_WRITE) == 0;
}

/* Commit the next chunk: its data, its slices of the block records,
 * start bits and line marks, and the chunk map's page holding its entry.
 * Fresh anonymous memory reads as zero, so its start bits and line marks
 * are clear. Its blocks go on the free list, lowest address on top. */
static void commit_chunk(void) {
  uint64_t c = g_heap_committed >> CHUNK_SHIFT;
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  int i;
  if (g_heap_committed + CHUNK_SIZE > g_heap_reserved) {
    gc_die("heap reservation exhausted (AVANT_HEAP_RESERVE_GB raises it)");
  }
  {
    uintptr_t e = (uintptr_t)&g_chunks[c];
    uintptr_t pg = e & ~(uintptr_t)4095;
    if (!commit((void *)pg, 4096) ||
        !commit(g_heap_base + g_heap_committed, CHUNK_SIZE) ||
        !commit(&g_blocks[b0], BLOCKS_PER_CHUNK * sizeof(Block)) ||
        !commit(&g_starts[b0 * START_WORDS], BLOCKS_PER_CHUNK * START_WORDS * sizeof(uint64_t)) ||
        !commit(&g_lines[b0 * LINES_PER_BLOCK], BLOCKS_PER_CHUNK * LINES_PER_BLOCK)) {
      gc_die("out of memory (a heap chunk could not be committed)");
    }
  }
  g_chunks[c].kind = CHUNK_SMALL;
  g_chunks[c].used = 0;
  for (i = (int)BLOCKS_PER_CHUNK - 1; i >= 0; i--) {
    uint64_t k = b0 + (uint64_t)i;
    Block *b = &g_blocks[k];
    b->base = g_heap_base + (k << BLOCK_SHIFT);
    b->line_mark = &g_lines[k * LINES_PER_BLOCK];
    b->start = &g_starts[k * START_WORDS];
    b->in_use = 0;
    b->next = g_free_blocks;
    g_free_blocks = b;
  }
  g_heap_committed += CHUNK_SIZE;
}

/* A free block, its start bits and line marks clear, not yet in use
 * (gc_new_block makes it so). Never fails: past the reservation it
 * dies. Memory is not charged here; g_heap_bytes is gc_new_block's. */
Block *gc_block_take(void) {
  Block *b;
  if (!g_heap_base) {
    gc_heap_reserve();
  }
  if (!g_free_blocks) {
    commit_chunk();
  }
  b = g_free_blocks;
  g_free_blocks = b->next;
  b->next = NULL;
  memset(b->line_mark, 0, LINES_PER_BLOCK);
  memset(b->start, 0, START_WORDS * sizeof(uint64_t));
  chunk_of(b->base)->used++;
  return b;
}

/* Back to the free list (the block must already be out of use). Its
 * memory stays committed: returning it to the OS is W14a2's (§2.5). */
void gc_block_give(Block *b) {
  b->in_use = 0;
  chunk_of(b->base)->used--;
  b->next = g_free_blocks;
  g_free_blocks = b;
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

/* The static image: the executable's own mapped range, from its ELF
 * header to the end of its data (both defined by the linker; weak, so a
 * link that lacks them only loses the shortcut). String literals and
 * other static objects lie there, and nothing the collector allocates
 * does (O29). */
extern const char __ehdr_start[] __attribute__((weak));
extern const char _end[] __attribute__((weak));

static inline int in_static_image(const void *p) {
  return __ehdr_start && _end && (const char *)p >= __ehdr_start && (const char *)p < _end;
}

/* H1 (gc-audit.md §2): p is a heap payload iff it is 8-aligned and its
 * header is the start of an object in a block in use, or p is a listed
 * large object's payload. The order of the tests is the order of their
 * cost: one compare for the committed heap (blocks hold every non-large
 * object), one for the static image, then the large-object hash. Large
 * objects are malloc'ed (W14a1 keeps the large list), never inside the
 * reservation, so an address there needs no large lookup. */
int is_heap_payload(void *p, Block **ob, Large **ol) {
  uintptr_t off;
  if (!p || (uintptr_t)p % ALIGN) {
    return 0;
  }
  if (heap_offset((uint8_t *)p - sizeof(AvantHeader), &off)) {
    Block *b = &g_blocks[off >> BLOCK_SHIFT];
    uint64_t g = off >> 3;
    if (!b->in_use || !((g_starts[g >> 6] >> (g & 63)) & 1ULL)) {
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
  if (in_static_image(p)) {
    return 0;
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
 * An interior slot (a field's or element's address) can be rooted;
 * the young copy must still move the object and keep the same offset
 * into it. In a block: the nearest start bit at or before p, a 64-bit
 * word of the start bitmap at a time, bounded by the block (an object
 * never spans two blocks, H4), and p must lie inside that object.
 */
void *gc_object_payload(void *p, Block **ob, Large **ol) {
  uintptr_t off;
  if (is_heap_payload(p, ob, ol)) {
    return p;
  }
  if (!p) {
    return NULL;
  }
  if (heap_offset(p, &off)) {
    Block *b = &g_blocks[off >> BLOCK_SHIFT];
    uint8_t *addr = (uint8_t *)p;
    size_t g;
    size_t w;
    uint64_t bits;
    if (!b->in_use) {
      return NULL;
    }
    g = (size_t)((off & (BLOCK_SIZE - 1)) >> 3);
    w = g >> 6;
    bits = b->start[w] & (~0ULL >> (63 - (g & 63)));
    while (!bits) {
      if (w == 0) {
        return NULL;
      }
      bits = b->start[--w];
    }
    {
      size_t cand = (w * 64 + (size_t)(63 - __builtin_clzll(bits))) * 8;
      AvantHeader *h = (AvantHeader *)(b->base + cand);
      uint8_t *hs = (uint8_t *)h;
      if (addr >= hs && addr < hs + obj_total(h)) {
        if (ob) {
          *ob = b;
        }
        if (ol) {
          *ol = NULL;
        }
        return payload_of(h);
      }
    }
    return NULL;
  }
  if (in_static_image(p)) {
    return NULL;
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
    memset(b->line_mark, 0, LINES_PER_BLOCK);
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
