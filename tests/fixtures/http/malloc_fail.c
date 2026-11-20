/* An LD_PRELOAD shim for tests/cases/test_std_http_safety.av: while the
 * environment variable AVANT_TEST_MALLOC_FAIL holds a number of bytes,
 * malloc of that many bytes or more answers NULL, as when memory runs
 * out; every other call is glibc's. The program under test sets and
 * clears the variable around the one operation whose allocation failure
 * the test checks. Built with
 * cc -shared -fPIC -o malloc_fail.so malloc_fail.c */
#define _GNU_SOURCE
#include <stdlib.h>

extern void *__libc_malloc(size_t n);

void *malloc(size_t n) {
  if (n >= 65536) {
    const char *v = getenv("AVANT_TEST_MALLOC_FAIL");
    if (v && v[0] >= '1' && v[0] <= '9' && n >= (size_t)strtoull(v, NULL, 10)) {
      return NULL;
    }
  }
  return __libc_malloc(n);
}
