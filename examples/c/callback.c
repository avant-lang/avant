/* Buffer helper for the Wave G callback proof (tests/cases/ffi.av).
 * The sort itself is libc qsort; Avant supplies the comparator. */
#include <stdlib.h>

typedef struct CInt {
  int v;
} CInt;

CInt *cb_buf_new(int n) { return (CInt *)malloc(sizeof(CInt) * (size_t)n); }
void cb_buf_set(CInt *b, int i, int v) { b[i].v = v; }
int cb_buf_get(CInt *b, int i) { return b[i].v; }
void cb_buf_free(CInt *b) { free(b); }
