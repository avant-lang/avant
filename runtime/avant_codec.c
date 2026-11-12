#include "avant_rt.h"
#include "avant_gc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

static uint32_t crc_table[4][256];
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
    crc_table[0][i] = c;
  }
  for (i = 0; i < 256; i++) {
    uint32_t c = crc_table[0][i];
    for (j = 1; j < 4; j++) {
      c = crc_table[0][c & 255u] ^ (c >> 8);
      crc_table[j][i] = c;
    }
  }
  crc_ready = 1;
}

/* crc32_bytes(b: Bytes) (Stage 29 W-INT2: the bytes themselves, as the
 * references' uint8_t buffers; the Array(Int) form is gone). */
int32_t avant_crc32_bytes(const char *s) {
  crc_init();
  uint32_t crc = 0xFFFFFFFFu;
  int32_t i = 0;
  int32_t n = 0;
  const uint8_t *src = (const uint8_t *)"";
  if (s) {
    n = (int32_t)avant_str_n(s);
    src = (const uint8_t *)s;
  }
  /* Slicing by 4: the same CRC recurrence applied to four bytes at
   * once through the derived tables; the result is bit-identical. */
  for (; i + 4 <= n; i += 4) {
    uint32_t c = crc ^ src[i];
    crc = crc_table[3][c & 255u] ^ crc_table[2][((c >> 8) ^ src[i + 1]) & 255u] ^
          crc_table[1][((c >> 16) ^ src[i + 2]) & 255u] ^
          crc_table[0][((c >> 24) ^ src[i + 3]) & 255u];
  }
  for (; i < n; i++) {
    crc = crc_table[0][(crc ^ src[i]) & 255u] ^ (crc >> 8);
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
  /* gc-lint: holds nothing (the digest is in C locals; the text is not
   * read after the allocation). */
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
  avant_str_tag(out);
  return out;
}

/* sha256_word0(b: Bytes) (Stage 29 W-INT2: the bytes themselves). */
int32_t avant_sha256_word0(const char *s) {
  uint32_t hashes[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  int32_t i;
  int32_t n = 0;
  const uint8_t *src = (const uint8_t *)"";
  if (s) {
    n = (int32_t)avant_str_n(s);
    src = (const uint8_t *)s;
  }
  /* Eight independent chains (index i & 7): eight steps at a time keep
   * the same per-element arithmetic and the same result. With SSE2 the
   * eight chains are two 4 x 32-bit vectors and the eight bytes are
   * widened into them (Stage 29 W-INT2: with byte input GCC no longer
   * vectorizes the chain loop, 3.7x the time of W-INT1's lanes). */
  i = 0;
#if defined(__SSE2__)
  {
    __m128i lo = _mm_loadu_si128((const __m128i *)(const void *)&hashes[0]);
    __m128i hi = _mm_loadu_si128((const __m128i *)(const void *)&hashes[4]);
    const __m128i zero = _mm_setzero_si128();
    for (; i + 8 <= n; i += 8) {
      __m128i b = _mm_loadl_epi64((const __m128i *)(const void *)(src + i));
      __m128i w = _mm_unpacklo_epi8(b, zero);
      __m128i bl = _mm_unpacklo_epi16(w, zero);
      __m128i bh = _mm_unpackhi_epi16(w, zero);
      lo = _mm_add_epi32(_mm_add_epi32(_mm_slli_epi32(lo, 5), lo), bl);
      hi = _mm_add_epi32(_mm_add_epi32(_mm_slli_epi32(hi, 5), hi), bh);
      lo = _mm_xor_si128(_mm_add_epi32(lo, _mm_slli_epi32(lo, 10)), _mm_srli_epi32(lo, 6));
      hi = _mm_xor_si128(_mm_add_epi32(hi, _mm_slli_epi32(hi, 10)), _mm_srli_epi32(hi, 6));
    }
    _mm_storeu_si128((__m128i *)(void *)&hashes[0], lo);
    _mm_storeu_si128((__m128i *)(void *)&hashes[4], hi);
  }
#else
  for (; i + 8 <= n; i += 8) {
    int k;
    for (k = 0; k < 8; k++) {
      uint32_t hash = hashes[k];
      hash = ((hash << 5) + hash) + src[i + k];
      hash = (hash + (hash << 10)) ^ (hash >> 6);
      hashes[k] = hash;
    }
  }
#endif
  for (; i < n; i++) {
    uint32_t idx = (uint32_t)i & 7u;
    uint32_t hash = hashes[idx];
    hash = ((hash << 5) + hash) + src[i];
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
