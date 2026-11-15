/* The C side of cenum.h. A value given back as
 * raw (method_of, color_of, req_fill, call_color) may be no member: the
 * Avant side traps when it reads one. */
#include <stdlib.h>
#include "cenum.h"

color_t next_color(color_t c) {
  return c == RED ? GREEN : c == GREEN ? BLUE : RED;
}
int method_code(enum http_method m) {
  return 100 + (int)m;
}
enum http_method method_of(int raw) {
  return (enum http_method)raw;
}
color_t color_of(int raw) {
  return (color_t)raw;
}
unsigned int big_value(enum big b) {
  return (unsigned int)b;
}
int clash_value(enum clash c) {
  return (int)c;
}
struct req *req_new(void) {
  struct req *r = calloc(1, sizeof(struct req));
  return r;
}
void req_fill(struct req *r, int raw_method) {
  r->method = (enum http_method)raw_method;
  r->color = BLUE;
  r->n = 3;
}
int req_method_code(struct req *r) {
  return 100 + (int)r->method;
}
/* A callback (written by hand in the test: avant bind binds no function
 * pointer). */
typedef void (*color_cb)(color_t c);
void call_color(color_cb f, int raw);
void call_color(color_cb f, int raw) {
  f((color_t)raw);
}
