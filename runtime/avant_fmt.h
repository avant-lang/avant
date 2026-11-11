/* The runtime's float writers (Stage 28 W18S, O48): `%.*f` (`%g` retired
 * with the shortest round-trip text, Stage 29 W4b1)
 * without snprintf, byte for byte what glibc prints.
 *
 * A finite double is m * 2^e exactly (m < 2^53). `%.Pf` prints
 * round(|v| * 10^P) with the decimal point P digits from the right, the
 * rounding to nearest with ties to even on the exact binary value (glibc's
 * rule in the default rounding mode). For P <= 16, m * 5^P < 2^91 fits in
 * 128 bits, so v * 10^P = (m * 5^P) * 2^(e + P) is an exact shift: right
 * shifts keep the remainder for the rounding, left shifts are exact. The
 * writers here take the case where the rounded result fits in 64 bits
 * (|v| below about 1.8e19 / 10^P); everything else (NaN, infinities, huge
 * values, `%g`'s exponent form) returns -1 and the caller uses snprintf,
 * so the output is always glibc's. Proven against snprintf on random and
 * edge values (records/2026-11-10-s28-w18s, fmt/).
 *
 * `%g` (precision 6) is `%.(5-X)f` with trailing zeros and a bare point
 * removed when its decimal exponent X (after rounding to six significant
 * digits) is in [-4, 5], which is the fixed writer's range; X itself is
 * found by rounding at the estimated exponent and stepping once when the
 * rounded value has seven digits or five.
 *
 * Header-only (static): avant_str.o and avant_buf.o each carry a copy, as
 * neither object may grow a new global symbol for the other. */
#ifndef AVANT_FMT_H
#define AVANT_FMT_H

#include <stdint.h>
#include <string.h>

static const uint64_t avant_fmt_pow5[17] = {
    1ull, 5ull, 25ull, 125ull, 625ull, 3125ull, 15625ull, 78125ull, 390625ull, 1953125ull, 9765625ull,
    48828125ull, 244140625ull, 1220703125ull, 6103515625ull, 30517578125ull, 152587890625ull};

static const uint64_t avant_fmt_pow10[20] = {
    1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull,
    10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull, 100000000000000ull,
    1000000000000000ull, 10000000000000000ull, 100000000000000000ull, 1000000000000000000ull,
    10000000000000000000ull};

static const char avant_fmt_d2[200] = {
    '0', '0', '0', '1', '0', '2', '0', '3', '0', '4', '0', '5', '0', '6', '0', '7', '0', '8', '0', '9',
    '1', '0', '1', '1', '1', '2', '1', '3', '1', '4', '1', '5', '1', '6', '1', '7', '1', '8', '1', '9',
    '2', '0', '2', '1', '2', '2', '2', '3', '2', '4', '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',
    '3', '0', '3', '1', '3', '2', '3', '3', '3', '4', '3', '5', '3', '6', '3', '7', '3', '8', '3', '9',
    '4', '0', '4', '1', '4', '2', '4', '3', '4', '4', '4', '5', '4', '6', '4', '7', '4', '8', '4', '9',
    '5', '0', '5', '1', '5', '2', '5', '3', '5', '4', '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',
    '6', '0', '6', '1', '6', '2', '6', '3', '6', '4', '6', '5', '6', '6', '6', '7', '6', '8', '6', '9',
    '7', '0', '7', '1', '7', '2', '7', '3', '7', '4', '7', '5', '7', '6', '7', '7', '7', '8', '7', '9',
    '8', '0', '8', '1', '8', '2', '8', '3', '8', '4', '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',
    '9', '0', '9', '1', '9', '2', '9', '3', '9', '4', '9', '5', '9', '6', '9', '7', '9', '8', '9', '9'};

/* round(|v| * 10^prec), ties to even, into *q; 0 when it does not fit in
 * 64 bits or v is not finite (the caller falls back). *neg gets v's sign
 * bit. 0 <= prec <= 16. */
static int avant_fmt_scaled(double v, int prec, uint64_t *q, int *neg) {
  uint64_t bits;
  uint64_t m;
  int bexp;
  int s;
  unsigned __int128 n;
  memcpy(&bits, &v, sizeof bits);
  *neg = (int)(bits >> 63);
  bexp = (int)((bits >> 52) & 0x7ff);
  if (bexp == 0x7ff) {
    return 0;
  }
  m = bits & ((1ull << 52) - 1);
  if (bexp == 0) {
    s = -1074 + prec;
  } else {
    m |= 1ull << 52;
    s = bexp - 1075 + prec;
  }
  if (m == 0) {
    *q = 0;
    return 1;
  }
  n = (unsigned __int128)m * avant_fmt_pow5[prec];
  if (s >= 0) {
    if (s >= 64 || (n >> (64 - s)) != 0) {
      return 0;
    }
    *q = (uint64_t)(n << s);
    return 1;
  }
  s = -s;
  if (s >= 92) {
    /* n < 2^91 <= 2^(s-1): below one half, rounds to 0. */
    *q = 0;
    return 1;
  }
  {
    unsigned __int128 r = n >> s;
    unsigned __int128 rem = n - (r << s);
    unsigned __int128 half = (unsigned __int128)1 << (s - 1);
    if (rem > half || (rem == half && (r & 1))) {
      r += 1;
    }
    if (r >> 64) {
      return 0;
    }
    *q = (uint64_t)r;
  }
  return 1;
}

/* The decimal digits of x, right-aligned ending at end (exclusive); returns
 * the start. */
static char *avant_fmt_u64(char *end, uint64_t x) {
  while (x >= 100) {
    unsigned d = (unsigned)(x % 100);
    x /= 100;
    end -= 2;
    end[0] = avant_fmt_d2[2 * d];
    end[1] = avant_fmt_d2[2 * d + 1];
  }
  if (x >= 10) {
    end -= 2;
    end[0] = avant_fmt_d2[2 * x];
    end[1] = avant_fmt_d2[2 * x + 1];
  } else {
    *--end = (char)('0' + x);
  }
  return end;
}

/* `%.*f` of v into out (at least 48 bytes) from the rounded q: the length. */
static int avant_fmt_fixed_q(char *out, uint64_t q, int neg, int prec) {
  char tmp[48];
  char *end = tmp + sizeof tmp;
  char *p;
  int n = 0;
  uint64_t ip = q / avant_fmt_pow10[prec];
  uint64_t fp = q - ip * avant_fmt_pow10[prec];
  if (prec > 0) {
    p = avant_fmt_u64(end, fp);
    while (end - p < prec) {
      *--p = '0';
    }
    *--p = '.';
    end = p;
  }
  p = avant_fmt_u64(end, ip);
  if (neg) {
    out[n++] = '-';
  }
  memcpy(out + n, p, (size_t)(tmp + sizeof tmp - p));
  return n + (int)(tmp + sizeof tmp - p);
}

/* snprintf(out, 48, "%.*f", prec, v) for 0 <= prec <= 16, or -1 when v is
 * outside the writer's range (the caller then calls snprintf). */
static int avant_fmt_fixed(char *out, double v, int prec) {
  uint64_t q;
  int neg;
  if (!avant_fmt_scaled(v, prec, &q, &neg)) {
    return -1;
  }
  return avant_fmt_fixed_q(out, q, neg, prec);
}

#endif
