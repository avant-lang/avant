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

/* The collector's state (avant_gc_internal.h). Zero-initialized, so the
 * type tables stay in .bss; the fields whose start value is not zero are
 * set by gc_heap_static_init before main runs (and before any other
 * constructor of the runtime could allocate or lock). */
GcHeap gc_heap;

__attribute__((constructor(101))) static void gc_heap_static_init(void) {
  pthread_mutex_init(&gc_heap.world, NULL);
  pthread_mutex_init(&gc_heap.threads_mu, NULL);
  pthread_mutex_init(&gc_heap.hs_mu, NULL);
  pthread_cond_init(&gc_heap.hs_stopped_cv, NULL);
  pthread_cond_init(&gc_heap.hs_resume_cv, NULL);
  gc_heap.full_trigger = FULL_TRIGGER_MIN;
  gc_heap.set.defrag = 1;
}

/* Whether payload word W of an object with header H is a reference slot
 * by its type map (the verifier's inverse question). */
int gc_type_word_is_ref(const AvantHeader *h, size_t w) {
  uint32_t id = h->type_id;
  if (id == AVANT_TYPE_PTRS) {
    return 1;
  }
  if (id == AVANT_TYPE_BYTES || id >= MAX_TYPES || !gc_heap.type_set[id]) {
    return 0;
  }
  const TypeMap *m = &gc_heap.types[id];
  if (m->stride) {
    w %= m->stride;
  }
  if (w / 64 >= m->nchunks) {
    return 0;
  }
  return (m->words[w / 64] >> (w % 64)) & 1u;
}

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

void gc_work_push(void *p) {
  if (gc_heap.work_n == gc_heap.work_cap) {
    gc_heap.work_cap = gc_heap.work_cap ? gc_heap.work_cap * 2 : 256;
    gc_heap.work = gc_xrealloc(gc_heap.work, gc_heap.work_cap * sizeof(void *));
  }
  gc_heap.work[gc_heap.work_n++] = p;
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
 * returned), and the six side tables and the chunk map at their
 * ratios (records 1/512, start bits 1/64, mark bits 1/64, line marks
 * 1/256, the large-object page table 1/1024, the card table 1/CARD_SIZE
 * (Stage 28 W16t), the map one GcChunk per chunk). All or nothing. */
static int reserve_all(uint64_t bytes) {
  uint64_t nblocks = bytes >> BLOCK_SHIFT;
  uint64_t nchunks = bytes >> CHUNK_SHIFT;
  uint64_t rec_len = nblocks * sizeof(Block);
  uint64_t st_len = nblocks * START_WORDS * sizeof(uint64_t);
  uint64_t ln_len = nblocks * LINES_PER_BLOCK;
  uint64_t pg_len = (bytes >> PAGE_SHIFT) * sizeof(uint32_t);
  uint64_t cd_len = bytes >> CARD_SHIFT;
  uint64_t ch_len = (nchunks * sizeof(GcChunk) + 4095) & ~(uint64_t)4095;
  uint8_t *raw = reserve(bytes + CHUNK_SIZE);
  void *rec = raw ? reserve(rec_len) : NULL;
  void *st = rec ? reserve(st_len) : NULL;
  void *mk = st ? reserve(st_len) : NULL;
  void *ln = mk ? reserve(ln_len) : NULL;
  void *pg = ln ? reserve(pg_len) : NULL;
  void *cd = pg ? reserve(cd_len) : NULL;
  void *ch = cd ? reserve(ch_len) : NULL;
  if (!ch) {
    unreserve(raw, bytes + CHUNK_SIZE);
    unreserve(rec, rec_len);
    unreserve(st, st_len);
    unreserve(mk, st_len);
    unreserve(ln, ln_len);
    unreserve(pg, pg_len);
    unreserve(cd, cd_len);
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
    gc_heap.base = base;
  }
  /* Forwarding in the header holds an address in 48 bits
   * (gc_set_forward): the whole range must lie below 2^48. */
  if ((uint64_t)(uintptr_t)gc_heap.base + bytes > (1ull << 48)) {
    gc_die("the heap's address range is not below 2^48");
  }
  gc_heap.blocks = rec;
  gc_heap.starts = st;
  gc_heap.marks = mk;
  gc_heap.lines = ln;
  gc_heap.pages = pg;
  gc_heap.cards = cd;
  gc_heap.chunks = ch;
  gc_heap.reserved = bytes;
  gc_heap.committed = 0;
  return 1;
}

/* The reservation is AVANT_HEAP_RESERVE_GB GiB (gc_heap.set.reserve_gb,
 * default 64); when the system refuses it (ulimit -v, a container's
 * address-space limit), the size is halved until it succeeds, down to 1
 * GiB. It is the heap's ceiling for the process's life (gc_block_take).
 * Called once, by gc_init_slow. */
void gc_heap_reserve(void) {
  uint64_t gb;
  if (gc_heap.base) {
    return;
  }
  gc_heap.line_epoch = 1;
  /* 0 only if a block is taken before gc_init_slow read the settings. */
  gb = gc_heap.set.reserve_gb ? gc_heap.set.reserve_gb : HEAP_RESERVE_GB_DEFAULT;
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
  uint64_t c = gc_heap.committed >> CHUNK_SHIFT;
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  uint64_t i;
  if (gc_heap.committed + CHUNK_SIZE > gc_heap.reserved) {
    gc_die("heap reservation exhausted (AVANT_HEAP_RESERVE_GB raises it)");
  }
  {
    uintptr_t e0 = (uintptr_t)&gc_heap.chunks[c] & ~(uintptr_t)4095;
    uintptr_t e1 = ((uintptr_t)&gc_heap.chunks[c + 1] + 4095) & ~(uintptr_t)4095;
    if (!commit((void *)e0, (uint64_t)(e1 - e0)) ||
        !commit(gc_heap.base + gc_heap.committed, CHUNK_SIZE) ||
        !commit(&gc_heap.blocks[b0], BLOCKS_PER_CHUNK * sizeof(Block)) ||
        !commit(&gc_heap.starts[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t)) ||
        !commit(&gc_heap.marks[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t)) ||
        !commit(&gc_heap.lines[b0 * LINES_PER_BLOCK], BLOCKS_PER_CHUNK * LINES_PER_BLOCK) ||
        !commit(&gc_heap.pages[c * PAGES_PER_CHUNK], PAGES_PER_CHUNK * sizeof(uint32_t)) ||
        !commit(&gc_heap.cards[(uint64_t)c << (CHUNK_SHIFT - CARD_SHIFT)], CHUNK_SIZE >> CARD_SHIFT)) {
      gc_die("out of memory (a heap chunk could not be committed)");
    }
  }
  for (i = 0; i < BLOCKS_PER_CHUNK; i++) {
    uint64_t k = b0 + i;
    Block *b = &gc_heap.blocks[k];
    b->base = gc_heap.base + (k << BLOCK_SHIFT);
    b->line_mark = &gc_heap.lines[k * LINES_PER_BLOCK];
    b->start = &gc_heap.starts[k * START_WORDS];
    b->in_use = 0;
  }
  memset(&gc_heap.chunks[c], 0, sizeof(GcChunk));
  gc_heap.chunks[c].kind = CHUNK_FREE;
  gc_heap.committed += CHUNK_SIZE;
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
  if (!gc_heap.base) {
    gc_heap_reserve();
  }
  hw = gc_heap.committed >> CHUNK_SHIFT;
  if (gc_heap.free_chunks >= n) {
    for (c = 0; c < hw; c++) {
      if (gc_heap.chunks[c].kind != CHUNK_FREE) {
        run = 0;
        continue;
      }
      if (++run == n) {
        uint64_t first = c + 1 - n;
        uint64_t k;
        gc_heap.free_chunks -= n;
        for (k = first; k <= c; k++) {
          uint64_t b;
          /* A decommitted chunk's data is read-write again (zero); its
           * side slices were madvised (zero) and its records keep their
           * addresses. A reused chunk's mark bits may be stale: a full
           * collection clears only the chunks in use when it starts. */
          if (gc_heap.chunks[k].decommitted) {
            if (!commit(chunk_base(k), CHUNK_SIZE)) {
              gc_die("out of memory (a heap chunk could not be committed)");
            }
            gc_heap.chunks[k].decommitted = 0;
          } else {
            memset(&gc_heap.marks[k * CHUNK_START_WORDS], 0, CHUNK_START_WORDS * sizeof(uint64_t));
          }
          for (b = k * BLOCKS_PER_CHUNK; b < (k + 1) * BLOCKS_PER_CHUNK; b++) {
            gc_heap.blocks[b].in_use = 0;
            gc_heap.blocks[b].clean = 0;
          }
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

/* Chunk C back to CHUNK_FREE (its data stays committed; dirty unless
 * the caller returned its pages). */
void gc_chunk_give(uint64_t c) {
  uint8_t was = gc_heap.chunks[c].kind;
  memset(&gc_heap.chunks[c], 0, sizeof(GcChunk));
  gc_heap.chunks[c].kind = CHUNK_FREE;
  gc_heap.chunks[c].dirty = was != CHUNK_FREE;
  gc_heap.chunks[c].idle = (uint32_t)gc_heap.return_clock;
  gc_heap.free_chunks++;
}

/* Give the pages of [P, P + LEN) back to the OS (page-aligned): they stay
 * mapped read-write and read as zero when touched again. */
void gc_return_pages(void *p, size_t len) {
  if (len) {
    madvise(p, len, MADV_DONTNEED);
  }
}

/* A chunk of small-object blocks: every block on its own free list,
 * lowest address on top. */
static uint64_t make_small_chunk(void) {
  uint64_t c = gc_chunk_take(1);
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  GcChunk *ch = &gc_heap.chunks[c];
  int dirty = ch->dirty;
  int i;
  ch->kind = CHUNK_SMALL;
  ch->dirty = 0;
  ch->used = 0;
  ch->free = NULL;
  ch->nfree = BLOCKS_PER_CHUNK;
  for (i = (int)BLOCKS_PER_CHUNK - 1; i >= 0; i--) {
    Block *b = &gc_heap.blocks[b0 + (uint64_t)i];
    b->in_use = 0;
    b->clean = dirty ? 0 : 1;
    b->next = ch->free;
    ch->free = b;
  }
  if (dirty) {
    gc_heap.free_resident += CHUNK_SIZE;
  }
  if (c < gc_heap.small_hint) {
    gc_heap.small_hint = c;
  }
  return c;
}

/* A free block, its start bits and line marks clear, not yet in use
 * (gc_new_block makes it so): from the lowest small chunk that has one.
 * Never fails: past the reservation it dies. Memory is not charged
 * here; gc_heap.heap_bytes is gc_new_block's. */
Block *gc_block_take(void) {
  Block *b;
  GcChunk *ch = NULL;
  uint64_t hw;
  uint64_t c;
  if (!gc_heap.base) {
    gc_heap_reserve();
  }
  hw = gc_heap.committed >> CHUNK_SHIFT;
  for (c = gc_heap.small_hint; c < hw; c++) {
    if (gc_heap.chunks[c].kind == CHUNK_SMALL && gc_heap.chunks[c].nfree) {
      ch = &gc_heap.chunks[c];
      break;
    }
  }
  if (!ch) {
    c = make_small_chunk();
    ch = &gc_heap.chunks[c];
  }
  gc_heap.small_hint = c;
  b = ch->free;
  ch->free = b->next;
  ch->nfree--;
  ch->used++;
  b->next = NULL;
  if (!b->clean) {
    gc_heap.free_resident -= BLOCK_SIZE;
  }
  b->clean = 0;
  memset(b->line_mark, 0, LINES_PER_BLOCK);
  memset(b->start, 0, START_WORDS * sizeof(uint64_t));
  return b;
}

/* Back to its chunk's free list (the block must already be out of use).
 * Its memory stays resident until gc_heap_return. */
void gc_block_give(Block *b) {
  uint64_t c = (uint64_t)(b->base - gc_heap.base) >> CHUNK_SHIFT;
  GcChunk *ch = &gc_heap.chunks[c];
  b->in_use = 0;
  b->clean = 0;
  b->idle = gc_heap.return_clock;
  ch->used--;
  if (ch->used == 0) {
    ch->idle = (uint32_t)gc_heap.return_clock;
  }
  b->next = ch->free;
  ch->free = b;
  ch->nfree++;
  gc_heap.free_resident += BLOCK_SIZE;
  if (c < gc_heap.small_hint) {
    gc_heap.small_hint = c;
  }
}

/* Decommit chunk C, wholly free: its data returned and made PROT_NONE
 * (no commit charge), its side slices returned (they read as zero), and
 * it is CHUNK_FREE. Its block records stay (they hold only addresses). */
static void decommit_chunk(uint64_t c) {
  uint64_t b0 = c * BLOCKS_PER_CHUNK;
  uint8_t kind = gc_heap.chunks[c].kind;
  if (kind == CHUNK_LARGE) {
    gc_large_chunk_gone(c);
  }
  if (kind != CHUNK_FREE) {
    gc_chunk_give(c);
  }
  gc_return_pages(chunk_base(c), CHUNK_SIZE);
  mprotect(chunk_base(c), CHUNK_SIZE, PROT_NONE);
  gc_return_pages(&gc_heap.starts[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t));
  gc_return_pages(&gc_heap.marks[b0 * START_WORDS], CHUNK_START_WORDS * sizeof(uint64_t));
  gc_return_pages(&gc_heap.lines[b0 * LINES_PER_BLOCK], BLOCKS_PER_CHUNK * LINES_PER_BLOCK);
  gc_return_pages(&gc_heap.pages[c * PAGES_PER_CHUNK], PAGES_PER_CHUNK * sizeof(uint32_t));
  gc_return_pages(&gc_heap.cards[(uint64_t)c << (CHUNK_SHIFT - CARD_SHIFT)], CHUNK_SIZE >> CARD_SHIFT);
  gc_heap.chunks[c].dirty = 0;
  gc_heap.chunks[c].decommitted = 1;
}

/* The resident free bytes of chunk C: free blocks not madvised, the
 * dirty free pages of a large-object chunk, or a free chunk's whole data
 * while it is dirty. */
static uint64_t chunk_resident_free(uint64_t c) {
  GcChunk *ch = &gc_heap.chunks[c];
  uint64_t n = 0;
  if (ch->kind == CHUNK_SMALL) {
    Block *b;
    for (b = ch->free; b; b = b->next) {
      if (!b->clean) {
        n += BLOCK_SIZE;
      }
    }
  } else if (ch->kind == CHUNK_LARGE) {
    uint32_t w;
    for (w = 0; w < PAGE_WORDS; w++) {
      n += (uint64_t)__builtin_popcountll(ch->dirty_pages[w] & ~ch->alloc[w]) * PAGE_SIZE;
    }
  } else if (ch->kind == CHUNK_FREE && !ch->decommitted && ch->dirty) {
    n = CHUNK_SIZE;
  }
  return n;
}

/* Returning memory after a full collection (gc-design.md §2.5, §5.5,
 * O53, Q12). Two rules keep a cycle from returning what the next one
 * takes back. Only idle memory goes back: memory freed before this full
 * collection began and not reused since (a block's, a chunk's idle
 * stamp below the current count of full collections), so a buffer freed
 * and reallocated every cycle (a parse pool, a sort's scratch) stays
 * resident. And free memory up to the retention target, the larger of
 * 4 MiB and a quarter of the live bytes, stays resident. The rest goes
 * back, highest chunks first: a wholly free or empty chunk is
 * decommitted (madvised, made PROT_NONE, its side slices madvised; one
 * whose memory is already returned always is: keeping it costs only
 * commit charge), then free blocks are madvised one by one, then the
 * dirty free pages of large-object chunks. */
void gc_heap_return(uint64_t live) {
  uint64_t keep = live / 4 > (4ull << 20) ? live / 4 : (4ull << 20);
  uint64_t hw = gc_heap.committed >> CHUNK_SHIFT;
  uint64_t now = gc_heap.return_clock;
  uint64_t total = 0;
  uint64_t c;
  if (!hw) {
    return;
  }
  for (c = 0; c < hw; c++) {
    total += chunk_resident_free(c);
  }
  for (c = hw; c-- > 0;) {
    GcChunk *ch = &gc_heap.chunks[c];
    int empty = ((ch->kind == CHUNK_SMALL || ch->kind == CHUNK_LARGE) && ch->used == 0) ||
                (ch->kind == CHUNK_FREE && !ch->decommitted);
    uint64_t r;
    if (!empty || ch->idle >= now) {
      continue;
    }
    r = chunk_resident_free(c);
    if (r == 0 || total > keep) {
      if (ch->kind == CHUNK_SMALL) {
        gc_heap.free_resident -= r;
      }
      total -= r;
      decommit_chunk(c);
    }
  }
  for (c = hw; c-- > 0 && total > keep;) {
    GcChunk *ch = &gc_heap.chunks[c];
    if (ch->kind == CHUNK_SMALL) {
      Block *b;
      for (b = ch->free; b && total > keep; b = b->next) {
        if (!b->clean && b->idle < now) {
          gc_return_pages(b->base, BLOCK_SIZE);
          b->clean = 1;
          gc_heap.free_resident -= BLOCK_SIZE;
          total -= BLOCK_SIZE;
        }
      }
    } else if (ch->kind == CHUNK_LARGE && ch->idle < now) {
      uint32_t p = 0;
      while (p < PAGES_PER_CHUNK && total > keep) {
        uint32_t q = p;
        /* One madvise per run of dirty free pages. */
        while (q < PAGES_PER_CHUNK && total > keep &&
               ((ch->dirty_pages[q >> 6] & ~ch->alloc[q >> 6]) >> (q & 63) & 1ULL)) {
          ch->dirty_pages[q >> 6] &= ~(1ULL << (q & 63));
          total -= PAGE_SIZE;
          q++;
        }
        if (q > p) {
          gc_return_pages(chunk_base(c) + ((size_t)p << PAGE_SHIFT), (size_t)(q - p) << PAGE_SHIFT);
          p = q;
        } else {
          p++;
        }
      }
    }
  }
}

/* The live lines of B, eight line marks at a time against the epoch
 * (Stage 28 W15d, O94: the young sweep's word-wise reading, W15c's O87;
 * before, a byte loop over the 128 marks). */
int gc_block_live_lines(Block *b) {
  uint64_t epoch8 = line_epoch8();
  int n = 0;
  int k;
  for (k = 0; k < LINE_WORDS; k++) {
    uint64_t m;
    memcpy(&m, b->line_mark + 8 * k, sizeof m);
    n += live_byte_count(live_bytes_mask(m, epoch8));
  }
  return n;
}

/* The line sweep of block B over the lines LO..HI (Stage 28 W15c, O87;
 * moved here from avant_gc_sticky.c in W15d, O94, so the full sweep,
 * gc_keep_if_live, shares it with the young sweep): a line no survivor
 * marked at the current epoch is free, and its start bits are cleared.
 * Word-wise: the block's 128 line marks are read 8
 * at a time and compared with the epoch replicated into every byte; one
 * pass frees the young range's dead lines (their start bits cleared, two
 * lines per start word), counts the block's live lines and finds its
 * first hole, which the byte-wise sweep, gc_block_live_lines and
 * gc_reset_block_bump's find_hole did in three passes. Returns the live
 * line count and leaves the block's bump and limit on its first hole (as
 * gc_reset_block_bump does: search 0; a full block's cursor at its end).
 * *FREED counts the freed lines of the range. */
int gc_sweep_block_lines(Block *b, int lo, int hi, uint64_t *freed) {
  uint64_t epoch8 = line_epoch8();
  uint64_t live_bits[LINE_WORDS];
  uint64_t range_lo = (uint64_t)lo;
  uint64_t range_hi = (uint64_t)hi;
  int live = 0;
  int k;
  int first_free = -1;
  int hole_end = LINES_PER_BLOCK;
  for (k = 0; k < LINE_WORDS; k++) {
    uint64_t m;
    memcpy(&m, b->line_mark + 8 * k, sizeof m);
    live_bits[k] = live_bytes_mask(m, epoch8);
    live += live_byte_count(live_bits[k]);
  }
  for (k = 0; k < LINE_WORDS; k++) {
    uint64_t freeb = ~live_bits[k] & 0x8080808080808080ULL;
    int base = 8 * k;
    if (first_free < 0 && freeb) {
      first_free = base + (__builtin_ctzll(freeb) >> 3);
    }
    /* The young range's free lines in this group: clear their start bits
     * (lines 2j and 2j + 1 share start word j: the low and high half). */
    if (freeb == 0x8080808080808080ULL && (uint64_t)base >= range_lo && (uint64_t)base + 8 <= range_hi) {
      /* The whole group is free and young: four start words. */
      b->start[base >> 1] = 0;
      b->start[(base >> 1) + 1] = 0;
      b->start[(base >> 1) + 2] = 0;
      b->start[(base >> 1) + 3] = 0;
      *freed += 8;
    } else if ((uint64_t)base + 8 > range_lo && (uint64_t)base < range_hi && freeb) {
      int j;
      for (j = 0; j < 4; j++) {
        uint64_t pair = (freeb >> (16 * j)) & 0x8080ULL;
        int l0 = base + 2 * j;
        uint64_t clear = 0;
        if ((pair & 0x80ULL) && (uint64_t)l0 >= range_lo && (uint64_t)l0 < range_hi) {
          clear |= 0xffffffffULL;
          (*freed)++;
        }
        if ((pair & 0x8000ULL) && (uint64_t)(l0 + 1) >= range_lo && (uint64_t)(l0 + 1) < range_hi) {
          clear |= 0xffffffff00000000ULL;
          (*freed)++;
        }
        b->start[l0 >> 1] &= ~clear;
      }
    }
  }
  if (first_free >= 0) {
    /* The hole ends at the first live line after it. */
    int l = first_free + 1;
    while (l < LINES_PER_BLOCK) {
      int kk = l >> 3;
      uint64_t lb = live_bits[kk] >> (8 * (l & 7));
      if (lb) {
        hole_end = l + (__builtin_ctzll(lb) >> 3);
        break;
      }
      l = (kk + 1) * 8;
    }
    b->bump = b->base + (size_t)first_free * LINE_SIZE;
    b->limit = b->base + (size_t)hole_end * LINE_SIZE;
  } else {
    b->bump = b->base + BLOCK_SIZE;
    b->limit = b->base + BLOCK_SIZE;
  }
  b->search = first_free >= 0 ? 0 : LINES_PER_BLOCK;
  return live;
}


size_t gc_obj_total(const AvantHeader *h) {
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
int gc_is_heap_payload(void *p, Block **ob, AvantHeader **ol) {
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
  if (!((gc_heap.starts[g >> 6] >> (g & 63)) & 1ULL)) {
    return 0;
  }
  kind = gc_heap.chunks[off >> CHUNK_SHIFT].kind;
  if (kind == CHUNK_SMALL) {
    Block *b = &gc_heap.blocks[off >> BLOCK_SHIFT];
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
  const GcChunk *ch = &gc_heap.chunks[c];
  uint8_t *hs;
  if (ch->kind == CHUNK_LARGE) {
    uint32_t v = gc_heap.pages[off >> PAGE_SHIFT];
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
  if (gc_is_heap_payload(p, ob, ol)) {
    return p;
  }
  if (!p || !heap_offset(p, &off)) {
    return NULL;
  }
  if (gc_heap.chunks[off >> CHUNK_SHIFT].kind == CHUNK_SMALL) {
    Block *b = &gc_heap.blocks[off >> BLOCK_SHIFT];
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
      if (addr >= hs && addr < hs + gc_obj_total_any(h)) {
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
    if (h && addr >= (uint8_t *)h && addr < (uint8_t *)h + gc_obj_total(h)) {
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
  return gc_is_heap_payload(payload, NULL, NULL);
}

/* The contract (ABI name kept, gc-design.md §3.1): 1 when a young
 * collection can move this object, 0 when no collection before the next
 * full one moves it; .parallel keeps a raw address only on 0. Under S+
 * (GC_STICKY_COPY, Stage 28 W15c) a young block object may be copied, so
 * it is 1 for one that is not pinned; mature and large objects stay put,
 * so a parallel numeric loop can keep their address. Under candidate S
 * (GC_STICKY_COPY 0) no young collection moves anything, so it is 0 for
 * every object (Stage 28 W15b). (Under E, deleted in W15d, it was 1 for
 * every young block object.) */
int32_t avant_is_young(void *payload) {
#if !GC_STICKY_COPY
  (void)payload;
  return 0;
#else
  AvantHeader *h;
  if (!payload || ((uintptr_t)payload & (ALIGN - 1)) != 0) {
    return 0;
  }
  h = avant_header(payload);
  /* A large object never moves, even when it is still in the young set. */
  if (h->flags & (AVANT_FLAG_LARGE | AVANT_FLAG_PINNED)) {
    return 0;
  }
  if ((h->flags & (AVANT_FLAG_YOUNG | AVANT_FLAG_HEAP)) ==
      (AVANT_FLAG_YOUNG | AVANT_FLAG_HEAP)) {
    return 1;
  }
  return 0;
#endif
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

/* The start of a full collection's line marking (gc-design.md §2.2,
 * Stage 28 W15b): the epoch advances, so every line mark of every block
 * is stale at once (no walk of the block lists zeroing them, as before).
 * Epochs run 1..255; past 255 every committed small chunk's line marks
 * are zeroed (one memset of 16 KiB per chunk) and the epoch restarts at
 * 1. Nothing survives a full collection by an old mark: the full mark
 * remarks every live object's lines, and a block entering use has its
 * marks zeroed (gc_block_take, gc_new_block's cache path). */
void gc_line_epoch_advance(void) {
  uint64_t c;
  uint64_t n;
  if (gc_heap.line_epoch < 255) {
    gc_heap.line_epoch++;
    return;
  }
  n = gc_heap.committed >> CHUNK_SHIFT;
  for (c = 0; c < n; c++) {
    if (gc_heap.chunks[c].kind == CHUNK_SMALL) {
      memset(&gc_heap.lines[c * BLOCKS_PER_CHUNK * LINES_PER_BLOCK], 0,
             BLOCKS_PER_CHUNK * LINES_PER_BLOCK);
    }
  }
  gc_heap.line_epoch = 1;
}

/* The start of a full collection: every committed chunk's slice of the
 * mark bitmap cleared with one memset (64 KiB per chunk), instead of a
 * walk over every object's header (gc-design.md §2.2). */
void gc_clear_side_marks(void) {
  uint64_t c;
  uint64_t n = gc_heap.committed >> CHUNK_SHIFT;
  for (c = 0; c < n; c++) {
    if (gc_heap.chunks[c].kind != CHUNK_FREE) {
      memset(&gc_heap.marks[c * BLOCKS_PER_CHUNK * START_WORDS], 0, BLOCKS_PER_CHUNK * START_WORDS * sizeof(uint64_t));
    }
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
  TypeMap *m = &gc_heap.types[type_id];
  if (m->words && m->words != &m->first) {
    free(m->words);
  }
  m->first = word_bits;
  m->words = &m->first;
  m->nchunks = 1;
  m->stride = 0;
  m->nrefs = (uint32_t)__builtin_popcountll(word_bits);
  gc_heap.type_set[type_id] = 1;
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
  TypeMap *m = &gc_heap.types[type_id];
  uint32_t chunk = word_base / 64;
  if (!gc_heap.type_set[type_id]) {
    m->first = 0;
    m->words = &m->first;
    m->nchunks = 1;
    m->stride = stride_words;
    m->nrefs = 0;
    gc_heap.type_set[type_id] = 1;
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
