#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* The operating system's clock, entropy and sleep, for std.time and
 * std.random. A program links this object only when it imports one of
 * them, so no other program carries it. */

/* n bytes of the system's entropy (getrandom), into p. A failure the
 * kernel reports for good (no getrandom, or the call refused) ends the
 * program: a program that asks for unpredictable bytes must not get
 * predictable ones. */
static void entropy_fill(unsigned char *p, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = getrandom(p + got, n - got, 0);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      fflush(stdout);
      fprintf(stderr, "avant runtime: cannot read the system's entropy (getrandom): %s\n", strerror(errno));
      exit(1);
    }
    got += (size_t)r;
  }
}

/* 64 bits of the system's entropy. */
uint64_t avant_os_entropy_u64(void) {
  uint64_t v = 0;
  entropy_fill((unsigned char *)&v, sizeof v);
  return v;
}

/* n bytes of the system's entropy as Bytes; n is checked by the caller
 * (0 or more, under the String ceiling). The bytes are written into the
 * new object after its allocation: nothing is held across it. */
/* gc-lint: holds nothing (no reference is read after the allocation). */
char *avant_os_entropy_bytes(avant_int n) {
  char *out = avant_str_alloc((uint64_t)n);
  if (n > 0) {
    /* Not native: the bytes go into a heap object a collection could
     * move. getrandom does not block once the system has booted. */
    entropy_fill((unsigned char *)out, (size_t)n);
  }
  out[n] = 0;
  avant_str_tag(out);
  return out;
}

/* The wall clock (CLOCK_REALTIME): nanoseconds since 1970-01-01 UTC, an
 * Int until the year 2262. */
avant_int avant_os_wall_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (avant_int)ts.tv_sec * 1000000000 + (avant_int)ts.tv_nsec;
}

/* Sleeps ms milliseconds (0 or more, checked by the caller), the whole
 * time even when a signal interrupts it. The sleep is native: other
 * threads may collect meanwhile. */
void avant_os_sleep_ms(avant_int ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  avant_gc_native_begin();
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
  avant_gc_native_end();
}
