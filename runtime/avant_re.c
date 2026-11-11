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

int32_t avant_regex_take_status(void) {
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

/* re2c scanners, when the compiler generated them for this program.
 * The generated object provides the strong versions; these weak
 * defaults keep a program without literal patterns linkable. */
__attribute__((weak)) int32_t avant_re2c_owns(void *re) {
  (void)re;
  return 0;
}

__attribute__((weak)) void *avant_re2c_handle(int32_t id) {
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

void *avant_regex_compile(const char *pat, int32_t flags) {
  ReEntry *e;
  pcre2_code *code;
  int32_t ngroups = 0;
  size_t n;
  char *copy;
  if (!pat) {
    pat = "";
  }
  flags = flags ? 1 : 0;
  g_re_status = 0;
  pthread_mutex_lock(&g_cache_mu);
  e = cache_find(pat, flags);
  if (e) {
    e->refs += 1;
    g_cache_tick += 1;
    e->used = g_cache_tick;
    pthread_mutex_unlock(&g_cache_mu);
    return e;
  }
  pthread_mutex_unlock(&g_cache_mu);

  code = compile_pat(pat, flags, &ngroups);
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
  e->flags = flags;
  e->code = code;
  e->ngroups = ngroups;
  e->refs = 1;
  e->cached = 0;
  e->used = 0;

  pthread_mutex_lock(&g_cache_mu);
  {
    ReEntry *again = cache_find(pat, flags);
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

int32_t avant_regex_groups(void *handle) {
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

int32_t avant_regex_group_index(void *handle, const char *name) {
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

int64_t avant_regex_find(void *handle, const char *s, int32_t from);

/* Span pairs of this thread's most recent avant_regex_find, copied
 * out of the PCRE2 ovector before any later match can overwrite it.
 * The Avant side reads the pairs it needs into the Match value at
 * once, so no allocation happens on the find path. */
static __thread int32_t g_last_spans[128];
static __thread int32_t g_last_pairs;

/* Packed (lo, hi) of pair i of the last match, or -1 when the pair
 * did not participate or does not exist. */
int64_t avant_regex_span_pair(int32_t i) {
  int32_t lo;
  int32_t hi;
  if (i < 0 || i >= g_last_pairs) {
    return -1;
  }
  lo = g_last_spans[2 * i];
  hi = g_last_spans[2 * i + 1];
  return ((int64_t)lo << 32) | (uint32_t)hi;
}

/* Packed (lo, hi) of pair i of the match at or after `from`,
 * recomputed without allocating. This is the path for groups beyond
 * the ones a Match value carries inline. */
int64_t avant_regex_span_at(void *handle, const char *s, int32_t from,
                            int32_t i) {
  if (avant_regex_find(handle, s, from) < 0) {
    return -1;
  }
  return avant_regex_span_pair(i);
}

/* Finds the first match at or after `from` and keeps its span pairs
 * in thread-local scratch for avant_regex_span_pair. Returns the
 * packed whole-match pair, or -1 on no match. */
int64_t avant_regex_find(void *handle, const char *s, int32_t from) {
  ReEntry *e;
  pcre2_match_data *md;
  size_t n;
  int rc;
  PCRE2_SIZE *ov;
  int32_t pairs;
  int32_t i;
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
    if (!avant_re2c_find_into(handle, s, from, local, 2)) {
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
  if (rc < 0) {
    return -1;
  }
  ov = pcre2_get_ovector_pointer(md);
  pairs = e->ngroups + 1;
  if (pairs > 64) {
    pairs = 64;
  }
  for (i = 0; i < pairs; i++) {
    PCRE2_SIZE a = ov[2 * i];
    PCRE2_SIZE b = ov[2 * i + 1];
    if (a == PCRE2_UNSET) {
      g_last_spans[2 * i] = -1;
      g_last_spans[2 * i + 1] = -1;
    } else {
      g_last_spans[2 * i] = (int32_t)a;
      g_last_spans[2 * i + 1] = (int32_t)b;
    }
  }
  g_last_pairs = pairs;
  return avant_regex_span_pair(0);
}

int32_t avant_regex_count(void *handle, const char *s) {
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
    if (rc < 0) {
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

int32_t avant_regex_error_offset(void) {
  return g_re_off;
}

/* A Regex used after close (Stage 29 W4b2, D295 R11): a program error,
 * not an honest "no match". Printed as a trap (stdout first, status 1). */
void avant_regex_closed(void) {
  fflush(stdout);
  fputs("avant runtime: Regex used after close\n", stderr);
  exit(1);
}
