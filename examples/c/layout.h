#ifndef AVANT_LAYOUT_H
#define AVANT_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Point {
  double x;
  double y;
} Point;

typedef struct Pad {
  _Bool a;
  int b;
  _Bool c;
  double d;
} Pad;

typedef struct Nest {
  Point a;
  _Bool flag;
  double z;
} Nest;

typedef struct StrRec {
  int a;
  char *p;
  uint64_t u;
  double d;
} StrRec;

struct Opq;

Point *point_new(double x, double y);
Pad *pad_new(void);
int pad_get_b(Pad *p);
int pad_off_b(void);
int pad_off_c(void);
int pad_off_d(void);
int pad_size(void);
Nest *nest_new(void);
int nest_off_flag(void);
int nest_off_z(void);
int nest_size(void);
StrRec *str_new(void);
int str_off_p(void);
int str_off_u(void);
int str_off_d(void);
int str_size(void);
int str_get_a(StrRec *r);
uint64_t str_get_u(StrRec *r);
double str_get_d(StrRec *r);
const char *str_get_p(StrRec *r);
struct Opq *opq_new(void);
int opq_id(struct Opq *p);

#endif
