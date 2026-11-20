#include "avant_rt.h"
#include "avant_gc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif


typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

/* The freeze (avant_rt.h, AVANT_ARRAY_FROZEN). The
 * cap word is read with a relaxed atomic load, a plain load on x86, and
 * changed only by compare-and-swap while a task may hold the array, so
 * spawns that capture one array from several threads (a task that
 * spawns) and tasks ending at once keep the count exact. Every read that
 * can meet a frozen word is atomic, the push fast paths' included (as
 * the generated code's frozen check is): a task's end swaps the word from
 * its own thread while the owner may read it before joining. A read
 * after the frozen test passed meets no other writer: only a thread that
 * holds the array unfrozen can freeze it. */
static inline int32_t arr_cap_word(AvantArray *a) {
  return __atomic_load_n(&a->cap, __ATOMIC_RELAXED);
}

/* A change to a frozen array traps, naming the method call of this
 * thread that froze it, else as a task's freeze (arr_frozen_trap). */
static __attribute__((noreturn, cold, noinline)) void arr_frozen_trap(void *arr, int32_t op);

#define ARR_FROZEN_TRAP(a, op)                                   \
  do {                                                           \
    if (__builtin_expect(arr_cap_word(a) < 0, 0)) {              \
      arr_frozen_trap((a), (op));                                \
    }                                                            \
  } while (0)

/* The same for the runtime's own element stores and fills, which are of
 * arrays of numbers: no method call freezes one (only an array of
 * structs holds a method's receiver), so only a task's freeze can meet
 * them, and the trap is the task's (the shorter call keeps the hot
 * loops around it, avant_bwt_inverse's, as they were). */
#define ARR_FROZEN_TRAP_NUM(a, op)                               \
  do {                                                           \
    if (__builtin_expect(arr_cap_word(a) < 0, 0)) {              \
      avant_trap_frozen(op);                                     \
    }                                                            \
  } while (0)

/* One more task reads arr; depth > 1 freezes the arrays it holds too, a
 * level each (an Array(Array(T)) is depth 2), after arr itself, so its
 * elements cannot change while they are counted. */
void avant_array_freeze(void *arr, int32_t depth) {
  AvantArray *a = (AvantArray *)arr;
  int32_t c;
  int32_t n;
  if (!a) {
    return;
  }
  c = arr_cap_word(a);
  do {
    if (c >= 0) {
      n = AVANT_ARRAY_FROZEN | 1;
    } else {
      if ((c & 0x7fffffff) == 0x7fffffff) {
        fflush(stdout);
        fputs("avant runtime: an array is captured by more than 2147483646 running tasks\n", stderr);
        exit(1);
      }
      n = c + 1;
    }
  } while (!__atomic_compare_exchange_n(&a->cap, &c, n, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
  if (depth > 1 && a->buf) {
    int32_t i;
    for (i = 0; i < a->size; i++) {
      avant_array_freeze(((void **)a->buf)[i], depth - 1);
    }
  }
}

/* The capacity a frozen array's last thaw writes back: what its buffer
 * holds, by the buffer's header (a frozen array never grows, so the
 * buffer is the one it had; the header may round the bytes up, never
 * past the memory it owns), at least the size. */
static int32_t arr_thawed_cap(AvantArray *a, uint64_t elem_size) {
  uint64_t c;
  if (!a->buf) {
    return 0;
  }
  if (elem_size == 0) {
    return a->size;
  }
  c = (uint64_t)avant_header(a->buf)->size / elem_size;
  if (c > (uint64_t)AVANT_SIZE_MAX) {
    c = (uint64_t)AVANT_SIZE_MAX;
  }
  if (c < (uint64_t)a->size) {
    c = (uint64_t)a->size;
  }
  return (int32_t)c;
}

/* One task that read arr has ended: the arrays it holds first (depth >
 * 1), while arr is still frozen, then arr; the last thaw writes back the
 * capacity. elem_size is the size of the innermost level's elements. A
 * thaw of an array that is not frozen is the compiler's error. The task's
 * end runs it (lower_task.av, thaw_captures); when a trap may end a task
 * without ending the program, that end must run it too. */
void avant_array_thaw(void *arr, int32_t depth, uint64_t elem_size) {
  AvantArray *a = (AvantArray *)arr;
  int32_t c;
  int32_t n;
  if (!a) {
    return;
  }
  if (depth > 1 && a->buf) {
    int32_t i;
    for (i = 0; i < a->size; i++) {
      avant_array_thaw(((void **)a->buf)[i], depth - 1, elem_size);
    }
  }
  c = arr_cap_word(a);
  do {
    if (c >= 0) {
      fflush(stdout);
      fputs("avant runtime: internal error: a thaw of an array that is not frozen\n", stderr);
      abort();
    }
    if ((c & 0x7fffffff) == 1) {
      n = arr_thawed_cap(a, depth > 1 ? sizeof(void *) : elem_size);
    } else {
      n = c - 1;
    }
  } while (!__atomic_compare_exchange_n(&a->cap, &c, n, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
}

/* The freeze of a method call on an element. A call of a method that
 * changes its struct receiver, an element of an array, freezes that
 * array for the call when the call may reach it (the compiler decides:
 * compiler/mir/lower_agg.av held_method_call): the receiver is the
 * element's address, so a growth that moved the buffer, or a removal or
 * reorder, would leave the method writing an old buffer, a slot past the
 * size or another element. The freeze is the task's (one more count in
 * the cap word), so every change of the size or order traps as for a
 * task; an element store or a fill does not (they change values, not
 * places), which avant_array_frozen tells by this thread's holds.
 *
 * The holds are a stack per thread, innermost last: each names the
 * caller's frame slot that holds the array (a collection that moves the
 * array rewrites the slot), the element size the last thaw needs, and
 * the call's name and site for the trap's text. A trap that ends a task
 * or an HTTP handler thaws the holds the code below its recovery point
 * took (avant_array_hold_drop, from avant_task_trapped, while those
 * frames are still intact). */
typedef struct ArrHold {
  void **slot;
  uint64_t elem_size;
  const char *name;
  const char *site;
} ArrHold;

static __thread ArrHold *g_arr_holds;
static __thread int g_arr_hold_n;
static __thread int g_arr_hold_cap;

void avant_array_hold(void **slot, uint64_t elem_size, const char *name, const char *site) {
  if (g_arr_hold_n == g_arr_hold_cap) {
    int cap = g_arr_hold_cap ? g_arr_hold_cap * 2 : 16;
    ArrHold *h = (ArrHold *)realloc(g_arr_holds, (size_t)cap * sizeof(ArrHold));
    if (!h) {
      fflush(stdout);
      fputs("avant runtime: out of memory for a method call's freeze\n", stderr);
      exit(1);
    }
    g_arr_holds = h;
    g_arr_hold_cap = cap;
  }
  g_arr_holds[g_arr_hold_n].slot = slot;
  g_arr_holds[g_arr_hold_n].elem_size = elem_size;
  g_arr_holds[g_arr_hold_n].name = name;
  g_arr_holds[g_arr_hold_n].site = site;
  g_arr_hold_n++;
  avant_array_freeze(*slot, 1);
}

void avant_array_unhold(void **slot) {
  ArrHold *h;
  if (g_arr_hold_n <= 0 || g_arr_holds[g_arr_hold_n - 1].slot != slot) {
    fflush(stdout);
    fputs("avant runtime: internal error: a method call's thaw that is not the innermost freeze\n", stderr);
    abort();
  }
  h = &g_arr_holds[--g_arr_hold_n];
  avant_array_thaw(*h->slot, 1, h->elem_size);
}

__attribute__((visibility("hidden"))) int avant_array_hold_depth(void) {
  return g_arr_hold_n;
}

__attribute__((visibility("hidden"))) void avant_array_hold_drop(int n) {
  while (g_arr_hold_n > n) {
    ArrHold *h = &g_arr_holds[--g_arr_hold_n];
    avant_array_thaw(*h->slot, 1, h->elem_size);
  }
}

/* A change (op, AVANT_FROZEN_*) to the frozen array arr, from generated
 * code. An element store or a fill passes when this thread's method
 * calls hold every count of the freeze; anything else traps, naming the
 * innermost method call of this thread that froze arr, else as a task's
 * freeze. */
void avant_array_frozen(void *arr, int32_t op) {
  AvantArray *a = (AvantArray *)arr;
  int32_t c = arr_cap_word(a);
  int32_t mine = 0;
  int i;
  if (c >= 0) {
    return;
  }
  for (i = g_arr_hold_n - 1; i >= 0; i--) {
    if (*g_arr_holds[i].slot == arr) {
      mine++;
    }
  }
  if ((op == AVANT_FROZEN_STORE || op == AVANT_FROZEN_FILL) && mine == (c & 0x7fffffff)) {
    return;
  }
  arr_frozen_trap(arr, op);
}

/* A store through a struct method's receiver p, in a program with held
 * method calls that spawns (compiler/mir/lower_agg.av recv_store_check).
 * When p lies in an array this thread's call holds, and the freeze has
 * more counts than this thread's holds of that array, a task reads it
 * (one the call started: a task that read it before the call made the
 * call's element place trap), and the store would change an element
 * under it: it traps at the store's site, naming the held call. */
void avant_array_recv_store(void *p, const char *site) {
  int i;
  int j;
  if (g_arr_hold_n == 0) {
    return;
  }
  for (i = g_arr_hold_n - 1; i >= 0; i--) {
    AvantArray *a = (AvantArray *)*g_arr_holds[i].slot;
    char *lo;
    int32_t c;
    int32_t mine = 0;
    if (!a || !a->buf) {
      continue;
    }
    lo = (char *)a->buf;
    if ((char *)p < lo || (char *)p >= lo + (uint64_t)a->size * g_arr_holds[i].elem_size) {
      continue;
    }
    c = arr_cap_word(a);
    if (c >= 0) {
      return;
    }
    for (j = g_arr_hold_n - 1; j >= 0; j--) {
      if (*g_arr_holds[j].slot == (void *)a) {
        mine++;
      }
    }
    if (mine == (c & 0x7fffffff)) {
      return;
    }
    avant_trap_frozen_call(AVANT_FROZEN_RECV, g_arr_holds[i].name, site);
  }
}

static void arr_frozen_trap(void *arr, int32_t op) {
  int i;
  for (i = g_arr_hold_n - 1; i >= 0; i--) {
    if (*g_arr_holds[i].slot == arr) {
      avant_trap_frozen_call(op, g_arr_holds[i].name, g_arr_holds[i].site);
    }
  }
  avant_trap_frozen(op);
}

/* min_cap is checked against the ceiling here, before anything is
 * allocated: every growth comes through this one. The ceiling is the
 * element count's or, for wide elements, one object's storage
 * (avant_array_max); the last doubling stops at it, so an Array(Int)
 * grown by push reaches 536870911 elements, not only the 268435456 of
 * the last doubling that fits. */
static void array_grow(AvantArray *a, int64_t min_cap, uint64_t elem_size, uint32_t buf_type_id) {
  uint64_t max = avant_array_max(elem_size);
  if (min_cap < 0 || (uint64_t)min_cap > max) {
    avant_trap_array_size(elem_size);
  }
  int32_t ncap = a->cap ? a->cap : 4;
  while (ncap < min_cap) {
    if ((uint64_t)ncap * 2 > max) {
      ncap = (int32_t)max;
      break;
    }
    ncap *= 2;
  }
  if ((uint64_t)ncap > max) {
    ncap = (int32_t)max;
  }
  uint64_t bytes = (uint64_t)ncap * elem_size;
  /* RC1: the allocation may collect; a is a handle, a->buf is read after
   * it (RC2). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &a);
  /* A large buffer grows in place when the pages after it are free (the
   * elements stay where they are; the room added is zeroed unless the
   * elements are scalars). */
  if (a->buf && a->size > 0 && avant_gc_large_extend(a->buf, bytes, buf_type_id == AVANT_TYPE_BYTES)) {
    a->cap = ncap;
    return;
  }
  /* A buffer of scalars is not zeroed (a large one, as Go's growslice
   * leaves pointer-free storage): the elements below the size are copied
   * in now, and nothing reads one past the size before a push writes it.
   * A buffer of references is zeroed: the collector reads all its words. */
  void *nbuf = buf_type_id == AVANT_TYPE_BYTES ? avant_alloc_bytes_room(bytes) : avant_alloc(bytes, buf_type_id);
  if (a->buf && a->size > 0) {
    memcpy(nbuf, a->buf, (size_t)a->size * (size_t)elem_size);
    /* A large pointer buffer may be mature (a mature-allocating thread; young
     * otherwise) and hold young elements. */
    if (buf_type_id != AVANT_TYPE_BYTES) {
      avant_barrier(nbuf);
    }
  }
  void *obuf = a->buf;
  avant_barrier(a);
  a->buf = nbuf;
  a->cap = ncap;
  /* The array no longer names its old buffer: a large one goes back now
   * unless a root still names it, or it is mature with references
   * (avant_gc_large_drop), as a Buf's storage does. */
  if (obuf) {
    (void)avant_gc_large_drop(obuf, 2);
  }
}

/* The push and reserve entries (RC1, avant_gc.h; they
 * were named safepoints once): the fast path neither allocates nor
 * collects; the growth path keeps the array (and push_ptr's element) in
 * handles across array_grow and forms the slot after it (RC2). */
/* An element store barriers the buffer with its slot (avant_barrier_slot:
 * a large pointer buffer marks the slot's card) and not the array object,
 * whose size and capacity are not references (remembering
 * the array object at every push made the young collection scan the
 * buffer it names whole, and defeated the cards); array_grow
 * barriers the array object when it stores a new buffer. */
void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  void *slot = NULL;
  if (!a) {
    return NULL;
  }
  if (a->buf && a->size < arr_cap_word(a)) {
    slot = (uint8_t *)a->buf + (size_t)a->size * (size_t)elem_size;
    a->size += 1;
    if (a->buf) {
      avant_barrier_slot_young(a->buf, slot);
    }
    return slot;
  }
  /* A frozen array's cap word is negative, so its push always comes
   * here: the fast path above needs no test of its own. */
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_PUSH);
  {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &a);
    if (a->size >= a->cap) {
      array_grow(a, (int64_t)a->size + 1, elem_size, buf_type_id);
    }
    slot = (uint8_t *)a->buf + (size_t)a->size * (size_t)elem_size;
    a->size += 1;
    if (a->buf) {
      avant_barrier_slot_young(a->buf, slot);
    }
  }
  return slot;
}

void avant_array_push_ptr(void *arr, void *elem, uint32_t buf_type_id) {
  void *slot;
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  if (a->buf && a->size < arr_cap_word(a)) {
    slot = (uint8_t *)a->buf + (size_t)a->size * sizeof(void *);
    /* The barrier before the store (RC5). */
    avant_barrier_slot_young(a->buf, slot);
    memcpy(slot, &elem, sizeof(void *));
    a->size += 1;
    return;
  }
  {
    /* No collecting call between the slot and the store (in a shared
     * program too: other threads collect only at this one's
     * safepoints). */
    AVANT_GC_SCOPE(2);
    AVANT_GC_ROOT(0, &a);
    AVANT_GC_ROOT(1, &elem);
    slot = avant_array_push_slot(a, sizeof(void *), buf_type_id);
    if (slot) {
      memcpy(slot, &elem, sizeof(void *));
    }
  }
}

/* set, get and pop of a pointer element never collect and are no
 * safepoint, so no thread collects while one runs (the safepoint
 * handshake). Before the handshake a shared program took the world lock around
 * each, so that another thread's collection could not move the buffer
 * between the load and the store. */
void avant_array_set_ptr(void *arr, avant_int i, void *elem) {
  AvantArray *a = (AvantArray *)arr;
  if (a && a->buf) {
    /* The barrier before the store (RC5). */
    avant_barrier_slot_young(a->buf, &((void **)a->buf)[i]);
    ((void **)a->buf)[i] = elem;
  }
}

void *avant_array_get_ptr(void *arr, avant_int i) {
  AvantArray *a = (AvantArray *)arr;
  if (a && a->buf && i >= 0 && i < a->size) {
    return ((void **)a->buf)[i];
  }
  return NULL;
}

/* `xs.pop()` of a reference: the last element, or NULL (the optional's
 * nil) on an empty array. The popped word is cleared (RC5: the
 * overwrite first), so the collector, which reads every word of the
 * buffer, no longer reaches the element through it. */
void *avant_array_pop_ptr(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  void *v;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_POP);
  }
  if (a && a->buf && a->size > 0) {
    a->size -= 1;
    v = ((void **)a->buf)[a->size];
    avant_gc_overwrite(a->buf);
    ((void **)a->buf)[a->size] = NULL;
    return v;
  }
  return NULL;
}

void avant_array_push_i32(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  if (a->buf && a->size < arr_cap_word(a)) {
    ((int32_t *)a->buf)[a->size] = v;
    a->size += 1;
    return;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_PUSH);
  {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &a);
    if (a->size >= a->cap) {
      array_grow(a, (int64_t)a->size + 1, sizeof(int32_t), AVANT_TYPE_BYTES);
    }
    ((int32_t *)a->buf)[a->size] = v;
    a->size += 1;
  }
}

/* The push of an Int, as avant_array_push_i32's of an
 * Int32. */
void avant_array_push_i64(void *arr, int64_t v) {
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  if (a->buf && a->size < arr_cap_word(a)) {
    ((int64_t *)a->buf)[a->size] = v;
    a->size += 1;
    return;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_PUSH);
  {
    AVANT_GC_SCOPE(1);
    AVANT_GC_ROOT(0, &a);
    if (a->size >= a->cap) {
      array_grow(a, (int64_t)a->size + 1, sizeof(int64_t), AVANT_TYPE_BYTES);
    }
    ((int64_t *)a->buf)[a->size] = v;
    a->size += 1;
  }
}

void avant_array_clear(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_CLEAR);
    a->size = 0;
  }
}

/* Zeroes the elements LO to HI (excluded) of a buffer whose elements
 * hold references, past the array's size (a pop's slot, a clear's
 * elements): the collector reads every word of a buffer, so a removed
 * reference left there would keep its object alive (RC5: the overwrite
 * first). */
void avant_array_zero_range(void *arr, avant_int lo, avant_int hi, uint64_t elem_size) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || lo < 0 || hi <= lo) {
    return;
  }
  avant_gc_overwrite(a->buf);
  memset((uint8_t *)a->buf + (size_t)lo * (size_t)elem_size, 0, (size_t)(hi - lo) * (size_t)elem_size);
}

/* gc-lint: holds nothing (the growth is its last statement; array_grow roots the array). */
void avant_array_reserve(void *arr, avant_int n, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  if (!a) {
    return;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_RESERVE);
  if (n <= a->cap) {
    return;
  }
  array_grow(a, n, elem_size, buf_type_id);
}

void *avant_array_pop_slot(void *arr, uint64_t elem_size) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_POP);
  }
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
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  sort_i32((int32_t *)a->buf, a->size);
}

static int cmp_i64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a;
  int64_t y = *(const int64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmp_f64(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  int xn = x != x;
  int yn = y != y;
  if (xn && yn) {
    return 0;
  }
  if (xn) {
    return 1;
  }
  if (yn) {
    return -1;
  }
  return x < y ? -1 : (x > y ? 1 : 0);
}

static int str_cmp_raw(const char *a, const char *b) {
  size_t na;
  size_t nb;
  a = avant_str_bytes(a, &na);
  b = avant_str_bytes(b, &nb);
  size_t n = na < nb ? na : nb;
  int c = n ? memcmp(a, b, n) : 0;
  if (c != 0) {
    return c;
  }
  return na < nb ? -1 : (na > nb ? 1 : 0);
}

static int cmp_str(const void *a, const void *b) {
  return str_cmp_raw(*(const char *const *)a, *(const char *const *)b);
}

/* An Array(Int) (Int is 64-bit): when every element
 * fits 32 bits, which is the common case, the elements are narrowed in
 * place into the buffer's first half (element i's four bytes overlap only
 * elements at or before i, already read), sorted by sort_i32's radix and
 * counting sorts, and widened back from the last (element i's eight bytes
 * overlap only narrowed elements at or after i, already widened);
 * otherwise qsort. The same order, with no memory beyond sort_i32's. */
void avant_array_sort_i64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int64_t *w;
  int32_t *n32;
  int32_t n;
  int32_t i;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  w = (int64_t *)a->buf;
  n = a->size;
  for (i = 0; i < n; i++) {
    if (w[i] < INT32_MIN || w[i] > INT32_MAX) {
      qsort(a->buf, (size_t)n, sizeof(int64_t), cmp_i64);
      return;
    }
  }
  n32 = (int32_t *)a->buf;
  for (i = 0; i < n; i++) {
    n32[i] = (int32_t)w[i];
  }
  sort_i32(n32, n);
  for (i = n - 1; i >= 0; i--) {
    w[i] = (int64_t)n32[i];
  }
}

/* An Array(UInt32): its order is sort_i32's on the keys with the sign bit
 * flipped (i32_order), in place. */
void avant_array_sort_u32(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  uint32_t *u;
  int32_t i;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  u = (uint32_t *)a->buf;
  for (i = 0; i < a->size; i++) {
    u[i] ^= 0x80000000u;
  }
  sort_i32((int32_t *)a->buf, a->size);
  for (i = 0; i < a->size; i++) {
    u[i] ^= 0x80000000u;
  }
}

void avant_array_sort_u64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (a && a->buf && a->size > 1) {
    qsort(a->buf, (size_t)a->size, sizeof(uint64_t), cmp_u64);
  }
}

void avant_array_sort_f64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (a && a->buf && a->size > 1) {
    qsort(a->buf, (size_t)a->size, sizeof(double), cmp_f64);
  }
}

void avant_array_sort_str(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (a && a->buf && a->size > 1) {
    /* The strings change places in their buffer (RC5). */
    avant_gc_overwrite(a->buf);
    qsort(a->buf, (size_t)a->size, sizeof(char *), cmp_str);
  }
}

void avant_array_sort_bool(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int32_t zeros = 0;
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_SORT);
  }
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  {
    int8_t *p = (int8_t *)a->buf;
    for (i = 0; i < a->size; i++) {
      if (!p[i]) {
        zeros++;
      }
    }
    for (i = 0; i < a->size; i++) {
      p[i] = i < zeros ? 0 : 1;
    }
  }
}

void avant_array_reverse(void *arr, uint64_t elem_size) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int32_t j;
  uint8_t tmp[64];
  if (a) {
    ARR_FROZEN_TRAP(a, AVANT_FROZEN_REVERSE);
  }
  if (!a || !a->buf || a->size <= 1 || elem_size == 0) {
    return;
  }
  /* The elements change places in their buffer; references among them
   * move to other words of it (RC5; nothing for a buffer without
   * references). */
  avant_gc_overwrite(a->buf);
  if (elem_size > sizeof(tmp)) {
    uint8_t *big = (uint8_t *)malloc((size_t)elem_size);
    if (!big) {
      return;
    }
    i = 0;
    j = a->size - 1;
    while (i < j) {
      uint8_t *x = (uint8_t *)a->buf + (size_t)i * (size_t)elem_size;
      uint8_t *y = (uint8_t *)a->buf + (size_t)j * (size_t)elem_size;
      memcpy(big, x, (size_t)elem_size);
      memcpy(x, y, (size_t)elem_size);
      memcpy(y, big, (size_t)elem_size);
      i++;
      j--;
    }
    free(big);
    return;
  }
  i = 0;
  j = a->size - 1;
  while (i < j) {
    uint8_t *x = (uint8_t *)a->buf + (size_t)i * (size_t)elem_size;
    uint8_t *y = (uint8_t *)a->buf + (size_t)j * (size_t)elem_size;
    memcpy(tmp, x, (size_t)elem_size);
    memcpy(x, y, (size_t)elem_size);
    memcpy(y, tmp, (size_t)elem_size);
    i++;
    j--;
  }
}

/* RC1: the source and the copy are handles across the two allocations;
 * the source's buffer is read after the last one (RC2). */
void *avant_array_copy(void *arr, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  AvantArray *r = NULL;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &a);
  AVANT_GC_ROOT(1, &r);
  r = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (a && a->buf && a->size > 0) {
    void *nbuf = avant_alloc((uint64_t)a->size * elem_size, buf_type_id);
    memcpy(nbuf, a->buf, (size_t)a->size * (size_t)elem_size);
    if (buf_type_id != AVANT_TYPE_BYTES) {
      /* A large pointer buffer may be mature (a mature-allocating thread;
       * young otherwise): barrier it. */
      avant_barrier(nbuf);
    }
    avant_barrier(r);
    r->buf = nbuf;
    r->size = a->size;
    r->cap = a->size;
  }
  return r;
}

/* xs[start..stop], xs.slice(start, stop): a new array holding a copy of
 * the elements start up to stop; traps unless 0 <= start <= stop <= size,
 * as a String's slice does. */
void *avant_array_slice(void *arr, avant_int start, avant_int stop, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  AvantArray *r = NULL;
  avant_int n = a ? a->size : 0;
  if (start < 0 || stop < start || stop > n) {
    avant_trap_slice(start, stop, n);
  }
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &a);
  AVANT_GC_ROOT(1, &r);
  r = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (stop > start) {
    int32_t len = (int32_t)(stop - start);
    void *nbuf = avant_alloc((uint64_t)len * elem_size, buf_type_id);
    /* a->buf is read after the allocation (RC2). */
    memcpy(nbuf, (char *)a->buf + (size_t)start * (size_t)elem_size, (size_t)len * (size_t)elem_size);
    if (buf_type_id != AVANT_TYPE_BYTES) {
      avant_barrier(nbuf);
    }
    avant_barrier(r);
    r->buf = nbuf;
    r->size = len;
    r->cap = len;
  }
  return r;
}

/* The slots lo up to hi of a pointer-carrying buffer, each barriered
 * before a move puts a reference there (RC5, as a push barriers its
 * slot). */
static void arr_barrier_slots(AvantArray *a, avant_int lo, avant_int hi, uint64_t elem_size, uint32_t buf_type_id) {
  if (buf_type_id == AVANT_TYPE_BYTES || !a->buf) {
    return;
  }
  for (avant_int k = lo; k < hi; k++) {
    avant_barrier_slot_young(a->buf, (uint8_t *)a->buf + (size_t)k * (size_t)elem_size);
  }
}

/* xs.insert(i, v): room for one element at i, the elements from i on
 * moved up one; the slot at i is returned for the caller's store, as
 * avant_array_push_slot's is. Traps unless 0 <= i <= size. */
void *avant_array_insert_slot(void *arr, avant_int i, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  avant_int n = a ? a->size : 0;
  if (i < 0 || i > n) {
    avant_trap_insert(i, n);
  }
  if (!a) {
    return NULL;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_INSERT);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &a);
  (void)avant_array_push_slot(a, elem_size, buf_type_id);
  /* a->buf is read after the push's growth (RC2). */
  uint8_t *at = (uint8_t *)a->buf + (size_t)i * (size_t)elem_size;
  if (n > i) {
    /* References move to other words of the buffer (RC5). */
    avant_gc_overwrite(a->buf);
    arr_barrier_slots(a, i + 1, n + 1, elem_size, buf_type_id);
    memmove(at + elem_size, at, (size_t)(n - i) * (size_t)elem_size);
  }
  if (buf_type_id != AVANT_TYPE_BYTES) {
    avant_barrier_slot_young(a->buf, at);
  }
  return at;
}

/* xs.delete_at(i), after the caller read the element at i (its bounds
 * checked there): the elements after i moved down one, the last slot
 * cleared so it keeps nothing alive, the size one less. */
void avant_array_delete_at(void *arr, avant_int i, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || i < 0 || i >= a->size) {
    return;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_DELETE_AT);
  avant_int n = a->size;
  uint8_t *at = (uint8_t *)a->buf + (size_t)i * (size_t)elem_size;
  /* References are removed and move to other words of the buffer (RC5). */
  avant_gc_overwrite(a->buf);
  if (n - 1 > i) {
    arr_barrier_slots(a, i, n - 1, elem_size, buf_type_id);
    memmove(at, at + elem_size, (size_t)(n - 1 - i) * (size_t)elem_size);
  }
  memset((uint8_t *)a->buf + (size_t)(n - 1) * (size_t)elem_size, 0, (size_t)elem_size);
  a->size = (int32_t)(n - 1);
}

/* xs.concat(ys): ys's elements appended to xs, in order (ys may be xs:
 * its size is read first, its buffer after the growth). A frozen xs
 * traps also when ys is empty, as every other change does. */
void avant_array_concat(void *dst, void *src, uint64_t elem_size, uint32_t buf_type_id) {
  AvantArray *a = (AvantArray *)dst;
  AvantArray *b = (AvantArray *)src;
  if (!a) {
    return;
  }
  ARR_FROZEN_TRAP(a, AVANT_FROZEN_CONCAT);
  if (!b || b->size <= 0) {
    return;
  }
  avant_int m = b->size;
  avant_int n = a->size;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &a);
  AVANT_GC_ROOT(1, &b);
  if (n + m > a->cap) {
    array_grow(a, n + m, elem_size, buf_type_id);
  }
  /* Both buffers are read after the growth (RC2); a bulk copy barriers
   * the buffer first (RC5). */
  if (buf_type_id != AVANT_TYPE_BYTES) {
    avant_barrier(a->buf);
  }
  memcpy((uint8_t *)a->buf + (size_t)n * (size_t)elem_size, b->buf, (size_t)m * (size_t)elem_size);
  a->size = (int32_t)(n + m);
}

int32_t avant_array_contains_i32(void *arr, int32_t v) { return avant_array_index_i32(arr, v) >= 0 ? 1 : 0; }
int32_t avant_array_contains_i64(void *arr, int64_t v) { return avant_array_index_i64(arr, v) >= 0 ? 1 : 0; }
int32_t avant_array_contains_u64(void *arr, uint64_t v) { return avant_array_index_u64(arr, v) >= 0 ? 1 : 0; }
int32_t avant_array_contains_f64(void *arr, double v) { return avant_array_index_f64(arr, v) >= 0 ? 1 : 0; }
int32_t avant_array_contains_str(void *arr, const char *v) { return avant_array_index_str(arr, v) >= 0 ? 1 : 0; }
int32_t avant_array_contains_ptr(void *arr, void *v) { return avant_array_index_ptr(arr, v) >= 0 ? 1 : 0; }

avant_int avant_array_index_i32(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (((int32_t *)a->buf)[i] == v) { return i; } }
  return -1;
}

avant_int avant_array_index_i64(void *arr, int64_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (((int64_t *)a->buf)[i] == v) { return i; } }
  return -1;
}

avant_int avant_array_index_u64(void *arr, uint64_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (((uint64_t *)a->buf)[i] == v) { return i; } }
  return -1;
}

avant_int avant_array_index_f64(void *arr, double v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (((double *)a->buf)[i] == v) { return i; } }
  return -1;
}

avant_int avant_array_index_str(void *arr, const char *v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (avant_str_eq(((char **)a->buf)[i], v)) { return i; } }
  return -1;
}

int32_t avant_array_contains_bool(void *arr, int32_t v) { return avant_array_index_bool(arr, v) >= 0 ? 1 : 0; }

avant_int avant_array_index_bool(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if ((((int8_t *)a->buf)[i] != 0) == (v != 0)) { return i; } }
  return -1;
}

avant_int avant_array_index_ptr(void *arr, void *v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  if (!a || !a->buf) { return -1; }
  for (i = 0; i < a->size; i++) { if (((void **)a->buf)[i] == v) { return i; } }
  return -1;
}

int32_t avant_array_min_i32(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int32_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((int32_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((int32_t *)a->buf)[i] < best) { best = ((int32_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

int64_t avant_array_min_i64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int64_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((int64_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((int64_t *)a->buf)[i] < best) { best = ((int64_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

uint32_t avant_array_min_u32(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint32_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((uint32_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((uint32_t *)a->buf)[i] < best) { best = ((uint32_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

uint64_t avant_array_min_u64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint64_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((uint64_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((uint64_t *)a->buf)[i] < best) { best = ((uint64_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

double avant_array_min_f64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  double best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  /* sort's order (NaN after every number): the
   * smallest number, NaN only when every element is NaN. */
  best = ((double *)a->buf)[0];
  for (i = 1; i < a->size; i++) { double v = ((double *)a->buf)[i]; if (v < best || best != best) { best = v; } }
  if (found) { *found = 1; }
  return best;
}

const char *avant_array_min_str(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  const char *best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return AVANT_STR_EMPTY; }
  best = ((char **)a->buf)[0];
  for (i = 1; i < a->size; i++) { const char *v = ((char **)a->buf)[i]; if (str_cmp_raw(v, best) < 0) { best = v; } }
  if (found) { *found = 1; }
  return best;
}

int32_t avant_array_max_i32(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int32_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((int32_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((int32_t *)a->buf)[i] > best) { best = ((int32_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

int64_t avant_array_max_i64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int64_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((int64_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((int64_t *)a->buf)[i] > best) { best = ((int64_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

uint32_t avant_array_max_u32(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint32_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((uint32_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((uint32_t *)a->buf)[i] > best) { best = ((uint32_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

uint64_t avant_array_max_u64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint64_t best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  best = ((uint64_t *)a->buf)[0];
  for (i = 1; i < a->size; i++) { if (((uint64_t *)a->buf)[i] > best) { best = ((uint64_t *)a->buf)[i]; } }
  if (found) { *found = 1; }
  return best;
}

double avant_array_max_f64(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  double best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return 0; }
  /* sort's order (NaN after every number): NaN when any element
   * is NaN, else the largest number. */
  best = ((double *)a->buf)[0];
  for (i = 1; i < a->size; i++) { double v = ((double *)a->buf)[i]; if (v > best || v != v) { best = v; } }
  if (found) { *found = 1; }
  return best;
}

const char *avant_array_max_str(void *arr, int32_t *found) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  const char *best;
  if (found) { *found = 0; }
  if (!a || !a->buf || a->size <= 0) { return AVANT_STR_EMPTY; }
  best = ((char **)a->buf)[0];
  for (i = 1; i < a->size; i++) { const char *v = ((char **)a->buf)[i]; if (str_cmp_raw(v, best) > 0) { best = v; } }
  if (found) { *found = 1; }
  return best;
}

int32_t avant_array_sum_i32(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int64_t s = 0;
  if (!a || !a->buf) { return 0; }
  /* An Array(Int32): the int64_t total cannot overflow below 2^32
   * elements; the sum is checked once against Int32. */
  for (i = 0; i < a->size; i++) { s += ((int32_t *)a->buf)[i]; }
  if (s < INT32_MIN || s > INT32_MAX) { avant_trap_overflow(); }
  return (int32_t)s;
}

int64_t avant_array_sum_i64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int64_t s = 0;
  if (!a || !a->buf) { return 0; }
  for (i = 0; i < a->size; i++) { if (__builtin_add_overflow(s, ((int64_t *)a->buf)[i], &s)) { avant_trap_overflow(); } }
  return s;
}

uint32_t avant_array_sum_u32(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint64_t s = 0;
  if (!a || !a->buf) { return 0; }
  for (i = 0; i < a->size; i++) { s += ((uint32_t *)a->buf)[i]; }
  if (s > UINT32_MAX) { avant_trap_overflow(); }
  return (uint32_t)s;
}

uint64_t avant_array_sum_u64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  uint64_t s = 0;
  if (!a || !a->buf) { return 0; }
  for (i = 0; i < a->size; i++) { if (__builtin_add_overflow(s, ((uint64_t *)a->buf)[i], &s)) { avant_trap_overflow(); } }
  return s;
}

double avant_array_sum_f64(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  double s = 0;
  if (!a || !a->buf) { return 0; }
  for (i = 0; i < a->size; i++) { s += ((double *)a->buf)[i]; }
  return s;
}

void avant_array_fill_u8(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  if (a) {
    ARR_FROZEN_TRAP_NUM(a, AVANT_FROZEN_FILL);
  }
  if (!a || !a->buf || a->size <= 0) {
    return;
  }
  memset(a->buf, (int)(unsigned char)v, (size_t)a->size);
}

/* An Array(Int) (or UInt64, by its bits) filled with v. */
void avant_array_fill_i64(void *arr, int64_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int64_t *p;
  if (a) {
    ARR_FROZEN_TRAP_NUM(a, AVANT_FROZEN_FILL);
  }
  if (!a || !a->buf || a->size <= 0) {
    return;
  }
  p = (int64_t *)a->buf;
  if (v == 0 || v == -1) {
    memset(p, v == 0 ? 0 : 0xff, (size_t)a->size * sizeof(int64_t));
    return;
  }
#if defined(__SSE2__)
  {
    /* Two elements a store, as fill_i32 stores four. */
    __m128i x = _mm_set1_epi64x(v);
    int32_t n = a->size;
    i = 0;
    while (i + 4 <= n) {
      _mm_storeu_si128((__m128i *)(p + i), x);
      _mm_storeu_si128((__m128i *)(p + i + 2), x);
      i += 4;
    }
    while (i < n) {
      p[i] = v;
      i++;
    }
  }
#else
  for (i = 0; i < a->size; i++) {
    p[i] = v;
  }
#endif
}

void avant_array_fill_i32(void *arr, int32_t v) {
  AvantArray *a = (AvantArray *)arr;
  int32_t n;
  int32_t i;
  int32_t *p;
  if (a) {
    ARR_FROZEN_TRAP_NUM(a, AVANT_FROZEN_FILL);
  }
  if (!a || !a->buf || a->size <= 0) {
    return;
  }
  n = a->size;
  p = (int32_t *)a->buf;
  /* 0 and -1 are a repeated byte. A* fills came_from with -1. */
  if (v == 0 || v == -1) {
    memset(p, v == 0 ? 0 : 0xff, (size_t)n * sizeof(int32_t));
    return;
  }
#if defined(__SSE2__)
  {
    __m128i x = _mm_set1_epi32(v);
    i = 0;
    while (i + 8 <= n) {
      _mm_storeu_si128((__m128i *)(p + i), x);
      _mm_storeu_si128((__m128i *)(p + i + 4), x);
      i += 8;
    }
    while (i + 4 <= n) {
      _mm_storeu_si128((__m128i *)(p + i), x);
      i += 4;
    }
    while (i < n) {
      p[i] = v;
      i++;
    }
  }
#else
  for (i = 0; i < n; i++) {
    p[i] = v;
  }
#endif
}

/* The BWT entries take the references' widths (LangArena compress.c and
 * compress.rs): the input, the transform and
 * the inverse's result are bytes (Buf: uint8_t, :7, :22, :256); the
 * suffix array, its scratch copy and the inverse's next links are
 * Array(Int) (size_t, :38, :44, :245; usize in Rust); the ranks and the
 * counting sort's counts are Array(Int32) (int, :66, :93). Every value
 * is an index or a rank below the size, so the arithmetic stays in
 * int32_t. */
typedef struct {
  uint8_t *data;
  int32_t size;
  int32_t cap;
} BwtBuf;

static avant_int *bwt_ix(void *arr, int32_t need) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size < need || need < 0) {
    return NULL;
  }
  /* The array is written: one a running task reads cannot change. */
  ARR_FROZEN_TRAP_NUM(a, AVANT_FROZEN_STORE);
  return (avant_int *)a->buf;
}

static int32_t *bwt_i32(void *arr, int32_t need) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size < need || need < 0) {
    return NULL;
  }
  /* The array is written: one a running task reads cannot change. */
  ARR_FROZEN_TRAP_NUM(a, AVANT_FROZEN_STORE);
  return (int32_t *)a->buf;
}

static uint8_t *bwt_bytes(void *buf, int32_t need) {
  BwtBuf *b = (BwtBuf *)buf;
  if (!b || !b->data || b->size < need || need < 0) {
    return NULL;
  }
  return b->data;
}

/* Stable counting sort of sa by rank[(sa[i] + add) % n]. */
static int order_by_rank(avant_int *sa, const int32_t *rank, avant_int *tmp, int32_t *count,
                         int32_t n, int32_t csz, int32_t add) {
  int32_t i;
  int32_t sum;
  if (n <= 1) {
    return 1;
  }
  if (csz <= 0) {
    return 0;
  }
  memset(count, 0, (size_t)csz * sizeof(int32_t));
  if (add <= 0) {
    for (i = 0; i < n; i++) {
      int32_t key = rank[sa[i]];
      if ((uint32_t)key >= (uint32_t)csz) {
        return 0;
      }
      count[key]++;
    }
  } else {
    for (i = 0; i < n; i++) {
      int32_t idx = (int32_t)sa[i] + add;
      if (idx >= n) {
        idx -= n;
      }
      int32_t key = rank[idx];
      if ((uint32_t)key >= (uint32_t)csz) {
        return 0;
      }
      count[key]++;
    }
  }
  sum = 0;
  for (i = 0; i < csz; i++) {
    int32_t c = (int32_t)count[i];
    count[i] = sum;
    sum += c;
  }
  /* The counts are below n: read as unsigned, the slot is a plain 32-bit
   * load (a sign-extending load of a just-stored count forwards slowly:
   * Compress::BWTEncode 437 -> 320 ms, the same instructions). */
  if (add <= 0) {
    for (i = 0; i < n; i++) {
      int32_t idx = (int32_t)sa[i];
      tmp[(uint32_t)count[rank[idx]]++] = idx;
    }
  } else {
    for (i = 0; i < n; i++) {
      int32_t idx = (int32_t)sa[i];
      int32_t j = idx + add;
      if (j >= n) {
        j -= n;
      }
      tmp[(uint32_t)count[rank[j]]++] = idx;
    }
  }
  memcpy(sa, tmp, (size_t)n * sizeof(avant_int));
  return 1;
}

/* 1 when the last ordered suffix got rank n - 1. */
static int rerank(avant_int *sa, int32_t *rank, int32_t *neu, int32_t n, int32_t add) {
  int32_t prev;
  int32_t i;
  if (n <= 0) {
    return 1;
  }
  prev = (int32_t)sa[0];
  neu[prev] = 0;
  for (i = 1; i < n; i++) {
    int32_t curr = (int32_t)sa[i];
    int32_t pk = prev + add;
    int32_t ck = curr + add;
    int32_t r;
    if (pk >= n) {
      pk -= n;
    }
    if (ck >= n) {
      ck -= n;
    }
    r = neu[prev];
    if (rank[prev] != rank[curr] || rank[pk] != rank[ck]) {
      r++;
    }
    neu[curr] = r;
    prev = curr;
  }
  memcpy(rank, neu, (size_t)n * sizeof(int32_t));
  return neu[sa[n - 1]] == n - 1;
}

avant_int avant_bwt_transform(void *input_arr, void *sa_arr, void *rank_arr, void *tmp_arr,
                            void *count_arr, void *out_arr) {
  BwtBuf *in_b = (BwtBuf *)input_arr;
  int32_t n;
  uint8_t *input;
  avant_int *sa;
  int32_t *rank;
  avant_int *tmp;
  int32_t *count;
  uint8_t *out;
  int32_t csz;
  int32_t counts[256];
  int32_t positions[256];
  int32_t temp_counts[256];
  int32_t i;
  int32_t total;
  int32_t original;
  if (!in_b || !in_b->data) {
    return 0;
  }
  n = in_b->size;
  csz = 0;
  if (count_arr) {
    csz = ((AvantArray *)count_arr)->size;
  }
  input = bwt_bytes(input_arr, n);
  sa = bwt_ix(sa_arr, n);
  rank = bwt_i32(rank_arr, n);
  tmp = bwt_ix(tmp_arr, n);
  count = bwt_i32(count_arr, csz > 0 ? csz : 1);
  out = bwt_bytes(out_arr, n);
  if (!input || !sa || !rank || !tmp || !count || !out || n <= 0) {
    return 0;
  }
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < n; i++) {
    counts[input[i] & 255]++;
  }
  total = 0;
  for (i = 0; i < 256; i++) {
    positions[i] = total;
    total += counts[i];
  }
  memset(temp_counts, 0, sizeof(temp_counts));
  for (i = 0; i < n; i++) {
    int32_t byte = (int32_t)(input[i] & 255);
    int32_t pos = positions[byte] + temp_counts[byte];
    sa[pos] = i;
    temp_counts[byte]++;
  }
  if (n > 1) {
    int32_t current_rank = 0;
    int32_t prev_char = (int32_t)(input[sa[0]] & 255);
    int32_t k;
    for (i = 0; i < n; i++) {
      int32_t idx = (int32_t)sa[i];
      int32_t ch = (int32_t)(input[idx] & 255);
      if (ch != prev_char) {
        current_rank++;
        prev_char = ch;
      }
      rank[idx] = current_rank;
    }
    k = 1;
    while (k < n) {
      if (!order_by_rank(sa, rank, tmp, count, n, csz, k)) {
        break;
      }
      if (!order_by_rank(sa, rank, tmp, count, n, csz, 0)) {
        break;
      }
      /* The new ranks go into count (n + 1 Int32 slots, free between
       * the sorts), as the reference's int new_rank (compress.c:125). */
      if (csz < n || rerank(sa, rank, count, n, k)) {
        break;
      }
      if (k > n / 2) {
        break;
      }
      k *= 2;
    }
  }
  original = 0;
  for (i = 0; i < n; i++) {
    int32_t suffix = (int32_t)sa[i];
    if (suffix == 0) {
      out[i] = input[n - 1];
      original = i;
    } else {
      out[i] = input[suffix - 1];
    }
  }
  return original;
}

void avant_bwt_inverse(void *bwt_arr, void *next_arr, void *out_arr, avant_int start) {
  BwtBuf *bwt_b = (BwtBuf *)bwt_arr;
  int32_t n;
  uint8_t *bwt;
  avant_int *next;
  uint8_t *out;
  int32_t counts[256];
  int32_t positions[256];
  int32_t temp_counts[256];
  int32_t i;
  int32_t total;
  int32_t idx;
  if (!bwt_b || !bwt_b->data) {
    return;
  }
  n = bwt_b->size;
  bwt = bwt_bytes(bwt_arr, n);
  next = bwt_ix(next_arr, n);
  out = bwt_bytes(out_arr, n);
  /* start names an element (out of range it would read past next). */
  if (!bwt || !next || !out || n <= 0 || start < 0 || start >= n) {
    return;
  }
  memset(counts, 0, sizeof(counts));
  for (i = 0; i < n; i++) {
    counts[bwt[i] & 255]++;
  }
  total = 0;
  for (i = 0; i < 256; i++) {
    positions[i] = total;
    total += counts[i];
  }
  memset(temp_counts, 0, sizeof(temp_counts));
  for (i = 0; i < n; i++) {
    int32_t byte = (int32_t)(bwt[i] & 255);
    int32_t pos = positions[byte] + temp_counts[byte];
    next[pos] = i;
    temp_counts[byte]++;
  }
  /* An index of the input (the caller's original slot). */
  idx = (int32_t)start;
  for (i = 0; i < n; i++) {
    idx = (int32_t)next[idx];
    out[i] = bwt[idx];
  }
}
