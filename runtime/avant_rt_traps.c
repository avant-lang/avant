#include "avant_rt.h"

#include <stdio.h>
#include <stdlib.h>

/* Language traps and checked arithmetic (Wave B safety floor).
 * Moved out of avant_rt.c in the collector stage, Wave 2: these are
 * self-contained (print and exit) and share no collector state. */

/* Every trap (Stage 29 W4b2, D295 R16) flushes the program's stdout
 * first, so the output a program printed before the trap comes before
 * its message, prints "avant runtime: WHAT", then " at FILE:LINE:COL"
 * when the compiler named the site, and exits with status 1. The site is
 * set by avant_trap_at, which the generated code calls in the trap's cold
 * block just before the trap (a program built by an older compiler, and a
 * trap the runtime raises inside one of its own functions, print no
 * site). */
static __thread const char *g_trap_loc;

void avant_trap_at(const char *loc) {
  g_trap_loc = loc;
}

static void trap_head(void) {
  fflush(stdout);
  fputs("avant runtime: ", stderr);
}

__attribute__((noreturn)) static void trap_tail(void) {
  if (g_trap_loc) {
    fprintf(stderr, " at %s", g_trap_loc);
  }
  fputc('\n', stderr);
  exit(1);
}

/* Integer arithmetic that has no correct value (Stage 29 W4b2): what is
 * op (0 +, 1 -, 2 *, 3 /, 4 %, 5 -x) on type (0 Int, 1 Int64, 2 UInt64,
 * 3 another integer width), overflowing (div_zero 0) or dividing by zero
 * (div_zero 1). */
void avant_trap_arith(int32_t op, int32_t type, int32_t div_zero) {
  static const char *ops[] = {"+", "-", "*", "/", "%", "-x"};
  static const char *types[] = {"Int", "Int64", "UInt64", "an integer"};
  int o = op >= 0 && op <= 5 ? op : 0;
  int t = type >= 0 && type <= 3 ? type : 3;
  trap_head();
  if (div_zero) {
    fprintf(stderr, "division by zero in %s %s", types[t], ops[o]);
  } else {
    fprintf(stderr, "integer overflow in %s %s", types[t], ops[o]);
  }
  trap_tail();
}

/* pop on an empty Array (Stage 29 W4b2: it said "index -1 out of bounds"). */
void avant_trap_empty(int32_t what) {
  static const char *names[] = {"pop on an empty Array", "shift on an empty Array"};
  trap_head();
  fputs(names[what == 1 ? 1 : 0], stderr);
  trap_tail();
}

void avant_trap_bounds(int32_t index, int32_t size) {
  trap_head();
  fprintf(stderr, "index %d out of bounds (size %d)", index, size);
  trap_tail();
}

void avant_trap_div_zero(void) {
  trap_head();
  fprintf(stderr, "division by zero");
  trap_tail();
}

void avant_trap_overflow(void) {
  trap_head();
  fprintf(stderr, "integer overflow");
  trap_tail();
}

/* A shift amount outside 0..width - 1 (Stage 28 W11b, C1). kind: 0 Int,
 * 1 Int64, 2 UInt64, + 8 when the amount is a UInt64 (printed unsigned). */
void avant_trap_shift(int64_t amount, int32_t kind) {
  static const char *names[] = {"Int", "Int64", "UInt64"};
  int t = kind & 3;
  trap_head();
  if (t > 2) {
    t = 0;
  }
  int top = t == 0 ? 31 : 63;
  if (kind & 8) {
    fprintf(stderr, "shift amount %llu out of range for %s (0..%d)", (unsigned long long)amount, names[t], top);
  } else {
    fprintf(stderr, "shift amount %lld out of range for %s (0..%d)", (long long)amount, names[t], top);
  }
  trap_tail();
}

/* Float64.to_i of NaN, an infinity, or a value outside the Int range
 * (Stage 28 W11b, C2). NaN prints as nan whatever its sign bit (the
 * float text prints it so). */
void avant_trap_to_i(double x) {
  char text[64];
  /* The value as Avant prints it (W4b1's shortest round-trip text). */
  avant_fmt_float_text(text, x);
  trap_head();
  fprintf(stderr, "to_i of Float64 %s out of range for Int", text);
  trap_tail();
}

/* A conversion between integer types whose value the target cannot hold
 * (Stage 29 W4b1, D295 R1: the correct value or a trap). kind is
 * from * 4 + to, each 0 Int, 1 Int64, 2 UInt64; a UInt64 value arrives as
 * its bits and prints unsigned. */
void avant_trap_convert(int64_t value, int32_t kind) {
  static const char *names[] = {"Int", "Int64", "UInt64"};
  static const char *methods[] = {"to_i", "to_i64", "to_u64"};
  int from = (kind >> 2) & 3;
  int to = kind & 3;
  trap_head();
  if (from > 2) {
    from = 0;
  }
  if (to > 2) {
    to = 0;
  }
  if (from == 2) {
    fprintf(stderr, "%s of %s %llu out of range for %s", methods[to], names[from], (unsigned long long)value, names[to]);
  } else {
    fprintf(stderr, "%s of %s %lld out of range for %s", methods[to], names[from], (long long)value, names[to]);
  }
  trap_tail();
}

/* A byte stored outside 0..255 (Stage 29 W4b1, D295 R3): push_byte, a Buf
 * element, chr, repeat_byte. */
void avant_trap_byte(int32_t value) {
  trap_head();
  fprintf(stderr, "byte %d out of range (0..255)", value);
  trap_tail();
}

/* A slice whose bounds are not 0 <= start <= stop <= size (Stage 29 W4b1,
 * D295 R4: slices trap like indexing). */
void avant_trap_slice(int32_t start, int32_t stop, int32_t size) {
  trap_head();
  fprintf(stderr, "slice %d..%d out of range (size %d)", start, stop, size);
  trap_tail();
}

/* A fixed-point precision outside 0..64 (Stage 29 W4b1: fmt_float and
 * push_fmt_f used to clamp it to 0..16 silently). */
void avant_trap_precision(int32_t prec) {
  trap_head();
  fprintf(stderr, "precision %d out of range (0..64)", prec);
  trap_tail();
}

/* A negative count where a size is meant (Stage 29 W4b1). what names the
 * operation: 0 Array(T).new's size, 1 repeat_byte's count. */
void avant_trap_negative(int32_t n, int32_t what) {
  static const char *names[] = {"Array size", "repeat_byte count"};
  trap_head();
  fprintf(stderr, "%s %d is negative", names[what == 1 ? 1 : 0], n);
  trap_tail();
}

/* An Avant value passed to or stored as a C scalar type that cannot hold
 * it (Stage 29 W1b, D64 as amended; D215: the correct value or a trap).
 * kind is type.av's cs_*: 1 Int8, 2 Int16, 3 UInt8, 4 UInt16, 5 UInt32. */
void avant_trap_c_range(int64_t value, int32_t kind) {
  static const char *names[] = {"?", "Int8", "Int16", "UInt8", "UInt16", "UInt32"};
  static const char *ranges[] = {"?", "-128..127", "-32768..32767", "0..255", "0..65535", "0..4294967295"};
  int k = kind >= 1 && kind <= 5 ? kind : 0;
  trap_head();
  fprintf(stderr, "%lld out of range for the C type %s (%s)", (long long)value, names[k], ranges[k]);
  trap_tail();
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

/* main's (or run's) Int result outside 0..255 (Stage 29 W4b2, D295 R14):
 * the OS keeps only its low byte, so 256 failures would read as success. */
void avant_trap_exit_status(int32_t status) {
  trap_head();
  fprintf(stderr, "exit status %d out of range (0..255)", status);
  trap_tail();
}

/* A C function declared `: String` that returned NULL (Stage 29 W4b2,
 * D295 R15); `String?` reads NULL as nil. */
void avant_trap_c_null(const char *name) {
  trap_head();
  fprintf(stderr, "C function %s returned NULL for String (declare String?)", name);
  trap_tail();
}
