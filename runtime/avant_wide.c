#include "avant_rt.h"

#include <stdint.h>

static __thread int32_t g_wide_hi;

int32_t avant_wide_hi(void) {
  return g_wide_hi;
}

static uint64_t pack_u(int32_t lo, int32_t hi) {
  return (uint64_t)(uint32_t)lo | ((uint64_t)(uint32_t)hi << 32);
}

static int64_t pack_s(int32_t lo, int32_t hi) {
  return (int64_t)pack_u(lo, hi);
}

static int32_t finish_u(uint64_t v) {
  g_wide_hi = (int32_t)(uint32_t)(v >> 32);
  return (int32_t)(uint32_t)v;
}

static int32_t finish_s(int64_t v) {
  return finish_u((uint64_t)v);
}

static int64_t i64_abs(int64_t x) {
  if (x >= 0) {
    return x;
  }
  if (x == INT64_MIN) {
    return x;
  }
  return -x;
}

static int64_t crystal_floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  int64_t r = a % b;
  if (r != 0 && ((a < 0 && b > 0) || (a > 0 && b < 0))) {
    q -= 1;
  }
  return q;
}

static int64_t simple_div(int64_t a, int64_t b) {
  if (b == 0) {
    return 0;
  }
  if ((a >= 0 && b > 0) || (a < 0 && b < 0)) {
    return crystal_floor_div(a, b);
  }
  if (a == INT64_MIN && b == -1) {
    return a;
  }
  return -(i64_abs(a) / i64_abs(b));
}

static int64_t simple_mod(int64_t a, int64_t b) {
  if (b == 0) {
    return 0;
  }
  return a - simple_div(a, b) * b;
}

int32_t avant_wide_op(int32_t op, int32_t a_lo, int32_t a_hi, int32_t b_lo, int32_t b_hi) {
  uint64_t ua = pack_u(a_lo, a_hi);
  uint64_t ub = pack_u(b_lo, b_hi);
  int64_t sa = pack_s(a_lo, a_hi);
  int64_t sb = pack_s(b_lo, b_hi);
  switch (op) {
  case 0:
    return finish_u(ua + ub);
  case 1:
    return finish_u(ua - ub);
  case 2:
    return finish_u(ua * ub);
  case 3:
    return finish_s(simple_div(sa, sb));
  case 4:
    return finish_s(simple_mod(sa, sb));
  case 5:
    if (ub == 0) {
      return finish_u(0);
    }
    return finish_u(ua / ub);
  case 6: {
    int32_t n = b_lo;
    if (n <= 0) {
      return finish_u(ua);
    }
    if (n >= 64) {
      return finish_u(0);
    }
    return finish_u(ua << n);
  }
  case 7: {
    int32_t n = b_lo;
    if (n <= 0) {
      return finish_u(ua);
    }
    if (n >= 64) {
      return finish_u(0);
    }
    return finish_u(ua >> n);
  }
  case 8:
    return finish_u(ua & ub);
  case 9:
    return finish_u(ua | ub);
  case 10:
    return finish_u(ua ^ ub);
  case 11:
    g_wide_hi = 0;
    return ua < ub ? 1 : 0;
  case 12:
    g_wide_hi = 0;
    return ua == ub ? 1 : 0;
  case 13:
    g_wide_hi = 0;
    return sa < sb ? 1 : 0;
  default:
    g_wide_hi = 0;
    return 0;
  }
}
