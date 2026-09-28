#include "avant_rt.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef int32_t (*avant_task_fn)(void *);

typedef struct {
  avant_task_fn fn;
  void *arg;
  int32_t result;
  pthread_t thread;
} AvantTask;

static void *task_main(void *p) {
  AvantTask *t = (AvantTask *)p;
  avant_gc_register_thread();
  t->result = t->fn(t->arg);
  avant_gc_unregister_thread();
  return NULL;
}

void *avant_spawn(void *fn, void *arg) {
  avant_gc_defer_enter();
  AvantTask *t = (AvantTask *)avant_alloc(sizeof(AvantTask), AVANT_TYPE_BYTES);
  t->fn = (avant_task_fn)fn;
  t->arg = arg;
  t->result = 0;
  avant_pin(t);
  avant_gc_defer_leave();
  if (pthread_create(&t->thread, NULL, task_main, t) != 0) {
    return NULL;
  }
  return t;
}

int32_t avant_join(void *handle) {
  AvantTask *t = (AvantTask *)handle;
  if (!t) {
    return 0;
  }
  pthread_join(t->thread, NULL);
  return t->result;
}

int32_t avant_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int32_t avant_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int32_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}
