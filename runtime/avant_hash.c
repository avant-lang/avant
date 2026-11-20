#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* Hash(K, V): one table for every key and value type the compiler builds.
 *
 * The hash's descriptor (avant_rt.h, AVANT_HASH_K_* and AVANT_HASH_V_*)
 * gives the key's width in bytes (1, 2, 4 or 8), whether an integer key
 * is signed, whether the key is a String, the value's width, and whether
 * the value is a reference the collector traces. Keys and values are
 * stored unboxed at their own width: a Hash(Int32, Int32) holds 4 + 4
 * bytes an entry, a Hash(Int, Int) 8 + 8.
 *
 * An integer key crosses the boundary as a 64-bit integer, extended by
 * its type's signedness (the generated code does it), and is hashed and
 * compared as that integer, so every width hashes the same value the
 * same way. A value never crosses as a value: the entries return its
 * address in the table, and the generated code loads or stores it with
 * its own type (a scalar needs no barrier); a reference value is stored
 * by the runtime, behind the barrier (avant_hash_set_ref_ik, _sk).
 *
 * The layout. The hash object's first three words are its three buffers
 * and its only references (the generated code registers type 1023 with
 * those three words): keys (a pointer buffer for String keys), vals (a
 * pointer buffer for reference values) and meta (no references).
 *
 * Two layouts, one per kind of key (is_compact below says why), both in
 * the order of insertion. The open layout (integer keys): one slot per
 * table position, keys[i] and vals[i] slot i's; meta is a state byte per
 * slot (0 empty, 1 full, 2 deleted; 0x80 below), then the positions in
 * the order of insertion (uint32 each). Linear probing; a deletion marks
 * its position deleted, which no insertion takes again before the next
 * growth, so no entry ever moves while the table lives.
 *
 * The compact layout (String and struct keys): the table is an index of
 * uint32 per position (0 empty, else the entry's number plus one), and
 * the entries are dense, in insertion order: keys[e], vals[e], a state
 * byte per entry (0 deleted, 1 live; 0x80 below) and, for String keys, a
 * uint32 hash per entry, in meta after the index. A deletion slides the
 * index's cluster back and marks its entry deleted; a growth copies the
 * live entries in order. In both, a growth when a quarter or more of the entries
 * made since the last one are deleted compacts the table at its size.
 *
 * The seed. Every hash function takes the hash's seed (h->seed), set at
 * creation from the process's seed (hash_seed), drawn from the system's
 * entropy, so a program's keys fall on other positions in every run and
 * keys chosen to collide in one run do not collide in the next. No
 * output depends on it: every walk is in the order of insertion.
 *
 * Struct keys. A struct whose fields are all integers, Bools and enums
 * and whose size is 1, 2, 4 or 8 bytes is an integer key: the generated
 * code builds the integer from its fields, so its padding is zero, and
 * it takes the integer entries. Every other struct key is passed by
 * address (the _xk entries) and stored as its bytes, kw of them, with a
 * uint32 hash per entry in meta as a String key's: such a hash is an
 * AvantHashX (avant_hash_create_typed), which carries the key's fields
 * as the compiler gave them, the text "W@OFF,..." where W is the width
 * of an integer field (1, 2, 4 or 8; a Bool and an enum are integers)
 * or s for a String, and OFF its byte offset; a nested struct's fields
 * are listed in place. The key's hash and equality read those fields
 * only, so padding never enters either, and they agree with the
 * struct's ==, which compares the same fields (an integer by its bits,
 * a String by its bytes). A key with no String and no padding (flat) is
 * hashed by its words and compared with memcmp. The key's address is
 * the caller's: the generated code keeps the key in a local that its
 * frame roots, so a collection inside an entry updates the key's String
 * fields there, and the entry reads them after its last allocation.
 */

typedef struct {
  void *keys;
  void *vals;
  void *meta;
  int32_t size;
  int32_t cap;
  uint32_t desc;
  uint32_t seed;
  /* The compact layout's entries used so far, deleted ones included. */
  int32_t used;
  /* The descriptor decoded once at creation: the key's and the value's
   * width, the value's width as a shift, and the layout (1 compact). */
  uint8_t kw;
  uint8_t vw;
  uint8_t vshift;
  uint8_t compact;
  /* The changes a walk checks (a wrapping count): one for each key
   * added (a growth's moves are not additions) and HASH_DELETED for each
   * key removed (a deletion, each key a clear removes). A walk of the
   * hash (for, each; a Set's) reads it before its first step and again
   * after each step: an unchanged count is the common case, and a change
   * of exactly one deletion is allowed when the visited entry is the one
   * deleted (avant_hash_walk_ok); anything else traps, since the walk
   * would miss an added key or the walk's order would be lost. The
   * generated code reads it as the tenth field of its %Hash layout
   * (compiler/mir/lower_arr.av), at the offset asserted below. */
  uint32_t changes;
  /* The running tasks that read this hash (avant_rt.h, the freeze of a
   * value holding a hash): 0 when none, and then every operation that
   * changes the hash may run; otherwise each traps
   * (avant_trap_frozen_hash). It takes the four bytes of padding after
   * changes, so a hash is no larger for it. */
  int32_t frozen;
} AvantHash;

_Static_assert(offsetof(AvantHash, changes) == 48, "the generated code reads a hash's count of changes at byte 48");

/* A deletion's step of the count of changes: an addition's is 1, so a
 * step that only deleted (fewer than 65536 keys) and one that added are
 * told apart by the low 16 bits. */
#define HASH_DELETED 0x10000u
_Static_assert(sizeof(AvantHash) == 56, "the freeze count takes the hash's padding: a hash is still 56 bytes");

/* The freeze test of every operation that changes a hash: one load and
 * a compare. The count is read and changed atomically, as an array's
 * cap word is (avant_array.c): a task's end lowers it from its own
 * thread while the owner may read it before joining. */
static inline int32_t hash_frozen(const AvantHash *h) {
  return __atomic_load_n(&h->frozen, __ATOMIC_RELAXED);
}

/* The guard of a change made while the key's own hash or equality runs
 * for this hash (the _uk entries, below). */
static inline void probe_guard(const AvantHash *h, int32_t op);

#define HASH_FROZEN_TRAP(h, op)                                  \
  do {                                                           \
    if (__builtin_expect(hash_frozen(h) != 0, 0)) {              \
      avant_trap_frozen_hash(op);                                \
    }                                                            \
  } while (0)

/* A hash made by avant_hash_create_typed: the hash, then what its
 * struct key or value needs. Its type is AVANT_TYPE_HASH too: the
 * collector traces the first three words only. */
typedef struct {
  AvantHash h;
  /* The key's and the value's sizes in bytes, and the type ids of their
   * buffers (AVANT_TYPE_BYTES, AVANT_TYPE_PTRS or the element type's). */
  uint32_t kw;
  uint32_t vw;
  uint32_t ktid;
  uint32_t vtid;
  /* The type id of an Array of the values (values(): an Array's element
   * buffer, whose stride is the value's size, not its slot's). */
  uint32_t atid;
  /* A key whose type defines its own hash and equality (the _uk entries,
   * set by avant_hash_user after the creation): functions the compiler
   * generates per key type, which feed the key at KEY to the hasher HS
   * and compare the keys at A and B (1 equal). Both run Avant code, which
   * may allocate and collect. NULL for every other hash. */
  void (*uhash)(const void *key, void *hs);
  int32_t (*ueq)(const void *a, const void *b);
  /* A struct key: its fields, each (offset << 8) | kind, kind the width
   * of an integer field or XK_STR; flat when they cover every byte and
   * none is a String; krefs when one is a String. */
  uint16_t nf;
  uint8_t flat;
  uint8_t krefs;
  uint32_t field[];
} AvantHashX;

#define XK_STR 0x80u

static inline AvantHashX *hx(const AvantHash *h) {
  return (AvantHashX *)h;
}

/* The process's seed (above): drawn once, at the first hash made, from
 * the system's entropy; AVANT_HASH_SEED (a decimal number) fixes it, for
 * a measurement that must repeat its probes exactly. Every hash takes
 * it at its creation, so a race between two threads' first hashes only
 * gives each its own seed. With it, the secret String keys are hashed
 * under (hash_bytes): four 64-bit words, drawn from the system's entropy
 * as well, or derived from the fixed seed (AVANT_HASH_SEED=0 gives the
 * same secret in every run). The secret is written before the seed is
 * marked set, so a hash that read the seed reads the secret it goes
 * with. */
static uint32_t g_hash_seed;
static int g_hash_seed_set;
static uint64_t g_hash_secret[4];

static uint64_t splitmix64(uint64_t *x) {
  uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

static __attribute__((noinline, cold)) uint32_t hash_seed_draw(void) {
  uint32_t v = 0;
  uint64_t k[4];
  int i;
  const char *fixed = getenv("AVANT_HASH_SEED");
  int drawn = 0;
  if (fixed && *fixed) {
    v = (uint32_t)strtoul(fixed, NULL, 10);
  } else if (getrandom(&v, sizeof v, GRND_NONBLOCK) == (ssize_t)sizeof v &&
             getrandom(k, sizeof k, GRND_NONBLOCK) == (ssize_t)sizeof k) {
    drawn = 1;
  } else {
    /* No entropy yet (early boot) or no getrandom: the clock, the
     * process id and an address, mixed. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t m = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 30) ^ ((uint64_t)getpid() << 16) ^ (uint64_t)(uintptr_t)&v;
    m ^= m >> 33;
    m *= 0xff51afd7ed558ccdull;
    m ^= m >> 33;
    v = (uint32_t)m;
    uint64_t x = m ^ ((uint64_t)ts.tv_nsec << 32);
    for (i = 0; i < 4; i++) {
      k[i] = splitmix64(&x);
    }
    drawn = 1;
  }
  if (!drawn) {
    /* The fixed seed: the secret is a function of it alone. */
    uint64_t x = (uint64_t)v ^ 0x6a09e667f3bcc909ull;
    for (i = 0; i < 4; i++) {
      k[i] = splitmix64(&x);
    }
  }
  for (i = 0; i < 4; i++) {
    __atomic_store_n(&g_hash_secret[i], k[i], __ATOMIC_RELAXED);
  }
  __atomic_store_n(&g_hash_seed, v, __ATOMIC_RELAXED);
  __atomic_store_n(&g_hash_seed_set, 1, __ATOMIC_RELEASE);
  return v;
}

static inline uint32_t hash_seed(void) {
  if (__builtin_expect(__atomic_load_n(&g_hash_seed_set, __ATOMIC_ACQUIRE), 1)) {
    return __atomic_load_n(&g_hash_seed, __ATOMIC_RELAXED);
  }
  return hash_seed_draw();
}

/* What a lookup of an absent key points at: zeros, read by the generated
 * code with the value's type and ignored, since the found flag is 0. A
 * value whose slot is wider than these is read by the generated code
 * only when it was found (compiler/mir/lower_hash.av hash_find_load). */
#define HASH_NONE_BYTES 256
static const uint64_t g_hash_none[HASH_NONE_BYTES / 8];
/* A nil hash's slot for a set or an upsert of a scalar value (a get then
 * reads 0, a set stores nothing; a struct value is set through the slot
 * entries, which refuse a nil hash), and the copy of a value a deletion
 * took (a wider one: a thread's own buffer). */
static __thread uint64_t g_hash_scratch[2];
static __thread uint64_t g_hash_taken[2];
static __thread void *g_hash_wide_taken;
static __thread size_t g_hash_wide_taken_n;

static inline unsigned kw_of(const AvantHash *h) {
  return h->kw;
}

static inline unsigned vw_of(const AvantHash *h) {
  return h->vw;
}

static int key_is_str(const AvantHash *h) {
  return (h->desc & AVANT_HASH_K_STR) != 0;
}

/* A struct key passed by address (an AvantHashX). */
static inline int key_is_struct(const AvantHash *h) {
  return (h->desc & AVANT_HASH_K_STRUCT) != 0;
}

/* A key whose hash is kept per entry in meta: a String or a struct. */
static inline int key_hashed(const AvantHash *h) {
  return (h->desc & (AVANT_HASH_K_STR | AVANT_HASH_K_STRUCT)) != 0;
}

/* The key buffer holds references: String keys, or struct keys with a
 * String field. */
static inline int key_refs(const AvantHash *h) {
  return key_is_str(h) || (key_is_struct(h) && hx(h)->krefs);
}

/* A key's size in its buffer. */
static inline size_t key_bytes(const AvantHash *h) {
  return key_is_struct(h) ? hx(h)->kw : kw_of(h);
}

static int val_is_ref(const AvantHash *h) {
  return (h->desc & AVANT_HASH_V_REF) != 0;
}

/* A hash made by avant_hash_create_typed (an AvantHashX). */
static inline int is_typed(const AvantHash *h) {
  return (h->desc & AVANT_HASH_TYPED) != 0;
}

/* A value's own size: a struct value's is the AvantHashX's (its slot is
 * that size rounded up to a power of two, 1 << vshift, the stride of the
 * value buffer's type map). */
static inline size_t val_bytes(const AvantHash *h) {
  return is_typed(h) ? hx(h)->vw : h->vw;
}

/* The value buffer holds references: one per value (V_REF), or the
 * reference words of struct values (V_WORDS). One test. */
static inline int val_refs(const AvantHash *h) {
  return (h->desc & (AVANT_HASH_V_REF | AVANT_HASH_V_WORDS)) != 0;
}

/* A String key's hash: a keyed multiply-mix, of the kind of wyhash,
 * rapidhash and foldhash. Its step is the folded multiply: the 128-bit
 * product of two 64-bit words, its halves xored, which spreads every
 * input bit over the result in one multiplication. A key of 16 bytes or
 * fewer is read as two words in a fixed number of loads: 4 to 16 bytes
 * by four 4-byte loads at both ends that overlap as needed, 1 to 3 bytes
 * by its first, middle and last byte; a longer key folds each 16 bytes
 * into the state, then its last 16 (which may overlap the ones before).
 * No load reads outside the key's bytes. The two words are xored with
 * secret words before they meet in the final fold, and the state starts
 * from a secret word with the hash's seed, rotated by the length (a
 * length change moves secret bits, which the key's bytes cannot undo).
 * The secret is the process's (hash_seed_draw): someone who knows the
 * function but not the secret cannot choose keys that collide, where
 * FNV-1a with the seed in its start value kept the low bits of every
 * key's hash a function of the low bits of its bytes alone, whatever
 * the seed. The table takes the 64-bit result folded to 32 bits.
 *
 * The key is read through RD8 and RD4, which load a word at an offset:
 * hash_bytes reads one buffer, hash_concat_bytes two pieces as one key,
 * and both give the same hash for the same bytes. */
static inline uint64_t hash_fold(uint64_t a, uint64_t b) {
  __uint128_t r = (__uint128_t)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

#define HASH_KEYED_BODY(RD8, RD4, RD1, n, seed, out) \
  do { \
    uint64_t k0_ = __atomic_load_n(&g_hash_secret[0], __ATOMIC_RELAXED); \
    uint64_t k1_ = __atomic_load_n(&g_hash_secret[1], __ATOMIC_RELAXED); \
    uint64_t st_ = k0_ ^ (uint64_t)(seed); \
    uint64_t a_, b_; \
    size_t n_ = (n); \
    unsigned r_ = (unsigned)(n_ & 63); \
    st_ = (st_ >> r_) | (st_ << ((64 - r_) & 63)); \
    if (__builtin_expect(n_ <= 16, 1)) { \
      if (n_ >= 4) { \
        size_t m_ = (n_ >> 3) << 2; \
        a_ = (RD4(0) << 32) | RD4(m_); \
        b_ = (RD4(n_ - 4) << 32) | RD4(n_ - 4 - m_); \
      } else if (n_ > 0) { \
        a_ = (RD1(0) << 16) | (RD1(n_ >> 1) << 8) | RD1(n_ - 1); \
        b_ = 0; \
      } else { \
        a_ = 0; \
        b_ = 0; \
      } \
    } else { \
      size_t o_ = 0; \
      while (n_ - o_ > 16) { \
        st_ = hash_fold(RD8(o_) ^ k1_, RD8(o_ + 8) ^ st_); \
        o_ += 16; \
      } \
      a_ = RD8(n_ - 16); \
      b_ = RD8(n_ - 8); \
    } \
    uint64_t h_ = hash_fold(a_ ^ k1_, b_ ^ st_); \
    (out) = (uint32_t)(h_ ^ (h_ >> 32)); \
  } while (0)

static inline uint64_t hb_rd8(const unsigned char *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static inline uint64_t hb_rd4(const unsigned char *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

/* Inlined at each lookup and store: as a call it cost Calculator::Interpreter,
 * CacheSimulation and LZWEncode 2% to 4.5% of their instructions. */
static inline __attribute__((always_inline)) uint32_t hash_bytes(const char *s, size_t n, uint32_t seed) {
  const unsigned char *p = (const unsigned char *)s;
  uint32_t out;
  if (!p) {
    n = 0;
  }
#define HB_RD8(o) hb_rd8(p + (o))
#define HB_RD4(o) hb_rd4(p + (o))
#define HB_RD1(o) ((uint64_t)p[o])
  HASH_KEYED_BODY(HB_RD8, HB_RD4, HB_RD1, n, seed, out);
#undef HB_RD8
#undef HB_RD4
#undef HB_RD1
  return out;
}

/* The hash a String key of the N bytes at BYTES takes in every Hash and
 * Set of this process: for tests (the hash's known answers under a fixed
 * seed, and its spread) and diagnostics. It takes the bytes and their
 * count, never a string object: a `lib` block gives it a String as C's
 * `char *`, which for a view that ends its base is the address of the
 * view's bytes inside the base, with no header of its own before it. */
avant_int avant_hash_str_hash(const char *bytes, avant_int n) {
  if (!bytes || n <= 0) {
    return (avant_int)hash_bytes(AVANT_STR_EMPTY, 0, hash_seed());
  }
  return (avant_int)hash_bytes(bytes, (size_t)n, hash_seed());
}

static uint32_t hash_i32(int32_t k) {
  uint32_t x = (uint32_t)k;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

/* An integer key: a key within 32 bits hashes its low word (its high word
 * is the sign extension, so hi is 0); the high word of a wider key is
 * folded into the low one first. */
static uint32_t hash_int(int64_t k, uint32_t seed) {
  int32_t lo = (int32_t)k;
  uint32_t hi = (uint32_t)(int32_t)(k >> 32) - (uint32_t)(lo >> 31);
  return hash_i32((int32_t)(((uint32_t)lo ^ (hi * 0x9e3779b1u)) ^ seed));
}

static int bytes_eq(const char *a, size_t na, const char *b, size_t nb) {
  if (!a) {
    a = AVANT_STR_EMPTY;
    na = 0;
  }
  if (!b) {
    b = AVANT_STR_EMPTY;
    nb = 0;
  }
  return na == nb && memcmp(a, b, na) == 0;
}

/* An integer key at entry i, extended to 64 bits by its signedness. */
static inline __attribute__((always_inline)) int64_t ikey_at(const AvantHash *h, const void *keys, int32_t i) {
  int sg = (h->desc & AVANT_HASH_K_SIGNED) != 0;
  switch (kw_of(h)) {
  case 1:
    return sg ? (int64_t)((const int8_t *)keys)[i] : (int64_t)((const uint8_t *)keys)[i];
  case 2:
    return sg ? (int64_t)((const int16_t *)keys)[i] : (int64_t)((const uint16_t *)keys)[i];
  case 4:
    return sg ? (int64_t)((const int32_t *)keys)[i] : (int64_t)((const uint32_t *)keys)[i];
  default:
    return ((const int64_t *)keys)[i];
  }
}

static inline __attribute__((always_inline)) void ikey_put(const AvantHash *h, void *keys, int32_t i, int64_t k) {
  switch (kw_of(h)) {
  case 1:
    ((uint8_t *)keys)[i] = (uint8_t)k;
    break;
  case 2:
    ((uint16_t *)keys)[i] = (uint16_t)k;
    break;
  case 4:
    ((uint32_t *)keys)[i] = (uint32_t)k;
    break;
  default:
    ((int64_t *)keys)[i] = k;
    break;
  }
}

/* One key or value of width W (1, 2, 4 or 8) copied or zeroed: a switch,
 * so no call to memcpy for a few bytes. */
static inline __attribute__((always_inline)) void cp_w(void *d, const void *s, unsigned w) {
  switch (w) {
  case 1:
    *(uint8_t *)d = *(const uint8_t *)s;
    break;
  case 2:
    *(uint16_t *)d = *(const uint16_t *)s;
    break;
  case 4:
    *(uint32_t *)d = *(const uint32_t *)s;
    break;
  default:
    *(uint64_t *)d = *(const uint64_t *)s;
    break;
  }
}

/* W bytes of any width: cp_w's switch for 1, 2, 4 and 8, memcpy else. */
static inline void cp_n(void *d, const void *s, size_t w) {
  if (w == 1 || w == 2 || w == 4 || w == 8) {
    cp_w(d, s, (unsigned)w);
  } else {
    memcpy(d, s, w);
  }
}

static inline __attribute__((always_inline)) void zero_w(void *d, unsigned w) {
  switch (w) {
  case 1:
    *(uint8_t *)d = 0;
    break;
  case 2:
    *(uint16_t *)d = 0;
    break;
  case 4:
    *(uint32_t *)d = 0;
    break;
  default:
    *(uint64_t *)d = 0;
    break;
  }
}

static const char *skey_at(const void *keys, int32_t i) {
  return ((const char *const *)keys)[i];
}

static inline void *val_at(const AvantHash *h, void *vals, int32_t i) {
  return (char *)vals + ((size_t)i << h->vshift);
}

/* One value's slot copied: a width of 1, 2, 4 or 8 (h->vw), or a struct
 * value's slot over 8 bytes (h->vw is 0) by memcpy. */
static inline __attribute__((always_inline)) void cp_val(const AvantHash *h, void *d, const void *s) {
  switch (h->vw) {
  case 1:
    *(uint8_t *)d = *(const uint8_t *)s;
    break;
  case 2:
    *(uint16_t *)d = *(const uint16_t *)s;
    break;
  case 4:
    *(uint32_t *)d = *(const uint32_t *)s;
    break;
  case 8:
    *(uint64_t *)d = *(const uint64_t *)s;
    break;
  default:
    memcpy(d, s, (size_t)1 << h->vshift);
    break;
  }
}

/* A new entry's value zeroed: zero_w of its width (a struct value of a
 * slot over 8 bytes has h->vw 0, so zero_w clears its first 8 bytes; the
 * generated code stores the whole value at once, before anything can
 * collect, and a buffer of struct values holding references never holds
 * one a deletion left behind: erase clears it). */
static inline __attribute__((always_inline)) void zero_val(const AvantHash *h, void *d) {
  zero_w(d, h->vw);
}

/* A thread's buffer of at least N bytes, kept in *P (of size *PN) and
 * grown by malloc. */
static void *thread_buf(void **p, size_t *pn, size_t n) {
  if (*pn < n) {
    void *q = malloc(n);
    if (!q) {
      avant_trap_text("out of memory for a Hash value");
    }
    free(*p);
    *p = q;
    *pn = n;
  }
  return *p;
}

/* Bit 0x80 of an entry's state: the key is a heap string whose length is
 * the header size. A probe then reads that length. avant_str_n confirms
 * an unknown pointer with a block walk, and that walk was the cache. The
 * bit is set only when the header length already agreed with
 * avant_str_n, so a C string keeps the slow check. */
#define HASH_HEAP_LEN 0x80u

static int st_full(uint8_t st) {
  return (st & 0x7fu) == 1;
}

static uint8_t occ_mark(const char *key, size_t n) {
  if (key && ((uintptr_t)key & 7u) == 0) {
    AvantHeader *hd = avant_header((void *)key);
    if ((hd->flags & AVANT_FLAG_HEAP) && hd->type_id == AVANT_TYPE_BYTES && hd->size > 0 &&
        (size_t)hd->size - 1 == n) {
      return (uint8_t)(1u | HASH_HEAP_LEN);
    }
  }
  return 1;
}

static size_t skey_n(const char *s, uint8_t st) {
  if (st & HASH_HEAP_LEN) {
    return (size_t)avant_header((void *)s)->size - 1;
  }
  return avant_str_n(s);
}

/* Whether the String key SLOT (a table's: flat, a heap string or a
 * literal, whose header size is its bytes and terminator; or null, the
 * empty key) is the N bytes at S. A compact probe calls it on a hash that
 * agrees: it reads the key's header and bytes, one cache line for a
 * short key, and not the entry's state (the header is the length for
 * every flat key). A key of 16 bytes or fewer is compared in two loads
 * of each side, overlapping, as the hash reads it (a longer one by
 * memcmp: a word loop up to 64 bytes cost LZWEncode-Clever 1.6% and
 * Template::Parse 0.6% more instructions than libc's vector compare). */
static inline __attribute__((always_inline)) int skey_eq(const char *slot, const char *s, size_t n) {
  size_t m;
  if (!slot) {
    return n == 0;
  }
  m = (size_t)avant_header((void *)slot)->size - 1;
  if (m != n) {
    return 0;
  }
  const unsigned char *a = (const unsigned char *)slot;
  const unsigned char *b = (const unsigned char *)s;
  if (n <= 16) {
    if (n >= 8) {
      return ((hb_rd8(a) ^ hb_rd8(b)) | (hb_rd8(a + n - 8) ^ hb_rd8(b + n - 8))) == 0;
    }
    if (n >= 4) {
      return ((hb_rd4(a) ^ hb_rd4(b)) | (hb_rd4(a + n - 4) ^ hb_rd4(b + n - 4))) == 0;
    }
    if (n == 0) {
      return 1;
    }
    return a[0] == b[0] && a[n >> 1] == b[n >> 1] && a[n - 1] == b[n - 1];
  }
  return memcmp(a, b, n) == 0;
}

/* ---- the two layouts: positions, entries, growth, deletion ---- */

/* The layout a hash takes, by its key: the compact one for String and
 * struct keys, the open one for integer keys. Both keep the entries in
 * the order of their insertion, so every walk is in that order. On the
 * benchmark programs the compact table read String keys faster (its
 * dense entries are read in the order they were inserted) and used less
 * memory; for integer keys it cost 10% to 12% of the time of the two
 * integer-keyed programs (the index is one more dependent load before a
 * key that compares in one instruction), and the open table with an
 * order beside it costs nothing a lookup sees. Every function below
 * serves both layouts, so the choice is the flag avant_hash_create sets
 * from the key. */
static inline int is_compact(const AvantHash *h) {
  return h->compact;
}

/* The most entries a table of CAP positions takes before it grows (or
 * compacts): an entry deleted since the last growth still counts. */
static int32_t fill_max(int32_t cap) {
  return cap - cap / 4;
}

/* Compact meta: uint64 index[cap], uint8 state[fill_max(cap)], then (String
 * and struct keys) uint32 hash[fill_max(cap)] at the next 4-byte boundary.
 * An index position holds its entry's number plus one in its low word (0:
 * empty) and the entry's hash in its high word: a probe compares the hash
 * in the position it loaded, and reads an entry's key only when the hash
 * agrees (with the hash in the entries alone, each probe read the index
 * and then the entry's hash, two cache lines apart in a large table:
 * Calculator::Interpreter, LZWEncode and CacheSimulation spent a fifth of
 * their time in those loads).
 * Open meta: uint8 state[cap] (0 empty, 1 live, 2 deleted: a deleted
 * position is not taken again until the next growth, so no entry moves
 * while the table lives), then uint32 order[fill_max(cap)] at the next
 * 4-byte boundary: the positions in the order of insertion. */
static inline size_t meta_hash_off(const AvantHash *h, int32_t cap) {
  if (!is_compact(h)) {
    return ((size_t)cap + 3) & ~(size_t)3;
  }
  size_t off = (size_t)cap * 8 + (size_t)fill_max(cap);
  return (off + 3) & ~(size_t)3;
}

static uint64_t meta_bytes(const AvantHash *h, int32_t cap) {
  if (!cap) {
    return 0;
  }
  if (!is_compact(h)) {
    return (uint64_t)meta_hash_off(h, cap) + (uint64_t)fill_max(cap) * 4;
  }
  if (key_hashed(h)) {
    return (uint64_t)meta_hash_off(h, cap) + (uint64_t)fill_max(cap) * 4;
  }
  return (uint64_t)cap * 8 + (uint64_t)fill_max(cap);
}

/* The entries a table of CAP positions has room for. */
static int32_t entry_cap(const AvantHash *h, int32_t cap) {
  return is_compact(h) ? fill_max(cap) : cap;
}

static inline uint64_t *index_of(const AvantHash *h) {
  return (uint64_t *)h->meta;
}

/* A compact index position for entry E of hash HV. */
static inline uint64_t ix_word(int32_t e, uint32_t hv) {
  return ((uint64_t)hv << 32) | ((uint32_t)e + 1u);
}

/* The entry a compact index word names, or -1. */
static inline int32_t ix_entry(uint64_t w) {
  return (int32_t)(uint32_t)w - 1;
}

static inline uint8_t *states_of(const AvantHash *h) {
  if (is_compact(h)) {
    return (uint8_t *)h->meta + (size_t)h->cap * 8;
  }
  return (uint8_t *)h->meta;
}

static inline uint32_t *hashes_of(const AvantHash *h) {
  return (uint32_t *)((char *)h->meta + meta_hash_off(h, h->cap));
}

/* The open layout's positions in the order of insertion. */
static inline uint32_t *order_of(const AvantHash *h) {
  return (uint32_t *)((char *)h->meta + meta_hash_off(h, h->cap));
}

static inline uint8_t ent_state(const AvantHash *h, int32_t e) {
  return states_of(h)[e];
}

/* The entry a position names, or -1: the index's (compact), or the
 * position's own when it is live (open). */
static inline int32_t pos_entry(const AvantHash *h, int32_t i) {
  if (is_compact(h)) {
    return ix_entry(index_of(h)[i]);
  }
  return st_full(states_of(h)[i]) ? i : -1;
}

static inline uint32_t ent_hash(const AvantHash *h, int32_t e) {
  if (key_hashed(h)) {
    return hashes_of(h)[e];
  }
  return hash_int(ikey_at(h, h->keys, e), h->seed);
}

/* The number of entries a walk visits in order, deleted ones included
 * (the entries made since the last growth). */
static int32_t walk_end(const AvantHash *h) {
  return h->used;
}

/* The entry (an index into keys, vals and the states) of the N-th in
 * the order of insertion: N itself (compact), or its position (open). */
static inline int32_t walk_slot(const AvantHash *h, int32_t n) {
  if (is_compact(h)) {
    return n;
  }
  return (int32_t)order_of(h)[n];
}

static int ent_live(const AvantHash *h, int32_t e) {
  return st_full(ent_state(h, e));
}

/* A lookup. Found: 1 and *ent the entry. Absent: 0 and *pos the position
 * an insertion takes (meaningless when the table has no positions). An
 * integer key is compared at its stored width (the key came extended from
 * a value of that width, so its truncation is that value). The layout and
 * the width are chosen once, outside the probe loop: one loop per pair,
 * whose position test (POS, below) and key compare are a load and a
 * compare each. */
#define POS_OPEN(i) (st[i] == 0 ? -1 : (st_full(st[i]) ? (i) : -2))
#define POS_COMPACT(i) ix_entry(ix[i])

#define FIND_INT_AT(T, POS)                                                         \
  do {                                                                            \
    const T *ks = (const T *)h->keys;                                             \
    T want = (T)key;                                                              \
    for (n = 0; n < cap; n++) {                                                   \
      int32_t e = POS(i);                                                         \
      if (e < 0) {                                                                \
        *pos = i;                                                                 \
        return 0;                                                                 \
      }                                                                           \
      if (ks[e] == want) {                                                        \
        *ent = e;                                                                 \
        return 1;                                                                 \
      }                                                                           \
      i = (i + 1) & mask;                                                         \
    }                                                                             \
  } while (0)

/* The open layout's probe: a live position whose key is the one wanted
 * is found, an empty one ends it, a deleted one is passed over. */
#define FIND_INT_OPEN_AT(T)                                                         \
  do {                                                                            \
    const T *ks = (const T *)h->keys;                                             \
    T want = (T)key;                                                              \
    for (n = 0; n < cap; n++) {                                                   \
      uint8_t s = st[i];                                                          \
      if (s == 1 && ks[i] == want) {                                              \
        *ent = i;                                                                 \
        return 1;                                                                 \
      }                                                                           \
      if (s == 0) {                                                               \
        *pos = i;                                                                 \
        return 0;                                                                 \
      }                                                                           \
      i = (i + 1) & mask;                                                         \
    }                                                                             \
  } while (0)

#define FIND_INT_WIDTHS(POS)                                                        \
  do {                                                                            \
    switch (kw) {                                                                 \
    case 1:                                                                       \
      FIND_INT_AT(uint8_t, POS);                                                  \
      break;                                                                      \
    case 2:                                                                       \
      FIND_INT_AT(uint16_t, POS);                                                 \
      break;                                                                      \
    case 4:                                                                       \
      FIND_INT_AT(uint32_t, POS);                                                 \
      break;                                                                      \
    default:                                                                      \
      FIND_INT_AT(uint64_t, POS);                                                 \
      break;                                                                      \
    }                                                                             \
  } while (0)

static int find_int(const AvantHash *h, int64_t key, uint32_t hv, int32_t *ent, int32_t *pos) {
  int32_t cap = h->cap;
  if (!cap) {
    *pos = 0;
    return 0;
  }
  int32_t mask = cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  int32_t n;
  unsigned kw = kw_of(h);
  if (is_compact(h)) {
    const uint64_t *ix = index_of(h);
    FIND_INT_WIDTHS(POS_COMPACT);
  } else {
    const uint8_t *st = states_of(h);
    switch (kw) {
    case 1:
      FIND_INT_OPEN_AT(uint8_t);
      break;
    case 2:
      FIND_INT_OPEN_AT(uint16_t);
      break;
    case 4:
      FIND_INT_OPEN_AT(uint32_t);
      break;
    default:
      FIND_INT_OPEN_AT(uint64_t);
      break;
    }
  }
  *pos = 0;
  return 0;
}

/* A String key's probe of the open layout: a deleted position (POS gives
 * -2) is passed over. */
#define FIND_BYTES_SKIP_AT(POS)                                                          \
  do {                                                                            \
    for (k = 0; k < cap; k++) {                                                   \
      int32_t e = POS(i);                                                         \
      if (e < 0) {                                                                \
        if (e == -1) {                                                            \
          *pos = i;                                                               \
          return 0;                                                               \
        }                                                                         \
      } else if (hs[e] == hv) {                                                   \
        const char *slot = skey_at(keys, e);                                      \
        if (bytes_eq(slot, skey_n(slot, es[e]), s, n)) {                          \
          *ent = e;                                                               \
          return 1;                                                               \
        }                                                                         \
      }                                                                           \
      i = (i + 1) & mask;                                                         \
    }                                                                             \
  } while (0)

static int find_bytes(const AvantHash *h, const char *s, size_t n, uint32_t hv, int32_t *ent, int32_t *pos) {
  int32_t cap = h->cap;
  if (!cap) {
    *pos = 0;
    return 0;
  }
  int32_t mask = cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  int32_t k;
  const uint32_t *hs = hashes_of(h);
  const uint8_t *es = states_of(h);
  const void *keys = h->keys;
  if (is_compact(h)) {
    const uint64_t *ix = index_of(h);
    for (k = 0; k < cap; k++) {
      uint64_t w = ix[i];
      if (!w) {
        *pos = i;
        return 0;
      }
      if ((uint32_t)(w >> 32) == hv) {
        int32_t e = ix_entry(w);
        if (skey_eq(skey_at(keys, e), s, n)) {
          *ent = e;
          return 1;
        }
      }
      i = (i + 1) & mask;
    }
  } else {
    const uint8_t *st = es;
    FIND_BYTES_SKIP_AT(POS_OPEN);
  }
  *pos = 0;
  return 0;
}

/* A new entry at position POS (from a failed lookup after ensure_cap),
 * of hash HV (a compact table's index keeps it): its number, state ST
 * set, the size counted. Its key, hash and value
 * are the caller's to store (the value is zero). */
static int32_t claim(AvantHash *h, int32_t pos, uint8_t st, uint32_t hv) {
  int32_t e = pos;
  if (is_compact(h)) {
    e = h->used;
    index_of(h)[pos] = ix_word(e, hv);
  } else {
    order_of(h)[h->used] = (uint32_t)pos;
  }
  h->used += 1;
  states_of(h)[e] = st;
  h->size += 1;
  /* A growth claims into a copy of the hash whose count is not written
   * back, so only a new key counts. */
  h->changes += 1;
  return e;
}

static void store_str_key(AvantHash *h, int32_t e, const char *key, uint32_t hv) {
  /* A mature key buffer is not walked from the roots: the slot about to
   * be stored may name a young string. The barrier before the store
   * (RC5). */
  avant_barrier_slot_young(h->keys, &((void **)h->keys)[e]);
  ((const char **)h->keys)[e] = key;
  hashes_of(h)[e] = hv;
}

static uint32_t key_type_id(const AvantHash *h) {
  if (key_is_struct(h)) {
    return hx(h)->ktid;
  }
  return key_is_str(h) ? AVANT_TYPE_PTRS : AVANT_TYPE_BYTES;
}

/* A struct key's String fields in the entry SLOT of the buffer KEYS get
 * the slot barrier before the entry is stored (RC5). */
static void xkey_barrier(const AvantHash *h, void *keys, char *slot) {
  const AvantHashX *x = hx(h);
  uint32_t i;
  for (i = 0; i < x->nf; i++) {
    if ((x->field[i] & 0xffu) == XK_STR) {
      avant_barrier_slot_young(keys, slot + (x->field[i] >> 8));
    }
  }
}

static uint32_t val_type_id(const AvantHash *h) {
  if (is_typed(h)) {
    return hx(h)->vtid;
  }
  return val_is_ref(h) ? AVANT_TYPE_PTRS : AVANT_TYPE_BYTES;
}

/* Moves the live entry E of the old buffers into the table DST (fresh,
 * no collection between): a position by its hash, the key, the hash, the
 * state and the value copied. */
static void move_entry(AvantHash *dst, const AvantHash *src, void *okeys, void *ovals, uint8_t st, uint32_t hv,
                       int32_t e) {
  int32_t mask = dst->cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  while (pos_entry(dst, i) >= 0) {
    i = (i + 1) & mask;
  }
  int32_t ne = claim(dst, i, st, hv);
  if (key_is_str(src)) {
    avant_barrier_slot_young(dst->keys, &((void **)dst->keys)[ne]);
    ((void **)dst->keys)[ne] = ((void **)okeys)[e];
    hashes_of(dst)[ne] = hv;
  } else if (key_is_struct(src)) {
    size_t kw = hx(src)->kw;
    char *slot = (char *)dst->keys + (size_t)ne * kw;
    if (hx(src)->krefs) {
      xkey_barrier(src, dst->keys, slot);
    }
    memcpy(slot, (char *)okeys + (size_t)e * kw, kw);
    hashes_of(dst)[ne] = hv;
  } else {
    ikey_put(src, dst->keys, ne, ikey_at(src, okeys, e));
  }
  if (val_is_ref(src)) {
    avant_barrier_slot_young(dst->vals, (void **)val_at(src, dst->vals, ne));
  }
  /* A struct value's references: grow barriered the whole buffer. */
  cp_val(src, val_at(src, dst->vals, ne), val_at(src, ovals, e));
}

/* Each allocation may collect (RC1, avant_gc.h), so the hash and the new
 * buffers not yet published are handles, and the old buffers are read
 * from the hash after the last allocation (RC2). A reference copied into
 * a new buffer takes the slot's barrier (a large buffer is not young);
 * the hash takes one before it names them. */
/* A compact table's compaction where it is: the live entries slide down
 * over the deleted ones in their order, the freed tail is cleared, and
 * the index is filled again from the stored hashes. Nothing is
 * allocated, so a table that deletes as much as it inserts (a cache
 * that drops its oldest key for each new one) keeps one set of buffers.
 * A moved reference stays in its buffer: each buffer that holds
 * references takes one barrier for all of them (RC3), and a proof's
 * log is told first (RC5).
 * gc-lint: holds nothing (no allocation). */
static void compact_in_place(AvantHash *h) {
  int32_t end = h->used;
  int32_t w = 0;
  int32_t e;
  size_t kb = key_bytes(h);
  size_t vb = (size_t)1 << h->vshift;
  uint8_t *st = states_of(h);
  uint32_t *hs = hashes_of(h);
  int kr = key_refs(h);
  int vr = val_refs(h);
  if (kr) {
    avant_gc_overwrite(h->keys);
    avant_barrier(h->keys);
  }
  if (vr) {
    avant_gc_overwrite(h->vals);
    avant_barrier(h->vals);
  }
  for (e = 0; e < end; e++) {
    if (!st_full(st[e])) {
      continue;
    }
    if (w != e) {
      memcpy((char *)h->keys + (size_t)w * kb, (char *)h->keys + (size_t)e * kb, kb);
      memcpy(val_at(h, h->vals, w), val_at(h, h->vals, e), vb);
      st[w] = st[e];
      hs[w] = hs[e];
    }
    w++;
  }
  if (w < end) {
    if (kr) {
      memset((char *)h->keys + (size_t)w * kb, 0, (size_t)(end - w) * kb);
    }
    if (vr) {
      memset(val_at(h, h->vals, w), 0, (size_t)(end - w) * vb);
    }
    memset(st + w, 0, (size_t)(end - w));
  }
  uint64_t *ix = index_of(h);
  int32_t mask = h->cap - 1;
  memset(ix, 0, (size_t)h->cap * 8);
  for (e = 0; e < w; e++) {
    int32_t i = (int32_t)(hs[e] & (uint32_t)mask);
    while (ix[i] != 0) {
      i = (i + 1) & mask;
    }
    ix[i] = ix_word(e, hs[e]);
  }
  h->used = w;
}

/* An open table's compaction where it is (no new buffer): the deleted
 * positions become empty and each live entry is placed again from its
 * home, as absl's in-place rehash does for open addressing: the live
 * entries are first marked to place (state 3); each one marked is
 * placed at the first position of its probe path that is empty or still
 * to place (moved there, or swapped with the one there, which is then
 * placed in turn), or kept where it is when its path reaches it first.
 * A placed entry never moves again, so every position before it on its
 * path is full: lookups find it. The order of insertion is kept through
 * an inverse map of positions to order numbers (malloc'd, freed at the
 * end), the order array compacted first. Integer keys hold no
 * references; values that do take one barrier for the buffer (RC3) and
 * a proof's log is told first (RC5).
 * gc-lint: holds nothing (no allocation of the collector). */
static int open_compact_in_place(AvantHash *h) {
  int32_t cap = h->cap;
  int32_t mask = cap - 1;
  uint8_t *st = states_of(h);
  uint32_t *ord = order_of(h);
  int32_t *inv = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
  size_t kb = kw_of(h);
  size_t vb = (size_t)1 << h->vshift;
  uint64_t ktmp[2];
  unsigned char vstack[64];
  void *vtmp = vb <= sizeof(vstack) ? (void *)vstack : malloc(vb);
  int32_t n;
  int32_t w = 0;
  int32_t i;
  if (!inv || !vtmp) {
    free(inv);
    if (vtmp != (void *)vstack) {
      free(vtmp);
    }
    return 0;
  }
  if (val_refs(h)) {
    avant_gc_overwrite(h->vals);
    avant_barrier(h->vals);
  }
  for (n = 0; n < h->used; n++) {
    int32_t x = (int32_t)ord[n];
    if (st[x] == 1) {
      ord[w] = (uint32_t)x;
      inv[x] = w;
      w++;
    }
  }
  for (i = 0; i < cap; i++) {
    st[i] = st[i] == 1 ? 3 : 0;
  }
  for (i = 0; i < cap; i++) {
    while (st[i] == 3) {
      int32_t p = (int32_t)(hash_int(ikey_at(h, h->keys, i), h->seed) & (uint32_t)mask);
      while (p != i && st[p] == 1) {
        p = (p + 1) & mask;
      }
      if (p == i) {
        st[i] = 1;
        break;
      }
      int32_t r = inv[i];
      if (st[p] == 0) {
        memcpy((char *)h->keys + (size_t)p * kb, (char *)h->keys + (size_t)i * kb, kb);
        memcpy(val_at(h, h->vals, p), val_at(h, h->vals, i), vb);
        memset(val_at(h, h->vals, i), 0, vb);
        st[p] = 1;
        st[i] = 0;
        inv[p] = r;
        ord[r] = (uint32_t)p;
        break;
      }
      /* p holds an entry still to place: the two swap, the one now at p
       * is placed, the one now at i is placed next. */
      int32_t r2 = inv[p];
      memcpy(ktmp, (char *)h->keys + (size_t)p * kb, kb);
      memcpy((char *)h->keys + (size_t)p * kb, (char *)h->keys + (size_t)i * kb, kb);
      memcpy((char *)h->keys + (size_t)i * kb, ktmp, kb);
      memcpy(vtmp, val_at(h, h->vals, p), vb);
      memcpy(val_at(h, h->vals, p), val_at(h, h->vals, i), vb);
      memcpy(val_at(h, h->vals, i), vtmp, vb);
      st[p] = 1;
      inv[p] = r;
      ord[r] = (uint32_t)p;
      inv[i] = r2;
      ord[r2] = (uint32_t)i;
    }
  }
  h->used = w;
  free(inv);
  if (vtmp != (void *)vstack) {
    free(vtmp);
  }
  return 1;
}

static void grow_to(AvantHash *h, int32_t ncap);

/* gc-lint: holds nothing (the growth is its last statement; grow_to roots the hash). */
static void grow(AvantHash *h) {
  int32_t ncap = h->cap ? h->cap * 2 : 8;
  /* A table whose live entries are at most three quarters of the entries
   * it takes (a quarter or more deleted): the live ones are compacted at
   * the same size, a compact table's where it is. A table that deletes
   * as much as it inserts so stays at its size (a deleted position is
   * not taken again before a growth, so the threshold decides its
   * memory), and each compaction is followed by at least a quarter of
   * the table's entries made anew, so its cost stays a constant per
   * insertion. */
  if (h->cap && h->size <= fill_max(h->cap) - fill_max(h->cap) / 4) {
    if (is_compact(h)) {
      compact_in_place(h);
      return;
    }
    if (open_compact_in_place(h)) {
      return;
    }
    ncap = h->cap;
  }
  grow_to(h, ncap);
}

/* The table rebuilt at NCAP positions (a power of two), every live entry
 * moved into it. */
static void grow_to(AvantHash *h, int32_t ncap) {
  /* The slot ceiling, before anything is allocated: cap is a power of
   * two, so the next one would not fit. */
  if (ncap > AVANT_HASH_SLOTS_MAX || ncap <= 0) {
    avant_trap_size(AVANT_SIZE_HASH);
  }
  void *nkeys = NULL;
  void *nmeta = NULL;
  void *nvals = NULL;
  int32_t necap = entry_cap(h, ncap);
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &nkeys);
  AVANT_GC_ROOT(2, &nmeta);
  nkeys = avant_alloc((uint64_t)necap * key_bytes(h), key_type_id(h));
  nmeta = avant_alloc(meta_bytes(h, ncap), AVANT_TYPE_BYTES);
  nvals = avant_alloc((uint64_t)necap << h->vshift, val_type_id(h));

  void *okeys = h->keys;
  void *ovals = h->vals;
  void *ometa = h->meta;
  AvantHash dst = *h;
  dst.keys = nkeys;
  dst.vals = nvals;
  dst.meta = nmeta;
  dst.cap = ncap;
  dst.size = 0;
  dst.used = 0;

  int32_t end = walk_end(h);
  int32_t e;
  /* Struct values with references are copied in bulk: the new buffer
   * takes one barrier for all of them (RC3; a large buffer is not
   * young). */
  if (h->desc & AVANT_HASH_V_WORDS) {
    avant_barrier(nvals);
  }
  for (e = 0; e < end; e++) {
    int32_t x = walk_slot(h, e);
    uint8_t st = ent_state(h, x);
    if (!st_full(st)) {
      continue;
    }
    move_entry(&dst, h, okeys, ovals, st, ent_hash(h, x), x);
  }

  avant_barrier(h);
  h->keys = nkeys;
  h->vals = nvals;
  h->meta = nmeta;
  h->cap = ncap;
  h->size = dst.size;
  h->used = dst.used;
  /* The hash no longer names its old buffers: large ones go back now
   * unless a root still names them or they are mature with references. */
  if (okeys) {
    (void)avant_gc_large_drop(okeys, 2);
  }
  if (ovals) {
    (void)avant_gc_large_drop(ovals, 2);
  }
  if (ometa) {
    (void)avant_gc_large_drop(ometa, 2);
  }
}

/* `Hash(K, V).new(n)`: the new hash's table made at once with room for N
 * entries (the least power of two of at least 8 positions whose fill
 * limit takes them), as Rust's HashMap::with_capacity and Go's make(map,
 * n): a hash filled to a size known ahead grows no more until it passes
 * it. Nothing for N up to what the table already takes. The ceiling traps
 * as a growth's does. gc-lint: holds nothing (the growth is its last
 * statement; grow_to roots the hash). */
void avant_hash_reserve(void *hash, avant_int n) {
  AvantHash *h = (AvantHash *)hash;
  int32_t ncap = 8;
  if (!h || n <= 0 || (h->cap && n <= fill_max(h->cap))) {
    return;
  }
  while (fill_max(ncap) < n) {
    if (ncap >= AVANT_HASH_SLOTS_MAX) {
      avant_trap_size(AVANT_SIZE_HASH);
    }
    ncap *= 2;
  }
  grow_to(h, ncap);
}

static int needs_grow(const AvantHash *h);

/* Room for one more entry. gc-lint: holds nothing (grow is its last
 * statement and roots the hash itself). */
static void ensure_cap(AvantHash *h) {
  if (needs_grow(h)) {
    grow(h);
  }
}

static void erase_compact(AvantHash *h, int32_t e, int32_t mask, int str);
static void erase_open(AvantHash *h, int32_t e, int32_t mask, int str);

/* Removes the entry E a lookup found: no other entry moves, so a walk in
 * place goes on over the rest. */
static void erase(AvantHash *h, int32_t e) {
  int32_t mask = h->cap - 1;
  int str = key_is_str(h);
  /* The entry's references are cleared and, in the open layout, the
   * entries after it move back in the key and value buffers (RC5). One
   * test for both buffers while nothing is logged. */
  if (__builtin_expect(avant_gc_proof_logging, 0)) {
    if (key_refs(h)) {
      avant_gc_overwrite(h->keys);
    }
    if (val_refs(h)) {
      avant_gc_overwrite(h->vals);
    }
  }
  h->size -= 1;
  h->changes += HASH_DELETED;
  if (is_compact(h)) {
    erase_compact(h, e, mask, str);
    return;
  }
  erase_open(h, e, mask, str);
}

/* Compact: the entry is marked deleted (its references cleared) and its
 * index position's cluster slides back.
 * gc-lint: a store helper (erase makes the call on the two buffers first, RC5). */
static void erase_compact(AvantHash *h, int32_t e, int32_t mask, int str) {
  int32_t i = (int32_t)(ent_hash(h, e) & (uint32_t)mask);
  uint64_t *ix = index_of(h);
  while (ix_entry(ix[i]) != e) {
    i = (i + 1) & mask;
  }
  states_of(h)[e] = 0;
  if (str) {
    ((void **)h->keys)[e] = NULL;
  } else if (key_is_struct(h) && hx(h)->krefs) {
    memset((char *)h->keys + (size_t)e * hx(h)->kw, 0, hx(h)->kw);
  }
  if (val_refs(h)) {
    if (val_is_ref(h)) {
      ((void **)h->vals)[e] = NULL;
    } else {
      memset(val_at(h, h->vals, e), 0, (size_t)1 << h->vshift);
    }
  }
  for (;;) {
    int32_t k;
    ix[i] = 0;
    k = (i + 1) & mask;
    for (;;) {
      if (!ix[k]) {
        return;
      }
      int32_t home = (int32_t)((uint32_t)(ix[k] >> 32) & (uint32_t)mask);
      int move = (i <= k) ? (home <= i || home > k) : (home <= i && home > k);
      if (move) {
        ix[i] = ix[k];
        i = k;
        break;
      }
      k = (k + 1) & mask;
    }
  }
}

/* Open: the position is marked deleted and its references cleared; no
 * entry moves (a later lookup passes over it, an insertion does not take
 * it, the next growth drops it).
 * gc-lint: a store helper (erase makes the call on the two buffers first, RC5). */
static void erase_open(AvantHash *h, int32_t e, int32_t mask, int str) {
  (void)mask;
  states_of(h)[e] = 2;
  if (str) {
    ((void **)h->keys)[e] = NULL;
  }
  if (val_refs(h)) {
    if (val_is_ref(h)) {
      ((void **)h->vals)[e] = NULL;
    } else {
      memset(val_at(h, h->vals, e), 0, (size_t)1 << h->vshift);
    }
  }
}

/* ---- the entries ---- */

/* gc-lint: holds nothing (its input is an integer). */
void *avant_hash_create(uint32_t desc) {
  AvantHash *h = (AvantHash *)avant_alloc(sizeof(AvantHash), AVANT_TYPE_HASH);
  h->desc = desc;
  h->seed = hash_seed();
  h->kw = (uint8_t)(desc & AVANT_HASH_K_WIDTH);
  h->vw = (uint8_t)((desc >> AVANT_HASH_V_SHIFT) & AVANT_HASH_K_WIDTH);
  h->vshift = (uint8_t)__builtin_ctz(h->vw);
  h->compact = (desc & (AVANT_HASH_K_STR | AVANT_HASH_K_STRUCT)) != 0;
  return h;
}

/* A struct key's field list from its text (the header above): the
 * number of fields, and each written to FIELD when it is not NULL. */
static uint32_t xk_parse(const char *t, uint32_t *field) {
  uint32_t n = 0;
  while (t && *t) {
    uint32_t kind = *t == 's' ? XK_STR : (uint32_t)(*t - '0');
    uint32_t off = 0;
    t++;
    if (*t == '@') {
      t++;
    }
    while (*t >= '0' && *t <= '9') {
      off = off * 10 + (uint32_t)(*t - '0');
      t++;
    }
    if (field) {
      field[n] = (off << 8) | kind;
    }
    n++;
    if (*t == ',') {
      t++;
    }
  }
  return n;
}

/* A hash with a struct key (desc has AVANT_HASH_K_STRUCT, klayout its
 * fields) or a struct value: the key's and the value's sizes and the
 * buffers' type ids beside the descriptor (whose value width is then not
 * read).
 * gc-lint: holds nothing (klayout is a constant of the program, read
 * before the allocation). */
void *avant_hash_create_typed(uint32_t desc, const char *klayout, uint32_t kw, uint32_t vw, uint32_t ktid,
                              uint32_t vtid, uint32_t atid) {
  uint32_t nf = xk_parse(klayout, NULL);
  uint32_t field[nf ? nf : 1];
  uint32_t i;
  uint32_t covered = 0;
  int flat = 1;
  int krefs = 0;
  (void)xk_parse(klayout, field);
  for (i = 0; i < nf; i++) {
    if ((field[i] & 0xffu) == XK_STR) {
      krefs = 1;
      flat = 0;
    } else {
      covered += field[i] & 0xffu;
    }
  }
  if (covered != kw) {
    flat = 0;
  }
  AvantHashX *x = (AvantHashX *)avant_alloc(sizeof(AvantHashX) + (uint64_t)nf * sizeof(uint32_t), AVANT_TYPE_HASH);
  AvantHash *h = &x->h;
  h->desc = desc | AVANT_HASH_TYPED;
  h->seed = hash_seed();
  h->kw = (uint8_t)(desc & AVANT_HASH_K_WIDTH);
  /* A scalar or reference value of 1, 2, 4 or 8 bytes keeps the plain
   * table's copies (the descriptor's width); a struct value (no width in
   * the descriptor) takes a slot of its size VW rounded up to a power of
   * two, the stride of the value buffer's type map (VTID): a slot of 1,
   * 2, 4 or 8 bytes is that width, a wider one is copied by memcpy (h->vw
   * 0). */
  uint32_t vstride = 1;
  while (vstride < vw) {
    vstride <<= 1;
  }
  h->vw = (uint8_t)((desc >> AVANT_HASH_V_SHIFT) & AVANT_HASH_K_WIDTH);
  if (!h->vw && vstride <= 8) {
    h->vw = (uint8_t)vstride;
  }
  h->vshift = (uint8_t)__builtin_ctz(h->vw ? h->vw : vstride);
  h->compact = (desc & (AVANT_HASH_K_STR | AVANT_HASH_K_STRUCT)) != 0;
  x->kw = kw;
  x->vw = vw;
  x->ktid = ktid;
  x->vtid = vtid;
  x->atid = atid;
  x->uhash = NULL;
  x->ueq = NULL;
  x->nf = (uint16_t)nf;
  x->flat = (uint8_t)flat;
  x->krefs = (uint8_t)krefs;
  for (i = 0; i < nf; i++) {
    x->field[i] = field[i];
  }
  return h;
}

avant_int avant_hash_size(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  return h ? h->size : 0;
}

void avant_hash_clear(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  int32_t end;
  if (!h) {
    return;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_CLEAR);
  probe_guard(h, AVANT_FROZEN_HASH_CLEAR);
  if (!h->cap) {
    return;
  }
  end = entry_cap(h, h->cap);
  /* Every key and value reference leaves its buffer (RC5). */
  if (key_refs(h)) {
    avant_gc_overwrite(h->keys);
    memset(h->keys, 0, (size_t)end * key_bytes(h));
  }
  if (val_refs(h)) {
    avant_gc_overwrite(h->vals);
    memset(h->vals, 0, (size_t)end << h->vshift);
  }
  memset(h->meta, 0, (size_t)meta_bytes(h, h->cap));
  h->changes += HASH_DELETED * (uint32_t)h->size;
  h->size = 0;
  h->used = 0;
}

/* The value of KEY, or g_hash_none with *found 0. */
void *avant_hash_find_ik(void *hp, avant_int key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (!h || !find_int(h, key, hash_int(key, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

void *avant_hash_find_sk(void *hp, const char *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  size_t n;
  /* The key's bytes: a view's are its base's (a lookup only reads them;
   * a key the table keeps is always flat, the compiler's store calls see
   * to it). */
  key = avant_str_bytes(key, &n);
  if (!h || !find_bytes(h, key, n, hash_bytes(key, n, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

/* Whether one more entry needs a larger (or compacted) table first. */
static int needs_grow(const AvantHash *h) {
  return h->used >= fill_max(h->cap);
}

/* The value of KEY, inserted holding zero when absent (*found 0, else 1;
 * found may be NULL). GROW_FIRST: room is made before the lookup, as a
 * set always did; else only when the key is absent. A growth may
 * collect; the address stays valid until the caller's next call that may
 * collect. Scalar values only: the caller's store needs no barrier. When
 * no growth is due, the key is found or inserted with no handle scope
 * (nothing allocates on that path); a growth is the _slow function's. */
static void *put_ik_slow(AvantHash *h, avant_int key, uint32_t hv, int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (grow_first) {
    ensure_cap(h);
    if (find_int(h, key, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
  } else {
    ensure_cap(h);
    (void)find_int(h, key, hv, &e, &pos);
  }
  if (found) {
    *found = 0;
  }
  e = claim(h, pos, 1, hv);
  ikey_put(h, h->keys, e, key);
  zero_val(h, val_at(h, h->vals, e));
  return val_at(h, h->vals, e);
}

static void *put_ik(AvantHash *h, avant_int key, int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    if (found) {
      *found = 0;
    }
    g_hash_scratch[0] = 0;
    g_hash_scratch[1] = 0;
    return g_hash_scratch;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_STORE);
  uint32_t hv = hash_int(key, h->seed);
  int grow = needs_grow(h);
  if (!(grow_first && grow)) {
    if (find_int(h, key, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
    if (!grow) {
      if (found) {
        *found = 0;
      }
      e = claim(h, pos, 1, hv);
      ikey_put(h, h->keys, e, key);
      zero_val(h, val_at(h, h->vals, e));
      return val_at(h, h->vals, e);
    }
  }
  return put_ik_slow(h, key, hv, found, grow_first);
}

/* Inlined into each caller (put_sk, and insert_part's miss): as a call,
 * which the compiler chose once it had two, the stores that grow small
 * hashes cost more (the pure JSON parses of LangArena, whose objects are
 * such hashes, 0.2% more instructions). */
static inline __attribute__((always_inline)) void *put_sk_slow(AvantHash *h, const char *key, size_t n, uint32_t hv,
                                                               int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  if (grow_first) {
    ensure_cap(h);
    if (find_bytes(h, key, n, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      if (skey_at(h->keys, e) != key) {
        store_str_key(h, e, key, hv);
        states_of(h)[e] = occ_mark(key, n);
      }
      return val_at(h, h->vals, e);
    }
  } else {
    ensure_cap(h);
    (void)find_bytes(h, key, n, hv, &e, &pos);
  }
  if (found) {
    *found = 0;
  }
  e = claim(h, pos, occ_mark(key, n), hv);
  store_str_key(h, e, key, hv);
  zero_val(h, val_at(h, h->vals, e));
  return val_at(h, h->vals, e);
}

static void *put_sk(AvantHash *h, const char *key, int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    if (found) {
      *found = 0;
    }
    g_hash_scratch[0] = 0;
    g_hash_scratch[1] = 0;
    return g_hash_scratch;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_STORE);
  size_t n = avant_str_n(key);
  uint32_t hv = hash_bytes(key, n, h->seed);
  int grow = needs_grow(h);
  if (!(grow_first && grow)) {
    if (find_bytes(h, key, n, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      /* A set keeps the key it was given, as the table always did. */
      if (grow_first && skey_at(h->keys, e) != key) {
        store_str_key(h, e, key, hv);
        states_of(h)[e] = occ_mark(key, n);
      }
      return val_at(h, h->vals, e);
    }
    if (!grow) {
      if (found) {
        *found = 0;
      }
      e = claim(h, pos, occ_mark(key, n), hv);
      store_str_key(h, e, key, hv);
      zero_val(h, val_at(h, h->vals, e));
      return val_at(h, h->vals, e);
    }
  }
  return put_sk_slow(h, key, n, hv, found, grow_first);
}

/* ---- struct keys (the _xk entries) ---- */

static inline uint64_t xk_mix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 0x9e3779b97f4a7c15ull;
  return h ^ (h >> 29);
}

static inline uint64_t xk_int_at(const char *p, uint32_t w) {
  switch (w) {
  case 1:
    return *(const uint8_t *)p;
  case 2: {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
  }
  case 4: {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
  }
  default: {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
  }
  }
}

/* The hash of the struct key at K: each field, an integer by its bits and
 * a String by its bytes (hash_bytes), mixed in order with the seed; a flat
 * key by its 8-byte words (the last one zero-filled). */
static uint32_t xkey_hash(const AvantHash *h, const char *k) {
  const AvantHashX *x = hx(h);
  uint64_t a = (uint64_t)h->seed ^ 0x2545f4914f6cdd1dull;
  uint32_t i;
  if (x->flat) {
    uint32_t off = 0;
    while (off + 8 <= x->kw) {
      a = xk_mix(a, xk_int_at(k + off, 8));
      off += 8;
    }
    if (off < x->kw) {
      uint64_t v = 0;
      memcpy(&v, k + off, x->kw - off);
      a = xk_mix(a, v);
    }
  } else {
    for (i = 0; i < x->nf; i++) {
      uint32_t f = x->field[i];
      const char *p = k + (f >> 8);
      if ((f & 0xffu) == XK_STR) {
        const char *str;
        memcpy(&str, p, sizeof(str));
        size_t sn;
        const char *sd = avant_str_bytes(str, &sn);
        a = xk_mix(a, hash_bytes(sd, sn, h->seed));
      } else {
        a = xk_mix(a, xk_int_at(p, f & 0xffu));
      }
    }
  }
  return hash_i32((int32_t)(uint32_t)(a ^ (a >> 32)));
}

/* Whether the struct keys at A and B are equal, as the struct's ==
 * compares them: every field, an integer by its bits, a String by its
 * bytes. */
static int xkey_eq(const AvantHash *h, const char *a, const char *b) {
  const AvantHashX *x = hx(h);
  uint32_t i;
  if (x->flat) {
    return memcmp(a, b, x->kw) == 0;
  }
  for (i = 0; i < x->nf; i++) {
    uint32_t f = x->field[i];
    uint32_t off = f >> 8;
    if ((f & 0xffu) == XK_STR) {
      const char *sa;
      const char *sb;
      memcpy(&sa, a + off, sizeof(sa));
      memcpy(&sb, b + off, sizeof(sb));
      size_t na;
      size_t nb;
      const char *da = avant_str_bytes(sa, &na);
      const char *db = avant_str_bytes(sb, &nb);
      if (sa != sb && !bytes_eq(da, na, db, nb)) {
        return 0;
      }
    } else if (memcmp(a + off, b + off, f & 0xffu) != 0) {
      return 0;
    }
  }
  return 1;
}

/* A lookup of the struct key at K whose hash is HV, in the compact layout
 * (every struct-keyed hash is compact): as find_bytes. */
static int find_struct(const AvantHash *h, const char *k, uint32_t hv, int32_t *ent, int32_t *pos) {
  int32_t cap = h->cap;
  int32_t n;
  if (!cap) {
    *pos = 0;
    return 0;
  }
  int32_t mask = cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  const uint64_t *ix = index_of(h);
  const char *keys = (const char *)h->keys;
  size_t kw = hx(h)->kw;
  for (n = 0; n < cap; n++) {
    uint64_t w = ix[i];
    int32_t e = ix_entry(w);
    if (e < 0) {
      *pos = i;
      return 0;
    }
    if ((uint32_t)(w >> 32) == hv && xkey_eq(h, keys + (size_t)e * kw, k)) {
      *ent = e;
      return 1;
    }
    i = (i + 1) & mask;
  }
  *pos = 0;
  return 0;
}

/* A new entry for the struct key at K (its String fields barriered
 * first, RC5), its hash HV. */
static void store_xkey(AvantHash *h, int32_t e, const char *k, uint32_t hv) {
  const AvantHashX *x = hx(h);
  size_t kw = x->kw;
  char *slot = (char *)h->keys + (size_t)e * kw;
  uint32_t i;
  for (i = 0; x->krefs && i < x->nf; i++) {
    if ((x->field[i] & 0xffu) == XK_STR) {
      avant_barrier_slot_young(h->keys, slot + (x->field[i] >> 8));
    }
  }
  memcpy(slot, k, kw);
  hashes_of(h)[e] = hv;
}

/* As put_ik for a struct key at K. K is in the caller's frame, rooted
 * there: a growth's collection updates its String fields in place, and
 * the key is read from K after it (its hash, of the same bytes, holds). */
static void *put_xk(AvantHash *h, const char *k, int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    avant_trap_text("a Hash with a struct key was used before it was created");
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_STORE);
  uint32_t hv = xkey_hash(h, k);
  if (!(grow_first && needs_grow(h))) {
    if (find_struct(h, k, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
  }
  if (needs_grow(h)) {
    {
      AVANT_GC_SCOPE(1);
      AVANT_GC_ROOT(0, &h);
      grow(h);
    }
    if (find_struct(h, k, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
  }
  if (found) {
    *found = 0;
  }
  e = claim(h, pos, 1, hv);
  store_xkey(h, e, k, hv);
  zero_val(h, val_at(h, h->vals, e));
  return val_at(h, h->vals, e);
}

void *avant_hash_find_xk(void *hp, const void *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (!h || !find_struct(h, (const char *)key, xkey_hash(h, (const char *)key), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

void *avant_hash_put_xk(void *hp, const void *key) {
  return put_xk((AvantHash *)hp, (const char *)key, NULL, 1);
}

void *avant_hash_upsert_xk(void *hp, const void *key, int32_t *found) {
  return put_xk((AvantHash *)hp, (const char *)key, found, 0);
}

/* `h[k] = v` for a reference value and a struct key (RC1: the hash and
 * the value are handles; the key is the caller's, rooted in its frame). */
void avant_hash_set_ref_xk(void *hp, const void *key, void *val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &val);
  void **slot = (void **)put_xk(h, (const char *)key, NULL, 1);
  /* The barrier before the store (RC5). */
  avant_barrier_slot_young(h->vals, slot);
  *slot = val;
}

/* `h[k] = v` for a struct value with references: the value's address,
 * whose words the generated code stores after the barrier made here on
 * the value buffer (RC5; the form of avant_array_push_slot). */
static void *slot_after(AvantHash *h, void *slot) {
  avant_barrier_slot_young(h->vals, slot);
  return slot;
}

/* Inlined into each caller, as it was when the slot entries were its
 * only ones. */
static inline __attribute__((always_inline)) void slot_hash(const AvantHash *h) {
  if (!h) {
    avant_trap_text("a Hash was used before it was created");
  }
}

void *avant_hash_slot_ik(void *hp, avant_int key) {
  AvantHash *h = (AvantHash *)hp;
  slot_hash(h);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  void *slot = put_ik(h, key, NULL, 1);
  return slot_after(h, slot);
}

void *avant_hash_slot_sk(void *hp, const char *key) {
  AvantHash *h = (AvantHash *)hp;
  slot_hash(h);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  key = avant_str_flat(key);
  void *slot = put_sk(h, key, NULL, 1);
  return slot_after(h, slot);
}

void *avant_hash_slot_xk(void *hp, const void *key) {
  AvantHash *h = (AvantHash *)hp;
  slot_hash(h);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  void *slot = put_xk(h, (const char *)key, NULL, 1);
  return slot_after(h, slot);
}

/* `h[k] = v` for a scalar value: the generated code stores v at the
 * address. */
void *avant_hash_put_ik(void *hp, avant_int key) {
  return put_ik((AvantHash *)hp, key, NULL, 1);
}

/* A key the table keeps is flat (its probes read a key's bytes at its
 * address): a view key is copied first, the hash a handle across the
 * copy. */
static void *put_sk_view(AvantHash *h, const char *key, int32_t *found, int grow_first) {
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  key = avant_str_flat(key);
  return put_sk(h, key, found, grow_first);
}

void *avant_hash_put_sk(void *hp, const char *key) {
  if (__builtin_expect(avant_str_is_view(key), 0)) {
    return put_sk_view((AvantHash *)hp, key, NULL, 1);
  }
  return put_sk((AvantHash *)hp, key, NULL, 1);
}

/* A get fused with the set that follows it (compiler/mir/upsert.av), and
 * `inc`: room is made only for an absent key. */
void *avant_hash_upsert_ik(void *hp, avant_int key, int32_t *found) {
  return put_ik((AvantHash *)hp, key, found, 0);
}

void *avant_hash_upsert_sk(void *hp, const char *key, int32_t *found) {
  if (__builtin_expect(avant_str_is_view(key), 0)) {
    return put_sk_view((AvantHash *)hp, key, found, 0);
  }
  return put_sk((AvantHash *)hp, key, found, 0);
}

/* `h[k] = v` for a reference value: stored here, behind the barrier
 * (RC1: the growth may collect; the hash, the key and the value are
 * handles). */
void avant_hash_set_ref_ik(void *hp, avant_int key, void *val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &val);
  void **slot = (void **)put_ik(h, key, NULL, 1);
  /* The barrier before the store (RC5). */
  if (h) {
    avant_barrier_slot_young(h->vals, slot);
  }
  *slot = val;
}

void avant_hash_set_ref_sk(void *hp, const char *key, void *val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  AVANT_GC_ROOT(2, &val);
  key = avant_str_flat(key);
  void **slot = (void **)put_sk(h, key, NULL, 1);
  /* The barrier before the store (RC5). */
  if (h) {
    avant_barrier_slot_young(h->vals, slot);
  }
  *slot = val;
}

/* Hash.delete: removes the key and gives the address of a copy of the
 * value it held, *found 1; or g_hash_none, *found 0, when the key was
 * absent. erase does not allocate, so a reference value stays valid
 * until the caller's next call that may collect. */
static void *take_at(AvantHash *h, int32_t e, int32_t *found) {
  void *into = g_hash_taken;
  if (__builtin_expect(h->vshift > 4, 0)) {
    into = thread_buf(&g_hash_wide_taken, &g_hash_wide_taken_n, (size_t)1 << h->vshift);
  }
  cp_val(h, into, val_at(h, h->vals, e));
  erase(h, e);
  *found = 1;
  return into;
}

void *avant_hash_take_ik(void *hp, avant_int key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (h) {
    HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_DELETE);
  }
  if (!h || !find_int(h, key, hash_int(key, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  return take_at(h, e, found);
}

void *avant_hash_take_sk(void *hp, const char *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (h) {
    HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_DELETE);
  }
  size_t n;
  key = avant_str_bytes(key, &n);
  if (!h || !find_bytes(h, key, n, hash_bytes(key, n, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  return take_at(h, e, found);
}

void *avant_hash_take_xk(void *hp, const void *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (h) {
    HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_DELETE);
  }
  if (!h || !find_struct(h, (const char *)key, xkey_hash(h, (const char *)key), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  return take_at(h, e, found);
}

/* delete as a statement, whose value nobody reads. */
void avant_hash_del_sk(void *hp, const char *key) {
  int32_t found = 0;
  (void)avant_hash_take_sk(hp, key, &found);
}

void avant_hash_del_ik(void *hp, avant_int key) {
  int32_t found = 0;
  (void)avant_hash_take_ik(hp, key, &found);
}

void avant_hash_del_xk(void *hp, const void *key) {
  int32_t found = 0;
  (void)avant_hash_take_xk(hp, key, &found);
}

/* ---- keys with their own hash and equality (the _uk entries) ----
 *
 * A struct key whose type defines its own hash and equality (or holds
 * such a struct) is stored as a struct key passed by address is (its
 * bytes, a uint32 hash per entry, the compact layout), but its hash and
 * its equality are the type's: the functions avant_hash_user set. The
 * hash is computed by a hasher on the C stack: the generated hash
 * function adds the values the type's equality compares
 * (avant_hasher_add, avant_hasher_add_str), mixed in order with the
 * hash's seed, and the hasher's state is finished as a struct key's
 * hash is. Both functions run Avant code that may allocate and collect,
 * so inside an entry the hash is a handle, and every pointer into its
 * buffers is read from it again after each call (RC1, RC2). The stored
 * hashes are what a growth and a deletion read: no key is hashed again
 * outside an entry. A table slot's key passed to the equality is read
 * there before anything can collect (the generated function loads both
 * keys first); the caller's key is the caller's local, a frame root. */

typedef struct {
  uint64_t a;
  uint32_t seed;
} AvantHasher;

void avant_hasher_add(void *hs, uint64_t v) {
  AvantHasher *s = (AvantHasher *)hs;
  s->a = xk_mix(s->a, v);
}

void avant_hasher_add_str(void *hs, const char *str) {
  AvantHasher *s = (AvantHasher *)hs;
  size_t n;
  /* A view's bytes are its base's. */
  const char *d = avant_str_bytes(str, &n);
  s->a = xk_mix(s->a, hash_bytes(d, n, s->seed));
}

/* gc-lint: holds nothing (it stores two code addresses, allocates
 * nothing). */
void avant_hash_user(void *hp, void *hashfn, void *eqfn) {
  AvantHashX *x = hx((AvantHash *)hp);
  x->uhash = (void (*)(const void *, void *))hashfn;
  x->ueq = (int32_t (*)(const void *, const void *))eqfn;
}

/* The hashes whose key's own hash or equality this thread is running:
 * the handles of the entries' callers, innermost last. That code may
 * reach the hash it is called for (a key can hold a class that holds the
 * hash), and a store, a deletion or a clear there would move the entries
 * under the probe that called it, which then reads the old positions:
 * each of those traps while the hash is here (probe_guard). A lookup
 * stays allowed: it changes nothing. A trap that ends a task or an HTTP
 * handler drops the ones noted below its recovery point
 * (avant_hash_probe_drop, from avant_task_trapped). Past PROBE_MAX
 * nested probes the count goes on and the handles are not kept, so the
 * deeper ones are not guarded. */
#define PROBE_MAX 64
static __thread AvantHash *const *g_probes[PROBE_MAX];
static __thread int g_probe_n;

static inline void probe_push(AvantHash *const *hh) {
  int n = g_probe_n;
  if (n < PROBE_MAX) {
    g_probes[n] = hh;
  }
  g_probe_n = n + 1;
}

static inline void probe_pop(void) {
  g_probe_n--;
}

__attribute__((visibility("hidden"))) int avant_hash_probe_depth(void) {
  return g_probe_n;
}

__attribute__((visibility("hidden"))) void avant_hash_probe_drop(int n) {
  if (g_probe_n > n) {
    g_probe_n = n;
  }
}

static __attribute__((noinline, cold)) void probe_guard_slow(const AvantHash *h, int32_t op) {
  static const char *names[] = {"a store", "delete", "clear"};
  int i;
  int n = g_probe_n < PROBE_MAX ? g_probe_n : PROBE_MAX;
  for (i = 0; i < n; i++) {
    if (*g_probes[i] == h) {
      char text[256];
      snprintf(text, sizeof(text),
               "%s on a Hash while its key's own hash or equals runs for it: the probe that called them would read moved entries; a key type's hash and equals must not change the table they are called for",
               op >= 0 && op <= 2 ? names[op] : "a change");
      avant_trap_text(text);
    }
  }
}

/* The test of every change to a hash whose key may have its own hash
 * and equality, and of clear: one thread-local load while no such code
 * runs. */
static inline void probe_guard(const AvantHash *h, int32_t op) {
  if (__builtin_expect(g_probe_n != 0, 0)) {
    probe_guard_slow(h, op);
  }
}

/* The hash of the user key at K, by the type's hash; *HH is the caller's
 * handle of the hash (the call may collect). */
static uint32_t ukey_hash(AvantHash *const *hh, const void *k) {
  AvantHasher s;
  void (*fn)(const void *, void *) = hx(*hh)->uhash;
  s.seed = (*hh)->seed;
  s.a = (uint64_t)s.seed ^ 0x2545f4914f6cdd1dull;
  probe_push(hh);
  fn(k, &s);
  probe_pop();
  return hash_i32((int32_t)(uint32_t)(s.a ^ (s.a >> 32)));
}

/* As find_struct, with the type's equality: *HH is the caller's handle,
 * and the table's buffers are read from it again after every call. */
static int find_user(AvantHash *const *hh, const char *k, uint32_t hv, int32_t *ent, int32_t *pos) {
  int32_t cap = (*hh)->cap;
  int32_t n;
  if (!cap) {
    *pos = 0;
    return 0;
  }
  int32_t mask = cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  for (n = 0; n < cap; n++) {
    const AvantHash *h = *hh;
    uint64_t w = index_of(h)[i];
    int32_t e = ix_entry(w);
    if (e < 0) {
      *pos = i;
      return 0;
    }
    if ((uint32_t)(w >> 32) == hv) {
      const char *slot = (const char *)h->keys + (size_t)e * hx(h)->kw;
      probe_push(hh);
      int32_t eq = hx(h)->ueq(slot, k);
      probe_pop();
      if (eq) {
        *ent = e;
        return 1;
      }
    }
    i = (i + 1) & mask;
  }
  *pos = 0;
  return 0;
}

/* As put_xk for a user key at K (the caller's local, rooted in its
 * frame). */
static void *put_uk(AvantHash *h, const char *k, int32_t *found, int grow_first) {
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    avant_trap_text("a Hash with a struct key was used before it was created");
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_STORE);
  probe_guard(h, AVANT_FROZEN_HASH_STORE);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  uint32_t hv = ukey_hash(&h, k);
  if (!(grow_first && needs_grow(h))) {
    if (find_user(&h, k, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
  }
  if (needs_grow(h)) {
    grow(h);
    if (find_user(&h, k, hv, &e, &pos)) {
      if (found) {
        *found = 1;
      }
      return val_at(h, h->vals, e);
    }
  }
  if (found) {
    *found = 0;
  }
  e = claim(h, pos, 1, hv);
  store_xkey(h, e, k, hv);
  zero_val(h, val_at(h, h->vals, e));
  return val_at(h, h->vals, e);
}

void *avant_hash_find_uk(void *hp, const void *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    *found = 0;
    return (void *)g_hash_none;
  }
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (!find_user(&h, (const char *)key, ukey_hash(&h, key), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

void *avant_hash_put_uk(void *hp, const void *key) {
  return put_uk((AvantHash *)hp, (const char *)key, NULL, 1);
}

void *avant_hash_upsert_uk(void *hp, const void *key, int32_t *found) {
  return put_uk((AvantHash *)hp, (const char *)key, found, 0);
}

void avant_hash_set_ref_uk(void *hp, const void *key, void *val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &val);
  void **slot = (void **)put_uk(h, (const char *)key, NULL, 1);
  /* The barrier before the store (RC5). */
  avant_barrier_slot_young(h->vals, slot);
  *slot = val;
}

void *avant_hash_slot_uk(void *hp, const void *key) {
  AvantHash *h = (AvantHash *)hp;
  slot_hash(h);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  void *slot = put_uk(h, (const char *)key, NULL, 1);
  return slot_after(h, slot);
}

void *avant_hash_take_uk(void *hp, const void *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t e = 0;
  int32_t pos = 0;
  if (!h) {
    *found = 0;
    return (void *)g_hash_none;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_DELETE);
  probe_guard(h, AVANT_FROZEN_HASH_DELETE);
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (!find_user(&h, (const char *)key, ukey_hash(&h, key), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  return take_at(h, e, found);
}

void avant_hash_del_uk(void *hp, const void *key) {
  int32_t found = 0;
  (void)avant_hash_take_uk(hp, key, &found);
}

/* ---- the walk in place (for k, v in h; h.each; a Set's) ----
 *
 * A walk visits the entries in their order (the order of insertion),
 * reading each one where it is: the generated code keeps the entry's
 * number, asks for the next live one (avant_hash_walk) and loads the key
 * and the value through their addresses (avant_hash_walk_key, _val),
 * before anything can collect. Its count of changes (above) is read
 * before the first step and after each: a deleted entry only changes its
 * state, so a walk whose step deleted the entry it visits goes on; a key
 * added would be missed, and a deletion of another key traps too
 * (avant_hash_walk_ok tells the generated code which). Nothing here
 * changes the hash, so a walk of a hash that running tasks read is a
 * read like theirs. */

/* The number of the first live entry at or after N, or -1. */
int32_t avant_hash_walk(void *hp, int32_t n) {
  AvantHash *h = (AvantHash *)hp;
  if (!h || !h->cap || n < 0) {
    return -1;
  }
  int32_t end = walk_end(h);
  const uint8_t *st = states_of(h);
  if (is_compact(h)) {
    for (; n < end; n++) {
      if (st_full(st[n])) {
        return n;
      }
    }
    return -1;
  }
  const uint32_t *ord = order_of(h);
  for (; n < end; n++) {
    if (st_full(st[ord[n]])) {
      return n;
    }
  }
  return -1;
}

/* The address of entry E's key and value (E from avant_hash_walk, in the
 * same step: nothing collected or changed the hash since). */
void *avant_hash_walk_key(void *hp, int32_t e) {
  AvantHash *h = (AvantHash *)hp;
  return (char *)h->keys + (size_t)walk_slot(h, e) * key_bytes(h);
}

void *avant_hash_walk_val(void *hp, int32_t e) {
  AvantHash *h = (AvantHash *)hp;
  return val_at(h, h->vals, walk_slot(h, e));
}

/* After a step of a walk whose count of changes moved from C0: 1 when
 * the only change was the deletion of the visited entry E (the walk goes
 * on), else 0 when a key was added and 2 when another key was deleted
 * (the walk's trap names which). */
int32_t avant_hash_walk_ok(void *hp, uint32_t c0, int32_t e) {
  AvantHash *h = (AvantHash *)hp;
  uint32_t d = h->changes - c0;
  if ((d & (HASH_DELETED - 1u)) != 0) {
    return 0;
  }
  /* One key removed, and the visited entry is no longer live (or a clear
   * emptied the table): that key was the visited one. */
  if (d == HASH_DELETED && e >= 0 && (e >= walk_end(h) || !ent_live(h, walk_slot(h, e)))) {
    return 1;
  }
  return 2;
}

/* delete_if's deletion of the entry E it visits, by its number: no key
 * is hashed or compared again (a key's own hash and equals are not
 * called); an entry the predicate already deleted stays so. A frozen
 * hash traps as delete does; delete_if makes that test first with E -1,
 * so it traps on a frozen hash whatever its predicate says. */
void avant_hash_walk_delete(void *hp, int32_t e) {
  AvantHash *h = (AvantHash *)hp;
  if (!h) {
    return;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_DELETE);
  probe_guard(h, AVANT_FROZEN_HASH_DELETE);
  if (e < 0 || e >= walk_end(h)) {
    return;
  }
  int32_t x = walk_slot(h, e);
  if (ent_live(h, x)) {
    erase(h, x);
  }
}

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArrayBox;

/* The keys or the values of H as a new Array, in the walk's order (RC1,
 * avant_gc.h): the array and its buffer are allocated first, with the
 * hash and the array as handles, and filled from the hash after the last
 * allocation. A reference element buffer is a pointer buffer, barriered
 * (RC3). */
static void *hash_collect(AvantHash *h, int keys) {
  AvantArrayBox *a = NULL;
  int32_t n = h ? h->size : 0;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArrayBox *)avant_alloc(sizeof(AvantArrayBox), AVANT_TYPE_ARRAY_OBJ);
  if (n > 0) {
    int ref = keys ? key_refs(h) : val_refs(h);
    size_t w = keys ? key_bytes(h) : val_bytes(h);
    uint32_t tid = keys ? key_type_id(h) : (is_typed(h) ? hx(h)->atid : val_type_id(h));
    void *buf = avant_alloc((uint64_t)n * w, tid);
    int32_t end = walk_end(h);
    int32_t e;
    int32_t k = 0;
    for (e = 0; e < end && k < n; e++) {
      int32_t x = walk_slot(h, e);
      if (!ent_live(h, x)) {
        continue;
      }
      cp_n((char *)buf + (size_t)k * w, keys ? (char *)h->keys + (size_t)x * w : (char *)val_at(h, h->vals, x), w);
      k++;
    }
    if (ref) {
      avant_barrier(buf);
    }
    avant_barrier(a);
    a->buf = buf;
    a->size = k;
    a->cap = n;
  }
  return a;
}

void *avant_hash_keys(void *hp) {
  return hash_collect((AvantHash *)hp, 1);
}

void *avant_hash_values(void *hp) {
  return hash_collect((AvantHash *)hp, 0);
}

/* ---- the String-key helpers of the benchmark programs ---- */

/* A key slice traps like avant_str_slice. */
static void check_slice(const char *s, avant_int start, avant_int stop, const char **out, size_t *n) {
  size_t len;
  const char *d = avant_str_bytes(s, &len);
  if (start < 0 || stop < start || (size_t)stop > len) {
    avant_trap_slice(start, stop, (avant_int)len);
  }
  *out = d + start;
  *n = (size_t)(stop - start);
}

static void trim_slice(const char **p, size_t *n) {
  const char *s = *p;
  size_t len = *n;
  while (len && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) {
    s++;
    len--;
  }
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
    len--;
  }
  *p = s;
  *n = len;
}

static int is_str_int(const AvantHash *h) {
  return key_is_str(h) && !val_is_ref(h) && vw_of(h) == 8;
}

static int is_str_str(const AvantHash *h) {
  return key_is_str(h) && val_is_ref(h);
}

const char *avant_hash_get_str_slice(void *hp, const char *s, avant_int start, avant_int stop, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  if (found) {
    *found = 0;
  }
  if (!h || !is_str_str(h)) {
    return AVANT_STR_EMPTY;
  }
  const char *p = NULL;
  size_t n = 0;
  check_slice(s, start, stop, &p, &n);
  trim_slice(&p, &n);
  int32_t e = 0;
  int32_t pos = 0;
  if (!find_bytes(h, p, n, hash_bytes(p, n, h->seed), &e, &pos)) {
    return AVANT_STR_EMPTY;
  }
  if (found) {
    *found = 1;
  }
  return *(const char **)val_at(h, h->vals, e);
}

/* W bytes at offset O of the key A followed by B (W at most 8), read as
 * hash_bytes reads them from one buffer, little-endian. */
static inline uint64_t hc_rd(const unsigned char *a, size_t na, const unsigned char *b, size_t o, size_t w) {
  unsigned char t[8];
  if (o + w <= na) {
    memcpy(t, a + o, w);
  } else if (o >= na) {
    memcpy(t, b + (o - na), w);
  } else {
    memcpy(t, a + o, na - o);
    memcpy(t + (na - o), b, w - (na - o));
  }
  if (w == 8) {
    return hb_rd8(t);
  }
  if (w == 4) {
    return hb_rd4(t);
  }
  return t[0];
}

/* The hash of the key A followed by B: hash_bytes's of their bytes as
 * one buffer. */
static uint32_t hash_concat_bytes(const char *a, size_t na, const char *b, size_t nb, uint32_t seed) {
  const unsigned char *pa = (const unsigned char *)a;
  const unsigned char *pb = (const unsigned char *)b;
  uint32_t out;
  if (!pa) {
    na = 0;
  }
  if (!pb) {
    nb = 0;
  }
  if (!na) {
    return hash_bytes(b, nb, seed);
  }
  if (!nb) {
    return hash_bytes(a, na, seed);
  }
#define HC_RD8(o) hc_rd(pa, na, pb, (o), 8)
#define HC_RD4(o) hc_rd(pa, na, pb, (o), 4)
#define HC_RD1(o) hc_rd(pa, na, pb, (o), 1)
  HASH_KEYED_BODY(HC_RD8, HC_RD4, HC_RD1, na + nb, seed, out);
#undef HC_RD8
#undef HC_RD4
#undef HC_RD1
  return out;
}

static int bytes_eq_concat(const char *slot, size_t ns, const char *a, size_t na, const char *b, size_t nb) {
  if (!slot) {
    slot = AVANT_STR_EMPTY;
  }
  if (!a) {
    a = AVANT_STR_EMPTY;
    na = 0;
  }
  if (!b) {
    b = AVANT_STR_EMPTY;
    nb = 0;
  }
  if (ns != na + nb) {
    return 0;
  }
  if (na && memcmp(slot, a, na) != 0) {
    return 0;
  }
  if (nb && memcmp(slot + na, b, nb) != 0) {
    return 0;
  }
  return 1;
}

avant_int avant_hash_get_concat(void *hp, const char *a, const char *b, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  size_t na;
  size_t nb;
  a = avant_str_bytes(a, &na);
  b = avant_str_bytes(b, &nb);
  int32_t mask;
  int32_t i;
  int32_t n;
  if (found) {
    *found = 0;
  }
  if (!h || !is_str_int(h) || !h->cap) {
    return 0;
  }
  uint32_t hv = hash_concat_bytes(a, na, b, nb, h->seed);
  mask = h->cap - 1;
  i = (int32_t)(hv & (uint32_t)mask);
  /* A String key's table is compact: its index words hold the hashes. */
  const uint64_t *ix = index_of(h);
  for (n = 0; n < h->cap; n++) {
    uint64_t w = ix[i];
    int32_t e = ix_entry(w);
    if (e < 0) {
      return 0;
    }
    if ((uint32_t)(w >> 32) == hv) {
      const char *slot = skey_at(h->keys, e);
      if (bytes_eq_concat(slot, skey_n(slot, ent_state(h, e)), a, na, b, nb)) {
        if (found) {
          *found = 1;
        }
        return *(avant_int *)val_at(h, h->vals, e);
      }
    }
    i = (i + 1) & mask;
  }
  return 0;
}

/* ---- a String key given as a slice, read in place ----
 *
 * `h[s[a..b]]`, `h.get(s.slice(a))`, `h.has(buf.to_s())`,
 * `h[buf[a..b].to_s()] = v` (compiler/mir/lower_hash.av, key_part): the
 * key is the bytes start up to stop of a String (the _sk_part entries)
 * or of a Buf (the _bk_part ones), checked as the slice is
 * (avant_trap_slice), hashed and compared where they lie. The hash of a
 * key is a function of its bytes alone (hash_bytes), so the bytes in
 * place take the hash the String built from them would, whatever the
 * String's form (flat, a literal, a view). A lookup allocates nothing;
 * an insert builds the key once, at a miss, as a flat heap string. */

/* A Buf's fields (avant_buf.c). */
typedef struct {
  uint8_t *data;
  int32_t size;
  int32_t cap;
} HashPartBuf;

/* The bytes start up to stop of SRC, a String (IS_BUF 0) or a Buf, and
 * their count in *N; a slice outside 0 <= start <= stop <= size traps.
 * The address lies inside an object that may move: form it again after
 * any call that may collect. */
static inline __attribute__((always_inline)) const char *part_bytes(const void *src, int is_buf, avant_int start, avant_int stop, size_t *n) {
  const char *d;
  size_t len;
  if (is_buf) {
    const HashPartBuf *b = (const HashPartBuf *)src;
    len = b && b->data && b->size > 0 ? (size_t)b->size : 0;
    d = len ? (const char *)b->data : AVANT_STR_EMPTY;
  } else {
    d = avant_str_bytes((const char *)src, &len);
  }
  if (start < 0 || stop < start || (size_t)stop > len) {
    avant_trap_slice(start, stop, (avant_int)len);
  }
  *n = (size_t)(stop - start);
  return d + start;
}

static inline __attribute__((always_inline)) void *find_part(AvantHash *h, const void *src, int is_buf, avant_int start, avant_int stop, int32_t *found) {
  size_t n;
  const char *p = part_bytes(src, is_buf, start, stop, &n);
  int32_t e = 0;
  int32_t pos = 0;
  if (!h || !find_bytes(h, p, n, hash_bytes(p, n, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

void *avant_hash_find_sk_part(void *hp, const char *s, avant_int start, avant_int stop, int32_t *found) {
  return find_part((AvantHash *)hp, s, 0, start, stop, found);
}

void *avant_hash_find_bk_part(void *hp, void *buf, avant_int start, avant_int stop, int32_t *found) {
  return find_part((AvantHash *)hp, buf, 1, start, stop, found);
}

/* How an insert by a slice gives the value's address: as it is (a
 * scalar value, stored by the generated code), after its barrier for a
 * struct value (the form of avant_hash_slot_sk: a hash not created
 * traps), or after its barrier for a reference value (the form of
 * avant_hash_set_ref_sk: a hash not created takes the store in its
 * scratch words). */
#define PART_PLAIN 0
#define PART_SLOT 1
#define PART_REF 2

/* The miss of an insert by a slice: the key built from the bytes (the
 * allocation may collect: the hash and the source are handles, and the
 * bytes are formed again after it), then entered as put_sk_slow enters
 * a key it did not find. */
static __attribute__((noinline)) void *insert_part(AvantHash *h, const void *src, int is_buf, avant_int start,
                                                   size_t n, uint32_t hv, int how) {
  size_t m;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &src);
  char *key = avant_str_alloc((uint64_t)n);
  if (n) {
    memcpy(key, part_bytes(src, is_buf, start, start + (avant_int)n, &m), n);
  }
  key[n] = 0;
  avant_str_tag(key);
  void *v = put_sk_slow(h, key, n, hv, NULL, 0);
  return how != PART_PLAIN ? slot_after(h, v) : v;
}

/* `inc`, a Set's `add` and the store of a scalar value: the value's
 * address, an absent key entered holding zero (*found 0). */
static inline __attribute__((always_inline)) void *upsert_part(AvantHash *h, const void *src, int is_buf, avant_int start, avant_int stop, int32_t *found,
                         int how) {
  size_t n;
  const char *p = part_bytes(src, is_buf, start, stop, &n);
  int32_t e = 0;
  int32_t pos = 0;
  if (how == PART_SLOT) {
    slot_hash(h);
  }
  if (!h) {
    if (found) {
      *found = 0;
    }
    g_hash_scratch[0] = 0;
    g_hash_scratch[1] = 0;
    return g_hash_scratch;
  }
  HASH_FROZEN_TRAP(h, AVANT_FROZEN_HASH_STORE);
  uint32_t hv = hash_bytes(p, n, h->seed);
  if (find_bytes(h, p, n, hv, &e, &pos)) {
    if (found) {
      *found = 1;
    }
    void *v = val_at(h, h->vals, e);
    return how != PART_PLAIN ? slot_after(h, v) : v;
  }
  if (found) {
    *found = 0;
  }
  return insert_part(h, src, is_buf, start, n, hv, how);
}

void *avant_hash_upsert_sk_part(void *hp, const char *s, avant_int start, avant_int stop, int32_t *found) {
  return upsert_part((AvantHash *)hp, s, 0, start, stop, found, PART_PLAIN);
}

void *avant_hash_upsert_bk_part(void *hp, void *buf, avant_int start, avant_int stop, int32_t *found) {
  return upsert_part((AvantHash *)hp, buf, 1, start, stop, found, PART_PLAIN);
}

/* The store of a struct value (SLOT 1) or of a reference value (0): the
 * value's address after its barrier, the generated code's store after
 * it (PART_SLOT, PART_REF). */
void *avant_hash_slot_sk_part(void *hp, const char *s, avant_int start, avant_int stop, int32_t slot) {
  return upsert_part((AvantHash *)hp, s, 0, start, stop, NULL, slot ? PART_SLOT : PART_REF);
}

void *avant_hash_slot_bk_part(void *hp, void *buf, avant_int start, avant_int stop, int32_t slot) {
  return upsert_part((AvantHash *)hp, buf, 1, start, stop, NULL, slot ? PART_SLOT : PART_REF);
}

/* ---- the freeze of a value holding a hash (avant_rt.h) ---- */

/* One more (up) or one fewer task reads h. A thaw of a hash that is not
 * frozen is the compiler's error. */
static void hash_count(AvantHash *h, int up) {
  int32_t c = hash_frozen(h);
  int32_t n;
  do {
    if (up) {
      if (c == 0x7fffffff) {
        fflush(stdout);
        fputs("avant runtime: a hash is captured by more than 2147483646 running tasks\n", stderr);
        exit(1);
      }
      n = c + 1;
    } else {
      if (c <= 0) {
        fflush(stdout);
        fputs("avant runtime: internal error: a thaw of a hash that is not frozen\n", stderr);
        abort();
      }
      n = c - 1;
    }
  } while (!__atomic_compare_exchange_n(&h->frozen, &c, n, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
}

/* The next level's values that level v holds (depth > 1): an array's
 * elements, a hash's live values, each a reference. Frozen (thaw 0) or
 * thawed; v is frozen meanwhile, so they do not change. */
static void each_inner(void *v, int32_t depth, uint32_t hashes, int thaw, uint64_t elem_size) {
  int32_t i;
  if (hashes & 1u) {
    AvantHash *h = (AvantHash *)v;
    int32_t end = h->cap ? walk_end(h) : 0;
    for (i = 0; i < end; i++) {
      int32_t at = walk_slot(h, i);
      if (!ent_live(h, at)) {
        continue;
      }
      void *x = *(void **)val_at(h, h->vals, at);
      if (thaw) {
        avant_value_thaw(x, depth - 1, hashes >> 1, elem_size);
      } else {
        avant_value_freeze(x, depth - 1, hashes >> 1);
      }
    }
    return;
  }
  AvantArrayBox *a = (AvantArrayBox *)v;
  for (i = 0; a->buf && i < a->size; i++) {
    void *x = ((void **)a->buf)[i];
    if (thaw) {
      avant_value_thaw(x, depth - 1, hashes >> 1, elem_size);
    } else {
      avant_value_freeze(x, depth - 1, hashes >> 1);
    }
  }
}

void avant_value_freeze(void *v, int32_t depth, uint32_t hashes) {
  if (!v) {
    return;
  }
  if (hashes & 1u) {
    hash_count((AvantHash *)v, 1);
  } else {
    avant_array_freeze(v, 1);
  }
  if (depth > 1) {
    each_inner(v, depth, hashes, 0, 0);
  }
}

void avant_value_thaw(void *v, int32_t depth, uint32_t hashes, uint64_t elem_size) {
  if (!v) {
    return;
  }
  if (depth > 1) {
    each_inner(v, depth, hashes, 1, elem_size);
  }
  if (hashes & 1u) {
    hash_count((AvantHash *)v, 0);
  } else {
    avant_array_thaw(v, 1, depth > 1 ? sizeof(void *) : elem_size);
  }
}
