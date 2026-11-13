/* C helpers for the FFI fixtures (Stage 28 W8a, tests/fixtures/ffi/*.av,
 * tests/cases/test_ffi_llvm.av): every scalar conversion, strings and
 * bytes, opaque pointers, C layouts, callbacks, and structs by value
 * (byval.av, the SysV C ABI). Compiled with `--cc`. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- scalars */
int ffi_add_i32(int a, int b) { return a + b; }
int64_t ffi_mul_i64(int64_t a, int64_t b) { return a * b; }
uint64_t ffi_shl_u64(uint64_t a, int n) { return a << n; }
double ffi_scale(double x, double k) { return x * k; }
bool ffi_is_pos(int x) { return x > 0; }
int ffi_bool_to_int(bool b) { return b ? 7 : 3; }
bool ffi_not(bool b) { return !b; }
int ffi_mixed(int a, double b, bool c, int64_t d) { return a + (int)b + (c ? 100 : 0) + (int)d; }

/* ---- strings, bytes, opaque pointers */
int ffi_strlen(const char *s) { return (int)strlen(s); }
const char *ffi_static_str(void) { return "from C"; }
int ffi_bytes_sum(const unsigned char *b, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) s += b[i];
  return s;
}
int ffi_str_at(const char *s, int i) { return (unsigned char)s[i]; }

typedef struct Cell { int v; } Cell;
void *ffi_cell_new(int v) {
  Cell *c = malloc(sizeof(Cell));
  c->v = v;
  return c;
}
int ffi_cell_get(void *p) { return ((Cell *)p)->v; }
void ffi_cell_free(void *p) { free(p); }

/* ---- callbacks */
int ffi_apply(int (*f)(int), int x) { return f(x); }
int ffi_apply2(int (*f)(int, int), int a, int b) { return f(a, b); }
int ffi_count_if(bool (*pred)(int), int n) {
  int c = 0;
  for (int i = 0; i < n; i++) if (pred(i)) c++;
  return c;
}
double ffi_fold(double (*f)(double, double), double seed, int n) {
  double acc = seed;
  for (int i = 0; i < n; i++) acc = f(acc, (double)i);
  return acc;
}
int ffi_repeat(int (*f)(int), int n) {
  int s = 0;
  for (int i = 0; i < n; i++) s += f(i);
  return s;
}
void ffi_each(void (*f)(int), int n) {
  for (int i = 0; i < n; i++) f(i);
}

/* ---- C layouts */
typedef struct Pt { double x; double y; } Pt;
typedef struct Rec {
  int id;
  const char *name;
  bool on;
  Pt at;
  int64_t big;
  void *link;
} Rec;
Rec *ffi_rec_new(int id) {
  Rec *r = calloc(1, sizeof(Rec));
  r->id = id;
  r->at.x = 1.5;
  r->at.y = -2.0;
  r->big = 1;
  return r;
}
int ffi_rec_id(Rec *r) { return r->id; }
const char *ffi_rec_name(Rec *r) { return r->name ? r->name : "(null)"; }
int ffi_rec_name_len(Rec *r) { return r->name ? (int)strlen(r->name) : -1; }
int ffi_rec_on(Rec *r) { return r->on ? 1 : 0; }
double ffi_rec_x(Rec *r) { return r->at.x; }
int64_t ffi_rec_big(Rec *r) { return r->big; }
int ffi_rec_off_name(void) { return (int)offsetof(Rec, name); }
int ffi_rec_off_on(void) { return (int)offsetof(Rec, on); }
int ffi_rec_off_at(void) { return (int)offsetof(Rec, at); }
int ffi_rec_off_big(void) { return (int)offsetof(Rec, big); }
int ffi_rec_off_link(void) { return (int)offsetof(Rec, link); }
int ffi_rec_size(void) { return (int)sizeof(Rec); }
void ffi_rec_free(Rec *r) { free(r); }

/* ---- structs by value (byval.av): the SysV classification of each */
typedef struct P2 { int a; int b; } P2;                /* 8: one INTEGER */
typedef struct V2 { double x; double y; } V2;          /* 16: SSE, SSE */
typedef struct Mix { int a; double d; } Mix;           /* 16: INTEGER, SSE */
typedef struct Tri { int a; int b; int c; } Tri;       /* 12: INTEGER, INTEGER (4 bytes) */
typedef struct Big { int64_t a; int64_t b; int64_t c; } Big; /* 24: MEMORY */
typedef struct Named { int n; const char *s; } Named;  /* 16: INTEGER, INTEGER */
typedef struct Flags { bool a; bool b; int n; } Flags; /* 8: one INTEGER */
typedef struct Q2 { int64_t x; int64_t y; } Q2;        /* 16: INTEGER, INTEGER */

int ffi_p2_sum(P2 p) { return p.a * 10 + p.b; }
P2 ffi_p2_make(int a, int b) { P2 p = {a, b}; return p; }
double ffi_v2_dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
V2 ffi_v2_scale(V2 v, double k) { V2 r = {v.x * k, v.y * k}; return r; }
double ffi_mix(Mix m) { return m.a + m.d; }
Mix ffi_mix_make(int a, double d) { Mix m = {a, d}; return m; }
int ffi_tri(Tri t) { return t.a * 100 + t.b * 10 + t.c; }
Tri ffi_tri_make(int a) { Tri t = {a, a + 1, a + 2}; return t; }
int64_t ffi_big_sum(Big b) { return b.a + b.b * 2 + b.c * 3; }
Big ffi_big_make(int64_t a) { Big b = {a, a * 2, a * 3}; return b; }
int ffi_named(Named n) { return n.n * 1000 + (int)strlen(n.s); }
int ffi_flags(Flags f) { return (f.a ? 100 : 0) + (f.b ? 10 : 0) + f.n; }
/* five integer arguments, then a 16-byte INTEGER pair: one register is
 * left, so the pair goes to memory (the psABI's all-or-nothing rule) */
int64_t ffi_spill(int a, int b, int c, int d, int e, Q2 q, int f) {
  return a + b + c + d + e + q.x * 100 + q.y * 1000 + f * 10000;
}

/* tests/fixtures/ffi/foreign.av (Stage 28 W17b, O123): C threads, unknown
 * to Avant, call an Avant callback that allocates; foreign_wait joins them
 * natively (avant_gc_native_begin/end, the ABI for user C that blocks
 * while Avant threads run). */
#include <pthread.h>
#include <sched.h>
void avant_gc_native_begin(void);
void avant_gc_native_end(void);

typedef int32_t (*foreign_cb_t)(int32_t);
static pthread_t foreign_th[8];
static int32_t foreign_bad[8];
static int32_t foreign_n, foreign_rounds;
static foreign_cb_t foreign_cb;

static void *foreign_run(void *arg) {
  intptr_t k = (intptr_t)arg;
  int32_t bad = 0;
  for (int32_t i = 0; i < foreign_rounds; i++) {
    bad += foreign_cb((int32_t)(k * 1000000 + i));
  }
  foreign_bad[k] = bad;
  return NULL;
}

void foreign_start(foreign_cb_t f, int32_t n, int32_t rounds) {
  foreign_cb = f;
  foreign_n = n > 8 ? 8 : n;
  foreign_rounds = rounds;
  for (intptr_t k = 0; k < foreign_n; k++) {
    pthread_create(&foreign_th[k], NULL, foreign_run, (void *)k);
  }
}

int32_t foreign_wait(void) {
  int32_t bad = 0;
  avant_gc_native_begin();
  for (int32_t k = 0; k < foreign_n; k++) {
    pthread_join(foreign_th[k], NULL);
    bad += foreign_bad[k];
  }
  avant_gc_native_end();
  return bad;
}

/* tests/fixtures/ffi/foreign_exit.av (Stage 28 W17c, O126): C threads
 * that call an allocating Avant callback and then exit without telling
 * Avant, one wave after another. exit_start starts a wave of N threads and
 * returns (the main thread allocates and collects while they run and
 * exit); exit_join joins them natively. With EXPLICIT the threads register
 * themselves first (avant_gc_register_thread): they are then running, not
 * native, between callbacks and when they exit. Thread exit must
 * unregister them: a thread left in the collector's list after its exit is
 * freed thread-local state, and a later thread on the same (reused) stack
 * registers the same list node again. */
void avant_gc_register_thread(void);
static pthread_t exit_th[8];
static int32_t exit_bad[8];
static int32_t exit_n, exit_rounds, exit_explicit;
static foreign_cb_t exit_cb;

static void *exit_run(void *arg) {
  intptr_t k = (intptr_t)arg;
  int32_t bad = 0;
  if (exit_explicit) {
    avant_gc_register_thread();
  }
  for (int32_t i = 0; i < exit_rounds; i++) {
    bad += exit_cb((int32_t)(k * 1000000 + i));
  }
  exit_bad[k] = bad;
  return NULL;
}

void exit_start(foreign_cb_t f, int32_t n, int32_t rounds, bool explicit_reg) {
  exit_cb = f;
  exit_n = n > 8 ? 8 : n;
  exit_rounds = rounds;
  exit_explicit = explicit_reg;
  for (intptr_t k = 0; k < exit_n; k++) {
    pthread_create(&exit_th[k], NULL, exit_run, (void *)k);
  }
}

int32_t exit_join(void) {
  int32_t bad = 0;
  avant_gc_native_begin();
  for (int32_t k = 0; k < exit_n; k++) {
    pthread_join(exit_th[k], NULL);
    bad += exit_bad[k];
  }
  avant_gc_native_end();
  return bad;
}

/* cscalars.av (Stage 29 W1b, D64 as amended): every C scalar type as a
 * parameter, a result and a C layout's field. */
int8_t cs_neg_i8(int8_t x) { return (int8_t)-x; }
uint8_t cs_inc_u8(uint8_t x) { return (uint8_t)(x + 1u); }
int16_t cs_neg_i16(int16_t x) { return (int16_t)-x; }
uint16_t cs_dbl_u16(uint16_t x) { return (uint16_t)(x * 2u); }
uint32_t cs_inc_u32(uint32_t x) { return x + 1u; }
float cs_half_f32(float x) { return x / 2.0f; }
double cs_widen_f32(float x) { return (double)x; }
uint32_t cs_max_u32(void) { return 4294967295u; }
int8_t cs_min_i8(void) { return -128; }
double cs_sum(int8_t a, uint8_t b, int16_t c, uint16_t d, uint32_t e, float f) {
  return (double)a + (double)b + (double)c + (double)d + (double)e + (double)f;
}
int32_t cs_byte_sum(const uint8_t *p, int32_t n) {
  int32_t t = 0;
  for (int32_t i = 0; i < n; i++) {
    t += p[i];
  }
  return t;
}

typedef struct CsPix {
  uint8_t r;
  uint8_t g;
  uint16_t w;
  int16_t h;
  int8_t tag;
  uint32_t id;
  float gain;
} CsPix;

CsPix *cs_pix_new(void) { return (CsPix *)calloc(1, sizeof(CsPix)); }
int32_t cs_pix_size(void) { return (int32_t)sizeof(CsPix); }
int32_t cs_pix_off_id(void) { return (int32_t)offsetof(CsPix, id); }
int32_t cs_pix_off_gain(void) { return (int32_t)offsetof(CsPix, gain); }
void cs_pix_fill(CsPix *p) {
  p->r = 250;
  p->g = 7;
  p->w = 65535;
  p->h = -300;
  p->tag = -5;
  p->id = 4000000000u;
  p->gain = 0.25f;
}
/* What C reads back after Avant stored into the fields. */
double cs_pix_total(const CsPix *p) {
  return (double)p->r + (double)p->g + (double)p->w + (double)p->h + (double)p->tag + (double)p->id + (double)p->gain;
}

/* rereg.av (Stage 29 W1b, O70): a C thread calls the callback (it
 * registers implicitly, and the callback leaves a heap key in the
 * runtime's thread-local hash_last_key slot), unregisters itself
 * (avant_gc_unregister_thread), waits while the main thread collects, and
 * calls the callback again (it registers again). Unregistering clears the
 * thread's runtime-root slots: before, the slot kept naming the key the
 * main thread's collections had freed, and the second registration's
 * collections read it. */
void avant_gc_unregister_thread(void);
static pthread_t rereg_th;
static foreign_cb_t rereg_cb;
static int32_t rereg_bad;
static int rereg_phase;

static void *rereg_run(void *arg) {
  (void)arg;
  rereg_bad = rereg_cb(0);
  avant_gc_unregister_thread();
  __atomic_store_n(&rereg_phase, 1, __ATOMIC_RELEASE);
  while (__atomic_load_n(&rereg_phase, __ATOMIC_ACQUIRE) != 2) {
    sched_yield();
  }
  rereg_bad += rereg_cb(1);
  avant_gc_unregister_thread();
  return NULL;
}

void rereg_start(foreign_cb_t f) {
  rereg_cb = f;
  rereg_bad = 0;
  __atomic_store_n(&rereg_phase, 0, __ATOMIC_RELEASE);
  pthread_create(&rereg_th, NULL, rereg_run, NULL);
}

void rereg_wait(void) {
  avant_gc_native_begin();
  while (__atomic_load_n(&rereg_phase, __ATOMIC_ACQUIRE) != 1) {
    sched_yield();
  }
  avant_gc_native_end();
}

int32_t rereg_finish(void) {
  __atomic_store_n(&rereg_phase, 2, __ATOMIC_RELEASE);
  avant_gc_native_begin();
  pthread_join(rereg_th, NULL);
  avant_gc_native_end();
  return rereg_bad;
}

/* release.av (Stage 28 W16c, D266): a C struct whose String fields Avant
 * stores into and then releases; C keeps a pointer it read from a field
 * (rel_keep) and copies the struct (rel_copy), as Q9's fixture did. */
typedef struct RelPos {
  const char *tag;
  int32_t n;
} RelPos;

typedef struct RelHolder {
  const char *name;
  int32_t id;
  void *link;
  RelPos at;
  const char *note;
} RelHolder;

static const char *rel_kept;
static const char rel_c_text[] = "set-by-C";

RelHolder *rel_new(void) { return (RelHolder *)calloc(1, sizeof(RelHolder)); }
void rel_free(RelHolder *h) { free(h); }
void rel_keep(RelHolder *h) { rel_kept = h->name; }
int32_t rel_kept_is(const char *want) { return rel_kept && strcmp(rel_kept, want) == 0; }
void rel_forget(void) { rel_kept = NULL; }
void rel_copy(RelHolder *dst, const RelHolder *src) { *dst = *src; }
void rel_clear(RelHolder *h) { memset(h, 0, sizeof(RelHolder)); }
int32_t rel_name_is(const RelHolder *h, const char *want) { return h->name && strcmp(h->name, want) == 0; }
int32_t rel_note_is(const RelHolder *h, const char *want) { return h->note && strcmp(h->note, want) == 0; }
int32_t rel_name_null(const RelHolder *h) { return h->name == NULL; }
int32_t rel_note_null(const RelHolder *h) { return h->note == NULL; }
int32_t rel_tag_null(const RelHolder *h) { return h->at.tag == NULL; }
void rel_set_c_name(RelHolder *h) { h->name = rel_c_text; }
int32_t rel_name_is_c(const RelHolder *h) { return h->name == rel_c_text; }
int32_t rel_name_len(const RelHolder *h) { return h->name ? (int32_t)strlen(h->name) : -1; }

/* O138 (Stage 28 W18M): user C that registers root slots opens its own
 * frame first (rg_good); rg_bad registers one on whatever frame is
 * innermost (its Avant caller's stack record), which the runtime refuses
 * with a message instead of growing a stack array. */
void avant_gc_enter(void);
void avant_gc_leave(void);
void *avant_gc_root_begin(uint32_t count);
int32_t rg_good(const char *s) {
  void ***slots;
  avant_gc_enter();
  slots = (void ***)avant_gc_root_begin(1);
  slots[0] = (void **)&s;
  avant_gc_leave();
  return s != NULL;
}
int32_t rg_bad(const char *s) {
  void ***slots = (void ***)avant_gc_root_begin(40);
  slots[0] = (void **)&s;
  return s != NULL;
}

/* ---- O145 (Stage 28 W18C, buf_stale.av): a pointer from avant_buf_bytes
 * kept across a growth of its Buf names storage the growth replaced.
 * bs_keep keeps the pointer; bs_read_kept reads through it only under
 * AVANT_GC_VERIFY=1 (where the verifier promises to catch it: the old
 * storage is PROT_NONE until the next collection, the read faults, is
 * reported, and the handler opens the storage and lets the read go on);
 * elsewhere the read would be undefined, so it is skipped. Either way it
 * returns 1, so the program prints the same everywhere. */
#include <stdio.h>
uint8_t *avant_buf_bytes(void *buf);
static volatile uint8_t *bs_kept;
int32_t bs_keep(void *buf) {
  bs_kept = avant_buf_bytes(buf);
  return bs_kept ? bs_kept[0] : -1;
}
int32_t bs_fresh(void *buf) {
  uint8_t *p = avant_buf_bytes(buf);
  return p ? p[0] : -1;
}
int32_t bs_read_kept(void) {
  const char *v = getenv("AVANT_GC_VERIFY");
  if (bs_kept && v && v[0] == '1') {
    (void)bs_kept[0];
  }
  return 1;
}

/* ---- O134 (Stage 28 W18C, release_reuse.av): C frees a struct Avant
 * stored into without a `release`, and the next struct lands at the same
 * address. A one-slot allocator makes the reuse certain: rr_new always
 * returns the same zeroed memory; rr_free forgets it. rr_keep keeps the
 * name C read, as C code that took ownership of it would. */
static RelHolder rr_slot;
RelHolder *rr_new(void) {
  memset(&rr_slot, 0, sizeof rr_slot);
  return &rr_slot;
}
void rr_free(RelHolder *h) { (void)h; }
static const char *rr_kept;
void rr_keep(RelHolder *h) { rr_kept = h->name; }
int32_t rr_kept_is(const char *want) { return rr_kept && strcmp(rr_kept, want) == 0; }
void rr_set_c_name(RelHolder *h) { h->name = rel_c_text; }

/* ---- O125 (Stage 28 W18C, stall.av): user C that blocks without
 * avant_gc_native_begin/end. st_block marks that it has started, then
 * sleeps MS milliseconds in the running state, so a collection's
 * handshake waits for it the whole time; under AVANT_GC_VERIFY=1 the
 * handshake reports it after AVANT_GC_VERIFY_STALL_MS and keeps waiting. */
#include <stdatomic.h>
#include <time.h>
int nanosleep(const struct timespec *req, struct timespec *rem);
static atomic_int st_started;
int32_t st_block(int32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  atomic_store(&st_started, 1);
  while (nanosleep(&ts, &ts) != 0) {
  }
  return ms;
}
int32_t st_started_now(void) { return atomic_load(&st_started); }

/* ---- O120 (Stage 28 W18C, held_caps.av): C keeps pointers into 400 large
 * Bufs' storage, and one collection frees them all. hc_read reads through
 * the pointers FROM to TO, only under AVANT_GC_VERIFY=1 (where every freed
 * run is held PROT_NONE until the next collection, so each read faults and
 * is reported; until W18C only the first 256 runs were held, and the rest
 * were reused unchecked); it returns how many it went over. */
static volatile uint8_t *hc_p[400];
int32_t hc_keep(void *buf, int32_t i) {
  hc_p[i] = avant_buf_bytes(buf);
  return hc_p[i][0];
}
int32_t hc_read(int32_t from, int32_t to) {
  const char *v = getenv("AVANT_GC_VERIFY");
  int32_t n = 0;
  for (int32_t i = from; i < to; i++) {
    if (v && v[0] == '1') {
      (void)hc_p[i][100];
    }
    n++;
  }
  return n;
}

/* ---- O129 (Stage 28 W18C, exit_inside.av): C threads that exit inside an
 * Avant callback. Each thread calls the callback, which calls
 * ox_exit_here: it opens a frame of its own (avant_gc_enter) with a large
 * root array (avant_gc_root_begin, 1 MiB of slots, each filled with a
 * slot that holds NULL, so the pages are resident) and calls pthread_exit
 * with the frame still on its chain. ox_run runs N such threads one after
 * another and returns how much the resident set grew, in KiB: the frames
 * leaked before the fix (64 MiB for 64 threads); the exit frees them. */
#include <stdio.h>
void avant_gc_enter(void);
static foreign_cb_t ox_cb;
#define OX_SLOTS 131072
static long ox_rss_kb(void) {
  long size = 0, res = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f) {
    if (fscanf(f, "%ld %ld", &size, &res) != 2) {
      res = 0;
    }
    fclose(f);
  }
  return res * 4;
}
int32_t ox_exit_here(int32_t k) {
  static void *nothing = NULL;
  void ***slots;
  (void)k;
  avant_gc_enter();
  slots = (void ***)avant_gc_root_begin(OX_SLOTS);
  for (int32_t i = 0; i < OX_SLOTS; i++) {
    slots[i] = &nothing;
  }
  pthread_exit(NULL);
  return 0;
}
static void *ox_thread(void *arg) {
  (void)ox_cb((int32_t)(intptr_t)arg);
  return NULL;
}
int32_t ox_run(foreign_cb_t f, int32_t n) {
  long before;
  ox_cb = f;
  /* One thread first, so the C heap's arenas and the thread cache exist
   * before the measurement starts. */
  for (int32_t k = 0; k <= n; k++) {
    pthread_t th;
    if (k == 1) {
      before = ox_rss_kb();
    }
    avant_gc_native_begin();
    pthread_create(&th, NULL, ox_thread, (void *)(intptr_t)k);
    pthread_join(th, NULL);
    avant_gc_native_end();
  }
  return (int32_t)(ox_rss_kb() - before);
}

/* ---- stack_guard.av (Stage 29 W-FIX1b, O439): a SIGSEGV handler on an
 * alternate stack that says where the fault was against the faulting
 * thread's stack: in its guard page (the overflow was caught there), or
 * past it (a frame larger than the guard stepped over it). It ends the
 * program with status 42. ffi_guard_install() is called on the thread
 * that will overflow. */
static void ffi_guard_on_segv(int sig, siginfo_t *si, void *ctx) {
  (void)sig;
  (void)ctx;
  pthread_attr_t a;
  void *lo = 0;
  size_t size = 0, guard = 0;
  if (pthread_getattr_np(pthread_self(), &a) == 0) {
    pthread_attr_getstack(&a, &lo, &size);
    pthread_attr_getguardsize(&a, &guard);
  }
  uintptr_t f = (uintptr_t)si->si_addr, l = (uintptr_t)lo;
  const char *msg = "fault inside the stack or above it\n";
  if (f >= l - guard && f < l) {
    msg = "fault in the guard page\n";
  } else if (f < l - guard) {
    msg = "fault PAST the guard page: the guard was skipped\n";
  }
  ssize_t w = write(2, msg, strlen(msg));
  (void)w;
  _exit(42);
}

void ffi_guard_install(void) {
  stack_t ss;
  ss.ss_sp = malloc(1 << 16);
  ss.ss_size = 1 << 16;
  ss.ss_flags = 0;
  sigaltstack(&ss, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = ffi_guard_on_segv;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
}

/* opt_refs.av (Stage 29 W-FIX1d, O549): an optional reference is one
 * pointer at the C boundary, nil as NULL. One trio per kind, because a C
 * name is bound with one signature. */
#define FFI_OPT_TRIO(k) \
  void *ffi_opt_null_##k(void) { return NULL; } \
  void *ffi_opt_same_##k(void *p) { return p; } \
  long ffi_opt_is_null_##k(void *p) { return p == NULL; }
FFI_OPT_TRIO(s)
FFI_OPT_TRIO(b)
FFI_OPT_TRIO(a)
FFI_OPT_TRIO(h)
FFI_OPT_TRIO(u)
FFI_OPT_TRIO(k)
FFI_OPT_TRIO(j)

/* empty_end.av (Stage 29 W-FIX1c, O551): an object with no payload at
 * the last word of the heap's highest block. With avant_alloc's guard
 * (a 0-byte request takes one word) removed, the object's payload
 * address there is the first address past the committed heap; a young
 * collection copies the object and, when its copy finds a free block
 * without committing a new chunk, the fix-up leaves every reference to
 * it naming the old copy (splus_new_addr returns an address at or past
 * the committed range unchanged). The steps:
 *   1. objects allocated in mature space, kept on a chain, until the
 *      mature cursor has left four blocks free in its 4 MiB chunk (a
 *      fresh chunk's blocks are taken lowest first, so they are its last
 *      four);
 *   2. young 16-byte objects, dropped, until the young cursor is in the
 *      chunk's last block; a young collection, which frees the four
 *      blocks; then the same objects again until the young cursor is in
 *      the last block with exactly one word left (a 24-byte one shifts
 *      the parity once): freed blocks are taken last freed first, so
 *      other blocks of the four are still free then;
 *   3. avant_alloc(0, bytes): without the guard it takes that word; with
 *      it the object takes two words elsewhere. It is stored in a rooted
 *      holder and a young collection runs.
 * Returned, packed: bit 0..15 how many times step 3's position was
 * reached, 16..31 empty objects whose payload was not one word, 32..47
 * ones whose payload address was a block's first address, 48..62 ones the
 * holder no longer named as an object after the collection. */
typedef struct FfiHeader {
  uint32_t size;
  uint16_t type_id;
  uint16_t flags;
} FfiHeader;
void *avant_alloc(uint64_t nbytes, uint32_t type_id);
void avant_alloc_in_mature(int32_t on);
void avant_gc_add_root(void **slot);
void avant_gc_collect_young(void);
void avant_gc_collect(void);
void avant_barrier(void *obj);
int avant_is_heap(void *payload);
#define FFI_TYPE_PTRS 65535u
#define FFI_BLOCK ((uintptr_t)32768)
#define FFI_CHUNK ((uintptr_t)4 << 20)
#define FFI_FLAG_FORWARDED 4u
static __thread void *ffi_ee_keep;
static __thread void *ffi_ee_chain;
static __thread int ffi_ee_rooted;

static uintptr_t ffi_ee_block(void *payload) {
  return (((uintptr_t)payload - 8) % FFI_CHUNK) / FFI_BLOCK;
}

static uintptr_t ffi_ee_rest(void *payload, uintptr_t total) {
  return FFI_BLOCK - (((uintptr_t)payload - 8) % FFI_BLOCK) - total;
}

/* Young 16-byte objects, dropped, until one lies in block LAST of its
 * chunk (WORD 0), or until the cursor there has one word left (WORD 1). */
static int ffi_ee_young_to(uintptr_t last, int word) {
  int i;
  for (i = 0; i < 6 * 2048; i++) {
    void *p = avant_alloc(8, 0);
    uintptr_t rest;
    if (ffi_ee_block(p) != last) {
      continue;
    }
    if (!word) {
      return 1;
    }
    rest = ffi_ee_rest(p, 16);
    if (rest % 16 == 0 && rest >= 32) {
      p = avant_alloc(16, 0);
      if (ffi_ee_block(p) != last) {
        continue;
      }
      rest = ffi_ee_rest(p, 24);
    }
    if (rest == 8) {
      return 1;
    }
  }
  return 0;
}

int64_t ffi_empty_end(int64_t rounds) {
  int64_t reached = 0, nonword = 0, atblock = 0, stale = 0;
  int64_t r;
  uintptr_t last = FFI_CHUNK / FFI_BLOCK - 1;
  if (!ffi_ee_rooted) {
    avant_gc_add_root(&ffi_ee_keep);
    avant_gc_add_root(&ffi_ee_chain);
    ffi_ee_rooted = 1;
  }
  ffi_ee_keep = avant_alloc(64 * sizeof(void *), FFI_TYPE_PTRS);
  /* The guard itself, wherever the cursor is. */
  {
    void *e = avant_alloc(0, 0);
    if (((FfiHeader *)e - 1)->size != 8) {
      nonword++;
    }
  }
  for (r = 0; r < rounds && r < 60; r++) {
    int i;
    int at = 0;
    /* Step 1: 2,048-byte objects (16 to a block), each naming the one
     * before, until one ends the fifth block from the chunk's end. */
    avant_alloc_in_mature(1);
    for (i = 0; i < 16 * 300 && !at; i++) {
      void **f = (void **)avant_alloc(2040, FFI_TYPE_PTRS);
      f[0] = ffi_ee_chain;
      ffi_ee_chain = f;
      at = ffi_ee_block(f) == last - 4 && ffi_ee_rest(f, 2048) == 0;
    }
    avant_alloc_in_mature(0);
    /* Step 2. */
    if (!at || !ffi_ee_young_to(last, 0)) {
      continue;
    }
    avant_gc_collect_young();
    if (!ffi_ee_young_to(last, 1)) {
      continue;
    }
    reached++;
    /* Step 3. */
    {
      void **keep;
      void *e = avant_alloc(0, 0);
      void *e2;
      if (((FfiHeader *)e - 1)->size != 8) {
        nonword++;
      }
      if ((uintptr_t)e % FFI_BLOCK == 0) {
        atblock++;
      }
      keep = (void **)ffi_ee_keep;
      keep[r] = e;
      avant_barrier(keep);
      avant_gc_collect_young();
      keep = (void **)ffi_ee_keep;
      e2 = keep[r];
      if (!avant_is_heap(e2) || (((FfiHeader *)e2 - 1)->flags & FFI_FLAG_FORWARDED)) {
        stale++;
      }
    }
  }
  ffi_ee_chain = NULL;
  return reached | (nonword << 16) | (atblock << 32) | (stale << 48);
}

/* field_rule.av (the Valk stage, VALK-T, O585; D342 Q6): the field rule's
 * check. A reference-typed field holds null, an exact start or a static
 * address, never an address inside an object (runtime/avant_gc_internal.h
 * states the rule and what relies on it). A rooted pointer buffer names a
 * second one by its start, which is allowed; with BRK set its next slot
 * is given an address two words into that object, which only C can
 * store, and a young collection is asked for (then, in the breaking
 * form, a full one: with the nursery off no young one runs). Under
 * AVANT_GC_VERIFY=1 the
 * verifier reads the field before the collection traces it and stops the
 * program with "field rule" and the holder; without the verifier the
 * breaking form is never run (a trace that reads a header right before
 * that address reads the object's own words). Returns 1 when the holder's
 * first slot still names a heap object after the collection. */
static __thread void *ffi_fr_holder;
static __thread int ffi_fr_rooted;

int64_t ffi_field_rule(int64_t brk) {
  void **h;
  void **t;
  if (!ffi_fr_rooted) {
    avant_gc_add_root(&ffi_fr_holder);
    ffi_fr_rooted = 1;
  }
  ffi_fr_holder = avant_alloc(4 * sizeof(void *), FFI_TYPE_PTRS);
  t = (void **)avant_alloc(4 * sizeof(void *), FFI_TYPE_PTRS);
  h = (void **)ffi_fr_holder;
  h[0] = t;
  if (brk) {
    h[1] = (void *)(t + 2);
  }
  avant_barrier(h);
  avant_gc_collect_young();
  if (brk) {
    /* With the nursery off no young collection runs: the full one's
     * verification reads the field. */
    avant_gc_collect();
  }
  h = (void **)ffi_fr_holder;
  return h[0] != NULL && avant_is_heap(h[0]);
}

/* rem_invariant.av (the Valk stage, VALK-T2, O610): a store of V into the
 * first reference word of HOLDER (a class instance whose first field is a
 * reference), behind avant_barrier when BARRIER is set and with no barrier
 * otherwise, which only C can do. A holder that is mature and not
 * remembered then names a young object: the state AVANT_GC_VERIFY=1's
 * check of the generational invariant reports at the next young
 * collection, even while a root names the holder (and the collection's
 * trace therefore repairs it). Returns 1. */
void avant_barrier(void *obj);

int64_t ffi_rem_store(void *holder, void *v, int64_t barrier) {
  if (barrier) {
    avant_barrier(holder);
  }
  *(void **)holder = v;
  return 1;
}
