#include "avant_rt.h"
#include "avant_gc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Language traps and checked arithmetic (the safety floor).
 * Moved out of avant_rt.c with the collector's split: these are
 * self-contained (print and exit) and share no collector state. */

/* Every trap flushes the program's stdout
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

/* The trap's text is formed in a buffer of the thread's own: a trap in a task that may end the task (the recovery
 * point of avant_task.c: a pool worker running a spawned block, not
 * inside a C callback) hands it to avant_task_trapped, which returns to
 * the task's entry and ends the task, and its join gives the text and
 * the site as a TaskError; any other trap prints it as before (the
 * program's stdout flushed first, then "avant runtime: WHAT[ at SITE]",
 * status 1). */
static __thread char g_trap_text[1024];
static int g_trap_exiting;
static __thread size_t g_trap_n;

static void trap_head(void) {
  g_trap_n = 0;
  g_trap_text[0] = 0;
}

__attribute__((format(printf, 1, 2))) static void trap_out(const char *fmt, ...) {
  va_list ap;
  size_t room = sizeof g_trap_text - g_trap_n;
  int k;
  if (room <= 1) {
    return;
  }
  va_start(ap, fmt);
  k = vsnprintf(g_trap_text + g_trap_n, room, fmt, ap);
  va_end(ap);
  if (k > 0) {
    g_trap_n += (size_t)k < room ? (size_t)k : room - 1;
  }
}

/* One exit ends the process: the first thread to end it (a trap that
 * ends the program, `exit(status)` on any thread, the end of main in a
 * program with tasks) goes on; any other that tries meanwhile waits for
 * good, native, so the first one's exit work (stdout's check, the
 * collector's checks at exit, the private directory's removal) neither
 * meets a second exit, which would write the program's output twice and
 * remove the directory twice, nor waits for it at a safepoint (two exits
 * at once hung the program under AVANT_GC_VERIFY=1). */
void avant_exit_claim(void) {
  if (__atomic_exchange_n(&g_trap_exiting, 1, __ATOMIC_ACQ_REL)) {
    avant_gc_native_begin();
    for (;;) {
      pause();
    }
  }
}

__attribute__((noreturn)) static void trap_tail(void) {
  const char *site = g_trap_loc;
  g_trap_loc = NULL;
  if (avant_task_can_recover()) {
    avant_task_trapped(g_trap_text, site);
  }
  avant_exit_claim();
  fflush(stdout);
  fputs("avant runtime: ", stderr);
  fputs(g_trap_text, stderr);
  if (site) {
    fprintf(stderr, " at %s", site);
  }
  fputc('\n', stderr);
  exit(1);
}

/* A trap whose text a runtime unit formed (avant_str.c's argument
 * checks): the same path. */
void avant_trap_text(const char *what) {
  trap_head();
  trap_out("%s", what);
  trap_tail();
}

/* Integer overflow and division by zero raised by the generated code: fn names
 * the function and where it starts
 * ("Heap.pop (heap.av:12:3)"). Every check of a function shares one cold
 * block (identical bounds traps stay identical), so LLVM can merge and
 * fold them as before; the function is the site it can name without
 * costing the hot path. */
void avant_trap_overflow_in(int32_t fn) {
  trap_head();
  trap_out("integer overflow in %s", site_or(fn));
  trap_tail();
}

void avant_trap_bounds_in(avant_int index, avant_int size, int32_t fn) {
  trap_head();
  trap_out("index %lld out of bounds (size %lld) in %s", (long long)index, (long long)size, site_or(fn));
  trap_tail();
}

void avant_trap_div_zero_in(int32_t fn) {
  trap_head();
  trap_out("division by zero in %s", site_or(fn));
  trap_tail();
}

/* pop on an empty Array (it used to say "index -1 out of bounds"). */
void avant_trap_empty_at(int32_t what, int32_t site) {
  static const char *names[] = {"pop on an empty Array", "shift on an empty Array"};
  g_trap_loc = site_text(site);
  trap_head();
  trap_out("%s", names[what == 1 ? 1 : 0]);
  trap_tail();
}

/* A change to an Array a running task reads (avant_rt.h,
 * AVANT_ARRAY_FROZEN): op names the operation
 * (AVANT_FROZEN_*). The checks inside the runtime's own functions (push's
 * growth, pop, fill, sort, reverse, reserve) print no site; generated
 * code's (an element store, clear, a sort with a block) call the _at
 * form. */
static const char *frozen_op(int32_t op) {
  static const char *names[] = {"push", "pop", "clear", "fill", "sort", "reverse", "reserve", "an element store"};
  return op >= 0 && op <= 7 ? names[op] : "a change";
}

void avant_trap_frozen(int32_t op) {
  trap_head();
  trap_out("%s on an array a running task reads: a spawn or .parallel that captured it keeps it unchanged until the task ends; join the task first", frozen_op(op));
  trap_tail();
}

void avant_trap_frozen_at(int32_t op, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_frozen(op);
}

/* `m.with { }` on a mutex a task held when it trapped: the value may be half-changed, so the block does not run
 * on it. */
void avant_trap_poisoned(void) {
  trap_head();
  trap_out("Mutex.with on a poisoned mutex: a task trapped while it held this mutex, so its value may be half-changed");
  trap_tail();
}

void avant_trap_poisoned_at(int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_poisoned();
}

static __attribute__((noreturn, cold)) void avant_trap_bounds(avant_int index, avant_int size) {
  trap_head();
  trap_out("index %lld out of bounds (size %lld)", (long long)index, (long long)size);
  trap_tail();
}

static __attribute__((noreturn, cold)) void avant_trap_div_zero(void) {
  trap_head();
  trap_out("division by zero");
  trap_tail();
}

void avant_trap_overflow(void) {
  trap_head();
  trap_out("integer overflow");
  trap_tail();
}

/* A shift amount outside 0..width - 1. kind: 16 +
 * 0 Int, 1 Int32, 2 UInt64, 3 UInt32, + 8 for an unsigned amount (the
 * 16 kept an older seed's kinds apart). */
static __attribute__((noreturn, cold)) void avant_trap_shift(int64_t amount, int32_t kind) {
  static const char *names[] = {"Int", "Int32", "UInt64", "UInt32"};
  int t = kind & 3;
  trap_head();
  int top = (t == 1 || t == 3) ? 31 : 63;
  if (kind & 8) {
    trap_out("shift amount %llu out of range for %s (0..%d)", (unsigned long long)amount, names[t], top);
  } else {
    trap_out("shift amount %lld out of range for %s (0..%d)", (long long)amount, names[t], top);
  }
  trap_tail();
}

/* Float64.to_i of NaN, an infinity, or a value outside the Int range.
 * NaN prints as nan whatever its sign bit (the
 * float text prints it so). */
static __attribute__((noreturn, cold)) void avant_trap_to_i(double x) {
  char text[64];
  /* The value as Avant prints it (the shortest round-trip text). */
  avant_fmt_float_text(text, x);
  trap_head();
  trap_out("to_i of Float64 %s out of range for Int", text);
  trap_tail();
}

/* A conversion between integer types whose value the target cannot hold
 * (the correct value or a trap). kind is
 * 16 + from * 4 + to, each 0 Int, 1 Int32, 2 UInt64, 3 UInt32 (the 16
 * kept an older seed's kinds apart). An
 * unsigned value arrives as its bits and prints unsigned. */
static __attribute__((noreturn, cold)) void avant_trap_convert(int64_t value, int32_t kind) {
  static const char *names[] = {"Int", "Int32", "UInt64", "UInt32"};
  static const char *methods[] = {"to_i", "to_i32", "to_u64", "to_u32"};
  int from = (kind >> 2) & 3;
  int to = kind & 3;
  int unsigned_from = from == 2 || from == 3;
  trap_head();
  if (unsigned_from) {
    trap_out("%s of %s %llu out of range for %s", methods[to], names[from], (unsigned long long)value, names[to]);
  } else {
    trap_out("%s of %s %lld out of range for %s", methods[to], names[from], (long long)value, names[to]);
  }
  trap_tail();
}

/* A byte stored outside 0..255: push_byte, a Buf
 * element, chr, repeat_byte. */
void avant_trap_byte(avant_int value) {
  trap_head();
  trap_out("byte %lld out of range (0..255)", (long long)value);
  trap_tail();
}

/* A slice whose bounds are not 0 <= start <= stop <= size (slices trap
 * like indexing). */
void avant_trap_slice(avant_int start, avant_int stop, avant_int size) {
  trap_head();
  trap_out("slice %lld..%lld out of range (size %lld)", (long long)start, (long long)stop, (long long)size);
  trap_tail();
}

/* A fixed-point precision outside 0..64 (fmt_float and
 * push_fmt_f used to clamp it to 0..16 silently). */
void avant_trap_precision(avant_int prec) {
  trap_head();
  trap_out("precision %lld out of range (0..64)", (long long)prec);
  trap_tail();
}

/* A negative count where a size is meant. what names the
 * operation: 0 Array(T).new's size, 1 repeat_byte's count. */
/* A size or capacity past its ceiling (avant_rt.h,
 * AVANT_SIZE_MAX): kind is AVANT_SIZE_STRING, _BUF, _ARRAY or _HASH. */
void avant_trap_size(int32_t kind) {
  static const char *what[] = {"String", "Buf", "Array", "Hash"};
  int k = kind >= 0 && kind <= 3 ? kind : 0;
  trap_head();
  if (k == AVANT_SIZE_HASH) {
    trap_out("Hash would exceed %d slots", AVANT_HASH_SLOTS_MAX);
  } else {
    trap_out("%s would exceed %d %s", what[k], AVANT_SIZE_MAX, k == AVANT_SIZE_ARRAY ? "elements" : "bytes");
  }
  trap_tail();
}

/* Array(T).new(n) past the ceiling, with its site. */
void avant_trap_size_at(int32_t kind, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_size(kind);
}

void avant_trap_negative(avant_int n, int32_t what) {
  static const char *names[] = {"Array size", "repeat_byte count"};
  trap_head();
  trap_out("%s %lld is negative", names[what == 1 ? 1 : 0], (long long)n);
  trap_tail();
}

/* An Avant value passed to or stored as a C scalar type that cannot hold
 * it (the correct value or a trap).
 * kind is type.av's cs_*: 1 Int8, 2 Int16, 3 UInt8, 4 UInt16, 5 UInt32. */
void avant_trap_c_range(int64_t value, int32_t kind) {
  static const char *names[] = {"?", "Int8", "Int16", "UInt8", "UInt16", "UInt32", "Int32"};
  static const char *ranges[] = {"?", "-128..127", "-32768..32767", "0..255", "0..65535", "0..4294967295", "-2147483648..2147483647"};
  int k = kind >= 1 && kind <= 6 ? kind : 0;
  trap_head();
  trap_out("%lld out of range for the C type %s (%s)", (long long)value, names[k], ranges[k]);
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

/* The CPU check. The compiler records in every
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

/* main's (or run's) Int result outside 0..255:
 * the OS keeps only its low byte, so 256 failures would read as success. */
void avant_trap_exit_status_at(avant_int status, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  trap_out("exit status %lld out of range (0..255)", (long long)status);
  trap_tail();
}

/* A C function declared `: String` that returned NULL; `String?` reads NULL as
 * nil. */
void avant_trap_c_null_at(const char *name, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  trap_out("C function %s returned NULL for String (declare String?)", name);
  trap_tail();
}

/* An enum value that is no member: a value a C
 * function gave for an enum, or one reaching a switch that names every
 * member. name is the enum's. */
void avant_trap_enum_at(int64_t value, const char *name, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  trap_out("%lld is not a member of %s", (long long)value, name);
  trap_tail();
}

/* `x.expect(why)` on an optional that holds nil: the program's own text. */
void avant_trap_expect_at(const char *why, int32_t site) {
  g_trap_loc = site_text(site);
  trap_head();
  trap_out("%s", why ? why : "");
  trap_tail();
}

/* The sited forms of the traps above that the generated code raises: the site,
 * then the trap. */
void avant_trap_bounds_at(avant_int index, avant_int size, int32_t site) {
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

void avant_trap_byte_at(avant_int value, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_byte(value);
}

void avant_trap_negative_at(avant_int n, int32_t what, int32_t site) {
  g_trap_loc = site_text(site);
  avant_trap_negative(n, what);
}
