#include "avant_rt.h"
#include "avant_gc.h"

#include <stdint.h>
#include <string.h>
#include <zlib.h>

/* 0 ok, 1 bad input, 2 any other zlib failure. Thread-local so a
 * returning call can hand back the bytes and the status separately. */
static __thread int32_t g_zlib_status;

static void set_status(int32_t code) {
  g_zlib_status = code;
}

int32_t avant_zlib_take_status(void) {
  int32_t code = g_zlib_status;
  g_zlib_status = 0;
  return code;
}

/* The collector records the allocation size. The string length is that
 * size minus the trailing NUL, so a compressBound slack is dropped by
 * rewriting the header before the object is observed. */
static void tag_len(char *out, size_t n) {
  AvantHeader *h = avant_header(out);
  h->size = (uint32_t)(n + 1);
  out[n] = 0;
  avant_str_tag(out);
}

static void write_len(uint8_t *p, size_t n) {
  p[0] = (uint8_t)((n >> 24) & 255);
  p[1] = (uint8_t)((n >> 16) & 255);
  p[2] = (uint8_t)((n >> 8) & 255);
  p[3] = (uint8_t)(n & 255);
}

static uint32_t read_len(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

char *avant_zlib_compress(const char *s) {
  size_t n = avant_str_n(s);
  uLong bound = compressBound((uLong)n);
  uLong dest_len = bound;
  char *out;
  set_status(0);
  /* RC1: the text is a handle across the allocation. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  avant_size_check((uint64_t)bound + 4, AVANT_SIZE_STRING);
  out = (char *)avant_alloc((uint64_t)bound + 5, AVANT_TYPE_BYTES);
  write_len((uint8_t *)out, n);
  if (compress((Bytef *)(out + 4), &dest_len, (const Bytef *)(s ? s : ""), (uLong)n) != Z_OK) {
    set_status(2);
    return avant_str_empty();
  }
  tag_len(out, 4 + (size_t)dest_len);
  return out;
}

char *avant_zlib_uncompress(const char *s) {
  size_t n = avant_str_n(s);
  const uint8_t *p = (const uint8_t *)(s ? s : "");
  uint32_t orig;
  uLong dest_len;
  char *out;
  set_status(0);
  if (n < 4) {
    set_status(1);
    return avant_str_empty();
  }
  orig = read_len(p);
  dest_len = orig;
  /* A length past the String ceiling is data this runtime cannot
   * return: an error, not a trap (Stage 29 W-INT1). */
  if (orig > (uint32_t)AVANT_SIZE_MAX) {
    set_status(1);
    return avant_str_empty();
  }
  /* RC1/RC2: the text is a handle; its bytes are formed after the
   * allocation. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &s);
  out = avant_str_alloc((uint64_t)orig);
  p = (const uint8_t *)s;
  if (orig == 0) {
    out[0] = 0;
    avant_str_tag(out);
    return out;
  }
  if (uncompress((Bytef *)out, &dest_len, p + 4, (uLong)(n - 4)) != Z_OK || dest_len != orig) {
    set_status(1);
    return avant_str_empty();
  }
  out[orig] = 0;
  avant_str_tag(out);
  return out;
}

int32_t avant_zlib_compress_into(const char *s, void *buf) {
  size_t n = avant_str_n(s);
  uLong bound = compressBound((uLong)n);
  uLong dest_len = bound;
  int32_t at;
  int32_t need;
  uint8_t *data;
  set_status(0);
  if (!buf) {
    set_status(2);
    return 2;
  }
  at = avant_buf_size(buf);
  /* In 64 bits, against the Buf ceiling (Stage 29 W-INT1): the 32-bit
   * sum overflowed before its check. */
  if ((int64_t)at + 4 + (int64_t)bound > (int64_t)AVANT_SIZE_MAX) {
    set_status(2);
    return 2;
  }
  need = at + 4 + (int32_t)bound;
  /* RC1/RC2: the growth may collect; the text and the Buf are handles,
   * and the text's bytes are formed again after it. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_buf_ensure(buf, need);
  data = avant_buf_bytes(buf);
  if (!data) {
    set_status(2);
    return 2;
  }
  write_len(data + at, n);
  if (compress((Bytef *)(data + at + 4), &dest_len, (const Bytef *)(s ? s : ""), (uLong)n) != Z_OK) {
    set_status(2);
    return 2;
  }
  avant_buf_set_size(buf, at + 4 + (int32_t)dest_len);
  return 0;
}

int32_t avant_zlib_uncompress_into(const char *s, void *buf) {
  size_t n = avant_str_n(s);
  const uint8_t *p = (const uint8_t *)(s ? s : "");
  uint32_t orig;
  uLong dest_len;
  int32_t at;
  int32_t need;
  uint8_t *data;
  set_status(0);
  if (!buf || n < 4) {
    set_status(1);
    return 1;
  }
  orig = read_len(p);
  dest_len = orig;
  at = avant_buf_size(buf);
  if ((int64_t)at + (int64_t)orig > (int64_t)AVANT_SIZE_MAX) {
    set_status(2);
    return 2;
  }
  need = at + (int32_t)orig;
  if (orig == 0) {
    return 0;
  }
  /* RC1/RC2: the growth may collect; the text and the Buf are handles,
   * and the text's bytes are formed again after it. */
  AVANT_GC_SCOPE(2);
  AVANT_GC_ROOT(0, &s);
  AVANT_GC_ROOT(1, &buf);
  avant_buf_ensure(buf, need);
  p = (const uint8_t *)s;
  data = avant_buf_bytes(buf);
  if (!data) {
    set_status(2);
    return 2;
  }
  if (uncompress((Bytef *)(data + at), &dest_len, p + 4, (uLong)(n - 4)) != Z_OK || dest_len != orig) {
    set_status(1);
    return 1;
  }
  avant_buf_set_size(buf, at + (int32_t)orig);
  return 0;
}
