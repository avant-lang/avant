#include "avant_rt.h"

#include <stdio.h>
#include <stdlib.h>

/* Language traps and checked arithmetic (Wave B safety floor).
 * Moved out of avant_rt.c in the collector stage, Wave 2: these are
 * self-contained (print and exit) and share no collector state. */

/* Every trap (Stage 29 W4b2, D295 R16) flushes the program's stdout
 * first, so the output a program printed before the trap comes before
 * its message, prints "avant runtime: WHAT", then " at FILE:LINE:COL"
 * when the site is known, and exits with status 1. Generated code calls
 * the _at form of each trap, which takes the site's text as its last
 * argument (one noreturn call in the cold block, as before); the plain
 * forms stay for the trap calls inside the runtime's own functions and
 * for a program built by an older compiler (the seed), and print no
 * site. */
static __thread const char *g_trap_loc;

/* The program's trap texts, emitted by the compiler (@__avant_trap_sites);
 * a program without one has none (the weak reference is null). */
extern const char *const __avant_trap_sites[] __attribute__((weak));

static const char *site_text(int32_t site) {
  if (!__avant_trap_sites || site < 0) {
    return NULL;
  }
  return __avant_trap_sites[site];
}

static const char *site_or(int32_t site) {
  const char *t = site_text(site);
  return t ? t : "?";
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

/* Integer overflow and division by zero raised by the generated code
 * (Stage 29 W4b2, D295 R16): fn names the function and where it starts
 * ("Heap.pop (heap.av:12:3)"). Every check of a function shares one cold
 * block (identical bounds traps stay identical), so LLVM can merge and
 * fold them as before; the function is the site it can name without
 * costing the hot path. */
void avant_trap_overflow_in(int32_t fn) {
  trap_head();
  fprintf(stderr, "integer overflow in %s", site_or(fn));
  trap_tail();
}

void avant_trap_bounds_in(int32_t index, int32_t size, int32_t fn) {
  trap_head();
  fprintf(stderr, "index %d out of bounds (size %d) in %s", index, size, site_or(fn));
  trap_tail();
}

void avant_trap_div_zero_in(int32_t fn) {
  trap_head();
  fprintf(stderr, "division by zero in %s", site_or(fn));
  trap_tail();
}

/* pop on an empty Array (Stage 29 W4b2: it said "index -1 out of bounds"). */
void avant_trap_empty_at(int32_t what, int32_t site) {
  static const char *names[] = {"pop on an empty Array", "shift on an empty Array"};
  g_trap_loc = site_text(site);
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
/* A size or capacity past its ceiling (Stage 29 W-INT1; avant_rt.h,
 * AVANT_SIZE_MAX): kind is AVANT_SIZE_STRING, _BUF, _ARRAY or _HASH. */
void avant_trap_size(int32_t kind) {
  static const char *what[] = {"String", "Buf", "Array", "Hash"};
  int k = kind >= 0 && kind <= 3 ? kind : 0;
  trap_head();
  if (k == AVANT_SIZE_HASH) {
    fprintf(stderr, "Hash would exceed %d slots", AVANT_HASH_SLOTS_MAX);
  } else {
    fprintf(stderr, "%s would exceed %d %s", what[k], AVANT_SIZE_MAX, k == AVANT_SIZE_ARRAY ? "elements" : "bytes");
  }
  trap_tail();
}

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
void avant_trap_exit_status_at(int32_t status, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  fprintf(stderr, "exit status %d out of range (0..255)", status);
  trap_tail();
}

/* A C function declared `: String` that returned NULL (Stage 29 W4b2,
 * D295 R15); `String?` reads NULL as nil. */
void avant_trap_c_null_at(const char *name, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  fprintf(stderr, "C function %s returned NULL for String (declare String?)", name);
  trap_tail();
}

/* The sited forms of the traps above that the generated code raises
 * (Stage 29 W4b2): the site, then the trap. */
void avant_trap_bounds_at(int32_t index, int32_t size, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_bounds(index, size);
}

void avant_trap_div_zero_at(int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_div_zero();
}

void avant_trap_overflow_at(int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_overflow();
}

void avant_trap_shift_at(int64_t amount, int32_t kind, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_shift(amount, kind);
}

void avant_trap_to_i_at(double x, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_to_i(x);
}

void avant_trap_c_range_at(int64_t value, int32_t kind, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_c_range(value, kind);
}

void avant_trap_convert_at(int64_t value, int32_t kind, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_convert(value, kind);
}

void avant_trap_byte_at(int32_t value, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_byte(value);
}

void avant_trap_negative_at(int32_t n, int32_t what, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_negative(n, what);
}
