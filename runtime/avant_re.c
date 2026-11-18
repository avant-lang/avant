#define PCRE2_CODE_UNIT_WIDTH 8
#include "avant_rt.h"

#include <pcre2.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* std.regex backing (PCRE2, plus re2c scanners for literal patterns).
 * A compiled pattern is a cache entry: entries are keyed by pattern text
 * and flags, shared between Regex handles, reference counted, and kept
 * for reuse after the last handle closes until the bounded cache evicts
 * the least recently used idle entry. The mutex guards only the cache
 * table and the counts; matching itself is lock free on the shared code
 * with a thread-local match data. Match results leave this file as one
 * collector Bytes holding little-endian int32 span pairs, so a find
 * allocates exactly that buffer and nothing else.
 *
 * Every entry carries the limits its Regex was compiled with (the match
 * limit, the depth limit, the heap limit; std.regex's Regex.compile gives
 * the defaults): the limits are part of the cache key, so two compiles of
 * one pattern with other limits are two entries. An entry with limits of
 * its own has a match context holding them, which every match on it
 * passes; one with the defaults has none and matches as PCRE2 does with
 * no context, on the defaults PCRE2 was built with (runtime/build.sh):
 * a match context costs pcre2_jit_match a few instructions a call, 0.3%
 * of Etc::LogParser's, whose patterns keep the defaults. */

/* 0 ok, 1 invalid pattern, 2 allocation failure. */
static __thread int32_t g_re_status;

avant_int avant_regex_take_status(void) {
  int32_t code = g_re_status;
  g_re_status = 0;
  return code;
}

static __thread pcre2_match_data *g_md = NULL;
static __thread uint32_t g_md_pairs = 0;

static pcre2_match_data *md_for(pcre2_code *code) {
  uint32_t want = 16;
  uint32_t caps = 0;
  if (!code) {
    return NULL;
  }
  if (pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &caps) == 0) {
    want = caps + 1;
    if (want < 16) {
      want = 16;
    }
  }
  if (!g_md || g_md_pairs < want) {
    if (g_md) {
      pcre2_match_data_free(g_md);
    }
    g_md = pcre2_match_data_create(want, NULL);
    g_md_pairs = want;
  }
  return g_md;
}

/* The interpreter keeps its backtracking frames in the match data it is
 * given, and PCRE2 checks the heap limit only when that vector grows: on
 * the thread's one match data, a match after a deep one (whose frames may
 * reach the largest heap limit, 256 MiB by default) ran under a smaller
 * heap limit on frames it did not check, and the thread kept them for
 * its life. So the interpreter runs on a match data of its own, made for
 * the match (or for one count) and freed after it, its ovector copied
 * into the thread's: every interpreted match starts from PCRE2's first
 * frame vector, checked against its own limit, and keeps nothing. */
static pcre2_match_data *md_own(pcre2_code *code) {
  return pcre2_match_data_create_from_pattern(code, NULL);
}

/* The match's span pairs from own into md, whose ovector md_for sized
 * for the pattern. */
static void md_give(pcre2_match_data *own, pcre2_match_data *md) {
  uint32_t k = pcre2_get_ovector_count(own);
  uint32_t m = pcre2_get_ovector_count(md);
  memcpy(pcre2_get_ovector_pointer(md), pcre2_get_ovector_pointer(own),
         (size_t)(k < m ? k : m) * 2 * sizeof(PCRE2_SIZE));
}

/* One match on the interpreter, on a match data of its own. */
static int re_interp(pcre2_code *code, const char *s, size_t n, size_t from,
                     pcre2_match_data *md, pcre2_match_context *limits) {
  int rc;
  pcre2_match_data *own = md_own(code);
  if (!own) {
    return PCRE2_ERROR_NOMEMORY;
  }
  rc = pcre2_match(code, (PCRE2_SPTR)s, n, from, PCRE2_NO_JIT, own, limits);
  if (rc >= 0) {
    md_give(own, md);
  }
  pcre2_match_data_free(own);
  return rc;
}

/* A PCRE2 failure that is not "no match": the
 * match limit, the depth limit, the heap limit, no memory, an internal
 * error. It used to read as "no match": find gave nil, count stopped
 * early, replace left the rest of the text as it was. It is a trap now,
 * naming the operation and PCRE2's own words for the failure. A trap:
 * inside a task or an HTTP handler it fails that task or request alone;
 * anywhere else stdout is flushed first, then the message, status 1. No
 * caller holds the cache's lock or a match data of its own here. */
static __attribute__((noreturn)) void re_trap(const char *op, int rc) {
  PCRE2_UCHAR buf[160];
  char text[320];
  if (pcre2_get_error_message(rc, buf, sizeof buf) < 0) {
    memcpy(buf, "unknown error", 14);
  }
  snprintf(text, sizeof text, "%s: %s (PCRE2 error %d)", op, (const char *)buf, rc);
  avant_trap_text(text);
}

/* The JIT's machine stack is 32 KiB unless one is assigned, and a match
 * that needs more fails with PCRE2_ERROR_JIT_STACKLIMIT although nothing
 * is wrong with the pattern or the text (a group repeated a few thousand
 * times is enough); it read as "no match". Such a match is run again
 * here: first by the JIT on a stack of its own that may grow to
 * RE_JIT_STACK_MAX (address space until it is used; made for this one
 * match and given back after it, so a program whose matches are never
 * deep never pays for it and a thread keeps nothing), then, if even
 * that is not enough or the stack cannot be had, by the interpreter,
 * whose frames live on the heap. So the JIT's stack limit is never the
 * reason for an answer: what comes back is the match, "no match", or
 * one of the interpreter's own limits (re_trap). Measured on
 * ((a)|(b))*c over "ab" repeated: the JIT takes
 * about 48 bytes of stack per repetition, the interpreter about 660
 * bytes of heap and thirty times the time, and the interpreter stops
 * at its match limit near 2.4 million repetitions; with 256 MiB the
 * JIT answers past that point, so the interpreter is the last resort
 * for patterns whose JIT frames are larger.
 *
 * AVANT_RE_JIT_STACK_KB is an internal variable for the suite, not a
 * user setting: a number from 32 to 262144 makes that the stack's
 * limit in KiB, so tests/cases/test_std_regex.av can reach the
 * interpreter with a small text. It can only shrink the stack. */
#define RE_JIT_STACK_MAX (256u * 1024u * 1024u)

static size_t re_jit_stack_max(void) {
  const char *v = getenv("AVANT_RE_JIT_STACK_KB");
  if (v && v[0] >= '0' && v[0] <= '9') {
    long kb = strtol(v, NULL, 10);
    if (kb >= 32 && kb <= (long)(RE_JIT_STACK_MAX / 1024u)) {
      return (size_t)kb * 1024u;
    }
  }
  return RE_JIT_STACK_MAX;
}

/* The JIT's larger stack is bounded by the entry's heap limit too, so
 * that limit is the memory one deep match may take on either engine.
 * limits is the entry's match context, NULL for the defaults. */
static int re_deep(pcre2_code *code, const char *s, size_t n, size_t from,
                   pcre2_match_data *md, pcre2_match_context *limits,
                   size_t heap_bytes) {
  int rc = PCRE2_ERROR_JIT_STACKLIMIT;
  size_t max = re_jit_stack_max();
  pcre2_match_context *mctx;
  if (heap_bytes < max) {
    max = heap_bytes < 32768 ? 32768 : heap_bytes;
  }
  mctx = limits ? pcre2_match_context_copy(limits)
               : pcre2_match_context_create(NULL);
  if (mctx) {
    pcre2_jit_stack *stack =
        pcre2_jit_stack_create(max < 65536 ? max : 65536, max, NULL);
    if (stack) {
      pcre2_jit_stack_assign(mctx, NULL, stack);
      rc = pcre2_match(code, (PCRE2_SPTR)s, n, from, 0, md, mctx);
      pcre2_jit_stack_free(stack);
    }
    pcre2_match_context_free(mctx);
  }
  if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
    rc = re_interp(code, s, n, from, md, limits);
  }
  return rc;
}

/* re2c scanners, when the compiler generated them for this program.
 * The generated object provides the strong versions; these weak
 * defaults keep a program without literal patterns linkable. */
__attribute__((weak)) int32_t avant_re2c_owns(void *re) {
  (void)re;
  return 0;
}

__attribute__((weak)) void *avant_re2c_handle(avant_int id) {
  (void)id;
  return NULL;
}

__attribute__((weak)) int32_t avant_re2c_count(int32_t id, const char *s) {
  (void)id;
  (void)s;
  return 0;
}

__attribute__((weak)) int32_t avant_re2c_groups(int32_t id) {
  (void)id;
  return 0;
}

__attribute__((weak)) int32_t avant_re2c_find_into(void *re, const char *s,
                                                   int32_t from,
                                                   int32_t *spans,
                                                   int32_t max_pairs) {
  (void)re;
  (void)s;
  (void)from;
  (void)spans;
  (void)max_pairs;
  return 0;
}

#define RE_CACHE_MAX 128

typedef struct {
  char *pat;
  int32_t flags;
  uint32_t match_limit;
  uint32_t depth_limit;
  uint32_t heap_kib;
  pcre2_match_context *mctx;
  pcre2_code *code;
  int32_t ngroups;
  int32_t jit;
  int32_t refs;
  int32_t cached;
  uint64_t used;
} ReEntry;

static ReEntry *g_cache[RE_CACHE_MAX];
static int32_t g_cache_n;
static uint64_t g_cache_tick;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

/* Why the last compile on this thread failed:
 * PCRE2's error code and the pattern offset it names. */
static __thread int g_re_err;
static __thread int32_t g_re_off;

static pcre2_code *compile_pat(const char *pat, int32_t flags, int32_t *ngroups) {
  int err = 0;
  PCRE2_SIZE erroff = 0;
  uint32_t opts = 0;
  pcre2_code *code;
  if (flags & 1) {
    opts |= PCRE2_CASELESS;
  }
  code = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED, opts, &err,
                       &erroff, NULL);
  if (!code) {
    g_re_err = err;
    g_re_off = (int32_t)erroff;
  }
  if (code) {
    uint32_t caps = 0;
    (void)pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
    if (pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &caps) == 0) {
      *ngroups = (int32_t)caps;
    } else {
      *ngroups = 0;
    }
  }
  return code;
}

/* One compile's limits, as PCRE2 takes them. */
typedef struct {
  uint32_t match;
  uint32_t depth;
  uint32_t heap_kib;
} ReLimits;

static ReEntry *cache_find(const char *pat, int32_t flags, const ReLimits *lim) {
  int32_t i;
  for (i = 0; i < g_cache_n; i++) {
    ReEntry *e = g_cache[i];
    if (e->flags == flags && e->match_limit == lim->match &&
        e->depth_limit == lim->depth && e->heap_kib == lim->heap_kib &&
        strcmp(e->pat, pat) == 0) {
      return e;
    }
  }
  return NULL;
}

static void entry_free(ReEntry *e) {
  if (e->code) {
    pcre2_code_free(e->code);
  }
  if (e->mctx) {
    pcre2_match_context_free(e->mctx);
  }
  free(e->pat);
  free(e);
}

/* Caller holds g_cache_mu; the table is full. Makes room for one entry:
 * the least recently used idle entry is freed; when every entry is held,
 * the least recently used one leaves the table and lives on, uncached,
 * until its last handle closes (avant_regex_release frees it then). A
 * Regex is never closed for the program when it is dropped (the
 * collector runs no finalizer), so entries held by Regex values nobody
 * will close used to fill the table for good, and from then on every new
 * pattern was compiled again at each compile; now they only keep their
 * own memory. */
static void cache_make_room(void) {
  int32_t i;
  int32_t idle = -1;
  int32_t held = -1;
  int32_t best;
  for (i = 0; i < g_cache_n; i++) {
    if (g_cache[i]->refs == 0) {
      if (idle < 0 || g_cache[i]->used < g_cache[idle]->used) {
        idle = i;
      }
    } else if (held < 0 || g_cache[i]->used < g_cache[held]->used) {
      held = i;
    }
  }
  best = idle >= 0 ? idle : held;
  if (idle >= 0) {
    entry_free(g_cache[best]);
  } else {
    g_cache[best]->cached = 0;
  }
  g_cache_n -= 1;
  g_cache[best] = g_cache[g_cache_n];
}

/* Whether the cache's table holds an entry for this pattern, its flags
 * and the default limits: the suite's view of what the cache keeps
 * (tests/cases/test_std_regex.av), not a library call. */
avant_int avant_regex_cache_holds(const char *pat, avant_int flags) {
  ReLimits lim;
  avant_int found;
  if (!pat) {
    pat = "";
  }
  (void)pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &lim.match);
  (void)pcre2_config(PCRE2_CONFIG_DEPTHLIMIT, &lim.depth);
  (void)pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &lim.heap_kib);
  pthread_mutex_lock(&g_cache_mu);
  found = cache_find(pat, flags ? 1 : 0, &lim) != NULL;
  pthread_mutex_unlock(&g_cache_mu);
  return found;
}

/* A limit as PCRE2 takes it: below 1 is a program error (a trap naming
 * the parameter); past PCRE2's 32-bit field it is the largest it holds. */
static uint32_t re_limit(const char *name, avant_int v) {
  if (v < 1) {
    char text[160];
    snprintf(text, sizeof text, "regex.Regex.compile: %s must be at least 1 (got %lld)", name, (long long)v);
    avant_trap_text(text);
  }
  return v > (avant_int)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/* Whether these are the limits PCRE2 was built with (no match context
 * needed). */
static int re_default_limits(const ReLimits *lim) {
  uint32_t m = 0, d = 0, h = 0;
  return pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &m) >= 0 &&
         pcre2_config(PCRE2_CONFIG_DEPTHLIMIT, &d) >= 0 &&
         pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &h) >= 0 && lim->match == m &&
         lim->depth == d && lim->heap_kib == h;
}

/* match_limit and depth_limit are PCRE2's; heap_limit is in bytes,
 * rounded up to the KiB PCRE2 counts in. */
void *avant_regex_compile(const char *pat, avant_int flags,
                          avant_int match_limit, avant_int depth_limit,
                          avant_int heap_limit) {
  ReEntry *e;
  pcre2_code *code;
  pcre2_match_context *mctx;
  int32_t ngroups = 0;
  size_t n;
  char *copy;
  ReLimits lim;
  if (!pat) {
    pat = AVANT_STR_EMPTY;
  }
  int32_t fl = flags ? 1 : 0;
  lim.match = re_limit("match_limit", match_limit);
  lim.depth = re_limit("depth_limit", depth_limit);
  (void)re_limit("heap_limit", heap_limit);
  {
    avant_int kib = heap_limit / 1024 + (heap_limit % 1024 != 0);
    lim.heap_kib = kib > (avant_int)UINT32_MAX ? UINT32_MAX : (uint32_t)kib;
  }
  g_re_status = 0;
  pthread_mutex_lock(&g_cache_mu);
  e = cache_find(pat, fl, &lim);
  if (e) {
    e->refs += 1;
    g_cache_tick += 1;
    e->used = g_cache_tick;
    pthread_mutex_unlock(&g_cache_mu);
    return e;
  }
  pthread_mutex_unlock(&g_cache_mu);

  code = compile_pat(pat, fl, &ngroups);
  if (!code) {
    g_re_status = 1;
    return NULL;
  }
  n = strlen(pat);
  copy = (char *)malloc(n + 1);
  e = (ReEntry *)malloc(sizeof(ReEntry));
  mctx = NULL;
  if (!re_default_limits(&lim)) {
    mctx = pcre2_match_context_create(NULL);
    if (!mctx) {
      free(copy);
      copy = NULL;
    }
  }
  if (!copy || !e) {
    free(copy);
    free(e);
    if (mctx) {
      pcre2_match_context_free(mctx);
    }
    pcre2_code_free(code);
    g_re_status = 2;
    return NULL;
  }
  if (mctx) {
    pcre2_set_match_limit(mctx, lim.match);
    pcre2_set_depth_limit(mctx, lim.depth);
    pcre2_set_heap_limit(mctx, lim.heap_kib);
  }
  memcpy(copy, pat, n + 1);
  e->pat = copy;
  e->flags = fl;
  e->match_limit = lim.match;
  e->depth_limit = lim.depth;
  e->heap_kib = lim.heap_kib;
  e->mctx = mctx;
  e->code = code;
  e->ngroups = ngroups;
  {
    size_t jit_size = 0;
    e->jit = pcre2_pattern_info(code, PCRE2_INFO_JITSIZE, &jit_size) == 0 &&
             jit_size > 0;
  }
  e->refs = 1;
  e->cached = 0;
  e->used = 0;

  pthread_mutex_lock(&g_cache_mu);
  {
    ReEntry *again = cache_find(pat, fl, &lim);
    if (again) {
      entry_free(e);
      again->refs += 1;
      g_cache_tick += 1;
      again->used = g_cache_tick;
      pthread_mutex_unlock(&g_cache_mu);
      return again;
    }
  }
  if (g_cache_n == RE_CACHE_MAX) {
    cache_make_room();
  }
  g_cache_tick += 1;
  e->used = g_cache_tick;
  e->cached = 1;
  g_cache[g_cache_n] = e;
  g_cache_n += 1;
  pthread_mutex_unlock(&g_cache_mu);
  return e;
}

void avant_regex_release(void *handle) {
  ReEntry *e;
  if (!handle || avant_re2c_owns(handle)) {
    return;
  }
  e = (ReEntry *)handle;
  pthread_mutex_lock(&g_cache_mu);
  if (e->refs > 0) {
    e->refs -= 1;
  }
  if (!e->cached && e->refs == 0) {
    entry_free(e);
  }
  pthread_mutex_unlock(&g_cache_mu);
}

avant_int avant_regex_groups(void *handle) {
  ReEntry *e;
  if (!handle) {
    return 0;
  }
  if (avant_re2c_owns(handle)) {
    return avant_re2c_groups((int32_t)(uintptr_t)handle);
  }
  e = (ReEntry *)handle;
  return e->ngroups;
}

avant_int avant_regex_group_index(void *handle, const char *name) {
  ReEntry *e;
  int rc;
  if (!handle || !name || avant_re2c_owns(handle)) {
    return -1;
  }
  e = (ReEntry *)handle;
  rc = pcre2_substring_number_from_name(e->code, (PCRE2_SPTR)name);
  if (rc <= 0) {
    return -1;
  }
  return rc;
}

int64_t avant_regex_find(void *handle, const char *s, avant_int from);

/* Offsets travel as int32_t: in g_last_spans, the two halves of a packed
 * pair (avant_regex_span_pair) and the re2c scanners' spans. That holds
 * because no text is longer than a String may be (AVANT_SIZE_MAX, every
 * String is made through a check of it, a C string result included), so
 * every offset, the end of the text included, fits; widening String
 * sizes must widen these first. */
_Static_assert(AVANT_SIZE_MAX <= INT32_MAX,
               "regex offsets are int32_t: widen them with String sizes");

/* The text a compiled pattern scans (std.regex's find, span and count take
 * the program's String as it is: these calls never collect). PCRE2 reads
 * the bytes and their count, a view's in place. A pattern compiled into
 * the program (re2c) reads a string object to its terminator: a flat
 * string is passed as it is, and a view as a terminated copy in C memory
 * with a string header (RE_TEXT_FREE gives it back), so nothing here
 * allocates on the heap. */
typedef struct {
  const char *bytes;
  size_t n;
  char *own;
} ReText;

static int re_text(ReText *t, const char *s) {
  t->own = NULL;
  t->bytes = avant_str_bytes(s, &t->n);
  if (avant_str_is_view(s)) {
    AvantHeader *h = (AvantHeader *)malloc(sizeof(AvantHeader) + t->n + 1);
    if (!h) {
      return 0;
    }
    h->size = (uint32_t)(t->n + 1);
    h->type_id = AVANT_TYPE_BYTES;
    h->flags = AVANT_FLAG_STRMETA;
    t->own = (char *)(h + 1);
    memcpy(t->own, t->bytes, t->n);
    t->own[t->n] = 0;
  }
  return 1;
}

/* The string the compiled pattern reads: S itself, or the copy. */
static const char *re_text_obj(const ReText *t, const char *s) {
  return t->own ? t->own : s;
}

static void re_text_free(ReText *t) {
  if (t->own) {
    free((AvantHeader *)(void *)t->own - 1);
    t->own = NULL;
  }
}

/* Span pairs of this thread's most recent avant_regex_find, copied
 * out of the PCRE2 ovector before any later match can overwrite it.
 * The Avant side reads the pairs it needs into the Match value at
 * once, so no allocation happens on the find path. */
static __thread int32_t g_last_spans[128];
static __thread int32_t g_last_pairs;
/* A pattern with more than 63 groups (the pairs
 * were once capped at 64, so groups past the 63rd read as "did not take
 * part"): its pairs go to a buffer of this thread sized from the
 * pattern, and g_last_spans stays the home of every other match. */
static __thread int32_t *g_more_spans;
static __thread int32_t g_more_cap;

static int32_t *spans_for(int32_t pairs) {
  if (pairs <= 64) {
    return g_last_spans;
  }
  if (pairs > g_more_cap) {
    int32_t *next = (int32_t *)realloc(g_more_spans,
                                       (size_t)pairs * 2 * sizeof(int32_t));
    if (!next) {
      return NULL;
    }
    g_more_spans = next;
    g_more_cap = pairs;
  }
  return g_more_spans;
}

/* Packed (lo, hi) of pair i of the last match, or -1 when the pair
 * did not participate or does not exist. */
int64_t avant_regex_span_pair(avant_int i) {
  const int32_t *spans = g_last_pairs > 64 ? g_more_spans : g_last_spans;
  int32_t lo;
  int32_t hi;
  if (i < 0 || i >= g_last_pairs) {
    return -1;
  }
  lo = spans[2 * i];
  hi = spans[2 * i + 1];
  return ((int64_t)lo << 32) | (uint32_t)hi;
}

/* Why this thread's last avant_regex_find returned -2, or its last
 * avant_regex_try_count -1: PCRE2's error code. The Avant side traps
 * through avant_regex_failed with the operation it was running
 * (lib/std/regex.av, RegexState.op: 0 find, 1 replace, 2 split, 3
 * matches), or makes a RegexError of it. */
static __thread int g_re_match_err;

/* What this thread's last failed match was, for a RegexError
 * (std.regex's RegexErrorKind: 3 the match limit, 4 the depth limit, 5
 * the heap limit, 2 no memory, 6 anything else), and PCRE2's words for
 * it with the try_ operation `op` (0 try_find, 1 try_replace, 2
 * try_split, 3 try_matches, 4 try_count: "regex.Regex.try_find: match limit
 * exceeded (PCRE2 error -47)"). */
avant_int avant_regex_failure_kind(void) {
  switch (g_re_match_err) {
  case PCRE2_ERROR_MATCHLIMIT:
    return 3;
  case PCRE2_ERROR_DEPTHLIMIT:
    return 4;
  case PCRE2_ERROR_HEAPLIMIT:
    return 5;
  case PCRE2_ERROR_NOMEMORY:
    return 2;
  default:
    return 6;
  }
}

/* gc-lint: holds nothing (C memory only). */
char *avant_regex_failure_text(avant_int op) {
  static const char *const names[] = {"regex.Regex.try_find", "regex.Regex.try_replace",
                                      "regex.Regex.try_split", "regex.Regex.try_matches",
                                      "regex.Regex.try_count"};
  PCRE2_UCHAR msg[160];
  char buf[224];
  int n;
  char *out;
  if (pcre2_get_error_message(g_re_match_err, msg, sizeof msg) < 0) {
    memcpy(msg, "unknown error", 14);
  }
  n = snprintf(buf, sizeof buf, "%s: %s (PCRE2 error %d)",
               names[op >= 0 && op < 5 ? op : 0], (const char *)msg,
               g_re_match_err);
  if (n < 0) {
    n = 0;
  }
  if (n >= (int)sizeof buf) {
    n = (int)sizeof buf - 1;
  }
  out = avant_str_alloc((uint64_t)n);
  memcpy(out, buf, (size_t)n);
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

void avant_regex_failed(avant_int op) {
  static const char *const names[] = {"regex.Regex.find", "regex.Regex.replace",
                                      "regex.Regex.split", "regex.Regex.matches"};
  re_trap(names[op >= 0 && op < 4 ? op : 0], g_re_match_err);
}

/* Packed (lo, hi) of pair i of the match at or after `from`,
 * recomputed without allocating. This is the path for groups beyond
 * the ones a Match value carries inline. */
int64_t avant_regex_span_at(void *handle, const char *s, avant_int from,
                            avant_int i) {
  int64_t whole = avant_regex_find(handle, s, from);
  if (whole == -2) {
    re_trap("regex.Match.span", g_re_match_err);
  }
  if (whole < 0) {
    return -1;
  }
  return avant_regex_span_pair(i);
}

/* Finds the first match at or after `from` and keeps its span pairs
 * in thread-local scratch for avant_regex_span_pair. Returns the
 * packed whole-match pair, -1 on no match, or -2 when PCRE2 failed
 * (the caller traps through avant_regex_failed). */
int64_t avant_regex_find(void *handle, const char *s, avant_int from) {
  ReEntry *e;
  pcre2_match_data *md;
  size_t n;
  int rc;
  PCRE2_SIZE *ov;
  int32_t pairs;
  int32_t i;
  int32_t *spans;
  ReText txt;
  g_last_pairs = 0;
  if (!handle) {
    return -1;
  }
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (avant_re2c_owns(handle)) {
    int32_t local[4];
    int32_t groups = avant_re2c_groups((int32_t)(uintptr_t)handle);
    int32_t found;
    local[0] = local[1] = local[2] = local[3] = -1;
    /* A start past the text finds nothing; within it, it is a position. */
    if (from > (avant_int)avant_str_n(s)) {
      return -1;
    }
    if (!re_text(&txt, s)) {
      g_re_match_err = PCRE2_ERROR_NOMEMORY;
      return -2;
    }
    found = avant_re2c_find_into(handle, re_text_obj(&txt, s), from < 0 ? 0 : (int32_t)from, local, 2);
    re_text_free(&txt);
    if (!found) {
      return -1;
    }
    pairs = groups + 1;
    if (pairs > 64) {
      pairs = 64;
    }
    g_last_spans[0] = local[0];
    g_last_spans[1] = local[1];
    if (pairs > 1) {
      g_last_spans[2] = local[2];
      g_last_spans[3] = local[3];
    }
    g_last_pairs = pairs;
    return avant_regex_span_pair(0);
  }
  e = (ReEntry *)handle;
  /* PCRE2 reads the bytes by their count: a view's in place. */
  s = avant_str_bytes(s, &n);
  if (from < 0) {
    from = 0;
  }
  if ((size_t)from > n) {
    return -1;
  }
  md = md_for(e->code);
  if (!md) {
    return -1;
  }
  if (__builtin_expect(!e->jit, 0)) {
    rc = re_interp(e->code, s, n, (size_t)from, md, e->mctx);
  } else {
    rc = pcre2_match(e->code, (PCRE2_SPTR)s, n, (PCRE2_SIZE)from, 0, md, e->mctx);
    if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
      rc = re_deep(e->code, s, n, (size_t)from, md, e->mctx,
                   (size_t)e->heap_kib * 1024u);
    }
  }
  if (rc < 0) {
    if (rc == PCRE2_ERROR_NOMATCH) {
      return -1;
    }
    g_re_match_err = rc;
    return -2;
  }
  ov = pcre2_get_ovector_pointer(md);
  pairs = e->ngroups + 1;
  spans = spans_for(pairs);
  if (!spans) {
    g_re_match_err = PCRE2_ERROR_NOMEMORY;
    return -2;
  }
  for (i = 0; i < pairs; i++) {
    PCRE2_SIZE a = ov[2 * i];
    PCRE2_SIZE b = ov[2 * i + 1];
    if (a == PCRE2_UNSET) {
      spans[2 * i] = -1;
      spans[2 * i + 1] = -1;
    } else {
      spans[2 * i] = (int32_t)a;
      spans[2 * i + 1] = (int32_t)b;
    }
  }
  g_last_pairs = pairs;
  return avant_regex_span_pair(0);
}

/* The number of matches can be one more than the text's bytes (an empty
 * match at every position and at the end): 2^31 for a text of the
 * largest size, past int32_t, so it is counted in avant_int. A failed
 * match traps (Regex.count) when `trap` is set; else the count is -1,
 * its PCRE2 code in g_re_match_err (Regex.try_count). Inlined into both
 * entries, so count's loop keeps its constant. */
static inline __attribute__((always_inline)) avant_int re_count(void *handle, const char *s, int trap) {
  ReEntry *e;
  pcre2_match_data *md;
  size_t slen;
  PCRE2_SIZE start = 0;
  avant_int n = 0;
  int jit = 1;
  pcre2_match_data *own = NULL;
  pcre2_code *code;
  pcre2_match_context *mctx;
  if (!handle) {
    return 0;
  }
  if (!s) {
    s = AVANT_STR_EMPTY;
  }
  if (avant_re2c_owns(handle)) {
    ReText txt;
    int32_t got;
    if (!re_text(&txt, s)) {
      if (!trap) {
        g_re_match_err = PCRE2_ERROR_NOMEMORY;
        return -1;
      }
      re_trap("regex.Regex.count", PCRE2_ERROR_NOMEMORY);
    }
    got = avant_re2c_count((int32_t)(uintptr_t)handle, re_text_obj(&txt, s));
    re_text_free(&txt);
    return got;
  }
  e = (ReEntry *)handle;
  /* PCRE2 reads the bytes by their count: a view's in place. */
  s = avant_str_bytes(s, &slen);
  md = md_for(e->code);
  if (!md) {
    return 0;
  }
  /* pcre2_jit_match reaches the JIT code after a lighter entry than
   * pcre2_match. A pattern that was not JIT-compiled runs on the
   * interpreter, on a match data of this call's own (md_own). */
  if (!e->jit) {
    own = md_own(e->code);
    if (!own) {
      if (!trap) {
        g_re_match_err = PCRE2_ERROR_NOMEMORY;
        return -1;
      }
      re_trap("regex.Regex.count", PCRE2_ERROR_NOMEMORY);
    }
    md = own;
    jit = 0;
  }
  /* The code and the limits in locals: the calls in the loop would make
   * the compiler load them from the entry at every match. */
  code = e->code;
  mctx = e->mctx;
  while (start <= slen) {
    int rc;
    PCRE2_SIZE *ov;
    PCRE2_SIZE next;
    if (jit) {
      rc = pcre2_jit_match(code, (PCRE2_SPTR)s, slen, start, 0, md, mctx);
      if (rc == PCRE2_ERROR_JIT_BADOPTION) {
        jit = 0;
        rc = pcre2_match(code, (PCRE2_SPTR)s, slen, start, 0, md, mctx);
      }
    } else {
      rc = pcre2_match(code, (PCRE2_SPTR)s, slen, start, 0, md, mctx);
    }
    if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
      rc = re_deep(code, s, slen, start, md, mctx,
                   (size_t)e->heap_kib * 1024u);
    }
    if (rc < 0) {
      if (rc != PCRE2_ERROR_NOMATCH) {
        /* The trap may end only this task: the match data goes first. */
        if (own) {
          pcre2_match_data_free(own);
        }
        if (!trap) {
          g_re_match_err = rc;
          return -1;
        }
        re_trap("regex.Regex.count", rc);
      }
      break;
    }
    n = n + 1;
    ov = pcre2_get_ovector_pointer(md);
    next = ov[1];
    if (next <= start) {
      next = start + 1;
    }
    start = next;
  }
  if (own) {
    pcre2_match_data_free(own);
  }
  return n;
}

avant_int avant_regex_count(void *handle, const char *s) {
  return re_count(handle, s, 1);
}

avant_int avant_regex_try_count(void *handle, const char *s) {
  return re_count(handle, s, 0);
}

/* PCRE2's text for the last failed compile ("missing closing
 * parenthesis"), and the offset in the pattern it points at. */
/* gc-lint: holds nothing (C memory only). */
char *avant_regex_error_text(void) {
  PCRE2_UCHAR buf[256];
  int n = pcre2_get_error_message(g_re_err, buf, sizeof buf);
  char *out;
  if (n < 0) {
    n = 0;
  }
  out = avant_str_alloc((uint64_t)n);
  memcpy(out, buf, (size_t)n);
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

avant_int avant_regex_error_offset(void) {
  return g_re_off;
}

/* A Regex used after close: a program error, not an honest "no match".
 * A trap (re_trap's path). */
void avant_regex_closed(void) {
  avant_trap_text("regex.Regex used after close");
}
