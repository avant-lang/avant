#include "avant_rt.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * Bounded pool shared by spawn and .parallel (D35, Q38-A).
 * The size is the online processor count, read once, unless
 * AVANT_THREADS sets a positive integer. Workers never touch
 * g_world. Join or .parallel from a worker waits for a pool
 * thread, so a full pool deadlocks; those calls die.
 */

typedef int32_t (*avant_task_fn)(void *);
typedef void (*avant_par_fn)(int32_t, void *);

typedef struct {
  avant_task_fn fn;
  void *arg;
  int32_t result;
  int done;
  pthread_cond_t done_cv;
} AvantTask;

typedef struct ParDone {
  int remaining;
  pthread_cond_t cv;
} ParDone;

typedef struct Job {
  int kind;
  int32_t index;
  void *fn;
  void *arg;
  AvantTask *task;
  ParDone *par;
  struct Job *next;
} Job;

typedef struct Gate {
  pthread_mutex_t mu;
  pthread_cond_t cv;
} Gate;

static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cv = PTHREAD_COND_INITIALIZER;
static Job *queue_head;
static Job *queue_tail;
static int pool_started;
static __thread int on_worker;

static void die_task(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
}

static void *worker_main(void *p);

static void enqueue(Job *job) {
  job->next = NULL;
  if (queue_tail) {
    queue_tail->next = job;
  } else {
    queue_head = job;
  }
  queue_tail = job;
  pthread_cond_signal(&queue_cv);
}

static long pool_count(void) {
  const char *s = getenv("AVANT_THREADS");
  if (s && s[0]) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > (unsigned long long)LONG_MAX) {
      die_task("AVANT_THREADS must be a positive integer");
    }
    return (long)v;
  }
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) {
    n = 1;
  }
  return n;
}

static void ensure_pool(void) {
  long n;
  long i;
  pthread_mutex_lock(&pool_mu);
  if (pool_started) {
    pthread_mutex_unlock(&pool_mu);
    return;
  }
  pool_started = 1;
  pthread_mutex_unlock(&pool_mu);
  n = pool_count();
  for (i = 0; i < n; i++) {
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, NULL) != 0) {
      die_task("pool thread");
    }
    pthread_detach(th);
  }
}

static void run_job(Job *job) {
  if (job->kind == 1) {
    AvantTask *t = job->task;
    t->result = t->fn(t->arg);
    pthread_mutex_lock(&pool_mu);
    t->done = 1;
    pthread_cond_signal(&t->done_cv);
    pthread_mutex_unlock(&pool_mu);
  } else {
    avant_par_fn fn = (avant_par_fn)job->fn;
    ParDone *par = job->par;
    fn(job->index, job->arg);
    pthread_mutex_lock(&pool_mu);
    par->remaining -= 1;
    if (par->remaining == 0) {
      pthread_cond_signal(&par->cv);
    }
    pthread_mutex_unlock(&pool_mu);
  }
  free(job);
}

static void *worker_main(void *p) {
  (void)p;
  on_worker = 1;
  avant_gc_register_thread();
  /* Worker allocations stay in the non-moving mature space. A young
   * collection on another thread must not relocate a buffer this
   * worker is storing into. */
  avant_alloc_in_mature(1);
  for (;;) {
    Job *job;
    pthread_mutex_lock(&pool_mu);
    while (!queue_head) {
      pthread_cond_wait(&queue_cv, &pool_mu);
    }
    job = queue_head;
    queue_head = job->next;
    if (!queue_head) {
      queue_tail = NULL;
    }
    pthread_mutex_unlock(&pool_mu);
    run_job(job);
  }
  return NULL;
}

void *avant_spawn(void *fn, void *arg) {
  AvantTask *t;
  Job *job;
  avant_gc_defer_enter();
  t = (AvantTask *)avant_alloc(sizeof(AvantTask), AVANT_TYPE_BYTES);
  t->fn = (avant_task_fn)fn;
  t->arg = arg;
  t->result = 0;
  t->done = 0;
  if (pthread_cond_init(&t->done_cv, NULL) != 0) {
    avant_gc_defer_leave();
    return NULL;
  }
  avant_pin(t);
  avant_gc_defer_leave();
  job = (Job *)calloc(1, sizeof(Job));
  if (!job) {
    die_task("out of memory");
  }
  job->kind = 1;
  job->task = t;
  ensure_pool();
  pthread_mutex_lock(&pool_mu);
  enqueue(job);
  pthread_mutex_unlock(&pool_mu);
  return t;
}

int32_t avant_join(void *handle) {
  AvantTask *t = (AvantTask *)handle;
  int32_t result;
  if (!t) {
    return 0;
  }
  if (on_worker) {
    die_task("join from a pool worker");
  }
  pthread_mutex_lock(&pool_mu);
  while (!t->done) {
    pthread_cond_wait(&t->done_cv, &pool_mu);
  }
  result = t->result;
  pthread_mutex_unlock(&pool_mu);
  /* The spawn capture is pinned so the worker can read it. After join
   * it is the only pin keeping a shared matrix alive. */
  avant_unpin(t->arg);
  t->arg = NULL;
  return result;
}

void avant_parallel_for(int32_t n, void *fn, void *ctx) {
  ParDone done;
  int32_t i;
  if (n <= 0) {
    return;
  }
  if (on_worker) {
    die_task("parallel from a pool worker");
  }
  if (pthread_cond_init(&done.cv, NULL) != 0) {
    die_task("parallel cond");
  }
  done.remaining = n;
  ensure_pool();
  pthread_mutex_lock(&pool_mu);
  for (i = 0; i < n; i++) {
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) {
      die_task("out of memory");
    }
    job->kind = 2;
    job->index = i;
    job->fn = fn;
    job->arg = ctx;
    job->par = &done;
    enqueue(job);
  }
  while (done.remaining > 0) {
    pthread_cond_wait(&done.cv, &pool_mu);
  }
  pthread_mutex_unlock(&pool_mu);
  pthread_cond_destroy(&done.cv);
}

uint64_t avant_gate_new(void) {
  Gate *g = (Gate *)calloc(1, sizeof(Gate));
  if (!g) {
    die_task("out of memory");
  }
  if (pthread_mutex_init(&g->mu, NULL) != 0) {
    die_task("gate mutex");
  }
  if (pthread_cond_init(&g->cv, NULL) != 0) {
    die_task("gate cond");
  }
  return (uint64_t)(uintptr_t)g;
}

void avant_gate_lock(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_mutex_lock(&g->mu);
}

void avant_gate_unlock(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_mutex_unlock(&g->mu);
}

void avant_gate_wait(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_cond_wait(&g->cv, &g->mu);
}

void avant_gate_signal(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_cond_signal(&g->cv);
}

void avant_gate_broadcast(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_cond_broadcast(&g->cv);
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
