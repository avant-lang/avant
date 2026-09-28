#define PCRE2_CODE_UNIT_WIDTH 8
#include "avant_rt.h"

#include <pcre2.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_N 32

typedef struct {
  char *pat;
  int32_t caseless;
  pcre2_code *code;
} ReCache;

static ReCache g_cache[CACHE_N];
static int g_cache_used;

static __thread int32_t g_m0;
static __thread int32_t g_m1;
static __thread int32_t g_c0;
static __thread int32_t g_c1;

static pcre2_code *compile_pat(const char *pat, int32_t flags) {
  int err = 0;
  PCRE2_SIZE erroff = 0;
  uint32_t opts = 0;
  pcre2_code *code;
  if (flags & 1) {
    opts |= PCRE2_CASELESS;
  }
  code = pcre2_compile((PCRE2_SPTR)pat, PCRE2_ZERO_TERMINATED, opts, &err, &erroff, NULL);
  if (code) {
    (void)pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
  }
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

void *avant_re_compile(const char *pat, int32_t flags) {
  if (!pat) {
    pat = "";
  }
  return compile_pat(pat, flags);
}

int32_t avant_re_m0(void) {
  return g_m0;
}

int32_t avant_re_m1(void) {
  return g_m1;
}

int32_t avant_re_c0(void) {
  return g_c0;
}

int32_t avant_re_c1(void) {
  return g_c1;
}

int32_t avant_re_find(void *re, const char *s, int32_t from) {
  pcre2_code *code = (pcre2_code *)re;
  pcre2_match_data *md;
  size_t n;
  int rc;
  PCRE2_SIZE *ov;
  g_m0 = g_m1 = g_c0 = g_c1 = 0;
  if (!code) {
    return 0;
  }
  if (!s) {
    s = "";
  }
  n = avant_str_n(s);
  if (from < 0) {
    from = 0;
  }
  if ((size_t)from > n) {
    return 0;
  }
  md = md_for(code);
  if (!md) {
    return 0;
  }
  rc = pcre2_match(code, (PCRE2_SPTR)s, n, (PCRE2_SIZE)from, 0, md, NULL);
  if (rc < 0) {
    return 0;
  }
  ov = pcre2_get_ovector_pointer(md);
  g_m0 = (int32_t)ov[0];
  g_m1 = (int32_t)ov[1];
  if (rc > 1) {
    g_c0 = (int32_t)ov[2];
    g_c1 = (int32_t)ov[3];
  } else {
    g_c0 = g_m0;
    g_c1 = g_m1;
  }
  return 1;
}

static pcre2_code *cached(const char *pat, int32_t caseless) {
  int i;
  if (!pat) {
    pat = "";
  }
  for (i = 0; i < g_cache_used; i++) {
    if (g_cache[i].caseless == caseless && g_cache[i].pat && strcmp(g_cache[i].pat, pat) == 0) {
      return g_cache[i].code;
    }
  }
  pcre2_code *code = compile_pat(pat, caseless);
  if (!code) {
    return NULL;
  }
  if (g_cache_used < CACHE_N) {
    size_t n = strlen(pat);
    char *copy = (char *)malloc(n + 1);
    if (copy) {
      memcpy(copy, pat, n + 1);
      g_cache[g_cache_used].pat = copy;
      g_cache[g_cache_used].caseless = caseless;
      g_cache[g_cache_used].code = code;
      g_cache_used += 1;
    }
  }
  return code;
}

int32_t avant_re_count(const char *pat, const char *s, int32_t caseless) {
  pcre2_code *re = cached(pat, caseless ? 1 : 0);
  pcre2_match_data *md;
  size_t slen;
  PCRE2_SIZE start = 0;
  int32_t n = 0;
  if (!re) {
    return 0;
  }
  if (!s) {
    s = "";
  }
  slen = avant_str_n(s);
  md = md_for(re);
  if (!md) {
    return 0;
  }
  while (start <= slen) {
    int rc = pcre2_match(re, (PCRE2_SPTR)s, slen, start, 0, md, NULL);
    PCRE2_SIZE *ov;
    PCRE2_SIZE next;
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
