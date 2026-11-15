/* An LD_PRELOAD shim for tests/cases/test_gc_checks.av:
 * clock_gettime(CLOCK_REALTIME) answers an hour late,
 * as if the wall clock had been stepped forward between the moment a
 * deadline is read and the moment it is waited for. A deadline read from
 * the wall clock and waited for on it then lies an hour after the
 * moment meant (the wait sleeps that hour, or until a wake-up); one
 * read from CLOCK_MONOTONIC and waited for on it is unaffected. The
 * other direction of tests/fixtures/http/clock_step.c (an hour early),
 * which a stalled handshake's report would not show: a deadline in the
 * past wakes the wait at once and the report keys on the monotonic
 * clock. Built with cc -shared -fPIC -o clock_ahead.so clock_ahead.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <time.h>

int clock_gettime(clockid_t clk, struct timespec *ts) {
  static int (*real)(clockid_t, struct timespec *);
  int rc;
  if (!real) {
    real = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
  }
  rc = real(clk, ts);
  if (rc == 0 && clk == CLOCK_REALTIME) {
    ts->tv_sec += 3600;
  }
  return rc;
}
