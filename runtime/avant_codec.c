#include "avant_rt.h"
#include "avant_gc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
  size_t n;
  const uint8_t *p = (const uint8_t *)avant_str_bytes(s, &n);
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

