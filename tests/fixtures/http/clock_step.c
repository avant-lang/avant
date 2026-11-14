/* An LD_PRELOAD shim for tests/cases/test_std_http_safety.av (Stage 29
 * W-FIX1g, O572): clock_gettime(CLOCK_REALTIME) answers an hour early,
 * as if the wall clock had been stepped back between the moment a
 * deadline is read and the moment it is waited for. A deadline read
 * from the wall clock and waited for on it then lies an hour in the
 * past and the wait ends at once; one read from CLOCK_MONOTONIC and
 * waited for on it is unaffected. Built with
 * cc -shared -fPIC -o clock_step.so clock_step.c -ldl */
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
    ts->tv_sec -= 3600;
  }
  return rc;
}
