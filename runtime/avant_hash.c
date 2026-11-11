#include "avant_rt.h"
#include "avant_gc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  void *keys;
  void *vals;
  uint8_t *state;
  int32_t size;
  int32_t cap;
  int32_t key_kind;
  int32_t val_kind;
} AvantHash;

static __thread const char *g_last_key;
static __thread int g_last_key_rooted;

/* g_last_key names a heap key (or a static string), so it is a root of
 * this thread's collections (avant_gc_add_root, Stage 28 W14b2),
 * registered the first time a helper that sets it runs. Every write of
 * g_last_key comes after this reset in the same helper. */
static void last_key_reset(void) {
  if (!g_last_key_rooted) {
    g_last_key_rooted = 1;
    avant_gc_add_root((void **)&g_last_key);
  }
  g_last_key = "";
}

/* state bytes, then one uint32 hash per slot. cap is 0 or a power of two. */
static uint64_t state_bytes(int32_t cap) {
  return (uint64_t)cap + (uint64_t)cap * (uint64_t)sizeof(uint32_t);
}

static uint32_t *hashes_of(const AvantHash *h) {
  return (uint32_t *)(h->state + h->cap);
}

static uint32_t hash_bytes(const char *s, size_t n) {
  uint32_t h = 2166136261u;
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

static uint32_t hash_str(const char *s) {
  return hash_bytes(s, avant_str_n(s));
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

static uint32_t hash_key(const AvantHash *h, int64_t key_bits) {
  if (h->key_kind == AVANT_HASH_KEY_STR) {
    return hash_str((const char *)(uintptr_t)key_bits);
  }
  return hash_i32((int32_t)key_bits);
}

static int key_eq(const AvantHash *h, int64_t slot_bits, int64_t key_bits) {
  if (h->key_kind == AVANT_HASH_KEY_STR) {
    const char *a = (const char *)(uintptr_t)slot_bits;
    const char *b = (const char *)(uintptr_t)key_bits;
    return bytes_eq(a, avant_str_n(a), b, avant_str_n(b));
  }
  return (int32_t)slot_bits == (int32_t)key_bits;
}

static int64_t load_key(const AvantHash *h, int32_t i) {
  if (h->key_kind == AVANT_HASH_KEY_STR) {
    return (int64_t)(uintptr_t)((void **)h->keys)[i];
  }
  return (int64_t)((int32_t *)h->keys)[i];
}

/* Occupied slots are state 1. Bit 0x80 means the key is a heap string
 * whose length is the header size. A probe then reads that length.
 * avant_str_n confirms an unknown pointer with a block walk, and that
 * walk was the cache. The bit is set only when the header length already
 * agreed with avant_str_n, so a C string keeps the slow check. */
#define AVANT_HASH_HEAP_LEN 0x80u

static int slot_full(uint8_t st) {
  return (st & 0x7fu) == 1;
}

static size_t slot_n(const AvantHash *h, int32_t i) {
  const char *s = (const char *)(uintptr_t)load_key(h, i);
  if (h->state[i] & AVANT_HASH_HEAP_LEN) {
    return (size_t)avant_header((void *)s)->size - 1;
  }
  return avant_str_n(s);
}

static uint8_t occ_mark(const char *key, size_t n) {
  if (key && ((uintptr_t)key & 7u) == 0) {
    AvantHeader *h = avant_header((void *)key);
    if ((h->flags & AVANT_FLAG_HEAP) && h->type_id == AVANT_TYPE_BYTES && h->size > 0 &&
        (size_t)h->size - 1 == n) {
      return (uint8_t)(1u | AVANT_HASH_HEAP_LEN);
    }
  }
  return 1;
}

static void store_key(AvantHash *h, int32_t i, int64_t key_bits) {
  if (h->key_kind == AVANT_HASH_KEY_STR) {
    ((void **)h->keys)[i] = (void *)(uintptr_t)key_bits;
  } else {
    ((int32_t *)h->keys)[i] = (int32_t)key_bits;
  }
}

static void store_val_i32(AvantHash *h, int32_t i, int32_t val) {
  ((int32_t *)h->vals)[i] = val;
}

static void store_val_str(AvantHash *h, int32_t i, const char *val) {
  ((const char **)h->vals)[i] = val;
}

static int lookup(AvantHash *h, int64_t key_bits, uint32_t hv, int32_t *out) {
  if (!h->cap) {
    return 0;
  }
  int32_t mask = h->cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  int32_t tomb = -1;
  int32_t n;
  for (n = 0; n < h->cap; n++) {
    uint8_t st = h->state[i];
    if (st == 0) {
      *out = tomb >= 0 ? tomb : i;
      return 0;
    }
    if (st == 2) {
      if (tomb < 0) {
        tomb = i;
      }
    } else if (key_eq(h, load_key(h, i), key_bits)) {
      *out = i;
      return 1;
    }
    i = (i + 1) & mask;
  }
  *out = tomb >= 0 ? tomb : 0;
  return 0;
}

static int lookup_bytes(AvantHash *h, const char *s, size_t n, uint32_t hv, int32_t *out) {
  if (!h->cap) {
    return 0;
  }
  int32_t mask = h->cap - 1;
  int32_t i = (int32_t)(hv & (uint32_t)mask);
  int32_t tomb = -1;
  int32_t k;
  for (k = 0; k < h->cap; k++) {
    uint8_t st = h->state[i];
    if (st == 0) {
      *out = tomb >= 0 ? tomb : i;
      return 0;
    }
    if (st == 2) {
      if (tomb < 0) {
        tomb = i;
      }
    } else if (slot_full(st) && hashes_of(h)[i] == hv) {
      const char *slot = (const char *)(uintptr_t)load_key(h, i);
      if (bytes_eq(slot, slot_n(h, i), s, n)) {
        *out = i;
        return 1;
      }
    }
    i = (i + 1) & mask;
  }
  *out = tomb >= 0 ? tomb : 0;
  return 0;
}

static int lookup_str(AvantHash *h, const char *key, int32_t *out) {
  size_t n = avant_str_n(key);
  return lookup_bytes(h, key, n, hash_bytes(key, n), out);
}

static void insert_raw(AvantHash *dst, int64_t key_bits, int32_t val_i32, const char *val_str) {
  int32_t i = 0;
  const char *skey = NULL;
  size_t sn = 0;
  uint32_t hv = 0;
  if (dst->key_kind == AVANT_HASH_KEY_STR) {
    skey = (const char *)(uintptr_t)key_bits;
    sn = avant_str_n(skey);
    hv = hash_bytes(skey, sn);
    lookup_bytes(dst, skey, sn, hv, &i);
  } else {
    hv = hash_key(dst, key_bits);
    lookup(dst, key_bits, hv, &i);
  }
  int full = slot_full(dst->state[i]);
  void *prev = NULL;
  if (full && dst->key_kind == AVANT_HASH_KEY_STR) {
    prev = (void *)(uintptr_t)load_key(dst, i);
  }
  store_key(dst, i, key_bits);
  /* A mature key or value buffer is not walked from the roots. The
   * slot just stored may point at a young string. */
  if (dst->key_kind == AVANT_HASH_KEY_STR && dst->keys) {
    avant_barrier_slot(dst->keys, &((void **)dst->keys)[i]);
  }
  if (dst->val_kind == AVANT_HASH_VAL_STR) {
    store_val_str(dst, i, val_str);
    if (dst->vals) {
      avant_barrier_slot(dst->vals, &((void **)dst->vals)[i]);
    }
  } else {
    store_val_i32(dst, i, val_i32);
  }
  if (dst->state && dst->key_kind == AVANT_HASH_KEY_STR) {
    hashes_of(dst)[i] = hv;
  }
  if (!full) {
    dst->state[i] = dst->key_kind == AVANT_HASH_KEY_STR ? occ_mark(skey, sn) : 1;
    dst->size += 1;
  } else if (dst->key_kind == AVANT_HASH_KEY_STR && prev != (void *)(uintptr_t)key_bits) {
    dst->state[i] = occ_mark(skey, sn);
  }
}

static uint32_t key_type_id(const AvantHash *h) {
  return h->key_kind == AVANT_HASH_KEY_STR ? AVANT_TYPE_PTRS : AVANT_TYPE_BYTES;
}

static uint32_t val_type_id(const AvantHash *h) {
  return h->val_kind == AVANT_HASH_VAL_STR ? AVANT_TYPE_PTRS : AVANT_TYPE_BYTES;
}

static uint64_t key_bytes(const AvantHash *h, int32_t cap) {
  size_t elem = h->key_kind == AVANT_HASH_KEY_STR ? sizeof(void *) : sizeof(int32_t);
  return (uint64_t)cap * (uint64_t)elem;
}

static uint64_t val_bytes(const AvantHash *h, int32_t cap) {
  size_t elem = h->val_kind == AVANT_HASH_VAL_STR ? sizeof(void *) : sizeof(int32_t);
  return (uint64_t)cap * (uint64_t)elem;
}

/* RC1 (avant_gc.h): each allocation may collect, so the hash and the new
 * buffers not yet published are handles, and the old buffers are read
 * from the hash after the last allocation (RC2). */
static void grow(AvantHash *h) {
  int32_t ncap = h->cap ? h->cap * 2 : 8;
  void *nkeys = NULL;
  uint8_t *nstate = NULL;
  void *nvals = NULL;
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &nkeys);
  AVANT_GC_ROOT(2, &nstate);
  nkeys = avant_alloc(key_bytes(h, ncap), key_type_id(h));
  nstate = (uint8_t *)avant_alloc(state_bytes(ncap), AVANT_TYPE_BYTES);
  nvals = avant_alloc(val_bytes(h, ncap), val_type_id(h));

  int32_t ocap = h->cap;
  void *okeys = h->keys;
  void *ovals = h->vals;
  uint8_t *ostate = h->state;
  AvantHash dst = *h;
  dst.keys = nkeys;
  dst.vals = nvals;
  dst.state = nstate;
  dst.cap = ncap;
  dst.size = 0;

  int32_t i;
  for (i = 0; i < ocap; i++) {
    if (!slot_full(ostate[i])) {
      continue;
    }
    int64_t kb = h->key_kind == AVANT_HASH_KEY_STR
                     ? (int64_t)(uintptr_t)((void **)okeys)[i]
                     : (int64_t)((int32_t *)okeys)[i];
    if (h->val_kind == AVANT_HASH_VAL_STR) {
      insert_raw(&dst, kb, 0, ((const char **)ovals)[i]);
    } else {
      insert_raw(&dst, kb, ((int32_t *)ovals)[i], NULL);
    }
  }

  avant_barrier(h);
  h->keys = nkeys;
  h->vals = nvals;
  h->state = nstate;
  h->cap = ncap;
  h->size = dst.size;
  /* The hash no longer names its old buffers: large ones go back now
   * unless a root still names them or they are mature with references
   * (Stage 28 W18M, O109). */
  if (okeys) {
    (void)avant_gc_large_drop(okeys, 2);
  }
  if (ovals) {
    (void)avant_gc_large_drop(ovals, 2);
  }
  if (ostate) {
    (void)avant_gc_large_drop(ostate, 2);
  }
}

/* gc-lint: holds nothing (grow is its last statement and roots the hash itself). */
static void ensure_cap(AvantHash *h) {
  if (h->size * 4 >= h->cap * 3) {
    grow(h);
  }
}

/* A key slice traps like avant_str_slice (Stage 29 W4b1, D295 R4). */
static void check_slice(const char *s, int32_t start, int32_t stop, const char **out, size_t *n) {
  size_t len = avant_str_n(s);
  if (start < 0 || stop < start || (size_t)stop > len) {
    avant_trap_slice(start, stop, (int32_t)len);
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

/* gc-lint: holds nothing (its inputs are integers). */
void *avant_hash_new(int32_t key_kind, int32_t val_kind) {
  AvantHash *h = (AvantHash *)avant_alloc(sizeof(AvantHash), AVANT_TYPE_HASH);
  h->key_kind = key_kind;
  h->val_kind = val_kind;
  return h;
}

int32_t avant_hash_size(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  return h ? h->size : 0;
}

void avant_hash_clear(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i;
  if (!h) {
    return;
  }
  if (h->key_kind == AVANT_HASH_KEY_STR && h->keys) {
    for (i = 0; i < h->cap; i++) {
      ((void **)h->keys)[i] = NULL;
    }
  }
  if (h->val_kind == AVANT_HASH_VAL_STR && h->vals) {
    for (i = 0; i < h->cap; i++) {
      ((void **)h->vals)[i] = NULL;
    }
  }
  if (h->state && h->cap > 0) {
    memset(h->state, 0, (size_t)state_bytes(h->cap));
  }
  h->size = 0;
}

/* The setters (RC1, avant_gc.h; Stage 28 W16a: they were W15a1's named
 * safepoints): grow may collect, so the hash and the key and value
 * arguments are handles across it. */
void avant_hash_set_i32(void *hp, const char *key, int32_t val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)(uintptr_t)key, val, NULL);
  }
}

void avant_hash_set_str(void *hp, const char *key, const char *val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(3);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  AVANT_GC_ROOT(2, &val);
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)(uintptr_t)key, 0, val);
  }
}

void avant_hash_set_i32k_i32(void *hp, int32_t key, int32_t val) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)key, val, NULL);
  }
}

int32_t avant_hash_get_i32(void *hp, const char *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h || !lookup_str(h, key, &i)) {
    if (found) {
      *found = 0;
    }
    return 0;
  }
  if (found) {
    *found = 1;
  }
  return ((int32_t *)h->vals)[i];
}

const char *avant_hash_get_str(void *hp, const char *key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h || !lookup_str(h, key, &i)) {
    if (found) {
      *found = 0;
    }
    return "";
  }
  if (found) {
    *found = 1;
  }
  return ((const char **)h->vals)[i];
}

int32_t avant_hash_get_i32k_i32(void *hp, int32_t key, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h || !lookup(h, (int64_t)key, hash_i32(key), &i)) {
    if (found) {
      *found = 0;
    }
    return 0;
  }
  if (found) {
    *found = 1;
  }
  return ((int32_t *)h->vals)[i];
}

/* Linear-probing deletion slides later cluster entries back into the hole
 * so a removed key does not leave a tombstone that later probes must walk. */
static void erase_at(AvantHash *h, int32_t i) {
  int32_t mask = h->cap - 1;
  uint32_t *hs = hashes_of(h);
  h->size -= 1;
  for (;;) {
    int32_t k;
    h->state[i] = 0;
    if (h->key_kind == AVANT_HASH_KEY_STR) {
      ((void **)h->keys)[i] = NULL;
    }
    k = (i + 1) & mask;
    for (;;) {
      uint32_t hv;
      int32_t home;
      int move;
      if (!slot_full(h->state[k])) {
        return;
      }
      if (h->key_kind == AVANT_HASH_KEY_STR) {
        hv = hs[k];
      } else {
        hv = hash_i32(((int32_t *)h->keys)[k]);
      }
      home = (int32_t)(hv & (uint32_t)mask);
      move = (i <= k) ? (home <= i || home > k) : (home <= i && home > k);
      if (move) {
        h->state[i] = h->state[k];
        hs[i] = hs[k];
        if (h->key_kind == AVANT_HASH_KEY_STR) {
          ((void **)h->keys)[i] = ((void **)h->keys)[k];
        } else {
          ((int32_t *)h->keys)[i] = ((int32_t *)h->keys)[k];
        }
        if (h->val_kind == AVANT_HASH_VAL_STR) {
          ((const char **)h->vals)[i] = ((const char **)h->vals)[k];
        } else {
          ((int32_t *)h->vals)[i] = ((int32_t *)h->vals)[k];
        }
        i = k;
        break;
      }
      k = (k + 1) & mask;
    }
  }
}

void avant_hash_del(void *hp, const char *key) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h || !h->cap || !lookup_str(h, key, &i)) {
    return;
  }
  erase_at(h, i);
}

void avant_hash_del_i32k(void *hp, int32_t key) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h || !h->cap || !lookup(h, (int64_t)key, hash_i32(key), &i)) {
    return;
  }
  erase_at(h, i);
}

/* RC1/RC2 (avant_gc.h; Stage 28 W16a, it was a named safepoint): grow
 * and the key's allocation may collect, so the hash and the text are
 * handles and the slice's pointer is formed again from the text after
 * each (its offset is an integer). */
int32_t avant_hash_inc_slice(void *hp, const char *s, int32_t start, int32_t stop) {
  AvantHash *h = (AvantHash *)hp;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &s);
  last_key_reset();
  if (!h || h->key_kind != AVANT_HASH_KEY_STR || h->val_kind != AVANT_HASH_VAL_I32) {
    return 0;
  }
  const char *p = NULL;
  size_t n = 0;
  if (!s) {
    s = "";
  }
  check_slice(s, start, stop, &p, &n);
  size_t off = (size_t)(p - s);
  uint32_t hv = hash_bytes(p, n);
  int32_t i = 0;
  if (lookup_bytes(h, p, n, hv, &i)) {
    int32_t c;
    if (__builtin_add_overflow(((int32_t *)h->vals)[i], 1, &c)) {
      avant_trap_overflow();
    }
    ((int32_t *)h->vals)[i] = c;
    g_last_key = (const char *)(uintptr_t)load_key(h, i);
    return c;
  }
  ensure_cap(h);
  p = s + off;
  lookup_bytes(h, p, n, hv, &i);
  char *key = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  p = s + off;
  if (n) {
    memcpy(key, p, n);
  }
  key[n] = 0;
  avant_str_tag(key);
  store_key(h, i, (int64_t)(uintptr_t)key);
  /* The key is young; a mature key buffer is not walked from the roots (C34). */
  avant_barrier_slot(h->keys, &((void **)h->keys)[i]);
  store_val_i32(h, i, 1);
  hashes_of(h)[i] = hv;
  if (!slot_full(h->state[i])) {
    h->state[i] = (uint8_t)(1u | AVANT_HASH_HEAP_LEN);
    h->size += 1;
  }
  g_last_key = key;
  return 1;
}

const char *avant_hash_last_key(void) {
  return g_last_key ? g_last_key : "";
}

const char *avant_hash_get_str_slice(void *hp, const char *s, int32_t start, int32_t stop, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  if (found) {
    *found = 0;
  }
  if (!h || h->key_kind != AVANT_HASH_KEY_STR || h->val_kind != AVANT_HASH_VAL_STR) {
    return "";
  }
  const char *p = NULL;
  size_t n = 0;
  check_slice(s, start, stop, &p, &n);
  trim_slice(&p, &n);
  int32_t i = 0;
  if (!lookup_bytes(h, p, n, hash_bytes(p, n), &i)) {
    return "";
  }
  if (found) {
    *found = 1;
  }
  return ((const char **)h->vals)[i];
}

static uint32_t hash_concat_bytes(const char *a, size_t na, const char *b, size_t nb) {
  uint32_t h = 2166136261u;
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

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArrayBox;

/* The keys or the values of H as a new Array (RC1, avant_gc.h; Stage 28
 * W16a): the array and its buffer are allocated first, with the hash
 * and the array as handles, and filled from the hash after the last
 * allocation. Before W16a the elements were copied into a malloc'ed
 * items[] first, and a collection at the array's allocation could move
 * them under it (RC2). STR: the elements are strings (a pointer buffer,
 * barriered, RC3); else Int32s. */
static void *hash_collect(AvantHash *h, int keys, int str) {
  AvantArrayBox *a = NULL;
  int32_t n = h ? h->size : 0;
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &a);
  a = (AvantArrayBox *)avant_alloc(sizeof(AvantArrayBox), AVANT_TYPE_ARRAY_OBJ);
  if (n > 0) {
    void *buf = avant_alloc((uint64_t)n * (str ? sizeof(char *) : sizeof(int32_t)),
                            str ? AVANT_TYPE_PTRS : AVANT_TYPE_BYTES);
    int32_t i;
    int32_t k = 0;
    for (i = 0; i < h->cap && k < n; i++) {
      if (!slot_full(h->state[i])) {
        continue;
      }
      if (str) {
        ((char **)buf)[k++] = keys ? (char *)(uintptr_t)load_key(h, i) : ((char **)h->vals)[i];
      } else {
        ((int32_t *)buf)[k++] = keys ? (int32_t)load_key(h, i) : ((int32_t *)h->vals)[i];
      }
    }
    if (str) {
      avant_barrier(buf);
    }
    avant_barrier(a);
    a->buf = buf;
    a->size = k;
    a->cap = n;
  }
  return a;
}

int32_t avant_hash_has_str(void *hp, const char *key) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    return 0;
  }
  return lookup_str(h, key, &i) ? 1 : 0;
}

int32_t avant_hash_has_i32k(void *hp, int32_t key) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    return 0;
  }
  return lookup(h, (int64_t)key, hash_i32(key), &i) ? 1 : 0;
}

int32_t avant_hash_inc_str(void *hp, const char *key, int32_t by) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    return 0;
  }
  /* RC1: grow may collect; the hash and the key are handles. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  if (lookup_str(h, key, &i)) {
    int32_t c;
    if (__builtin_add_overflow(((int32_t *)h->vals)[i], by, &c)) {
      avant_trap_overflow();
    }
    ((int32_t *)h->vals)[i] = c;
    return c;
  }
  ensure_cap(h);
  lookup_str(h, key, &i);
  store_key(h, i, (int64_t)(uintptr_t)key);
  if (h->keys) {
    avant_barrier_slot(h->keys, &((void **)h->keys)[i]);
  }
  store_val_i32(h, i, by);
  if (h->state && h->key_kind == AVANT_HASH_KEY_STR) {
    hashes_of(h)[i] = hash_str(key);
  }
  if (!slot_full(h->state[i])) {
    h->state[i] = occ_mark(key, avant_str_n(key));
    h->size += 1;
  }
  return by;
}

int32_t avant_hash_inc_i32k(void *hp, int32_t key, int32_t by) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    return 0;
  }
  /* RC1: grow may collect; the hash is a handle. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (lookup(h, (int64_t)key, hash_i32(key), &i)) {
    int32_t c;
    if (__builtin_add_overflow(((int32_t *)h->vals)[i], by, &c)) {
      avant_trap_overflow();
    }
    ((int32_t *)h->vals)[i] = c;
    return c;
  }
  ensure_cap(h);
  lookup(h, (int64_t)key, hash_i32(key), &i);
  store_key(h, i, (int64_t)key);
  store_val_i32(h, i, by);
  if (!slot_full(h->state[i])) {
    h->state[i] = 1;
    h->size += 1;
  }
  return by;
}

/* Get-or-insert (Stage 28 W18S, O49): the address of key's value, with key
 * inserted holding init when it was absent (*found then 0, else 1; found
 * may be NULL). The compiler calls it instead of a get where every path
 * from the get reaches a set of the same key before any other call
 * (compiler/mir/upsert.av), and reads and writes the value through the
 * address: the one lookup serves both. A growth here may collect; the
 * address stays valid until the caller's next call, and there is none
 * before its last use. Int values only, so the stores need no barrier.
 * A nil hash gives a per-thread scratch slot (a get then reads 0 and a
 * set stores nothing, as before). */
static __thread int32_t g_upsert_scratch;

int32_t *avant_hash_upsert_i32k(void *hp, int32_t key, int32_t init, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    if (found) {
      *found = 0;
    }
    g_upsert_scratch = init;
    return &g_upsert_scratch;
  }
  /* RC1: grow may collect; the hash is a handle. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &h);
  if (lookup(h, (int64_t)key, hash_i32(key), &i)) {
    if (found) {
      *found = 1;
    }
    return &((int32_t *)h->vals)[i];
  }
  if (found) {
    *found = 0;
  }
  ensure_cap(h);
  lookup(h, (int64_t)key, hash_i32(key), &i);
  store_key(h, i, (int64_t)key);
  store_val_i32(h, i, init);
  if (!slot_full(h->state[i])) {
    h->state[i] = 1;
    h->size += 1;
  }
  return &((int32_t *)h->vals)[i];
}

int32_t *avant_hash_upsert_i32(void *hp, const char *key, int32_t init, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    if (found) {
      *found = 0;
    }
    g_upsert_scratch = init;
    return &g_upsert_scratch;
  }
  /* RC1: grow may collect; the hash and the key are handles. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &h);
  AVANT_GC_ROOT(1, &key);
  if (lookup_str(h, key, &i)) {
    if (found) {
      *found = 1;
    }
    return &((int32_t *)h->vals)[i];
  }
  if (found) {
    *found = 0;
  }
  ensure_cap(h);
  lookup_str(h, key, &i);
  store_key(h, i, (int64_t)(uintptr_t)key);
  if (h->keys) {
    avant_barrier_slot(h->keys, &((void **)h->keys)[i]);
  }
  store_val_i32(h, i, init);
  if (h->state && h->key_kind == AVANT_HASH_KEY_STR) {
    hashes_of(h)[i] = hash_str(key);
  }
  if (!slot_full(h->state[i])) {
    h->state[i] = occ_mark(key, avant_str_n(key));
    h->size += 1;
  }
  return &((int32_t *)h->vals)[i];
}

int32_t avant_hash_get_or_i32(void *hp, const char *key, int32_t fallback) {
  int32_t found = 0;
  int32_t v = avant_hash_get_i32(hp, key, &found);
  return found ? v : fallback;
}

const char *avant_hash_get_or_str(void *hp, const char *key, const char *fallback) {
  int32_t found = 0;
  const char *v = avant_hash_get_str(hp, key, &found);
  return found ? v : (fallback ? fallback : "");
}

int32_t avant_hash_get_or_i32k_i32(void *hp, int32_t key, int32_t fallback) {
  int32_t found = 0;
  int32_t v = avant_hash_get_i32k_i32(hp, key, &found);
  return found ? v : fallback;
}

void *avant_hash_keys_str(void *hp) {
  return hash_collect((AvantHash *)hp, 1, 1);
}

void *avant_hash_keys_i32(void *hp) {
  return hash_collect((AvantHash *)hp, 1, 0);
}

void *avant_hash_vals_str(void *hp) {
  return hash_collect((AvantHash *)hp, 0, 1);
}

void *avant_hash_vals_i32(void *hp) {
  return hash_collect((AvantHash *)hp, 0, 0);
}

int32_t avant_hash_get_concat(void *hp, const char *a, const char *b, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  uint32_t hv = hash_concat_bytes(a, na, b, nb);
  int32_t mask;
  int32_t i;
  int32_t n;
  last_key_reset();
  if (found) {
    *found = 0;
  }
  if (!h || h->key_kind != AVANT_HASH_KEY_STR || h->val_kind != AVANT_HASH_VAL_I32 || !h->cap) {
    return 0;
  }
  mask = h->cap - 1;
  i = (int32_t)(hv & (uint32_t)mask);
  for (n = 0; n < h->cap; n++) {
    uint8_t st = h->state[i];
    if (st == 0) {
      return 0;
    }
    if (slot_full(st) && hashes_of(h)[i] == hv) {
      const char *slot = (const char *)(uintptr_t)load_key(h, i);
      if (bytes_eq_concat(slot, slot_n(h, i), a, na, b, nb)) {
        if (found) {
          *found = 1;
        }
        g_last_key = slot;
        return ((int32_t *)h->vals)[i];
      }
    }
    i = (i + 1) & mask;
  }
  return 0;
}
