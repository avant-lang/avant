#include "avant_rt.h"
#include "avant_gc.h"

#define BASE64_STATIC_DEFINE
#include "libbase64.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 0 ok, 1 invalid base64, 2 any other failure. Thread-local so a
 * returning call can hand back the bytes and the status separately. */
static __thread int32_t g_base64_status;

static void set_status(int32_t code) {
  g_base64_status = code;
}

int32_t avant_base64_take_status(void) {
  int32_t code = g_base64_status;
  g_base64_status = 0;
  return code;
}

static size_t encoded_len(size_t n) {
  return ((n + 2) / 3) * 4;
}

static size_t decoded_len(const unsigned char *p, size_t n) {
  size_t out_n = (n / 4) * 3;
  if (out_n > 0 && n > 0 && p[n - 1] == '=') {
    out_n -= 1;
  }
  if (out_n > 0 && n > 1 && p[n - 2] == '=') {
    out_n -= 1;
  }
  return out_n;
}

/* Decodes into out, which holds cap bytes. Returns 1 and sets *wrote on
 * success. Input whose length is not a multiple of four is invalid. */
static int decode_write(char *out, size_t cap, const unsigned char *p, size_t n, size_t *wrote) {
  size_t o = 0;
  if (n % 4 != 0) {
    return 0;
  }
  if (n == 0) {
    *wrote = 0;
    return 1;
  }
  if (!out) {
    return 0;
  }
  if (base64_decode((const char *)p, n, out, &o, 0) != 1 || o > cap) {
    return 0;
  }
  *wrote = o;
  return 1;
}

char *avant_base64_encode(const char *s) {
  size_t n = avant_str_n(s);
  size_t out_n = encoded_len(n);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  char *out;
  size_t wrote = 0;
  set_status(0);
  /* RC1/RC2: the text is a handle; its bytes are read after the
   * allocation. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  out = (char *)avant_alloc((uint64_t)out_n + 1, AVANT_TYPE_BYTES);
  p = (const unsigned char *)(s ? s : "");
  base64_encode((const char *)p, n, out, &wrote, 0);
  out[out_n] = 0;
  avant_str_tag(out);
  return out;
}

char *avant_base64_decode(const char *s) {
  size_t n = avant_str_n(s);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  size_t out_n = decoded_len(p, n);
  char *out;
  size_t wrote = 0;
  set_status(0);
  /* RC1/RC2, as in avant_base64_encode. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  out = (char *)avant_alloc((uint64_t)out_n + 1, AVANT_TYPE_BYTES);
  p = (const unsigned char *)(s ? s : "");
  if (!decode_write(out, out_n, p, n, &wrote)) {
    set_status(1);
    return avant_str_empty();
  }
  out[wrote] = 0;
  avant_str_tag(out);
  return out;
}

int32_t avant_base64_encode_into(const char *s, void *buf) {
  size_t n = avant_str_n(s);
  size_t out_n = encoded_len(n);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  int32_t at;
  int32_t need;
  char *dst;
  size_t wrote = 0;
  set_status(0);
  if (!buf) {
    set_status(2);
    return 2;
  }
  at = avant_buf_size(buf);
  if (out_n > (size_t)INT32_MAX - (size_t)at) {
    set_status(2);
    return 2;
  }
  need = at + (int32_t)out_n;
  /* RC1/RC2: the growth may collect; the text and the Buf are handles,
   * and the text's bytes are formed again after it. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_buf_ensure(buf, need);
  p = (const unsigned char *)(s ? s : "");
  dst = (char *)avant_buf_bytes(buf);
  if (!dst && out_n > 0) {
    set_status(2);
    return 2;
  }
  if (dst) {
    base64_encode((const char *)p, n, dst + at, &wrote, 0);
  }
  if (wrote > out_n) {
    wrote = out_n;
  }
  avant_buf_set_size(buf, at + (int32_t)wrote);
  return 0;
}

int32_t avant_base64_decode_into(const char *s, void *buf) {
  size_t n = avant_str_n(s);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  size_t out_n = decoded_len(p, n);
  int32_t at;
  int32_t need;
  char *dst;
  size_t wrote = 0;
  set_status(0);
  if (!buf) {
    set_status(1);
    return 1;
  }
  at = avant_buf_size(buf);
  if (out_n > (size_t)INT32_MAX - (size_t)at) {
    set_status(2);
    return 2;
  }
  need = at + (int32_t)out_n;
  /* RC1/RC2: the growth may collect; the text and the Buf are handles,
   * and the text's bytes are formed again after it. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_buf_ensure(buf, need);
  p = (const unsigned char *)(s ? s : "");
  dst = (char *)avant_buf_bytes(buf);
  if (!decode_write(dst ? dst + at : NULL, out_n, p, n, &wrote)) {
    set_status(1);
    return 1;
  }
  avant_buf_set_size(buf, at + (int32_t)wrote);
  return 0;
}
