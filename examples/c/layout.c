#include "layout.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

Point *point_new(double x, double y) {
  Point *p = malloc(sizeof(Point));
  if (!p) {
    return NULL;
  }
  p->x = x;
  p->y = y;
  return p;
}

Pad *pad_new(void) {
  Pad *p = calloc(1, sizeof(Pad));
  if (!p) {
    return NULL;
  }
  p->a = 1;
  p->b = 5;
  p->c = 0;
  p->d = 6.5;
  return p;
}

int pad_get_b(Pad *p) {
  return p->b;
}

int pad_off_b(void) { return (int)offsetof(Pad, b); }
int pad_off_c(void) { return (int)offsetof(Pad, c); }
int pad_off_d(void) { return (int)offsetof(Pad, d); }
int pad_size(void) { return (int)sizeof(Pad); }

Nest *nest_new(void) {
  Nest *n = calloc(1, sizeof(Nest));
  if (!n) {
    return NULL;
  }
  n->a.x = 1.0;
  n->a.y = 2.0;
  n->flag = 1;
  n->z = 4.0;
  return n;
}

int nest_off_flag(void) { return (int)offsetof(Nest, flag); }
int nest_off_z(void) { return (int)offsetof(Nest, z); }
int nest_size(void) { return (int)sizeof(Nest); }

StrRec *str_new(void) {
  return calloc(1, sizeof(StrRec));
}

int str_off_p(void) { return (int)offsetof(StrRec, p); }
int str_off_u(void) { return (int)offsetof(StrRec, u); }
int str_off_d(void) { return (int)offsetof(StrRec, d); }
int str_size(void) { return (int)sizeof(StrRec); }

int str_get_a(StrRec *r) { return r->a; }
uint64_t str_get_u(StrRec *r) { return r->u; }
double str_get_d(StrRec *r) { return r->d; }

const char *str_get_p(StrRec *r) {
  if (!r->p) {
    return "";
  }
  return r->p;
}

struct Opq {
  int n;
};

struct Opq *opq_new(void) {
  struct Opq *p = malloc(sizeof(struct Opq));
  if (!p) {
    return NULL;
  }
  p->n = 7;
  return p;
}

int opq_id(struct Opq *p) {
  if (!p) {
    return -1;
  }
  return p->n;
}
