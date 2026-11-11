#include "avant_rt.h"
#include "avant_gc.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * Bounded pool shared by spawn and .parallel (D35, Q38-A).
 * The size is the program's chosen thread count (avant_chosen_threads:
 * --threads, else online processors). Workers
 * never touch g_world. Join or .parallel from a worker waits for a
 * pool thread, so a full pool deadlocks; those calls die.
 */

/* A spawned block returns an Avant Int (Stage 29 W-INT1: avant_int). */
typedef avant_int (*avant_task_fn)(void *);
/* A .parallel thunk takes the element index as an Avant Int. */
typedef void (*avant_par_fn)(avant_int, void *);

typedef struct {
  avant_task_fn fn;
  void *arg;
  avant_int result;
  int done;
  pthread_cond_t done_cv;
  /* R1: roots arg (avant_gc_task_root_add) until fn has returned. */
  AvantGcTaskRoot root;
} AvantTask;

/* A .parallel call's completion count and condition, and its context's
 * task root (Stage 28 W17b, O122): the context is rooted, not pinned, for
 * the call's duration; a collection moves it like any rooted object and
 * rewrites the root, and each job reads the context from it. */
typedef struct ParDone {
  int remaining;
  pthread_cond_t cv;
  AvantGcTaskRoot root;
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

/* The program's one chosen thread count: the --threads flag when the
 * generated startup recorded one, else the online processor count. The
 * pool and the HTTP server's loop count both read this one value.
 * AVANT_THREADS, deprecated for one release by Q125-A, is not read. */
static long g_flag_threads;

void avant_set_flag_threads(long n) {
  g_flag_threads = n;
}

avant_int avant_chosen_threads(void) {
  if (g_flag_threads > 0) {
    return (int32_t)g_flag_threads;
  }
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) {
    n = 1;
  }
  return (int32_t)n;
}

static long pool_count(void) {
  return (long)avant_chosen_threads();
}

/* Called at a collecting entry (spawn, .parallel): pool_mu through
 * avant_gc_lock (native while contended; Stage 28 W17a). */
static void ensure_pool(void) {
  long n;
  long i;
  avant_gc_lock(&pool_mu);
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

/* A worker is native between jobs (Stage 28 W17a, rule 2): it returns to
 * running (a safepoint while a collection is in progress) before it reads
 * the job's heap argument and calls into Avant, and is native again once
 * the function has returned; the root drop and the pool lock that follow
 * touch only C memory and the root list (under the world lock, which the
 * collector holds while it collects). */
static void run_job(Job *job) {
  avant_gc_native_end();
  if (job->kind == 1) {
    AvantTask *t = job->task;
    /* The record is read from its root, which a collection rewrites when
     * the record moves (Stage 28 W17a; t->arg is its address at spawn). */
    t->result = t->fn(t->arg ? t->root.obj : NULL);
    avant_gc_native_begin();
    /* R1: the record is the task's until its function returns; from
     * here it lives only as long as something else names it. */
    if (t->arg) {
      avant_gc_task_root_drop(&t->root);
    }
    pthread_mutex_lock(&pool_mu);
    t->done = 1;
    /* Every waiting join wakes (C38: a handle may be joined twice). */
    pthread_cond_broadcast(&t->done_cv);
    pthread_mutex_unlock(&pool_mu);
  } else {
    avant_par_fn fn = (avant_par_fn)job->fn;
    ParDone *par = job->par;
    fn(job->index, par->root.obj);
    avant_gc_native_begin();
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
  /* A worker allocates young in its own young blocks (Stage 28 W17b);
   * until then it allocated in the non-moving mature space, because
   * the young cursor was one per process and, before W17a's handshake,
   * a young collection on another thread could move a buffer this
   * worker was storing into. */
  /* Native while it waits for a job (rule 2): a collection never waits
   * for an idle worker. */
  avant_gc_native_begin();
  for (;;) {
    Job *job;
    /* Plain lock and wait: the thread is native here. */
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

/* The task is C memory (C28, Stage 28 W14b1): it holds no reference the
 * collector must trace except arg, the capture record, which the task
 * roots (R1, Stage 28 W16b: avant_gc_task_root_add, a node in the task,
 * held in place while rooted) from here until its function has returned
 * (run_job drops the root). The record is no longer pinned: before W16b
 * the generated code pinned it and the join unpinned it, so a spawn that
 * was never joined kept its record (and whatever it captured) and a pin
 * entry for the life of the process. The join frees the task. Before
 * W14b1 the task itself was a heap object pinned here and never unpinned.
 *
 * The JoinHandle (C38, D225) is a small heap object with no references
 * (AVANT_TYPE_BYTES: the task pointer and the result), which the
 * generated code holds as a traced reference (compiler/mir/lower_agg.av,
 * value_agg_ty) and the collector reclaims like any object. The first
 * join waits, frees the task, stores the result and clears the task
 * pointer; every later join returns the stored result at once (future
 * semantics). Before C38 the handle was the task itself, so a second
 * join read freed memory and freed it again. */
typedef struct {
  AvantTask *task;
  avant_int result;
} AvantJoinHandle;

void *avant_spawn(void *fn, void *arg) {
  AvantJoinHandle *hd;
  AvantTask *t;
  Job *job;
  /* gc-lint: holds nothing (arg, the capture record, is a task root from
   * before the first call that may collect, read from the root after it;
   * C28, R1). The handle's allocation is the last call. */
  t = (AvantTask *)calloc(1, sizeof(AvantTask));
  if (!t) {
    die_task("out of memory");
  }
  t->fn = (avant_task_fn)fn;
  t->arg = arg;
  t->result = 0;
  t->done = 0;
  if (pthread_cond_init(&t->done_cv, NULL) != 0) {
    free(t);
    return NULL;
  }
  if (arg) {
    avant_gc_task_root_add(&t->root, arg);
  }
  job = (Job *)calloc(1, sizeof(Job));
  if (!job) {
    die_task("out of memory");
  }
  job->kind = 1;
  job->task = t;
  /* The job is queued before the handle exists (Stage 28 W17a): the pool
   * lock may wait natively (a collection may run there), and the handle
   * is then allocated last, so this function holds no heap reference
   * across any collecting call (the record is the task's root). The
   * task is C memory; the join finds it through the handle. */
  ensure_pool();
  avant_gc_lock(&pool_mu);
  enqueue(job);
  pthread_mutex_unlock(&pool_mu);
  hd = (AvantJoinHandle *)avant_alloc(sizeof(AvantJoinHandle), AVANT_TYPE_BYTES);
  hd->task = t;
  hd->result = 0;
  return hd;
}

avant_int avant_join(void *handle) {
  AvantJoinHandle *hd = (AvantJoinHandle *)handle;
  AvantTask *t;
  avant_int result;
  if (!hd) {
    return 0;
  }
  if (on_worker) {
    die_task("join from a pool worker");
  }
  /* The wait is native (Stage 28 W17a, rule 2): other threads collect
   * meanwhile and may move the handle, which this function holds in a
   * handle scope (RC1) and reads only while running (it was pinned until
   * W17a, because other threads collected without stopping this one,
   * D191). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &hd);
  avant_gc_lock(&pool_mu);
  /* hd->task is read again after each wait: a join that finishes first
   * clears it, and the task is then gone. */
  while (hd->task && !hd->task->done) {
    avant_gc_cond_wait(&hd->task->done_cv, &pool_mu);
  }
  t = hd->task;
  if (t) {
    hd->result = t->result;
    hd->task = NULL;
  }
  result = hd->result;
  pthread_mutex_unlock(&pool_mu);
  if (t) {
    /* The worker dropped the record's root before it last touched the
     * task under pool_mu (run_job), so the task can go now. */
    pthread_cond_destroy(&t->done_cv);
    free(t);
  }
  return result;
}

void avant_parallel_for(avant_int n, void *fn, void *ctx) {
  ParDone done;
  int32_t i;
  if (n <= 0) {
    return;
  }
  if (on_worker) {
    die_task("parallel from a pool worker");
  }
  /* The context is rooted before the first call that may collect (RC1;
   * ensure_pool's lock may wait natively), and read from the root after
   * it (Stage 28 W17b, O122: the generated code no longer pins it). */
  avant_gc_task_root_add(&done.root, ctx);
  if (pthread_cond_init(&done.cv, NULL) != 0) {
    die_task("parallel cond");
  }
  /* n is an Array's size, within the ceiling. */
  done.remaining = (int32_t)n;
  ensure_pool();
  avant_gc_lock(&pool_mu);
  for (i = 0; i < n; i++) {
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) {
      die_task("out of memory");
    }
    job->kind = 2;
    job->index = i;
    job->fn = fn;
    job->arg = NULL;
    job->par = &done;
    enqueue(job);
  }
  while (done.remaining > 0) {
    avant_gc_cond_wait(&done.cv, &pool_mu);
  }
  pthread_mutex_unlock(&pool_mu);
  pthread_cond_destroy(&done.cv);
  avant_gc_task_root_drop(&done.root);
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

/* A gate's lock and wait are native while they wait (Stage 28 W17a, rule
 * 2): the holder of a Mutex may stop at a safepoint inside its block, so
 * a thread waiting for it must not be waited for. */
void avant_gate_lock(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  avant_gc_lock(&g->mu);
}

void avant_gate_unlock(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_mutex_unlock(&g->mu);
}

void avant_gate_wait(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  avant_gc_cond_wait(&g->cv, &g->mu);
}

void avant_gate_signal(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_cond_signal(&g->cv);
}

void avant_gate_broadcast(uint64_t gate) {
  Gate *g = (Gate *)(uintptr_t)gate;
  pthread_cond_broadcast(&g->cv);
}

/* The 64-bit monotonic clock (Stage 29 W4b1, D295 R6): now_ms and now_us
 * are Int64 readings that do not wrap. */
int64_t avant_clock_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t avant_clock_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* The Int readings that wrap, kept for the compiler the seed v0.40.0
 * builds (its now_ms and now_us call these); removed when the seed is a
 * release with the 64-bit clock. */
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
