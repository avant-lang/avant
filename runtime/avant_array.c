#include "avant_rt.h"

#include <stdlib.h>
#include <string.h>

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
  /*
   * Store under defer so myc IR never holds an interior buffer
   * pointer across a collecting CALL. Grow may nest defer.
   */
  avant_gc_defer_enter();
  slot = avant_array_push_slot(arr, sizeof(void *), buf_type_id);
  if (slot) {
    memcpy(slot, &elem, sizeof(void *));
  }
  avant_gc_defer_leave();
  avant_barrier(a);
  if (a->buf) {
    avant_barrier(a->buf);
  }
}

void avant_array_set_ptr(void *arr, int32_t i, void *elem) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf) {
    return;
  }
  ((void **)a->buf)[i] = elem;
  avant_barrier(a);
  avant_barrier(a->buf);
}

void *avant_array_get_ptr(void *arr, int32_t i) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || i < 0 || i >= a->size) {
    return NULL;
  }
  return ((void **)a->buf)[i];
}

void *avant_array_pop_ptr(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size <= 0) {
    return NULL;
  }
  a->size -= 1;
  return ((void **)a->buf)[a->size];
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

void avant_array_sort_i32(void *arr) {
  AvantArray *a = (AvantArray *)arr;
  if (!a || !a->buf || a->size <= 1) {
    return;
  }
  qsort(a->buf, (size_t)a->size, sizeof(int32_t), cmp_i32);
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
