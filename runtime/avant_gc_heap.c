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
uint64_t *g_marks;
uint8_t *g_lines;
uint32_t *g_pages;
GcChunk *g_chunks;
static Block *g_free_blocks;
/* Chunks below the high-water mark that are CHUNK_FREE again. */
static uint64_t g_free_chunks;

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
 * returned), and the five side tables and the chunk map at their
 * ratios (records 1/512, start bits 1/64, mark bits 1/64, line marks
 * 1/256, the large-object page table 1/1024, the map one GcChunk per
 * chunk). All or nothing. */
static int reserve_all(uint64_t bytes) {
  uint64_t nblocks = bytes >> BLOCK_SHIFT;
  uint64_t nchunks = bytes >> CHUNK_SHIFT;
  uint64_t rec_len = nblocks * sizeof(Block);
  uint64_t st_len = nblocks * START_WORDS * sizeof(uint64_t);
  uint64_t ln_len = nblocks * LINES_PER_BLOCK;
  uint64_t pg_len = (bytes >> PAGE_SHIFT) * sizeof(uint32_t);
  uint64_t ch_len = (nchunks * sizeof(GcChunk) + 4095) & ~(uint64_t)4095;
  uint8_t *raw = reserve(bytes + CHUNK_SIZE);
  void *rec = raw ? reserve(rec_len) : NULL;
  void *st = rec ? reserve(st_len) : NULL;
  void *mk = st ? reserve(st_len) : NULL;
  void *ln = mk ? reserve(ln_len) : NULL;
  void *pg = ln ? reserve(pg_len) : NULL;
  void *ch = pg ? reserve(ch_len) : NULL;
  if (!ch) {
    unreserve(raw, bytes + CHUNK_SIZE);
    unreserve(rec, rec_len);
    unreserve(st, st_len);
    unreserve(mk, st_len);
    unreserve(ln, ln_len);
    unreserve(pg, pg_len);
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
  g_marks = mk;
  g_lines = ln;
  g_pages = pg;
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

/* Commit the next chunk past the high-water mark: its data, its slices
 * of the block records, start bits, mark bits, line marks and page
 * table, and the chunk map's pages holding its entry. Fresh anonymous
 * memory reads as zero, so every side slice is clear. Its block records
 * learn their addresses (fixed for the process's life); the chunk is
 * CHUNK_FREE, clean, for the caller to give a kind. */
static uint64_t commit_new_chunk(void) {
  uint64_t c = g_heap_committed >> CHUNK_SHIFT;
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  uint64_t i;
  if (g_heap_committed + CHUNK_SIZE > g_heap_reserved) {
    gc_die("heap reservation exhausted (AVANT_HEAP_RESERVE_GB raises it)");
  }
  {
    uintptr_t e0 = (uintptr_t)&g_chunks[c] & ~(uintptr_t)4095;
    uintptr_t e1 = ((uintptr_t)&g_chunks[c + 1] + 4095) & ~(uintptr_t)4095;
    if (!commit((void *)e0, (uint64_t)(e1 - e0)) ||
        !commit(g_heap_base + g_heap_committed, CHUNK_SIZE) ||
        !commit(&g_blocks[b0], BLOCKS_PER_CHUNK * sizeof(Block)) ||
        !commit(&g_starts[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t)) ||
        !commit(&g_marks[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t)) ||
        !commit(&g_lines[b0 * LINES_PER_BLOCK], BLOCKS_PER_CHUNK * LINES_PER_BLOCK) ||
        !commit(&g_pages[c * PAGES_PER_CHUNK], PAGES_PER_CHUNK * sizeof(uint32_t))) {
      gc_die("out of memory (a heap chunk could not be committed)");
    }
  }
  for (i = 0; i < BLOCKS_PER_CHUNK; i++) {
    uint64_t k = b0 + i;
    Block *b = &g_blocks[k];
    b->base = g_heap_base + (k << BLOCK_SHIFT);
    b->line_mark = &g_lines[k * LINES_PER_BLOCK];
    b->start = &g_starts[k * START_WORDS];
    b->in_use = 0;
  }
  memset(&g_chunks[c], 0, sizeof(GcChunk));
  g_chunks[c].kind = CHUNK_FREE;
  g_heap_committed += CHUNK_SIZE;
  return c;
}

/* N contiguous chunks for the caller to give a kind (CHUNK_SMALL, a
 * large-object chunk, or a huge object's run): the first run of N free
 * chunks below the high-water mark, else N new chunks past it. Their
 * GcChunk entries keep only `dirty`. Never fails: past the reservation
 * it dies. */
uint64_t gc_chunk_take(uint64_t n) {
  uint64_t hw;
  uint64_t c;
  uint64_t run = 0;
  if (!g_heap_base) {
    gc_heap_reserve();
  }
  hw = g_heap_committed >> CHUNK_SHIFT;
  if (g_free_chunks >= n) {
    for (c = 0; c < hw; c++) {
      if (g_chunks[c].kind != CHUNK_FREE) {
        run = 0;
        continue;
      }
      if (++run == n) {
        uint64_t first = c + 1 - n;
        uint64_t k;
        g_free_chunks -= n;
        /* A reused chunk's mark bits may be stale: a full collection
         * clears only the chunks in use when it starts. */
        for (k = first; k <= c; k++) {
          memset(&g_marks[k * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
        }
        return first;
      }
    }
  }
  c = commit_new_chunk();
  for (run = 1; run < n; run++) {
    commit_new_chunk();
  }
  return c;
}

/* Chunk C back to CHUNK_FREE (its data stays committed and dirty). */
void gc_chunk_give(uint64_t c) {
  uint8_t was = g_chunks[c].kind;
  memset(&g_chunks[c], 0, sizeof(GcChunk));
  g_chunks[c].kind = CHUNK_FREE;
  g_chunks[c].dirty = was != CHUNK_FREE;
  g_free_chunks++;
}

/* A chunk of small-object blocks: every block on the free list, lowest
 * address on top. */
static void make_small_chunk(void) {
  uint64_t c = gc_chunk_take(1);
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  int i;
  g_chunks[c].kind = CHUNK_SMALL;
  g_chunks[c].used = 0;
  for (i = (int)BLOCKS_PER_CHUNK - 1; i >= 0; i--) {
    Block *b = &g_blocks[b0 + (uint64_t)i];
    b->in_use = 0;
    b->next = g_free_blocks;
    g_free_blocks = b;
  }
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
    make_small_chunk();
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
 * memory stays committed. */
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

/* H1 (gc-audit.md §2): p is a heap payload iff it is 8-aligned and its
 * header is the start of a live object: in the committed range (one
 * compare; static objects such as string literals fail it at once, O29),
 * its granule's start bit set, and its chunk a small-object chunk whose
 * block is in use, or a large-object chunk (a large object's header has
 * its start bit too, gc-design.md §2.3; it is set exactly while the
 * object lives). OL receives a large object's header, OB a block
 * object's block. */
int is_heap_payload(void *p, Block **ob, AvantHeader **ol) {
  uintptr_t off;
  uint64_t g;
  uint8_t kind;
  if (!p || (uintptr_t)p % ALIGN) {
    return 0;
  }
  if (!heap_offset((uint8_t *)p - sizeof(AvantHeader), &off)) {
    return 0;
  }
  g = off >> 3;
  if (!((g_starts[g >> 6] >> (g & 63)) & 1ULL)) {
    return 0;
  }
  kind = g_chunks[off >> CHUNK_SHIFT].kind;
  if (kind == CHUNK_SMALL) {
    Block *b = &g_blocks[off >> BLOCK_SHIFT];
    if (!b->in_use) {
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
  if (kind == CHUNK_LARGE || kind == CHUNK_HUGE) {
    if (ob) {
      *ob = NULL;
    }
    if (ol) {
      *ol = avant_header(p);
    }
    return 1;
  }
  return 0;
}

/* The large object whose page run holds byte offset OFF of the heap, or
 * NULL: in a large-object chunk the page table names the run's first
 * page (one read, gc-design.md §2.4); in a huge run the object starts
 * its head chunk. The header is 8 bytes into the run's first page. */
static AvantHeader *large_holding(uintptr_t off) {
  uint64_t c = off >> CHUNK_SHIFT;
  const GcChunk *ch = &g_chunks[c];
  uint8_t *hs;
  if (ch->kind == CHUNK_LARGE) {
    uint32_t v = g_pages[off >> PAGE_SHIFT];
    if (!v) {
      return NULL;
    }
    hs = chunk_base(c) + ((size_t)(v - 1) << PAGE_SHIFT) + 8;
  } else if (ch->kind == CHUNK_HUGE) {
    hs = chunk_base(c) + 8;
  } else if (ch->kind == CHUNK_HUGE_CONT) {
    hs = chunk_base(ch->head) + 8;
  } else {
    return NULL;
  }
  if (!start_bit_at(hs)) {
    return NULL;
  }
  return (AvantHeader *)hs;
}

/*
 * Object start, or the payload of the object that contains p.
 * An interior slot (a field's or element's address) can be rooted;
 * the young copy must still move the object and keep the same offset
 * into it. In a block: the nearest start bit at or before p, a 64-bit
 * word of the start bitmap at a time, bounded by the block (an object
 * never spans two blocks, H4), and p must lie inside that object. In a
 * large-object chunk or a huge run: large_holding.
 */
void *gc_object_payload(void *p, Block **ob, AvantHeader **ol) {
  uintptr_t off;
  if (is_heap_payload(p, ob, ol)) {
    return p;
  }
  if (!p || !heap_offset(p, &off)) {
    return NULL;
  }
  if (g_chunks[off >> CHUNK_SHIFT].kind == CHUNK_SMALL) {
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
  {
    AvantHeader *h = large_holding(off);
    uint8_t *addr = (uint8_t *)p;
    if (h && addr >= (uint8_t *)h && addr < (uint8_t *)h + obj_total(h)) {
      if (ob) {
        *ob = NULL;
      }
      if (ol) {
        *ol = h;
      }
      return payload_of(h);
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

/* The start of a full collection: every committed chunk's slice of the
 * mark bitmap cleared with one memset (64 KiB per chunk), instead of a
 * walk over every object's header (gc-design.md §2.2). */
void gc_clear_side_marks(void) {
  uint64_t c;
  uint64_t n = g_heap_committed >> CHUNK_SHIFT;
  for (c = 0; c < n; c++) {
    if (g_chunks[c].kind != CHUNK_FREE) {
      memset(&g_marks[c * BLOCKS_PER_CHUNK * START_WORDS], 0, BLOCKS_PER_CHUNK * START_WORDS * sizeof(uint64_t));
    }
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
