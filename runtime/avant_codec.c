#include "avant_rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encoded_len(size_t n) {
  return ((n + 2) / 3) * 4;
}

static void b64_write(char *out, const unsigned char *p, size_t n) {
  size_t i = 0;
  size_t o = 0;
  while (i < n) {
    unsigned b0 = p[i];
    unsigned b1 = (i + 1 < n) ? p[i + 1] : 0;
    unsigned b2 = (i + 2 < n) ? p[i + 2] : 0;
    size_t remain = n - i;
    out[o++] = B64[b0 >> 2];
    out[o++] = B64[((b0 & 3) << 4) | (b1 >> 4)];
    if (remain == 1) {
      out[o++] = '=';
      out[o++] = '=';
    } else if (remain == 2) {
      out[o++] = B64[((b1 & 15) << 2) | (b2 >> 6)];
      out[o++] = '=';
    } else {
      out[o++] = B64[((b1 & 15) << 2) | (b2 >> 6)];
      out[o++] = B64[b2 & 63];
    }
    i += 3;
  }
}

char *avant_b64_encode(const char *s) {
  size_t n = avant_str_n(s);
  size_t out_n = b64_encoded_len(n);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  avant_gc_defer_enter();
  char *out = (char *)avant_alloc((uint64_t)out_n + 1, AVANT_TYPE_BYTES);
  b64_write(out, p, n);
  out[out_n] = 0;
  avant_gc_defer_leave();
  return out;
}

void avant_b64_encode_buf(void *buf, const char *s) {
  size_t n = avant_str_n(s);
  size_t out_n = b64_encoded_len(n);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  if (out_n > (size_t)INT32_MAX) {
    avant_buf_clear(buf);
    return;
  }
  avant_gc_defer_enter();
  avant_buf_clear(buf);
  avant_buf_ensure(buf, (int32_t)out_n);
  char *dst = (char *)avant_buf_bytes(buf);
  if (dst) {
    b64_write(dst, p, n);
  }
  avant_buf_set_size(buf, (int32_t)out_n);
  avant_gc_defer_leave();
}

static int b64_val(unsigned char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

static size_t b64_decoded_len(const unsigned char *p, size_t n) {
  size_t out_n = (n / 4) * 3;
  if (out_n > 0 && n > 0 && p[n - 1] == '=') {
    out_n -= 1;
  }
  if (out_n > 0 && n > 1 && p[n - 2] == '=') {
    out_n -= 1;
  }
  return out_n;
}

static size_t b64_decode_write(char *out, const unsigned char *p, size_t n, size_t out_n) {
  size_t i = 0;
  size_t o = 0;
  while (i < n && o < out_n) {
    int c0 = b64_val(p[i]);
    int c1 = (i + 1 < n) ? b64_val(p[i + 1]) : -1;
    int c2 = (i + 2 < n) ? b64_val(p[i + 2]) : -1;
    int c3 = (i + 3 < n) ? b64_val(p[i + 3]) : -1;
    if (c0 >= 0 && c1 >= 0 && o < out_n) {
      out[o++] = (char)(((c0 << 2) | (c1 >> 4)) & 255);
    }
    if (c2 >= 0 && o < out_n) {
      out[o++] = (char)(((c1 << 4) | (c2 >> 2)) & 255);
    }
    if (c3 >= 0 && o < out_n) {
      out[o++] = (char)(((c2 << 6) | c3) & 255);
    }
    i += 4;
  }
  return o;
}

char *avant_b64_decode(const char *s) {
  size_t n = avant_str_n(s);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  size_t out_n = b64_decoded_len(p, n);
  avant_gc_defer_enter();
  char *out = (char *)avant_alloc((uint64_t)out_n + 1, AVANT_TYPE_BYTES);
  size_t o = b64_decode_write(out, p, n, out_n);
  out[o] = 0;
  avant_gc_defer_leave();
  return out;
}

void avant_b64_decode_buf(void *buf, const char *s) {
  size_t n = avant_str_n(s);
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  size_t out_n = b64_decoded_len(p, n);
  if (out_n > (size_t)INT32_MAX) {
    avant_buf_clear(buf);
    return;
  }
  avant_gc_defer_enter();
  avant_buf_clear(buf);
  avant_buf_ensure(buf, (int32_t)out_n);
  char *dst = (char *)avant_buf_bytes(buf);
  size_t o = 0;
  if (dst) {
    o = b64_decode_write(dst, p, n, out_n);
  }
  avant_buf_set_size(buf, (int32_t)o);
  avant_gc_defer_leave();
}

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
  uint32_t i;
  uint32_t j;
  if (crc_ready) {
    return;
  }
  for (i = 0; i < 256; i++) {
    uint32_t c = i;
    for (j = 0; j < 8; j++) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    crc_table[i] = c;
  }
  crc_ready = 1;
}

int32_t avant_crc32_i32(void *arr) {
  crc_init();
  AvantArray *a = (AvantArray *)arr;
  uint32_t crc = 0xFFFFFFFFu;
  int32_t i;
  int32_t n = 0;
  const int32_t *src = NULL;
  if (a && a->buf && a->size > 0) {
    n = a->size;
    src = (const int32_t *)a->buf;
  }
  for (i = 0; i < n; i++) {
    crc = crc_table[(crc ^ (uint8_t)(src[i] & 255)) & 255u] ^ (crc >> 8);
  }
  return (int32_t)(crc ^ 0xFFFFFFFFu);
}

static void sha256_transform(uint32_t *state, const uint8_t *block) {
  static const uint32_t K[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^ (w[i - 15] >> 3);
    uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = state[0];
  b = state[1];
  c = state[2];
  d = state[3];
  e = state[4];
  f = state[5];
  g = state[6];
  h = state[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

char *avant_sha256(const char *s) {
  static const char HEX[] = "0123456789abcdef";
  uint32_t state[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  uint8_t block[64];
  size_t n = avant_str_n(s);
  const uint8_t *p = (const uint8_t *)(s ? s : "");
  uint64_t bit_len = (uint64_t)n * 8u;
  size_t off = 0;
  int i;
  char *out;
  while (off + 64 <= n) {
    sha256_transform(state, p + off);
    off += 64;
  }
  memset(block, 0, sizeof(block));
  if (n > off) {
    memcpy(block, p + off, n - off);
  }
  block[n - off] = 0x80;
  if (n - off >= 56) {
    sha256_transform(state, block);
    memset(block, 0, sizeof(block));
  }
  block[63] = (uint8_t)bit_len;
  block[62] = (uint8_t)(bit_len >> 8);
  block[61] = (uint8_t)(bit_len >> 16);
  block[60] = (uint8_t)(bit_len >> 24);
  block[59] = (uint8_t)(bit_len >> 32);
  block[58] = (uint8_t)(bit_len >> 40);
  block[57] = (uint8_t)(bit_len >> 48);
  block[56] = (uint8_t)(bit_len >> 56);
  sha256_transform(state, block);
  avant_gc_defer_enter();
  out = (char *)avant_alloc(65, AVANT_TYPE_BYTES);
  for (i = 0; i < 8; i++) {
    uint32_t v = state[i];
    out[i * 8 + 0] = HEX[(v >> 28) & 15];
    out[i * 8 + 1] = HEX[(v >> 24) & 15];
    out[i * 8 + 2] = HEX[(v >> 20) & 15];
    out[i * 8 + 3] = HEX[(v >> 16) & 15];
    out[i * 8 + 4] = HEX[(v >> 12) & 15];
    out[i * 8 + 5] = HEX[(v >> 8) & 15];
    out[i * 8 + 6] = HEX[(v >> 4) & 15];
    out[i * 8 + 7] = HEX[v & 15];
  }
  out[64] = 0;
  avant_gc_defer_leave();
  return out;
}

char *avant_zlib_compress(const char *s) {
  size_t n = avant_str_n(s);
  uLong bound = compressBound((uLong)n);
  uLong dest_len = bound;
  uint8_t *raw;
  char *out;
  avant_gc_defer_enter();
  raw = (uint8_t *)malloc((size_t)bound);
  if (!raw) {
    avant_gc_defer_leave();
    return avant_str_empty();
  }
  if (compress(raw, &dest_len, (const Bytef *)(s ? s : ""), (uLong)n) != Z_OK) {
    free(raw);
    avant_gc_defer_leave();
    return avant_str_empty();
  }
  out = (char *)avant_alloc((uint64_t)dest_len + 5, AVANT_TYPE_BYTES);
  out[0] = (char)((n >> 24) & 255);
  out[1] = (char)((n >> 16) & 255);
  out[2] = (char)((n >> 8) & 255);
  out[3] = (char)(n & 255);
  memcpy(out + 4, raw, (size_t)dest_len);
  out[4 + dest_len] = 0;
  free(raw);
  avant_gc_defer_leave();
  return out;
}

char *avant_zlib_uncompress(const char *s) {
  size_t n = avant_str_n(s);
  const uint8_t *p = (const uint8_t *)(s ? s : "");
  uint32_t orig;
  uLong dest_len;
  char *out;
  if (n < 4) {
    return avant_str_empty();
  }
  orig = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  dest_len = orig;
  avant_gc_defer_enter();
  out = (char *)avant_alloc((uint64_t)orig + 1, AVANT_TYPE_BYTES);
  if (orig == 0) {
    out[0] = 0;
    avant_gc_defer_leave();
    return out;
  }
  if (uncompress((Bytef *)out, &dest_len, p + 4, (uLong)(n - 4)) != Z_OK || dest_len != orig) {
    avant_gc_defer_leave();
    return avant_str_empty();
  }
  out[orig] = 0;
  avant_gc_defer_leave();
  return out;
}

int32_t avant_sha256_word0(void *arr) {
  uint32_t hashes[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  AvantArray *a = (AvantArray *)arr;
  int32_t i;
  int32_t n = 0;
  const int32_t *src = NULL;
  if (a && a->buf && a->size > 0) {
    n = a->size;
    src = (const int32_t *)a->buf;
  }
  for (i = 0; i < n; i++) {
    uint32_t idx = (uint32_t)i & 7u;
    uint32_t hash = hashes[idx];
    hash = ((hash << 5) + hash) + (uint8_t)(src[i] & 255);
    hash = (hash + (hash << 10)) ^ (hash >> 6);
    hashes[idx] = hash;
  }
  uint32_t h = hashes[0];
  uint32_t b0 = (h >> 24) & 255u;
  uint32_t b1 = (h >> 16) & 255u;
  uint32_t b2 = (h >> 8) & 255u;
  uint32_t b3 = h & 255u;
  return (int32_t)(b0 + (b1 << 8) + (b2 << 16) + (b3 << 24));
}
