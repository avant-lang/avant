#include "avant_rt.h"

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
    avant_barrier(dst->keys);
  }
  if (dst->val_kind == AVANT_HASH_VAL_STR) {
    store_val_str(dst, i, val_str);
    if (dst->vals) {
      avant_barrier(dst->vals);
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

static void grow(AvantHash *h) {
  avant_gc_defer_enter();
  int32_t ocap = h->cap;
  void *okeys = h->keys;
  void *ovals = h->vals;
  uint8_t *ostate = h->state;
  int32_t ncap = ocap ? ocap * 2 : 8;
  void *nkeys = avant_alloc(key_bytes(h, ncap), key_type_id(h));
  uint8_t *nstate = (uint8_t *)avant_alloc(state_bytes(ncap), AVANT_TYPE_BYTES);
  void *nvals = avant_alloc(val_bytes(h, ncap), val_type_id(h));

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
  avant_gc_defer_leave();
}

static void ensure_cap(AvantHash *h) {
  if (h->size * 4 >= h->cap * 3) {
    grow(h);
  }
}

static void clamp_slice(const char *s, int32_t start, int32_t stop, const char **out, size_t *n) {
  size_t len = avant_str_n(s);
  int32_t i = start;
  if (i < 0) {
    i = 0;
  }
  if ((size_t)i > len) {
    i = (int32_t)len;
  }
  int32_t end = stop;
  if (end < i) {
    end = i;
  }
  if ((size_t)end > len) {
    end = (int32_t)len;
  }
  *out = (s ? s : "") + i;
  *n = (size_t)(end - i);
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

void avant_hash_set_i32(void *hp, const char *key, int32_t val) {
  avant_gc_defer_enter();
  AvantHash *h = (AvantHash *)hp;
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)(uintptr_t)key, val, NULL);
  }
  avant_gc_defer_leave();
}

void avant_hash_set_str(void *hp, const char *key, const char *val) {
  avant_gc_defer_enter();
  AvantHash *h = (AvantHash *)hp;
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)(uintptr_t)key, 0, val);
  }
  avant_gc_defer_leave();
}

void avant_hash_set_i32k_i32(void *hp, int32_t key, int32_t val) {
  avant_gc_defer_enter();
  AvantHash *h = (AvantHash *)hp;
  if (h) {
    ensure_cap(h);
    insert_raw(h, (int64_t)key, val, NULL);
  }
  avant_gc_defer_leave();
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

int32_t avant_hash_inc_slice(void *hp, const char *s, int32_t start, int32_t stop) {
  AvantHash *h = (AvantHash *)hp;
  g_last_key = "";
  if (!h || h->key_kind != AVANT_HASH_KEY_STR || h->val_kind != AVANT_HASH_VAL_I32) {
    return 0;
  }
  const char *p = NULL;
  size_t n = 0;
  clamp_slice(s, start, stop, &p, &n);
  uint32_t hv = hash_bytes(p, n);
  int32_t i = 0;
  avant_gc_defer_enter();
  if (lookup_bytes(h, p, n, hv, &i)) {
    int32_t c = ((int32_t *)h->vals)[i] + 1;
    ((int32_t *)h->vals)[i] = c;
    g_last_key = (const char *)(uintptr_t)load_key(h, i);
    avant_gc_defer_leave();
    return c;
  }
  ensure_cap(h);
  lookup_bytes(h, p, n, hv, &i);
  char *key = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n) {
    memcpy(key, p, n);
  }
  key[n] = 0;
  avant_str_tag(key);
  store_key(h, i, (int64_t)(uintptr_t)key);
  store_val_i32(h, i, 1);
  hashes_of(h)[i] = hv;
  if (!slot_full(h->state[i])) {
    h->state[i] = (uint8_t)(1u | AVANT_HASH_HEAP_LEN);
    h->size += 1;
  }
  g_last_key = key;
  avant_gc_defer_leave();
  return 1;
}

const char *avant_hash_last_key(void) {
  return g_last_key ? g_last_key : "";
}

void avant_hash_walk_tls(void (*fn)(void **slot)) {
  if (fn) {
    fn((void **)&g_last_key);
  }
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
  clamp_slice(s, start, stop, &p, &n);
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

static void *hash_array_str(char **items, int32_t n) {
  avant_gc_defer_enter();
  AvantArrayBox *a = (AvantArrayBox *)avant_alloc(sizeof(AvantArrayBox), AVANT_TYPE_ARRAY_OBJ);
  char **buf = NULL;
  int32_t i;
  if (n > 0) {
    buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
    for (i = 0; i < n; i++) {
      buf[i] = items[i];
    }
  }
  a->buf = buf;
  a->size = n;
  a->cap = n;
  avant_gc_defer_leave();
  return a;
}

static void *hash_array_i32(int32_t *items, int32_t n) {
  avant_gc_defer_enter();
  AvantArrayBox *a = (AvantArrayBox *)avant_alloc(sizeof(AvantArrayBox), AVANT_TYPE_ARRAY_OBJ);
  int32_t *buf = NULL;
  int32_t i;
  if (n > 0) {
    buf = (int32_t *)avant_alloc((uint64_t)n * sizeof(int32_t), AVANT_TYPE_BYTES);
    for (i = 0; i < n; i++) {
      buf[i] = items[i];
    }
  }
  a->buf = buf;
  a->size = n;
  a->cap = n;
  avant_gc_defer_leave();
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
  avant_gc_defer_enter();
  if (lookup_str(h, key, &i)) {
    int32_t c = ((int32_t *)h->vals)[i] + by;
    ((int32_t *)h->vals)[i] = c;
    avant_gc_defer_leave();
    return c;
  }
  ensure_cap(h);
  lookup_str(h, key, &i);
  store_key(h, i, (int64_t)(uintptr_t)key);
  if (h->keys) {
    avant_barrier(h->keys);
  }
  store_val_i32(h, i, by);
  if (h->state && h->key_kind == AVANT_HASH_KEY_STR) {
    hashes_of(h)[i] = hash_str(key);
  }
  if (!slot_full(h->state[i])) {
    h->state[i] = occ_mark(key, avant_str_n(key));
    h->size += 1;
  }
  avant_gc_defer_leave();
  return by;
}

int32_t avant_hash_inc_i32k(void *hp, int32_t key, int32_t by) {
  AvantHash *h = (AvantHash *)hp;
  int32_t i = 0;
  if (!h) {
    return 0;
  }
  avant_gc_defer_enter();
  if (lookup(h, (int64_t)key, hash_i32(key), &i)) {
    int32_t c = ((int32_t *)h->vals)[i] + by;
    ((int32_t *)h->vals)[i] = c;
    avant_gc_defer_leave();
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
  avant_gc_defer_leave();
  return by;
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
  AvantHash *h = (AvantHash *)hp;
  char **items = NULL;
  int32_t n = 0;
  int32_t i;
  void *r;
  if (!h || h->size <= 0) {
    return hash_array_str(NULL, 0);
  }
  items = (char **)malloc((size_t)h->size * sizeof(char *));
  for (i = 0; i < h->cap; i++) {
    if (slot_full(h->state[i])) {
      items[n++] = (char *)(uintptr_t)load_key(h, i);
    }
  }
  r = hash_array_str(items, n);
  free(items);
  return r;
}

void *avant_hash_keys_i32(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  int32_t *items = NULL;
  int32_t n = 0;
  int32_t i;
  void *r;
  if (!h || h->size <= 0) {
    return hash_array_i32(NULL, 0);
  }
  items = (int32_t *)malloc((size_t)h->size * sizeof(int32_t));
  for (i = 0; i < h->cap; i++) {
    if (slot_full(h->state[i])) {
      items[n++] = (int32_t)load_key(h, i);
    }
  }
  r = hash_array_i32(items, n);
  free(items);
  return r;
}

void *avant_hash_vals_str(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  char **items = NULL;
  int32_t n = 0;
  int32_t i;
  void *r;
  if (!h || h->size <= 0) {
    return hash_array_str(NULL, 0);
  }
  items = (char **)malloc((size_t)h->size * sizeof(char *));
  for (i = 0; i < h->cap; i++) {
    if (slot_full(h->state[i])) {
      items[n++] = ((char **)h->vals)[i];
    }
  }
  r = hash_array_str(items, n);
  free(items);
  return r;
}

void *avant_hash_vals_i32(void *hp) {
  AvantHash *h = (AvantHash *)hp;
  int32_t *items = NULL;
  int32_t n = 0;
  int32_t i;
  void *r;
  if (!h || h->size <= 0) {
    return hash_array_i32(NULL, 0);
  }
  items = (int32_t *)malloc((size_t)h->size * sizeof(int32_t));
  for (i = 0; i < h->cap; i++) {
    if (slot_full(h->state[i])) {
      items[n++] = ((int32_t *)h->vals)[i];
    }
  }
  r = hash_array_i32(items, n);
  free(items);
  return r;
}

int32_t avant_hash_get_concat(void *hp, const char *a, const char *b, int32_t *found) {
  AvantHash *h = (AvantHash *)hp;
  size_t na = avant_str_n(a);
  size_t nb = avant_str_n(b);
  uint32_t hv = hash_concat_bytes(a, na, b, nb);
  int32_t mask;
  int32_t i;
  int32_t n;
  g_last_key = "";
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
