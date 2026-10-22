#include "avant_rt.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * g_avant_shared stays 0 until a second thread registers. The mutator
 * collects only inside avant_alloc, so a push, pop, or store that does
 * not allocate can skip the world lock. The locked path stays once
 * another thread exists.
 */
extern atomic_int g_avant_shared;

static int mutator_private(void) {
  return atomic_load_explicit(&g_avant_shared, memory_order_acquire) == 0;
}

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

static void array_grow(AvantArray *a, int32_t min_cap, uint64_t elem_size, uint32_t buf_type_id) {
  int32_t ncap = a->cap ? a->cap : 4;
  while (ncap < min_cap) {
    if (ncap > 1073741823) {
      ncap = min_cap;
      break;
    }
    ncap *= 2;
  }
  uint64_t bytes = (uint64_t)ncap * elem_size;
  void *nbuf = avant_alloc(bytes, buf_type_id);
  if (a->buf && a->size > 0) {
    memcpy(nbuf, a->buf, (size_t)a->size * (size_t)elem_size);
  }
  avant_barrier(a);
  a->buf = nbuf;
  a->cap = ncap;
}

void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  void *slot = NULL;
  if (!a) {
    return NULL;
  }
  if (a->buf && a->size < a->cap) {
    slot = (uint8_t *)a->buf + (size_t)a->size * (size_t)elem_size;
    a->size += 1;
    avant_barrier(a);
    if (a->buf) {
      avant_barrier(a->buf);
    }
    return slot;
  }
  avant_gc_defer_enter();
  if (a->size >= a->cap) {
    array_grow(a, a->size + 1, elem_size, buf_type_id);
  }
  slot = (uint8_t *)a->buf + (size_t)a->size * (size_t)elem_size;
  a->size += 1;
  avant_gc_defer_leave();
  avant_barrier(a);
  if (a->buf) {
    avant_barrier(a->buf);
  }
  return slot;
}

void avant_array_push_ptr(void *arr, void *elem, uint32_t buf_type_id) {
  void *slot;
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  if (mutator_private() && a->buf && a->size < a->cap) {
    slot = (uint8_t *)a->buf + (size_t)a->size * sizeof(void *);
    memcpy(slot, &elem, sizeof(void *));
    a->size += 1;
    avant_barrier(a);
    if (a->buf) {
      avant_barrier(a->buf);
    }
    return;
  }
  /*
   * Store under defer so myc IR never holds an interior buffer
   * pointer across a collecting CALL. Grow may nest defer.
   * push_slot already barriers the array and the buffer.
   */
  avant_gc_defer_enter();
  slot = avant_array_push_slot(arr, sizeof(void *), buf_type_id);
  if (slot) {
    memcpy(slot, &elem, sizeof(void *));
  }
  avant_gc_defer_leave();
}

void avant_array_set_ptr(void *arr, int32_t i, void *elem) {
  AvantArray *a = (AvantArray *)arr;
  /*
   * Another worker can be inside avant_alloc, which relocates nursery
   * objects. This call used to be a gc-leaf, so the buffer pointer in
   * a register was not a root. Hold the world lock across the store
   * so a collection cannot move buf between the load and the write.
   * A private mutator does not collect on this path.
   */
  if (mutator_private()) {
    if (a && a->buf) {
      ((void **)a->buf)[i] = elem;
      avant_barrier(a);
      avant_barrier(a->buf);
    }
    return;
  }
  avant_world_lock();
  if (a && a->buf) {
    ((void **)a->buf)[i] = elem;
    avant_barrier(a);
    avant_barrier(a->buf);
  }
  avant_world_unlock();
}

void *avant_array_get_ptr(void *arr, int32_t i) {
  AvantArray *a = (AvantArray *)arr;
  void *elem = NULL;
  if (mutator_private()) {
    if (a && a->buf && i >= 0 && i < a->size) {
      return ((void **)a->buf)[i];
    }
    return NULL;
  }
  avant_world_lock();
  if (a && a->buf && i >= 0 && i < a->size) {
    elem = ((void **)a->buf)[i];
  }
  avant_world_unlock();
  return elem;
}

void *avant_array_pop_ptr(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  void *elem = NULL;
  if (mutator_private()) {
    if (a && a->buf && a->size > 0) {
      a->size -= 1;
      return ((void **)a->buf)[a->size];
    }
    return NULL;
  }
  avant_world_lock();
  if (a && a->buf && a->size > 0) {
    a->size -= 1;
    elem = ((void **)a->buf)[a->size];
  }
  avant_world_unlock();
  return elem;
}

void avant_array_push_i32(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  if (a->buf && a->size < a->cap) {
    ((int32_t *)a->buf)[a->size] = v;
    a->size += 1;
    return;
  }
  avant_gc_defer_enter();
  if (a->size >= a->cap) {
    array_grow(a, a->size + 1, sizeof(int32_t), AVANT_TYPE_BYTES);
  }
  ((int32_t *)a->buf)[a->size] = v;
  a->size += 1;
  avant_gc_defer_leave();
}

void avant_array_clear(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    a->size = 0;
  }
}

void avant_array_reserve(void *arr, int32_t n, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || n <= a->cap) {
    return;
  }
  avant_gc_defer_enter();
  array_grow(a, n, elem_size, buf_type_id);
  avant_gc_defer_leave();
}

void *avant_array_pop_slot(void *arr, uint64_t elem_size) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size <= 0) {
    return NULL;
  }
  a->size -= 1;
  return (uint8_t *)a->buf + (size_t)a->size * (size_t)elem_size;
}

static int cmp_i32(const void *a, const void *b) {
  int32_t x = *(const int32_t *)a;
  int32_t y = *(const int32_t *)b;
  if (x < y) {
    return -1;
  }
  if (x > y) {
    return 1;
  }
  return 0;
}

/* Signed order as an unsigned key. XOR with the sign bit, which is what
 * Julia's uint_map does for a signed integer. */
static uint32_t i32_order(int32_t x) {
  return (uint32_t)x ^ 0x80000000u;
}

static int32_t i32_unorder(uint32_t u) {
  return (int32_t)(u ^ 0x80000000u);
}

static void sort_i32_insertion(int32_t *a, int32_t n) {
  int32_t i;
  for (i = 1; i < n; i++) {
    int32_t v = a[i];
    int32_t j = i;
    while (j > 0 && a[j - 1] > v) {
      a[j] = a[j - 1];
      j--;
    }
    a[j] = v;
  }
}

static int sort_i32_nondecreasing(const int32_t *a, int32_t n) {
  int32_t i;
  for (i = 1; i < n; i++) {
    if (a[i] < a[i - 1]) {
      return 0;
    }
  }
  return 1;
}

static int sort_i32_nonincreasing(const int32_t *a, int32_t n) {
  int32_t i;
  for (i = 1; i < n; i++) {
    if (a[i] > a[i - 1]) {
      return 0;
    }
  }
  return 1;
}

static void sort_i32_reverse(int32_t *a, int32_t n) {
  int32_t i = 0;
  int32_t j = n - 1;
  while (i < j) {
    int32_t t = a[i];
    a[i] = a[j];
    a[j] = t;
    i++;
    j--;
  }
}

/* Julia's radix chunk: about ln(n), capped at 10 bits, and an even cover
 * of the bits that are actually present. */
static int sort_i32_chunk(int32_t n, int bits) {
  int log2n;
  double guess;
  int iters;
  int chunk;
  if (n < 2 || bits < 1) {
    return 1;
  }
  log2n = 31 - __builtin_clz((uint32_t)n);
  guess = ((double)log2n + 1.0) * 0.693147 * 0.75 + 3.0;
  if (guess > 10.0) {
    guess = 10.0;
  }
  if (guess < 1.0) {
    guess = 1.0;
  }
  iters = (int)((double)bits / guess);
  if ((double)iters * guess < (double)bits) {
    iters++;
  }
  if (iters < 1) {
    iters = 1;
  }
  chunk = (bits + iters - 1) / iters;
  if (chunk < 1) {
    chunk = 1;
  }
  if (chunk > 10) {
    chunk = 10;
  }
  return chunk;
}

static void sort_i32_pass(const uint32_t *from, uint32_t *to, int32_t n, int shift, uint32_t mask,
                          uint32_t *counts, uint32_t nb) {
  int32_t i;
  uint32_t b;
  uint32_t sum = 0;
  memset(counts, 0, (size_t)nb * sizeof(uint32_t));
  for (i = 0; i < n; i++) {
    counts[(from[i] >> shift) & mask]++;
  }
  for (b = 0; b < nb; b++) {
    uint32_t c = counts[b];
    counts[b] = sum;
    sum += c;
  }
  for (i = 0; i < n; i++) {
    uint32_t key = from[i];
    to[counts[(key >> shift) & mask]++] = key;
  }
}

/* Range under half the length. Julia's ConsiderCountingSort uses that cut. */
static int sort_i32_counting(int32_t *a, int32_t n, uint32_t umin, uint32_t span) {
  uint32_t *counts;
  uint32_t v;
  int32_t i;
  int64_t idx = 0;
  counts = calloc((size_t)span + 1u, sizeof(uint32_t));
  if (!counts) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    counts[i32_order(a[i]) - umin]++;
  }
  for (v = 0; v <= span; v++) {
    uint32_t c = counts[v];
    int32_t val;
    uint32_t k;
    if (c == 0) {
      continue;
    }
    val = i32_unorder(umin + v);
    for (k = 0; k < c; k++) {
      a[idx + (int64_t)k] = val;
    }
    idx += (int64_t)c;
  }
  free(counts);
  return 1;
}

static int sort_i32_radix(int32_t *a, int32_t n, uint32_t umin, uint32_t span) {
  uint32_t *scratch;
  uint32_t *from;
  uint32_t *to;
  uint32_t counts[1024];
  int bits;
  int chunk;
  int shift;
  int32_t i;
  uint32_t mask;
  uint32_t nb;

  bits = 32 - __builtin_clz(span);
  chunk = sort_i32_chunk(n, bits);
  scratch = malloc((size_t)n * sizeof(uint32_t));
  if (!scratch) {
    return 0;
  }
  from = (uint32_t *)a;
  for (i = 0; i < n; i++) {
    int32_t x = a[i];
    from[i] = i32_order(x) - umin;
  }
  to = scratch;
  mask = (1u << chunk) - 1u;
  nb = mask + 1u;
  shift = 0;
  while (shift < bits) {
    sort_i32_pass(from, to, n, shift, mask, counts, nb);
    {
      uint32_t *swap = from;
      from = to;
      to = swap;
    }
    shift += chunk;
  }
  if (from == (uint32_t *)a) {
    for (i = 0; i < n; i++) {
      uint32_t key = from[i];
      a[i] = i32_unorder(key + umin);
    }
  } else {
    for (i = 0; i < n; i++) {
      a[i] = i32_unorder(from[i] + umin);
    }
  }
  free(scratch);
  return 1;
}

/*
 * Array.sort is Int-only. The profile of Sort::Self was glibc qsort: a
 * mergesort whose comparator is an indirect call. The copy was not the
 * time. This is the library sort, not the Avant quicksort (Sort::Quick).
 * n <= 128 is insertion. A longer array that is already ordered returns,
 * and one that is reverse-ordered is reversed. Otherwise the value span
 * is counted when it is under half the length, and radix-sorted over the
 * bits of that span when it is not.
 */
static void sort_i32(int32_t *a, int32_t n) {
  uint32_t umin;
  uint32_t umax;
  uint32_t span;
  int32_t i;
  if (n <= 128) {
    sort_i32_insertion(a, n);
    return;
  }
  if (sort_i32_nondecreasing(a, n)) {
    return;
  }
  if (n >= 500 && sort_i32_nonincreasing(a, n)) {
    sort_i32_reverse(a, n);
    return;
  }
  umin = i32_order(a[0]);
  umax = umin;
  for (i = 1; i < n; i++) {
    uint32_t u = i32_order(a[i]);
    if (u < umin) {
      umin = u;
    }
    if (u > umax) {
      umax = u;
    }
  }
  if (umin == umax) {
    return;
  }
  span = umax - umin;
  if ((uint64_t)span < (uint64_t)n / 2ull) {
    if (sort_i32_counting(a, n, umin, span)) {
      return;
    }
  }
  if (!sort_i32_radix(a, n, umin, span)) {
    qsort(a, (size_t)n, sizeof(int32_t), cmp_i32);
  }
}

void avant_array_sort_i32(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  sort_i32((int32_t *)a->buf, a->size);
}

void avant_array_fill_u8(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size <= 0) {
    return;
  }
  memset(a->buf, (int)(unsigned char)v, (size_t)a->size);
}

void avant_array_fill_i32(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t n;
  int32_t i;
  int32_t *p;
  if (!a || !a->buf || a->size <= 0) {
    return;
  }
  n = a->size;
  p = (int32_t *)a->buf;
  if (v == 0) {
    memset(p, 0, (size_t)n * sizeof(int32_t));
    return;
  }
  for (i = 0; i < n; i++) {
    p[i] = v;
  }
}
