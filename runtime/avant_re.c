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
 * allocates exactly that buffer and nothing else. */

/* 0 ok, 1 invalid pattern, 2 allocation failure. */
static __thread int32_t g_re_status;

avant_int avant_regex_take_status(void) {
  int32_t code = g_re_status;
  g_re_status = 0;
  return code;
}

static pcre2_match_data *md_for(pcre2_code *code) {
  static __thread pcre2_match_data *md = NULL;
  static __thread uint32_t pairs = 0;
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
  if (!md || pairs < want) {
    if (md) {
      pcre2_match_data_free(md);
    }
    md = pcre2_match_data_create(want, NULL);
    pairs = want;
  }
  return md;
}

/* A PCRE2 failure that is not "no match" (Stage 29 W-FIX1a, P40): the
 * match limit, the depth limit, the heap limit, no memory, an internal
 * error. It used to read as "no match": find gave nil, count stopped
 * early, replace left the rest of the text as it was. It is a trap now,
 * naming the operation and PCRE2's own words for the failure (stdout
 * first, status 1, as avant_regex_closed). */
static void re_trap(const char *op, int rc) {
  PCRE2_UCHAR buf[160];
  if (pcre2_get_error_message(rc, buf, sizeof buf) < 0) {
    memcpy(buf, "unknown error", 14);
  }
  fflush(stdout);
  fprintf(stderr, "avant runtime: %s: %s (PCRE2 error %d)\n", op,
          (const char *)buf, rc);
  exit(1);
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
 * ((a)|(b))*c over "ab" repeated (Stage 29 W-FIX1a): the JIT takes
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

static int re_deep(pcre2_code *code, const char *s, size_t n, size_t from,
                   pcre2_match_data *md) {
  int rc = PCRE2_ERROR_JIT_STACKLIMIT;
  size_t max = re_jit_stack_max();
  pcre2_match_context *mctx = pcre2_match_context_create(NULL);
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
    rc = pcre2_match(code, (PCRE2_SPTR)s, n, from, PCRE2_NO_JIT, md, NULL);
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
  pcre2_code *code;
  int32_t ngroups;
  int32_t refs;
  int32_t cached;
  uint64_t used;
} ReEntry;

static ReEntry *g_cache[RE_CACHE_MAX];
static int32_t g_cache_n;
static uint64_t g_cache_tick;
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;

/* Why the last compile on this thread failed (Stage 29 W4b2, D295 R11):
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

static ReEntry *cache_find(const char *pat, int32_t flags) {
  int32_t i;
  for (i = 0; i < g_cache_n; i++) {
    if (g_cache[i]->flags == flags && strcmp(g_cache[i]->pat, pat) == 0) {
      return g_cache[i];
    }
  }
  return NULL;
}

static void entry_free(ReEntry *e) {
  if (e->code) {
    pcre2_code_free(e->code);
  }
  free(e->pat);
  free(e);
}

/* Caller holds g_cache_mu. Evicts the least recently used idle entry. */
static int32_t cache_evict_idle(void) {
  int32_t i;
  int32_t best = -1;
  for (i = 0; i < g_cache_n; i++) {
    if (g_cache[i]->refs == 0) {
      if (best < 0 || g_cache[i]->used < g_cache[best]->used) {
        best = i;
      }
    }
  }
  if (best < 0) {
    return 0;
  }
  entry_free(g_cache[best]);
  g_cache_n -= 1;
  g_cache[best] = g_cache[g_cache_n];
  return 1;
}

void *avant_regex_compile(const char *pat, avant_int flags) {
  ReEntry *e;
  pcre2_code *code;
  int32_t ngroups = 0;
  size_t n;
  char *copy;
  if (!pat) {
    pat = "";
  }
  int32_t fl = flags ? 1 : 0;
  g_re_status = 0;
  pthread_mutex_lock(&g_cache_mu);
  e = cache_find(pat, fl);
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
  if (!copy || !e) {
    free(copy);
    free(e);
    pcre2_code_free(code);
    g_re_status = 2;
    return NULL;
  }
  memcpy(copy, pat, n + 1);
  e->pat = copy;
  e->flags = fl;
  e->code = code;
  e->ngroups = ngroups;
  e->refs = 1;
  e->cached = 0;
  e->used = 0;

  pthread_mutex_lock(&g_cache_mu);
  {
    ReEntry *again = cache_find(pat, fl);
    if (again) {
      entry_free(e);
      again->refs += 1;
      g_cache_tick += 1;
      again->used = g_cache_tick;
      pthread_mutex_unlock(&g_cache_mu);
      return again;
    }
  }
  if (g_cache_n < RE_CACHE_MAX || cache_evict_idle()) {
    g_cache_tick += 1;
    e->used = g_cache_tick;
    e->cached = 1;
    g_cache[g_cache_n] = e;
    g_cache_n += 1;
  }
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

/* Span pairs of this thread's most recent avant_regex_find, copied
 * out of the PCRE2 ovector before any later match can overwrite it.
 * The Avant side reads the pairs it needs into the Match value at
 * once, so no allocation happens on the find path. */
static __thread int32_t g_last_spans[128];
static __thread int32_t g_last_pairs;
/* A pattern with more than 63 groups (Stage 29 W-FIX1a, P40: the pairs
 * were capped at 64, so groups past the 63rd read as "did not take
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

/* Why this thread's last avant_regex_find returned -2: PCRE2's error
 * code. The Avant side traps through avant_regex_failed with the
 * operation it was running (lib/std/regex.av, RegexState.op: 0 find,
 * 1 replace, 2 split, 3 matches). */
static __thread int g_re_match_err;

void avant_regex_failed(avant_int op) {
  static const char *const names[] = {"Regex.find", "Regex.replace",
                                      "Regex.split", "Regex.matches"};
  re_trap(names[op >= 0 && op < 4 ? op : 0], g_re_match_err);
}

/* Packed (lo, hi) of pair i of the match at or after `from`,
 * recomputed without allocating. This is the path for groups beyond
 * the ones a Match value carries inline. */
int64_t avant_regex_span_at(void *handle, const char *s, avant_int from,
                            avant_int i) {
  int64_t whole = avant_regex_find(handle, s, from);
  if (whole == -2) {
    re_trap("Match.span", g_re_match_err);
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
  g_last_pairs = 0;
  if (!handle) {
    return -1;
  }
  if (!s) {
    s = "";
  }
  if (avant_re2c_owns(handle)) {
    int32_t local[4];
    int32_t groups = avant_re2c_groups((int32_t)(uintptr_t)handle);
    local[0] = local[1] = local[2] = local[3] = -1;
    /* A start past the text finds nothing; within it, it is a position. */
    if (from > (avant_int)avant_str_n(s)) {
      return -1;
    }
    if (!avant_re2c_find_into(handle, s, from < 0 ? 0 : (int32_t)from, local, 2)) {
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
  n = avant_str_n(s);
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
  rc = pcre2_match(e->code, (PCRE2_SPTR)s, n, (PCRE2_SIZE)from, 0, md, NULL);
  if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
    rc = re_deep(e->code, s, n, (size_t)from, md);
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

avant_int avant_regex_count(void *handle, const char *s) {
  ReEntry *e;
  pcre2_match_data *md;
  size_t slen;
  PCRE2_SIZE start = 0;
  int32_t n = 0;
  int jit = 1;
  if (!handle) {
    return 0;
  }
  if (!s) {
    s = "";
  }
  if (avant_re2c_owns(handle)) {
    return avant_re2c_count((int32_t)(uintptr_t)handle, s);
  }
  e = (ReEntry *)handle;
  slen = avant_str_n(s);
  md = md_for(e->code);
  if (!md) {
    return 0;
  }
  /* pcre2_jit_match reaches the JIT code after a lighter entry than
   * pcre2_match. A pattern that was not JIT-compiled falls back for
   * the rest of this call. */
  while (start <= slen) {
    int rc;
    PCRE2_SIZE *ov;
    PCRE2_SIZE next;
    if (jit) {
      rc = pcre2_jit_match(e->code, (PCRE2_SPTR)s, slen, start, 0, md, NULL);
      if (rc == PCRE2_ERROR_JIT_BADOPTION) {
        jit = 0;
        rc = pcre2_match(e->code, (PCRE2_SPTR)s, slen, start, 0, md, NULL);
      }
    } else {
      rc = pcre2_match(e->code, (PCRE2_SPTR)s, slen, start, 0, md, NULL);
    }
    if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
      rc = re_deep(e->code, s, slen, start, md);
    }
    if (rc < 0) {
      if (rc != PCRE2_ERROR_NOMATCH) {
        re_trap("Regex.count", rc);
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
  return n;
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

/* A Regex used after close (Stage 29 W4b2, D295 R11): a program error,
 * not an honest "no match". Printed as a trap (stdout first, status 1). */
void avant_regex_closed(void) {
  fflush(stdout);
  fputs("avant runtime: Regex used after close\n", stderr);
  exit(1);
}
