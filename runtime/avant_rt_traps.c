#include "avant_rt.h"

#include <stdio.h>
#include <stdlib.h>

/* Language traps and checked arithmetic (Wave B safety floor).
 * Moved out of avant_rt.c in the collector stage, Wave 2: these are
 * self-contained (print and exit) and share no collector state. */

void avant_trap_bounds(int32_t index, int32_t size) {
  fprintf(stderr, "avant runtime: index %d out of bounds (size %d)\n", index, size);
  exit(1);
}

void avant_trap_div_zero(void) {
  fprintf(stderr, "avant runtime: division by zero\n");
  exit(1);
}

void avant_trap_overflow(void) {
  fprintf(stderr, "avant runtime: integer overflow\n");
  exit(1);
}

/* A shift amount outside 0..width - 1 (Stage 28 W11b, C1). kind: 0 Int,
 * 1 Int64, 2 UInt64, + 8 when the amount is a UInt64 (printed unsigned). */
void avant_trap_shift(int64_t amount, int32_t kind) {
  static const char *names[] = {"Int", "Int64", "UInt64"};
  int t = kind & 3;
  if (t > 2) {
    t = 0;
  }
  int top = t == 0 ? 31 : 63;
  if (kind & 8) {
    fprintf(stderr, "avant runtime: shift amount %llu out of range for %s (0..%d)\n", (unsigned long long)amount, names[t], top);
  } else {
    fprintf(stderr, "avant runtime: shift amount %lld out of range for %s (0..%d)\n", (long long)amount, names[t], top);
  }
  exit(1);
}

int64_t avant_checked_add_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_sub_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_mul_i64(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_add_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_add_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_sub_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_sub_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

uint64_t avant_checked_mul_u64(uint64_t a, uint64_t b) {
  uint64_t r;
  if (__builtin_mul_overflow(a, b, &r)) {
    avant_trap_overflow();
  }
  return r;
}

int64_t avant_checked_div_i64(int64_t a, int64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  if (a == INT64_MIN && b == -1) {
    avant_trap_overflow();
  }
  return a / b;
}

int64_t avant_checked_rem_i64(int64_t a, int64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  if (a == INT64_MIN && b == -1) {
    return 0;
  }
  return a % b;
}

uint64_t avant_checked_div_u64(uint64_t a, uint64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  return a / b;
}

uint64_t avant_checked_rem_u64(uint64_t a, uint64_t b) {
  if (b == 0) {
    avant_trap_div_zero();
  }
  return a % b;
}
