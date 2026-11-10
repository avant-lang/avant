/* C helpers for the FFI fixtures (Stage 28 W8a, tests/fixtures/ffi/*.av,
 * tests/cases/test_ffi_llvm.av): every scalar conversion, strings and
 * bytes, opaque pointers, C layouts, callbacks, and structs by value
 * (byval.av, the SysV C ABI). Compiled with `--cc`. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
