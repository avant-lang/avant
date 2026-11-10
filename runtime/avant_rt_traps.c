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

/* Float64.to_i of NaN, an infinity, or a value outside the Int range
 * (Stage 28 W11b, C2). NaN prints as nan whatever its sign bit. */
void avant_trap_to_i(double x) {
  if (x != x) {
    fprintf(stderr, "avant runtime: to_i of Float64 nan out of range for Int\n");
  } else {
    fprintf(stderr, "avant runtime: to_i of Float64 %.17g out of range for Int\n", x);
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

/* The CPU check (Stage 28 W18S, O140). The compiler records in every
 * program the x86-64 level its code needs (`__avant_cpu_level`: 1 x86-64,
 * 2-4 x86-64-v2 to v4; 0x100 set for --cpu=native, with the compiling
 * machine's level). Before main, a CPU without that level stops the
 * program with a message naming the flag to rebuild with, instead of an
 * illegal-instruction crash somewhere later. This file is compiled for
 * plain x86-64, so the check itself runs anywhere. A program built by a
 * compiler older than the record has none (the weak reference is null). */
extern const int32_t __avant_cpu_level __attribute__((weak));

static int cpu_host_level(void) {
  __builtin_cpu_init();
  if (__builtin_cpu_supports("x86-64-v4")) {
    return 4;
  }
  if (__builtin_cpu_supports("x86-64-v3")) {
    return 3;
  }
  if (__builtin_cpu_supports("x86-64-v2")) {
    return 2;
  }
  return 1;
}

static const char *cpu_level_name(int level) {
  static const char *names[] = {"x86-64", "x86-64", "x86-64-v2", "x86-64-v3", "x86-64-v4"};
  return names[level >= 1 && level <= 4 ? level : 1];
}

__attribute__((constructor)) static void avant_cpu_check(void) {
  int need;
  int have;
  const int32_t *rec = &__avant_cpu_level;
  if (!rec) {
    return;
  }
  need = *rec & 0xff;
  have = cpu_host_level();
  if (need <= have) {
    return;
  }
  {
    static const char *flags[] = {"baseline", "baseline", "v2", "v3", "v4"};
    if (*rec & 0x100) {
      fprintf(stderr,
              "avant runtime: this program was built with --cpu=native on an %s machine, and this CPU has only %s: "
              "rebuild it with --cpu=%s (or on this machine)\n",
              cpu_level_name(need), cpu_level_name(have), flags[have]);
    } else {
      fprintf(stderr,
              "avant runtime: this program was built for %s (--cpu=%s), and this CPU has only %s: rebuild it with "
              "--cpu=%s\n",
              cpu_level_name(need), flags[need], cpu_level_name(have), flags[have]);
    }
  }
  exit(1);
}
