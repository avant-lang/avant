#include "avant_rt.h"
#include "avant_gc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
 * Two layouts, one per kind of key (is_compact below says why). The
 * open layout (integer keys): one slot per table position;
 * meta is a state byte per slot (0 empty, 1 full; 0x80 below) and, for
 * String keys, a uint32 hash per slot; keys[i] and vals[i] are slot i's.
 * Linear probing; a deletion slides the cluster back (no tombstones).
 *
 * The compact layout (String keys): the table is an index of
 * uint32 per position (0 empty, else the entry's number plus one), and
 * the entries are dense, in insertion order: keys[e], vals[e], a state
 * byte per entry (0 deleted, 1 live; 0x80 below) and, for String keys, a
 * uint32 hash per entry, in meta after the index. A deletion slides the
 * index's cluster back and marks its entry deleted; a growth copies the
 * live entries in order, so a walk is in insertion order.
 *
 * The seed. Every hash function takes the hash's seed (h->seed), set at
 * creation from g_hash_seed. It is 0 today, which hashes exactly as the
 * unseeded functions did; a per-process random seed is one assignment.
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
} AvantHash;

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

static uint32_t g_hash_seed = 0;

static __thread const char *g_last_key;
static __thread int g_last_key_rooted;

/* g_last_key names a heap key (or a static string), so it is a root of
 * this thread's collections (avant_gc_add_root), registered the first
 * time a helper that sets it runs. Every write of g_last_key comes after
 * this reset in the same helper. */
static void last_key_reset(void) {
  if (!g_last_key_rooted) {
    g_last_key_rooted = 1;
    avant_gc_add_root((void **)&g_last_key);
  }
  g_last_key = "";
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

static uint32_t hash_bytes(const char *s, size_t n, uint32_t seed) {
  uint32_t h = 2166136261u ^ seed;
  size_t i;
  if (!s) {
    return h;
  }
  for (i = 0; i < n; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
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
    a = "";
    na = 0;
  }
  if (!b) {
    b = "";
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

/* ---- the two layouts: positions, entries, growth, deletion ---- */

/* The layout a hash takes, by its key: the compact one for String keys,
 * the open one for integer keys. On the benchmark programs the compact
 * table read String keys faster (its dense entries are read in the order
 * they were inserted) and used less memory, and read integer keys slower
 * (the index is one more dependent load before a key that compares in
 * one instruction). Every function below serves both layouts for any key,
 * so the choice is the flag avant_hash_create sets from the key. */
static inline int is_compact(const AvantHash *h) {
  return h->compact;
}

/* The most entries a compact table of CAP positions holds before it
 * grows; an open table holds cap * 3 / 4 too, counted by size. */
static int32_t fill_max(int32_t cap) {
  return cap - cap / 4;
}

/* Compact meta: uint32 index[cap], uint8 state[fill_max(cap)], then (String
 * and struct keys) uint32 hash[fill_max(cap)] at the next 4-byte boundary.
 * Open meta: uint8 state[cap], then (String keys) uint32 hash[cap]. */
static inline size_t meta_hash_off(const AvantHash *h, int32_t cap) {
  if (!is_compact(h)) {
    return (size_t)cap;
  }
  size_t off = (size_t)cap * 4 + (size_t)fill_max(cap);
  return (off + 3) & ~(size_t)3;
}

static uint64_t meta_bytes(const AvantHash *h, int32_t cap) {
  int32_t ne = is_compact(h) ? fill_max(cap) : cap;
  if (!cap) {
    return 0;
  }
  if (key_hashed(h)) {
    return (uint64_t)meta_hash_off(h, cap) + (uint64_t)ne * 4;
  }
  return is_compact(h) ? (uint64_t)cap * 4 + (uint64_t)ne : (uint64_t)cap;
}

/* The entries a table of CAP positions has room for. */
static int32_t entry_cap(const AvantHash *h, int32_t cap) {
  return is_compact(h) ? fill_max(cap) : cap;
}

static inline uint32_t *index_of(const AvantHash *h) {
  return (uint32_t *)h->meta;
}

static inline uint8_t *states_of(const AvantHash *h) {
  if (is_compact(h)) {
    return (uint8_t *)h->meta + (size_t)h->cap * 4;
  }
  return (uint8_t *)h->meta;
}

static inline uint32_t *hashes_of(const AvantHash *h) {
  return (uint32_t *)((char *)h->meta + meta_hash_off(h, h->cap));
}

static inline uint8_t ent_state(const AvantHash *h, int32_t e) {
  return states_of(h)[e];
}

/* The entry a position names, or -1: the index's (compact), or the
 * position's own when it is full (open). */
static inline int32_t pos_entry(const AvantHash *h, int32_t i) {
  if (is_compact(h)) {
    return (int32_t)index_of(h)[i] - 1;
  }
  return st_full(states_of(h)[i]) ? i : -1;
}

static inline uint32_t ent_hash(const AvantHash *h, int32_t e) {
  if (key_hashed(h)) {
    return hashes_of(h)[e];
  }
  return hash_int(ikey_at(h, h->keys, e), h->seed);
}

/* The number of entries a walk visits in order: the entries used
 * (compact: the order of insertion) or the positions (open). */
static int32_t walk_end(const AvantHash *h) {
  return is_compact(h) ? h->used : h->cap;
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
#define POS_OPEN(i) (st_full(st[i]) ? (i) : -1)
#define POS_COMPACT(i) ((int32_t)ix[i] - 1)

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
    const uint32_t *ix = index_of(h);
    FIND_INT_WIDTHS(POS_COMPACT);
  } else {
    const uint8_t *st = states_of(h);
    FIND_INT_WIDTHS(POS_OPEN);
  }
  *pos = 0;
  return 0;
}

#define FIND_BYTES_AT(POS)                                                          \
  do {                                                                            \
    for (k = 0; k < cap; k++) {                                                   \
      int32_t e = POS(i);                                                         \
      if (e < 0) {                                                                \
        *pos = i;                                                                 \
        return 0;                                                                 \
      }                                                                           \
      if (hs[e] == hv) {                                                          \
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
    const uint32_t *ix = index_of(h);
    FIND_BYTES_AT(POS_COMPACT);
  } else {
    const uint8_t *st = es;
    FIND_BYTES_AT(POS_OPEN);
  }
  *pos = 0;
  return 0;
}

/* A new entry at position POS (from a failed lookup after ensure_cap):
 * its number, state ST set, the size counted. Its key, hash and value
 * are the caller's to store (the value is zero). */
static int32_t claim(AvantHash *h, int32_t pos, uint8_t st) {
  int32_t e = pos;
  if (is_compact(h)) {
    e = h->used;
    h->used += 1;
    index_of(h)[pos] = (uint32_t)e + 1u;
  }
  states_of(h)[e] = st;
  h->size += 1;
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
  int32_t ne = claim(dst, i, st);
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
static void grow(AvantHash *h) {
  int32_t ncap = h->cap ? h->cap * 2 : 8;
  /* A compact table with at least half its entries deleted: the live
   * ones are compacted into a table of the same size. */
  if (is_compact(h) && h->cap && h->size < fill_max(h->cap) / 2) {
    ncap = h->cap;
  }
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
    uint8_t st = ent_state(h, e);
    if (!st_full(st)) {
      continue;
    }
    move_entry(&dst, h, okeys, ovals, st, ent_hash(h, e), e);
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

/* Removes the entry E a lookup found: its position's cluster slides back,
 * so no tombstone is left. */
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
  uint32_t *ix = index_of(h);
  while ((int32_t)ix[i] - 1 != e) {
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
      int32_t ke = (int32_t)ix[k] - 1;
      if (ke < 0) {
        return;
      }
      int32_t home = (int32_t)(ent_hash(h, ke) & (uint32_t)mask);
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

/* Open: the entry's position is emptied and later entries of its cluster
 * move back into it.
 * gc-lint: a store helper (erase makes the call on the two buffers first, RC5). */
static void erase_open(AvantHash *h, int32_t e, int32_t mask, int str) {
  int32_t i = e;
  uint8_t *st = states_of(h);
  uint32_t *hs = hashes_of(h);
  size_t kw = kw_of(h);
  for (;;) {
    int32_t k;
    st[i] = 0;
    if (str) {
      ((void **)h->keys)[i] = NULL;
    }
    if (val_refs(h)) {
      if (val_is_ref(h)) {
        ((void **)h->vals)[i] = NULL;
      } else {
        memset(val_at(h, h->vals, i), 0, (size_t)1 << h->vshift);
      }
    }
    k = (i + 1) & mask;
    for (;;) {
      if (!st_full(st[k])) {
        return;
      }
      int32_t home = (int32_t)(ent_hash(h, k) & (uint32_t)mask);
      int move = (i <= k) ? (home <= i || home > k) : (home <= i && home > k);
      if (move) {
        st[i] = st[k];
        if (str) {
          hs[i] = hs[k];
        }
        cp_w((char *)h->keys + (size_t)i * kw, (char *)h->keys + (size_t)k * kw, (unsigned)kw);
        cp_val(h, val_at(h, h->vals, i), val_at(h, h->vals, k));
        i = k;
        break;
      }
      k = (k + 1) & mask;
    }
  }
}

/* ---- the entries ---- */

/* gc-lint: holds nothing (its input is an integer). */
void *avant_hash_create(uint32_t desc) {
  AvantHash *h = (AvantHash *)avant_alloc(sizeof(AvantHash), AVANT_TYPE_HASH);
  h->desc = desc;
  h->seed = g_hash_seed;
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
  h->seed = g_hash_seed;
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
  if (!h || !h->cap) {
    return;
  }
  end = walk_end(h);
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
  size_t n = avant_str_n(key);
  if (!h || !find_bytes(h, key, n, hash_bytes(key, n, h->seed), &e, &pos)) {
    *found = 0;
    return (void *)g_hash_none;
  }
  *found = 1;
  return val_at(h, h->vals, e);
}

/* Whether one more entry needs a larger (or compacted) table first. */
static int needs_grow(const AvantHash *h) {
  if (is_compact(h)) {
    return h->used >= fill_max(h->cap);
  }
  return (int64_t)h->size * 4 >= (int64_t)h->cap * 3;
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
  e = claim(h, pos, 1);
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
      e = claim(h, pos, 1);
      ikey_put(h, h->keys, e, key);
      zero_val(h, val_at(h, h->vals, e));
      return val_at(h, h->vals, e);
    }
  }
  return put_ik_slow(h, key, hv, found, grow_first);
}

static void *put_sk_slow(AvantHash *h, const char *key, size_t n, uint32_t hv, int32_t *found, int grow_first) {
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
  e = claim(h, pos, occ_mark(key, n));
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
      e = claim(h, pos, occ_mark(key, n));
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
        a = xk_mix(a, hash_bytes(str, avant_str_n(str), h->seed));
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
      if (sa != sb && !bytes_eq(sa, avant_str_n(sa), sb, avant_str_n(sb))) {
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
  const uint32_t *hs = hashes_of(h);
  const uint32_t *ix = index_of(h);
  const char *keys = (const char *)h->keys;
  size_t kw = hx(h)->kw;
  for (n = 0; n < cap; n++) {
    int32_t e = (int32_t)ix[i] - 1;
    if (e < 0) {
      *pos = i;
      return 0;
    }
    if (hs[e] == hv && xkey_eq(h, keys + (size_t)e * kw, k)) {
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
  e = claim(h, pos, 1);
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

static void slot_hash(const AvantHash *h) {
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

void *avant_hash_put_sk(void *hp, const char *key) {
  return put_sk((AvantHash *)hp, key, NULL, 1);
}

/* A get fused with the set that follows it (compiler/mir/upsert.av), and
 * `inc`: room is made only for an absent key. */
void *avant_hash_upsert_ik(void *hp, avant_int key, int32_t *found) {
  return put_ik((AvantHash *)hp, key, found, 0);
}

void *avant_hash_upsert_sk(void *hp, const char *key, int32_t *found) {
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
  size_t n = avant_str_n(key);
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
      if (!ent_live(h, e)) {
        continue;
      }
      cp_n((char *)buf + (size_t)k * w, keys ? (char *)h->keys + (size_t)e * w : (char *)val_at(h, h->vals, e), w);
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
  size_t len = avant_str_n(s);
  if (start < 0 || stop < start || (size_t)stop > len) {
    avant_trap_slice(start, stop, (avant_int)len);
  }
  *out = (s ? s : "") + start;
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

/* RC1/RC2 (avant_gc.h): grow and the key's allocation may collect, so
 * the hash and the text are handles and the slice's pointer is formed
 * again from the text after each (its offset is an integer). */
avant_int avant_hash_inc_slice(void *hp, const char *s, avant_int start, avant_int stop) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &s);
  last_key_reset();
  if (!h || !is_str_int(h)) {
    return 0;
  }
  const char *p = NULL;
  size_t n = 0;
  if (!s) {
    s = "";
  }
  check_slice(s, start, stop, &p, &n);
  size_t off = (size_t)(p - s);
  uint32_t hv = hash_bytes(p, n, h->seed);
  int32_t e = 0;
  int32_t pos = 0;
  if (find_bytes(h, p, n, hv, &e, &pos)) {
    avant_int c;
    avant_int *v = (avant_int *)val_at(h, h->vals, e);
    if (__builtin_add_overflow(*v, 1, &c)) {
      avant_trap_overflow();
    }
    *v = c;
    g_last_key = skey_at(h->keys, e);
    return c;
  }
  ensure_cap(h);
  p = s + off;
  (void)find_bytes(h, p, n, hv, &e, &pos);
  char *key = avant_str_alloc((uint64_t)n);
  p = s + off;
  if (n) {
    memcpy(key, p, n);
  }
  key[n] = 0;
  avant_str_tag(key);
  e = claim(h, pos, (uint8_t)(1u | HASH_HEAP_LEN));
  store_str_key(h, e, key, hv);
  *(avant_int *)val_at(h, h->vals, e) = 1;
  g_last_key = key;
  return 1;
}

const char *avant_hash_last_key(void) {
  return g_last_key ? g_last_key : "";
}

const char *avant_hash_get_str_slice(void *hp, const char *s, avant_int start, avant_int stop, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  if (found) {
    *found = 0;
  }
  if (!h || !is_str_str(h)) {
    return "";
  }
  const char *p = NULL;
  size_t n = 0;
  check_slice(s, start, stop, &p, &n);
  trim_slice(&p, &n);
  int32_t e = 0;
  int32_t pos = 0;
  if (!find_bytes(h, p, n, hash_bytes(p, n, h->seed), &e, &pos)) {
    return "";
  }
  if (found) {
    *found = 1;
  }
  return *(const char **)val_at(h, h->vals, e);
}

static uint32_t hash_concat_bytes(const char *a, size_t na, const char *b, size_t nb, uint32_t seed) {
  uint32_t h = 2166136261u ^ seed;
  size_t i;
  if (!a) {
    na = 0;
  }
  if (!b) {
    nb = 0;
  }
  for (i = 0; i < na; i++) {
    h ^= (uint8_t)a[i];
    h *= 16777619u;
  }
  for (i = 0; i < nb; i++) {
    h ^= (uint8_t)b[i];
    h *= 16777619u;
  }
  return h;
}

static int bytes_eq_concat(const char *slot, size_t ns, const char *a, size_t na, const char *b, size_t nb) {
  if (!slot) {
    slot = "";
  }
  if (!a) {
    a = "";
    na = 0;
  }
  if (!b) {
    b = "";
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
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  int32_t mask;
  int32_t i;
  int32_t n;
  last_key_reset();
  if (found) {
    *found = 0;
  }
  if (!h || !is_str_int(h) || !h->cap) {
    return 0;
  }
  uint32_t hv = hash_concat_bytes(a, na, b, nb, h->seed);
  mask = h->cap - 1;
  i = (int32_t)(hv & (uint32_t)mask);
  for (n = 0; n < h->cap; n++) {
    int32_t e = pos_entry(h, i);
    if (e < 0) {
      return 0;
    }
    if (hashes_of(h)[e] == hv) {
      const char *slot = skey_at(h->keys, e);
      if (bytes_eq_concat(slot, skey_n(slot, ent_state(h, e)), a, na, b, nb)) {
        if (found) {
          *found = 1;
        }
        g_last_key = slot;
        return *(avant_int *)val_at(h, h->vals, e);
      }
    }
    i = (i + 1) & mask;
  }
  return 0;
}

/* ---- the names the seed's compiler calls ----
 *
 * The compiler B, built by the seed release, deletes through these two
 * names, and links this runtime. They go when a release that emits
 * avant_hash_del_sk and avant_hash_del_ik is the seed. */

void avant_hash_del(void *hp, const char *key) {
  avant_hash_del_sk(hp, key);
}

void avant_hash_del_i32k(void *hp, avant_int key) {
  avant_hash_del_ik(hp, key);
}
