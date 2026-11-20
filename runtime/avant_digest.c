/* std.digest's runtime: CRC-32 (zlib's, the reflected polynomial
 * 0xEDB88320) and SHA-256 (FIPS 180-4), each the fastest code this
 * machine runs, chosen once from CPUID:
 *
 *  - CRC-32: Chromium's carry-less-multiply fold (third_party/
 *    crc32-chromium: four 128-bit lanes folded 64 bytes at a time with
 *    PCLMULQDQ, then a Barrett reduction; about 0.25 cycles a byte, the
 *    PCLMULQDQ throughput limit) for the multiple-of-16 bulk of an input
 *    of 64 bytes or more; slicing by 8 (eight 256-entry tables, eight
 *    bytes a step, about 1.9 cycles a byte) for the rest and on a CPU
 *    without PCLMULQDQ and SSE4.1.
 *  - SHA-256: the SHA extensions (third_party/sha-intrinsics: SHA256RNDS2
 *    for two rounds, SHA256MSG1 and MSG2 for the message schedule; about
 *    2 cycles a byte) on a CPU with SHA, SSE4.1 and SSSE3; the FIPS
 *    180-4 compression function in C otherwise (about 14 cycles a byte).
 *
 * The vendored code is compiled here, under per-function target
 * attributes, so the object runs on any x86-64 and picks its path at run
 * time; on another machine only the portable paths exist. The object
 * links only into a program that imports std.digest (pipeline.av
 * fill_std_link). */
#include "avant_rt.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define DIGEST_X86 1
#include <cpuid.h>
#endif

/* ---- CRC-32, slicing by 8 */

static uint32_t crc_tables[8][256];
static int crc_tables_ready;

static void crc_tables_init(void) {
  uint32_t i;
  uint32_t j;
  for (i = 0; i < 256; i++) {
    uint32_t c = i;
    for (j = 0; j < 8; j++) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    crc_tables[0][i] = c;
  }
  for (i = 0; i < 256; i++) {
    uint32_t c = crc_tables[0][i];
    for (j = 1; j < 8; j++) {
      c = crc_tables[0][c & 255u] ^ (c >> 8);
      crc_tables[j][i] = c;
    }
  }
  __atomic_store_n(&crc_tables_ready, 1, __ATOMIC_RELEASE);
}

/* The inner CRC (not inverted) over n bytes. */
static uint32_t crc_slice8(uint32_t crc, const uint8_t *p, size_t n) {
  while (n >= 8) {
    uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    crc = crc_tables[7][a & 255u] ^ crc_tables[6][(a >> 8) & 255u] ^ crc_tables[5][(a >> 16) & 255u] ^
          crc_tables[4][a >> 24] ^ crc_tables[3][b & 255u] ^ crc_tables[2][(b >> 8) & 255u] ^
          crc_tables[1][(b >> 16) & 255u] ^ crc_tables[0][b >> 24];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    crc = crc_tables[0][(crc ^ *p) & 255u] ^ (crc >> 8);
    p++;
    n--;
  }
  return crc;
}

#if defined(DIGEST_X86)
/* The names the Chromium excerpt expects from zlib's headers. */
#define zalign(n) __attribute__((aligned(n)))
#define ZLIB_INTERNAL
typedef size_t z_size_t;
#include "third_party/crc32-chromium/crc32_sse42_simd.c"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("sha,sse4.1,ssse3"))), apply_to = function)
#include "third_party/sha-intrinsics/sha256-x86.c"
#pragma clang attribute pop
#else
#pragma GCC push_options
#pragma GCC target("sha,sse4.1,ssse3")
#include "third_party/sha-intrinsics/sha256-x86.c"
#pragma GCC pop_options
#endif
#endif

/* 1 once chosen; bit 1 the PCLMULQDQ fold, bit 2 the SHA extensions. */
static int digest_paths;

static int digest_cpu(void) {
  int p = __atomic_load_n(&digest_paths, __ATOMIC_ACQUIRE);
  if (p != 0) {
    return p;
  }
  p = 1;
#if defined(DIGEST_X86)
  /* The suite's internal variable: the portable code alone, so the
   * suite checks it on a machine that has the fast paths. */
  const char *portable = getenv("AVANT_DIGEST_PORTABLE");
  if (!(portable && portable[0] == '1')) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    unsigned max = __get_cpuid_max(0, 0);
    int sse41 = 0;
    int ssse3 = 0;
    if (max >= 1 && __get_cpuid(1, &a, &b, &c, &d)) {
      sse41 = (c & bit_SSE4_1) != 0;
      ssse3 = (c & bit_SSSE3) != 0;
      if ((c & bit_PCLMUL) && sse41) {
        p |= 2;
      }
    }
    if (max >= 7 && __get_cpuid_count(7, 0, &a, &b, &c, &d)) {
      if ((b & bit_SHA) && sse41 && ssse3) {
        p |= 4;
      }
    }
  }
#endif
  if (!__atomic_load_n(&crc_tables_ready, __ATOMIC_ACQUIRE)) {
    crc_tables_init();
  }
  __atomic_store_n(&digest_paths, p, __ATOMIC_RELEASE);
  return p;
}

/* digest.crc32_update(crc, data): zlib's crc32(crc, data, n). */
uint32_t avant_digest_crc32(uint32_t crc, const char *s) {
  int paths = digest_cpu();
  size_t n;
  const uint8_t *p = (const uint8_t *)avant_str_bytes(s, &n);
  uint32_t c = ~crc;
#if defined(DIGEST_X86)
  if ((paths & 2) && n >= 64) {
    size_t bulk = n & ~(size_t)15;
    c = crc32_sse42_simd_(p, bulk, c);
    p += bulk;
    n -= bulk;
  }
#else
  (void)paths;
#endif
  return ~crc_slice8(c, p, n);
}

/* ---- SHA-256 */

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* FIPS 180-4's compression function over n / 64 blocks, the schedule
 * rolled in 16 words. */
static void sha256_blocks_c(uint32_t st[8], const uint8_t *p, size_t n) {
  while (n >= 64) {
    uint32_t w[16];
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    int i;
    for (i = 0; i < 64; i++) {
      uint32_t wi;
      if (i < 16) {
        wi = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
      } else {
        uint32_t w15 = w[(i - 15) & 15];
        uint32_t w2 = w[(i - 2) & 15];
        uint32_t s0 = ROR(w15, 7) ^ ROR(w15, 18) ^ (w15 >> 3);
        uint32_t s1 = ROR(w2, 17) ^ ROR(w2, 19) ^ (w2 >> 10);
        wi = w[i & 15] + s0 + w[(i - 7) & 15] + s1;
      }
      w[i & 15] = wi;
      {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha_k[i] + wi;
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
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
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
    p += 64;
    n -= 64;
  }
}

static void sha256_blocks(int paths, uint32_t st[8], const uint8_t *p, size_t n) {
#if defined(DIGEST_X86)
  if (paths & 4) {
    /* The vendored function counts in 32 bits: a Bytes is under 2 GiB,
     * and whole blocks go in. */
    sha256_process_x86(st, p, (uint32_t)(n & ~(size_t)63));
    return;
  }
#else
  (void)paths;
#endif
  sha256_blocks_c(st, p, n);
}

/* The 32-byte digest of the bytes into out. */
static void sha256_of(const char *s, uint8_t out[32]) {
  int paths = digest_cpu();
  uint32_t st[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  uint8_t block[128];
  size_t n;
  const uint8_t *p = (const uint8_t *)avant_str_bytes(s, &n);
  size_t whole = n & ~(size_t)63;
  size_t rest = n - whole;
  size_t tail;
  uint64_t bits = (uint64_t)n * 8u;
  int i;
  if (whole > 0) {
    sha256_blocks(paths, st, p, whole);
  }
  memset(block, 0, sizeof(block));
  if (rest > 0) {
    memcpy(block, p + whole, rest);
  }
  block[rest] = 0x80;
  tail = rest < 56 ? 64 : 128;
  for (i = 0; i < 8; i++) {
    block[tail - 1 - i] = (uint8_t)(bits >> (8 * i));
  }
  sha256_blocks(paths, st, block, tail);
  for (i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(st[i] >> 24);
    out[4 * i + 1] = (uint8_t)(st[i] >> 16);
    out[4 * i + 2] = (uint8_t)(st[i] >> 8);
    out[4 * i + 3] = (uint8_t)st[i];
  }
}

/* digest.sha256(data): the 32 bytes. */
char *avant_digest_sha256(const char *s) {
  uint8_t d[32];
  char *out;
  sha256_of(s, d);
  /* gc-lint: holds nothing (the digest is in a C local; the input is not
   * read after the allocation). */
  out = avant_str_alloc(32);
  memcpy(out, d, 32);
  out[32] = 0;
  avant_str_tag(out);
  return out;
}

/* digest.sha256_hex(data): the 64 lower-case hex digits. */
char *avant_digest_sha256_hex(const char *s) {
  static const char HEX[] = "0123456789abcdef";
  uint8_t d[32];
  char *out;
  int i;
  sha256_of(s, d);
  /* gc-lint: holds nothing (as above). */
  out = avant_str_alloc(64);
  for (i = 0; i < 32; i++) {
    out[2 * i] = HEX[d[i] >> 4];
    out[2 * i + 1] = HEX[d[i] & 15];
  }
  out[64] = 0;
  avant_str_tag(out);
  return out;
}
