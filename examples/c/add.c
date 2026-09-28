#include "add.h"

int avant_add(int a, int b) {
  return a + b;
}

void *avant_sentinel(void) {
  static int box = 1;
  return &box;
}

int avant_is_null(void *p) {
  return p == 0;
}
