/* sched_getaffinity and CPU_COUNT. */
#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Bounded pool shared by spawn and .parallel.
 * The bound is the program's chosen thread count (avant_chosen_threads:
 * --avant-threads=N or std.sync's set_pool_size, else the CPUs the process
 * may use): at most that many tasks run at once. Workers never touch g_world.
 *
 * Structured completion: a task's handle completes only when its block
 * and every task it spawned have ended. A task counts its unfinished
 * nested tasks (`pending`: one for its own block, one per nested task
 * not ended); its worker goes back to the pool when the block ends, no
 * worker blocks for it, and the last of them to end completes the task
 * and, in turn, counts down the task (or the .parallel) that spawned it,
 * all under pool_mu. A join of the outer task so waits for every task
 * spawned under it, and every array they captured is thawed by then. A
 * task spawned by a .parallel body counts in that .parallel's
 * `remaining`, so the .parallel returns after it too.
 *
 * Join or .parallel from a worker (a nested join) helps instead of
 * waiting for a pool thread: a joined task still queued is taken off the
 * queue and run on the joining worker's stack, and a .parallel's bodies
 * still queued are run there the same way; whatever another worker took,
 * the caller waits for, giving its run back as a gate wait does
 * (worker_wait), so a queued task may start meanwhile. A chain of joins
 * thus ends at a task that runs, and nested parallelism (a recursive
 * split that joins its halves) needs no more threads than the bound.
 *
 * A task waiting on a gate gives its run back: a worker that waits in a
 * Channel's send or recv or for a Mutex
 * (pool_block, from the gate functions below) stops counting as running,
 * so a queued task may start, on an idle worker or, when every worker
 * is waiting, on a new one, up to POOL_THREAD_LIMIT threads (or the
 * bound, if larger); past the limit the program is told once on
 * standard error and the queued task waits. When the wait ends, the task
 * waits for a run (pool_resume) before it goes on, so the bound holds.
 * Before, as many waiting tasks as the pool had threads kept a queued
 * task from ever starting, and the program hung with no message.
 */

/* A spawned block returns an Avant Int (avant_int). */
typedef avant_int (*avant_task_fn)(void *);
/* A .parallel thunk takes the element index as an Avant Int. */
typedef void (*avant_par_fn)(avant_int, void *);

typedef struct AvantTask {
  avant_task_fn fn;
  void *arg;
  avant_int result;
  int done;
  pthread_cond_t done_cv;
  /* Roots arg (avant_gc_task_root_add) until fn has returned. */
  AvantGcTaskRoot root;
  /* the block's end on a trap (avant_spawn_f), and the trap that
   * ended the task (malloc'd, kept for the run: join hands them out as
   * Strings, and a handle may be joined twice), or null. */
  void (*fail)(void *);
  char *what;
  char *site;
  /* Structured completion (under pool_mu): one for the block, plus one
   * per nested task not yet ended; what spawned it (a task, or a
   * .parallel through one of its bodies), counted down when this task
   * is done; and its queued job, until a worker takes it (a nested
   * join runs it in place). */
  int pending;
  struct AvantTask *parent;
  struct ParDone *parent_par;
  struct Job *job;
  /* A task whose result is not an Int: the handle the generated code
   * made (with a slot for the result), rooted from the spawn until the
   * task is done; the block stores its value there (avant_task_handle). */
  int typed;
  AvantGcTaskRoot hroot;
} AvantTask;

/* A .parallel call's completion count and condition, and its context's
 * task root: the context is rooted, not pinned, for
 * the call's duration; a collection moves it like any rooted object and
 * rewrites the root, and each job reads the context from it. */
typedef struct ParDone {
  int remaining;
  pthread_cond_t cv;
  AvantGcTaskRoot root;
  /* The bodies that trapped (each a recovery point: the trap is told
   * when it happens and the body ends), and the first one's trap. */
  int trapped;
  char *what;
  char *site;
} ParDone;

typedef struct Job {
  int kind;
  int32_t index;
  void *fn;
  void *arg;
  AvantTask *task;
  ParDone *par;
  struct Job *next;
  struct Job *prev;
  /* On the queue (under pool_mu); a .parallel's jobs are one block its
   * call owns and frees (owned), a spawn's job is freed once run. */
  int queued;
  int owned;
} Job;

/* The pool's threads at most: the bound (avant_chosen_threads), or this
 * many when the bound is smaller, so that tasks waiting on gates may
 * hold threads while queued tasks start. */
#define POOL_THREAD_LIMIT 1024

static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cv = PTHREAD_COND_INITIALIZER;
/* Tasks back from a gate wait, waiting for a run. */
static pthread_cond_t resume_cv = PTHREAD_COND_INITIALIZER;
static Job *queue_head;
static Job *queue_tail;
static int pool_started;
/* Under pool_mu: the bound, the thread limit, the workers started, the
 * ones running a task (not waiting on a gate), the ones waiting for a
 * job, the ones started that have not looked for a job yet, the tasks
 * waiting in pool_resume, and whether the limit's message was given. */
static long pool_bound;
static long pool_limit;
static long pool_threads;
static long pool_running;
static long pool_idle;
static long pool_starting;
static long pool_resuming;
static int pool_warned;
static __thread int on_worker;
/* The task, or the .parallel whose body, this thread runs now (a nested
 * spawn counts in it). */
static __thread AvantTask *cur_task;
static __thread ParDone *cur_par;
/* This worker's task waits on a gate in the current gate call. */
static __thread int pool_blocked;

static void die_task(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
}

/* ---- A trap ends its task.
 *
 * A pool worker running a spawned block keeps a recovery point (run_job):
 * a jump buffer, and what the thread had when the block started (its
 * innermost frame record, the gates it held, its C callback depth). A
 * trap of the language (avant_rt_traps.c's trap_tail) on that thread,
 * while no C callback is running inside the block (a trap there stays
 * the process's: C frames would be skipped), hands its text and site to
 * avant_task_trapped, which jumps back to the entry: no unwinding (the
 * collector runs no destructors, and the block's frames are garbage once
 * the frame chain is set back to the entry's). There the task ends:
 * every gate the block still held is poisoned and given up (a Mutex held
 * at the trap: its next `with` is told, avant_gate_lock_with), the
 * block's end on a trap runs (the generated `#fail` thunk: the captured
 * arrays thawed, the captured channels closed), and the join gives
 * the trap as a TaskError. The normal path pays the jump buffer's save
 * at the task's start (_setjmp: no signal mask) and two thread-local
 * stores; a gate's lock and unlock note the gate held (held_push).
 *
 * Not recovered (each a process trap, as before): a trap in a C
 * callback; the runtime's own failures (an allocation past the heap
 * limit, the collector's checks, die_task) and the library traps that do
 * not go through trap_tail; a trap on any other thread (main, an HTTP
 * loop) or in a .parallel body (a .parallel is one expression of its
 * caller, which waits for every body: no task to end).
 *
 * What the collector holds for the block (a `gc.hold`, a `pin` block's
 * pins) ends at the jump: a program that spawns opens them through the
 * pool's own entries (avant_task_pin, avant_task_hold_begin), which note
 * them. A C-layout field's pins stay, as a missing `release` leaves
 * them (memory, never correctness). */
typedef struct AvantRecover {
  jmp_buf jb;
  AvantGcFrame *top;
  int held;
  int cb;
  int pins;
  int holds;
  int arr_holds;
  char *what;
  char *site;
  /* The light jump buffer of a handler's recovery point
   * (avant_task_recover_call): __builtin_setjmp keeps the frame, the
   * stack pointer and the return address only (the function that sets
   * it saves the rest in its frame), about 20 instructions fewer a
   * request than _setjmp's full buffer with its pointer mangling. */
  void *light[5];
  int is_light;
} AvantRecover;

/* The freezes of method calls on elements this thread holds
 * (avant_array.c): their count at a recovery point, and the thaw of
 * those taken below it. */
int avant_array_hold_depth(void);
void avant_array_hold_drop(int n);

static __thread AvantRecover *g_recover;
static __thread int g_cb_depth;

/* The gates this thread holds, innermost last (their numbers: a word's
 * address is not stable across a collection, its number is). Past
 * HELD_MAX nested locks the count goes on and the numbers are not kept:
 * a trap then stays the process's. */
#define HELD_MAX 32
static __thread uint64_t g_held[HELD_MAX];
static __thread int g_held_n;

/* A lock and an unlock pair up, so the count needs no test; a slot past
 * HELD_MAX wraps (the count says the numbers are not all kept). Noting
 * only inside a recovery point (a pool worker's block) was measured: it
 * saves 3 instructions a lock on the main thread and costs 4 in a task
 * (the recovery point's load and test each way), so every thread notes. */
static inline void held_push(uint64_t id) {
  int n = g_held_n;
  g_held[n & (HELD_MAX - 1)] = id;
  g_held_n = n + 1;
}

static inline void held_pop(void) {
  g_held_n--;
}

/* The pins of `pin` blocks and the holds of `gc.hold` this thread has
 * open, through avant_task_pin and avant_task_hold_begin (a program that
 * spawns calls those): a trap that ends the task gives them back. A
 * pinned object does not move, so its address is kept as it is. Past
 * PIN_MAX open pins a trap stays the process's. */
#define PIN_MAX 32
static __thread void *g_pins[PIN_MAX];
static __thread int g_pin_n;
static __thread int g_hold_n;

void avant_task_pin(void *payload) {
  int n = g_pin_n;
  avant_pin(payload);
  if (n < PIN_MAX) {
    g_pins[n] = payload;
  }
  g_pin_n = n + 1;
}

void avant_task_unpin(void *payload) {
  avant_unpin(payload);
  if (g_pin_n > 0) {
    g_pin_n--;
  }
}

void avant_task_hold_begin(void) {
  g_hold_n++;
  avant_gc_hold_begin();
}

void avant_task_hold_end(void) {
  if (g_hold_n > 0) {
    g_hold_n--;
  }
  avant_gc_hold_end();
}

int avant_task_can_recover(void) {
  AvantRecover *r = g_recover;
  return r && g_cb_depth == r->cb && g_held_n <= HELD_MAX && g_pin_n <= PIN_MAX;
}

static char *dup_text(const char *s) {
  size_t n = s ? strlen(s) : 0;
  char *d = (char *)malloc(n + 1);
  if (!d) {
    die_task("out of memory");
  }
  if (n) {
    memcpy(d, s, n);
  }
  d[n] = 0;
  return d;
}

void avant_task_trapped(const char *what, const char *site) {
  AvantRecover *r = g_recover;
  /* The trap is told when it happens, joined or not, in the form a trap
   * that ends the program has (the program's output first). */
  fflush(stdout);
  if (site && *site) {
    fprintf(stderr, "avant runtime: %s at %s\n", what, site);
  } else {
    fprintf(stderr, "avant runtime: %s\n", what);
  }
  r->what = dup_text(what);
  r->site = dup_text(site ? site : "");
  /* The arrays the block's method calls froze are thawed while their
   * holders' frames are still intact. */
  avant_array_hold_drop(r->arr_holds);
  /* The block's frames are left: the chain is the entry's again. */
  avant_gc_top = r->top;
  if (r->is_light) {
    __builtin_longjmp(r->light, 1);
  }
  longjmp(r->jb, 1);
}

void avant_task_cb_enter(void) {
  g_cb_depth++;
}

void avant_task_cb_leave(void) {
  g_cb_depth--;
}

static void gate_dead(uint64_t id);

static void *worker_main(void *p);

/* A run is free and a task is queued: an idle worker takes it, else a
 * new worker starts, up to the limit (pool_mu held). Tasks back from a
 * gate wait go first. */
static void pool_kick(void) {
  if (pool_resuming > 0) {
    if (pool_running < pool_bound) {
      pthread_cond_signal(&resume_cv);
    }
    return;
  }
  if (!queue_head || pool_running >= pool_bound) {
    return;
  }
  if (pool_idle > 0) {
    pthread_cond_signal(&queue_cv);
    return;
  }
  if (pool_starting > 0) {
    /* A worker on its way takes the job when it first looks. */
    return;
  }
  if (pool_threads < pool_limit) {
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, NULL) != 0) {
      die_task("pool thread");
    }
    pthread_detach(th);
    pool_threads++;
    pool_starting++;
    return;
  }
  if (!pool_warned) {
    pool_warned = 1;
    fprintf(stderr,
            "avant runtime: tasks are queued while the pool's %ld threads are all taken, some by tasks waiting on a "
            "Channel or a Mutex, and the pool starts no more (its limit is %ld threads); the queued tasks wait for a "
            "thread to be free (--avant-threads=N bounds the tasks that run at once, and a task that waits lets a queued "
            "one start while threads remain)\n",
            pool_threads, pool_limit);
  }
}

static void enqueue_one(Job *job) {
  job->next = NULL;
  job->prev = queue_tail;
  job->queued = 1;
  if (queue_tail) {
    queue_tail->next = job;
  } else {
    queue_head = job;
  }
  queue_tail = job;
}

static void enqueue(Job *job) {
  enqueue_one(job);
  pool_kick();
}

/* Takes a queued job off the queue (pool_mu held): the head, for a
 * worker, or one a nested join runs in place. */
static void unlink_job(Job *job) {
  if (job->prev) {
    job->prev->next = job->next;
  } else {
    queue_head = job->next;
  }
  if (job->next) {
    job->next->prev = job->prev;
  } else {
    queue_tail = job->prev;
  }
  job->next = job->prev = NULL;
  job->queued = 0;
  if (job->kind == 1) {
    job->task->job = NULL;
  }
}

/* The worker's task is about to wait on a gate (a bucket's mutex may be
 * held: the lock order is bucket, then pool_mu). Once per gate call. */
static void pool_block(void) {
  if (!on_worker || pool_blocked) {
    return;
  }
  pool_blocked = 1;
  pthread_mutex_lock(&pool_mu);
  pool_running--;
  pool_kick();
  pthread_mutex_unlock(&pool_mu);
}

/* The gate call that waited is done (no bucket's mutex held): the task
 * waits for a run, natively, before it goes on. */
static void pool_resume(void) {
  if (!pool_blocked) {
    return;
  }
  pool_blocked = 0;
  avant_gc_lock(&pool_mu);
  pool_resuming++;
  while (pool_running >= pool_bound) {
    avant_gc_cond_wait(&resume_cv, &pool_mu);
  }
  pool_resuming--;
  pool_running++;
  pool_kick();
  pthread_mutex_unlock(&pool_mu);
}

/* The program's one chosen thread count: --avant-threads=N, which the
 * generated startup records, or std.sync's set_pool_size, else the CPUs
 * the process may use. The pool and the HTTP server's loop count both
 * read this one value. One atomic word holds it (0: not chosen) and the
 * POOL_SIZE_FIXED bit, set when the pool starts or a server takes its
 * loop count from it (avant_pool_size_fix): a set after that traps, so a
 * program never believes a size that does not hold. The word costs a
 * spawn nothing: only the pool's start reads it. */
#define POOL_SIZE_FIXED (1L << 40)
static long g_pool_size;

void avant_set_flag_threads(long n) {
  __atomic_store_n(&g_pool_size, n, __ATOMIC_RELEASE);
}

/* std.sync's set_pool_size(n). */
void avant_set_pool_size(avant_int n) {
  char msg[320];
  long s;
  if (n < 1 || n > AVANT_POOL_SIZE_MAX) {
    snprintf(msg, sizeof msg, "set_pool_size(%lld): the thread pool's size is from 1 to %d", (long long)n,
             AVANT_POOL_SIZE_MAX);
    avant_trap_text(msg);
  }
  s = __atomic_load_n(&g_pool_size, __ATOMIC_ACQUIRE);
  for (;;) {
    if (s & POOL_SIZE_FIXED) {
      snprintf(msg, sizeof msg,
               "set_pool_size(%lld) after the thread pool's size was fixed: it is fixed when the pool starts (the "
               "first spawn or .parallel) or when Http.listen without loops: takes its loop count from it",
               (long long)n);
      avant_trap_text(msg);
    }
    if (__atomic_compare_exchange_n(&g_pool_size, &s, (long)n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return;
    }
  }
}

/* The CPUs this process may run on: its affinity
 * mask (sched_getaffinity, CPU_COUNT), which `taskset`, a container's
 * cpuset and numactl set; the online processor count only when the mask
 * cannot be read. A mask past CPU_SETSIZE is read with a set sized to
 * it. A cgroup's CPU quota (cpu.max) is not read. The
 * pool's bound and `avant test`'s default --jobs both read it, once. */
avant_int avant_usable_cpus(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    int n = CPU_COUNT(&set);
    if (n > 0) {
      return n;
    }
  } else {
    int max = CPU_SETSIZE * 2;
    while (max <= 1 << 20) {
      cpu_set_t *big = CPU_ALLOC((size_t)max);
      size_t size = CPU_ALLOC_SIZE((size_t)max);
      int n = 0;
      int ok = 0;
      if (!big) {
        break;
      }
      CPU_ZERO_S(size, big);
      if (sched_getaffinity(0, size, big) == 0) {
        n = CPU_COUNT_S(size, big);
        ok = 1;
      }
      CPU_FREE(big);
      if (ok && n > 0) {
        return n;
      }
      if (ok) {
        break;
      }
      max *= 2;
    }
  }
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) {
    n = 1;
  }
  return (avant_int)n;
}

avant_int avant_chosen_threads(void) {
  long n = __atomic_load_n(&g_pool_size, __ATOMIC_ACQUIRE) & ~POOL_SIZE_FIXED;
  if (n > 0) {
    return (avant_int)n;
  }
  return avant_usable_cpus();
}

/* The chosen count, fixed from now on (the pool's start, a server's
 * loop count). */
avant_int avant_pool_size_fix(void) {
  __atomic_fetch_or(&g_pool_size, POOL_SIZE_FIXED, __ATOMIC_ACQ_REL);
  return avant_chosen_threads();
}

/* The pool's thread limit for a bound of n: POOL_THREAD_LIMIT, or n when
 * larger. AVANT_POOL_THREAD_LIMIT (2 to POOL_THREAD_LIMIT) can only
 * lower it, never below n: tests/cases/test_threads.av sets it to reach
 * the limit's message with a few waiting tasks; not a user knob. */
static long pool_thread_limit(long n) {
  long lim = POOL_THREAD_LIMIT;
  const char *v = getenv("AVANT_POOL_THREAD_LIMIT");
  if (v && *v) {
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end && *end == 0 && x >= 2 && x < lim) {
      lim = x;
    }
  }
  return n > lim ? n : lim;
}

/* Called at a collecting entry (spawn, .parallel): pool_mu through
 * avant_gc_lock (native while contended). */
static void ensure_pool(void) {
  long n;
  long i;
  avant_gc_lock(&pool_mu);
  if (pool_started) {
    pthread_mutex_unlock(&pool_mu);
    return;
  }
  pool_started = 1;
  n = (long)avant_pool_size_fix();
  pool_bound = n;
  pool_limit = pool_thread_limit(n);
  pool_threads = n;
  pool_starting = n;
  pthread_mutex_unlock(&pool_mu);
  for (i = 0; i < n; i++) {
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, NULL) != 0) {
      die_task("pool thread");
    }
    pthread_detach(th);
  }
}

/* After a trap returned to a recovery point: every gate the code below
 * it still held is poisoned and given up, and its pins and holds end
 * (the collector's state is the entry's again); a hold's end may
 * collect: the frames are the entry's, and nothing of the code below is
 * held. */
static void recover_end(AvantRecover *rec) {
  int i;
  for (i = g_held_n; i > rec->held; i--) {
    gate_dead(g_held[i - 1]);
  }
  g_held_n = rec->held;
  while (g_pin_n > rec->pins) {
    g_pin_n--;
    avant_unpin(g_pins[g_pin_n]);
  }
  while (g_hold_n > rec->holds) {
    avant_task_hold_end();
  }
}

/* A recovery point around one call C makes of an Avant function through
 * its callback thunk, on any thread: std.http's server runs each
 * handler's dispatch in one, so a trap in a handler fails that request
 * and the server goes on. fn's thunk brackets its own call as a C
 * callback (in a program that imports std.http, as in one that spawns:
 * lower_ffi.av), so the depth the trap must find is one more than here;
 * a trap inside a C callback the handler made finds a deeper one and
 * stays the process's, as do the runtime's own stops. A recovery point
 * already set (a server serving inside a spawned task) is kept and set
 * back. Returns 0 when fn returned, 1 when it trapped: the trap is told
 * on stderr, as a task's is, and what it held is given back as at a
 * task's end (recover_end). The normal path pays the light jump buffer's
 * save and a few thread-local stores. */
int avant_task_recover_call(int32_t (*fn)(void *, void *), void *a, void *b) {
  AvantRecover rec;
  AvantRecover *outer = g_recover;
  rec.top = avant_gc_top;
  rec.held = g_held_n;
  rec.cb = g_cb_depth + 1;
  rec.pins = g_pin_n;
  rec.holds = g_hold_n;
  rec.arr_holds = avant_array_hold_depth();
  rec.what = NULL;
  rec.site = NULL;
  rec.is_light = 1;
  g_recover = &rec;
  if (__builtin_setjmp(rec.light) == 0) {
    (void)fn(a, b);
    g_recover = outer;
    return 0;
  }
  g_recover = outer;
  recover_end(&rec);
  g_cb_depth = rec.cb - 1;
  free(rec.what);
  free(rec.site);
  return 1;
}

/* The task t is done (pool_mu held): its block ended and so did every
 * task it spawned. Every join waiting on it wakes (a handle may be
 * joined twice), and what spawned it counts down in turn; the parent's
 * record outlives this call, since the parent is not done before it. */
static void task_end(AvantTask *t) {
  while (t) {
    AvantTask *up;
    ParDone *par;
    if (--t->pending > 0) {
      return;
    }
    up = t->parent;
    par = t->parent_par;
    t->done = 1;
    if (t->typed) {
      /* The handle lives on as the program holds it: the result is in
       * it. */
      avant_gc_task_root_drop(&t->hroot);
    }
    pthread_cond_broadcast(&t->done_cv);
    if (par) {
      if (--par->remaining == 0) {
        pthread_cond_signal(&par->cv);
      }
      return;
    }
    t = up;
  }
}

/* A .parallel body is done (pool_mu held). */
static void par_end(ParDone *par) {
  if (--par->remaining == 0) {
    pthread_cond_signal(&par->cv);
  }
}

/* A spawned block, on a worker or in place under a nested join: running
 * on entry, native on return. The record is read from its root, which a
 * collection rewrites when the record moves (t->arg is its address at
 * spawn). */
static void run_task(AvantTask *t) {
  AvantRecover rec;
  AvantRecover *outer = g_recover;
  AvantTask *outer_task = cur_task;
  ParDone *outer_par = cur_par;
  rec.top = avant_gc_top;
  rec.held = g_held_n;
  rec.cb = g_cb_depth;
  rec.pins = g_pin_n;
  rec.holds = g_hold_n;
  rec.arr_holds = avant_array_hold_depth();
  rec.what = NULL;
  rec.site = NULL;
  rec.is_light = 0;
  g_recover = &rec;
  cur_task = t;
  cur_par = NULL;
  if (_setjmp(rec.jb) == 0) {
    t->result = t->fn(t->arg ? t->root.obj : NULL);
    g_recover = outer;
  } else {
    /* The block trapped (avant_task_trapped): the task ends here. */
    g_recover = outer;
    recover_end(&rec);
    g_cb_depth = rec.cb;
    t->result = 0;
    t->what = rec.what;
    t->site = rec.site;
    if (t->fail && t->arg) {
      t->fail(t->root.obj);
    }
  }
  cur_task = outer_task;
  cur_par = outer_par;
  avant_gc_native_begin();
  /* The record is the task's until its function returns; from
   * here it lives only as long as something else names it. */
  if (t->arg) {
    avant_gc_task_root_drop(&t->root);
  }
  pthread_mutex_lock(&pool_mu);
  task_end(t);
  pthread_mutex_unlock(&pool_mu);
}

/* One .parallel body, on a worker or in place: running on entry, native
 * on return. A trap in the body ends that body only (a light recovery
 * point): it is told when it happens, its result stays zero, and the
 * .parallel traps in its caller once every body has ended
 * (avant_parallel_trap). */
static void run_par(Job *job) {
  avant_par_fn fn = (avant_par_fn)job->fn;
  ParDone *par = job->par;
  AvantRecover rec;
  AvantRecover *outer = g_recover;
  AvantTask *outer_task = cur_task;
  ParDone *outer_par = cur_par;
  int trapped = 0;
  rec.top = avant_gc_top;
  rec.held = g_held_n;
  rec.cb = g_cb_depth;
  rec.pins = g_pin_n;
  rec.holds = g_hold_n;
  rec.arr_holds = avant_array_hold_depth();
  rec.what = NULL;
  rec.site = NULL;
  rec.is_light = 1;
  g_recover = &rec;
  cur_task = NULL;
  cur_par = par;
  if (__builtin_setjmp(rec.light) == 0) {
    fn(job->index, par->root.obj);
    g_recover = outer;
  } else {
    g_recover = outer;
    recover_end(&rec);
    g_cb_depth = rec.cb;
    trapped = 1;
  }
  cur_task = outer_task;
  cur_par = outer_par;
  avant_gc_native_begin();
  pthread_mutex_lock(&pool_mu);
  if (trapped) {
    if (par->trapped++ == 0) {
      par->what = rec.what;
      par->site = rec.site;
    } else {
      free(rec.what);
      free(rec.site);
    }
  }
  par_end(par);
  pthread_mutex_unlock(&pool_mu);
}

/* A worker is native between jobs (rule 2): it returns to
 * running (a safepoint while a collection is in progress) before it reads
 * the job's heap argument and calls into Avant, and is native again once
 * the function has returned; the root drop and the pool lock that follow
 * touch only C memory and the root list (under the world lock, which the
 * collector holds while it collects). */
static void run_job(Job *job) {
  avant_gc_native_end();
  if (job->kind == 1) {
    run_task(job->task);
    free(job);
  } else {
    run_par(job);
  }
}

/* A nested join or .parallel waits on cv until done() (pool_mu held,
 * the thread running): it gives its run back first, as a gate wait
 * does, so a queued task may start, and waits for a run again after. */
static void worker_wait(pthread_cond_t *cv, int (*done)(void *), void *what) {
  pool_running--;
  pool_kick();
  while (!done(what)) {
    avant_gc_cond_wait(cv, &pool_mu);
  }
  pool_resuming++;
  while (pool_running >= pool_bound) {
    avant_gc_cond_wait(&resume_cv, &pool_mu);
  }
  pool_resuming--;
  pool_running++;
  pool_kick();
}

static void *worker_main(void *p) {
  (void)p;
  on_worker = 1;
  avant_gc_register_thread();
  /* A worker allocates young in its own young blocks;
   * until then it allocated in the non-moving mature space, because
   * the young cursor was one per process and, before the safepoint handshake,
   * a young collection on another thread could move a buffer this
   * worker was storing into. */
  /* Native while it waits for a job (rule 2): a collection never waits
   * for an idle worker. */
  avant_gc_native_begin();
  /* Plain lock and wait: the thread is native here. */
  pthread_mutex_lock(&pool_mu);
  pool_starting--;
  for (;;) {
    Job *job;
    /* A job starts only within the bound, after the tasks back from a
     * gate wait (pool_kick). */
    while (!queue_head || pool_running >= pool_bound || pool_resuming > 0) {
      pool_idle++;
      pthread_cond_wait(&queue_cv, &pool_mu);
      pool_idle--;
    }
    job = queue_head;
    unlink_job(job);
    pool_running++;
    /* Another queued job may start too, on another worker. */
    pool_kick();
    pthread_mutex_unlock(&pool_mu);
    run_job(job);
    pthread_mutex_lock(&pool_mu);
    pool_running--;
    pool_kick();
  }
  return NULL;
}

/* The task is C memory: it holds no reference the
 * collector must trace except arg, the capture record, which the task
 * roots (avant_gc_task_root_add, a node in the task,
 * held in place while rooted) from here until its function has returned
 * (run_job drops the root). The record is not pinned: once
 * the generated code pinned it and the join unpinned it, so a spawn that
 * was never joined kept its record (and whatever it captured) and a pin
 * entry for the life of the process. The join frees the task. Earlier
 * still the task itself was a heap object pinned here and never unpinned.
 *
 * The JoinHandle is a small heap object with no references
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

/* a joined task that trapped leaves its trap in the handle's task
 * word, tagged with the low bit (the handle keeps its two words, so a
 * program's allocations are the same as before). */
typedef struct {
  char *what;
  char *site;
} TaskTrap;

static TaskTrap *handle_trap(AvantJoinHandle *hd) {
  uintptr_t w = (uintptr_t)hd->task;
  return (w & 1u) ? (TaskTrap *)(w & ~(uintptr_t)1u) : NULL;
}

void *avant_spawn(void *fn, void *arg) {
  return avant_spawn_f(fn, arg, NULL);
}

/* A new task for fn and its record arg (rooted), with its job; not
 * queued yet. */
static AvantTask *task_new(void *fn, void *arg, void *fail) {
  AvantTask *t;
  Job *job;
  t = (AvantTask *)calloc(1, sizeof(AvantTask));
  if (!t) {
    die_task("out of memory");
  }
  t->fn = (avant_task_fn)fn;
  t->arg = arg;
  t->result = 0;
  t->done = 0;
  t->pending = 1;
  t->fail = (void (*)(void *))fail;
  if (pthread_cond_init(&t->done_cv, NULL) != 0) {
    die_task("task cond");
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
  t->job = job;
  return t;
}

/* Queues t's job (pool_mu held). A task spawned inside a task, or
 * inside a .parallel body, counts in it until it ends (structured
 * completion). */
static void task_queue(AvantTask *t) {
  if (cur_task) {
    t->parent = cur_task;
    cur_task->pending++;
  } else if (cur_par) {
    t->parent_par = cur_par;
    cur_par->remaining++;
  }
  enqueue(t->job);
}

void *avant_spawn_f(void *fn, void *arg, void *fail) {
  AvantJoinHandle *hd;
  AvantTask *t;
  /* gc-lint: holds nothing (arg, the capture record, is a task root from
   * before the first call that may collect, read from the root after
   * it). The handle's allocation is the last call. */
  t = task_new(fn, arg, fail);
  /* The job is queued before the handle exists: the pool
   * lock may wait natively (a collection may run there), and the handle
   * is then allocated last, so this function holds no heap reference
   * across any collecting call (the record is the task's root). The
   * task is C memory; the join finds it through the handle. */
  ensure_pool();
  avant_gc_lock(&pool_mu);
  task_queue(t);
  pthread_mutex_unlock(&pool_mu);
  hd = (AvantJoinHandle *)avant_alloc(sizeof(AvantJoinHandle), AVANT_TYPE_BYTES);
  hd->task = t;
  hd->result = 0;
  return hd;
}

/* A spawn whose block's value is not an Int: the generated code made
 * the handle (its first word the task, then a slot for the value, typed
 * so the collector traces it) and the block stores its value there
 * (avant_task_handle) before it returns. The handle is rooted from here
 * until the task is done, so a dropped handle still takes the value;
 * it is read from its root after the calls that may collect. */
void *avant_spawn_h(void *fn, void *arg, void *fail, void *handle) {
  AvantTask *t;
  AvantJoinHandle *hd;
  t = task_new(fn, arg, fail);
  t->typed = 1;
  avant_gc_task_root_add(&t->hroot, handle);
  ensure_pool();
  avant_gc_lock(&pool_mu);
  hd = (AvantJoinHandle *)t->hroot.obj;
  hd->task = t;
  task_queue(t);
  pthread_mutex_unlock(&pool_mu);
  return hd;
}

/* The handle of the task this thread runs: where a typed block stores
 * its value. */
void *avant_task_handle(void) {
  return cur_task ? cur_task->hroot.obj : NULL;
}

static int task_done(void *p) {
  return ((AvantTask *)p)->done;
}

/* A join on a pool worker (a task joining a task it spawned): the joined
 * task, if no worker took it yet, runs here, in place; else the worker
 * waits for it, giving its run back (worker_wait). pool_mu held, the
 * thread running; hd is rooted by the caller and read again after each
 * call that may collect. */
static void join_on_worker(AvantJoinHandle **hdp) {
  for (;;) {
    AvantTask *t = (*hdp)->task;
    if (!t || handle_trap(*hdp) || t->done) {
      return;
    }
    if (t->job && t->job->queued) {
      Job *job = t->job;
      unlink_job(job);
      pthread_mutex_unlock(&pool_mu);
      run_task(t);
      free(job);
      avant_gc_native_end();
      avant_gc_lock(&pool_mu);
      continue;
    }
    worker_wait(&t->done_cv, task_done, t);
  }
}

avant_int avant_join(void *handle) {
  AvantJoinHandle *hd = (AvantJoinHandle *)handle;
  AvantTask *t;
  avant_int result;
  if (!hd) {
    return 0;
  }
  /* The wait is native (rule 2): other threads collect
   * meanwhile and may move the handle, which this function holds in a
   * handle scope (RC1) and reads only while running (it was pinned before
   * the safepoint handshake, because other threads collected without
   * stopping this one). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &hd);
  avant_gc_lock(&pool_mu);
  if (on_worker) {
    join_on_worker(&hd);
  }
  /* hd->task is read again after each wait: a join that finishes first
   * clears it, and the task is then gone. */
  while (hd->task && !handle_trap(hd) && !hd->task->done) {
    avant_gc_cond_wait(&hd->task->done_cv, &pool_mu);
  }
  t = handle_trap(hd) ? NULL : hd->task;
  if (t) {
    if (!t->typed) {
      hd->result = t->result;
    }
    hd->task = NULL;
    if (t->what) {
      TaskTrap *tt = (TaskTrap *)malloc(sizeof(TaskTrap));
      if (!tt) {
        die_task("out of memory");
      }
      tt->what = t->what;
      tt->site = t->site;
      hd->task = (AvantTask *)((uintptr_t)tt | 1u);
    }
  }
  result = t && t->typed ? 0 : hd->result;
  pthread_mutex_unlock(&pool_mu);
  if (t) {
    /* The worker dropped the record's root before it last touched the
     * task under pool_mu (run_task), so the task can go now. */
    pthread_cond_destroy(&t->done_cv);
    free(t);
  }
  return result;
}

char *avant_join_what(void *handle) {
  TaskTrap *tt = handle ? handle_trap((AvantJoinHandle *)handle) : NULL;
  return tt ? tt->what : NULL;
}

char *avant_join_site(void *handle) {
  TaskTrap *tt = handle ? handle_trap((AvantJoinHandle *)handle) : NULL;
  return tt ? tt->site : (char *)"";
}

static int par_done(void *p) {
  return ((ParDone *)p)->remaining == 0;
}

/* The trap a .parallel's bodies left, kept for avant_parallel_trap,
 * which the generated code calls right after avant_parallel_for and the
 * thaw of what the call froze. */
static __thread int g_par_trapped;
static __thread int g_par_n;
static __thread char *g_par_what;
static __thread char *g_par_site;

/* Runs fn on each index in 0..n on the pool and returns once every body
 * (and every task a body spawned) has ended: 0, or the number of bodies
 * that trapped. On a pool worker (a .parallel inside a task or a body),
 * the bodies still queued run here, in place, and the call waits for the
 * others giving its run back. */
int32_t avant_parallel_for(avant_int n, void *fn, void *ctx) {
  ParDone done;
  Job *jobs;
  int32_t i;
  int32_t trapped;
  if (n <= 0) {
    return 0;
  }
  /* The context is rooted before the first call that may collect (RC1;
   * ensure_pool's lock may wait natively), and read from the root after
   * it (the generated code does not pin it). */
  avant_gc_task_root_add(&done.root, ctx);
  if (pthread_cond_init(&done.cv, NULL) != 0) {
    die_task("parallel cond");
  }
  /* n is an Array's size, within the ceiling. */
  done.remaining = (int32_t)n;
  done.trapped = 0;
  done.what = NULL;
  done.site = NULL;
  /* One block for every body's job, the call's to free. */
  jobs = (Job *)calloc((size_t)n, sizeof(Job));
  if (!jobs) {
    die_task("out of memory");
  }
  ensure_pool();
  avant_gc_lock(&pool_mu);
  for (i = 0; i < n; i++) {
    Job *job = &jobs[i];
    job->kind = 2;
    job->index = i;
    job->fn = fn;
    job->arg = NULL;
    job->par = &done;
    job->owned = 1;
    enqueue_one(job);
  }
  pool_kick();
  if (on_worker) {
    /* The bodies no worker took yet run here, last first (the others
     * start from the queue's head). */
    for (i = (int32_t)n - 1; i >= 0; i--) {
      if (jobs[i].queued) {
        unlink_job(&jobs[i]);
        pthread_mutex_unlock(&pool_mu);
        run_par(&jobs[i]);
        avant_gc_native_end();
        avant_gc_lock(&pool_mu);
      }
    }
    worker_wait(&done.cv, par_done, &done);
  } else {
    while (done.remaining > 0) {
      avant_gc_cond_wait(&done.cv, &pool_mu);
    }
  }
  pthread_mutex_unlock(&pool_mu);
  pthread_cond_destroy(&done.cv);
  avant_gc_task_root_drop(&done.root);
  free(jobs);
  trapped = done.trapped;
  if (trapped) {
    g_par_trapped = trapped;
    g_par_n = (int)n;
    g_par_what = done.what;
    g_par_site = done.site;
  }
  return trapped;
}

/* After a .parallel whose bodies trapped, and after its thaw: the
 * .parallel traps in its caller (in a task, that ends the task; on the
 * main thread, the program), naming how many bodies trapped and the
 * first one's trap (each was told on stderr when it happened). */
void avant_parallel_trap(void) {
  char msg[1024];
  char *what = g_par_what;
  char *site = g_par_site;
  if (site && *site) {
    snprintf(msg, sizeof msg, ".parallel: %d of %d bodies trapped; the first: %s at %s", g_par_trapped, g_par_n,
             what ? what : "", site);
  } else {
    snprintf(msg, sizeof msg, ".parallel: %d of %d bodies trapped; the first: %s", g_par_trapped, g_par_n,
             what ? what : "");
  }
  g_par_what = NULL;
  g_par_site = NULL;
  g_par_trapped = 0;
  free(what);
  free(site);
  avant_trap_text(msg);
}

/* The gates under std.sync's Channel and Mutex. A gate is a word in its object
 * (the `gate` field), and
 * the gate functions take the object and the word's byte offset in it,
 * so the word's address w is computed in the call. The word is the
 * gate's number (avant_gate_new) shifted left by two, and two bits:
 * GATE_LOCKED, the lock of an uncontended gate, and GATE_QUEUED, set
 * while a thread waits on the gate, when the gate's state is an entry of
 * a table of GATE_BUCKETS buckets keyed by the number.
 *
 * Uncontended, a lock and an unlock are one compare-and-swap each on the
 * word and nothing else: no table, no mutex (an earlier table took a
 * bucket's mutex and searched it on every lock and unlock, 222
 * instructions per lock-unlock where the leaking pthread gate before it
 * took 78). A thread that finds the gate locked, or that waits on
 * it (avant_gate_wait), sets GATE_QUEUED under the bucket's mutex and
 * moves the gate's state into an entry (made then, from the bucket's
 * spare list); from then on every operation on the gate goes through
 * the entry, until an unlock that leaves no waiter clears the word back
 * to the plain number and gives the entry back. So a gate allocates
 * nothing and nothing has to be freed when its object dies: a program
 * that makes a million channels holds flat memory (once every gate
 * was a calloc'd mutex and condition that nothing freed), and the spare
 * lists hold at most as many entries as gates were waited on at once.
 *
 * The word's address is valid only while the calling thread runs: the
 * object may move at a collection, which can run while a thread waits
 * natively. So the address is read only before a thread's first native
 * wait in a call, never after it; the waiting thread then works on the
 * entry only (its lock is the entry's `locked` flag), and the word is
 * written back to plain only by an unlock, whose caller is running
 * throughout (an unlock never waits).
 *
 * A waiting thread waits natively on the entry's lock_cv or cond_cv
 * (rule 2): the lock's holder runs Avant code (a
 * Mutex.with block, a channel operation) and may stop at a safepoint. A
 * bucket's mutex is held only inside these functions and never across a
 * safepoint, so it is a plain lock (a short lock whose holder never stops
 * while holding it may be); a native wait releases it, and
 * so does the return from native while a collection runs
 * (native_end_holding). avant_gate_wait gives up the gate's lock and
 * waits on cond_cv; like a condition variable it may return without a
 * signal, and every caller loops on its condition. */
#define GATE_BUCKETS 1024
#define GATE_LOCKED 1ull
#define GATE_QUEUED 2ull
/* a task trapped while holding the gate (a Mutex
 * held in `with`): poisoned, for good. A poisoned word never takes the
 * fast paths, so only the slow paths read it; the number is shifted by
 * three since. */
#define GATE_POISON 4ull
#define GATE_BITS 7ull

typedef struct GateEnt {
  uint64_t id;
  int locked;
  /* the holder's task trapped; dead: it did so while the gate was
   * not queued, so the entry waits for the next lock to queue it (the
   * word still says locked). */
  int poisoned;
  int dead;
  int lock_waiters;
  int cond_waiters;
  pthread_cond_t lock_cv;
  pthread_cond_t cond_cv;
  struct GateEnt *next;
} GateEnt;

typedef struct {
  pthread_mutex_t mu;
  GateEnt *live;
  GateEnt *spare;
} __attribute__((aligned(64))) GateBucket;

static GateBucket gate_buckets[GATE_BUCKETS];
static uint64_t gate_next;
static pthread_once_t gate_once = PTHREAD_ONCE_INIT;

static void gate_init(void) {
  int i;
  for (i = 0; i < GATE_BUCKETS; i++) {
    if (pthread_mutex_init(&gate_buckets[i].mu, NULL) != 0) {
      die_task("gate mutex");
    }
  }
}

/* id is a word without its bits. */
static GateBucket *gate_bucket(uint64_t id) {
  return &gate_buckets[((id >> 3) * 0x9E3779B97F4A7C15ull) >> 54];
}

static GateEnt *gate_find(GateBucket *b, uint64_t id) {
  GateEnt *e;
  for (e = b->live; e; e = e->next) {
    if (e->id == id) {
      return e;
    }
  }
  return NULL;
}

/* A new entry for a gate being queued (b->mu held). */
static GateEnt *gate_take(GateBucket *b, uint64_t id) {
  GateEnt *e = b->spare;
  if (e) {
    b->spare = e->next;
  } else {
    e = (GateEnt *)calloc(1, sizeof(GateEnt));
    if (!e) {
      die_task("out of memory");
    }
    if (pthread_cond_init(&e->lock_cv, NULL) != 0 || pthread_cond_init(&e->cond_cv, NULL) != 0) {
      die_task("gate cond");
    }
  }
  e->id = id;
  e->locked = 0;
  e->poisoned = 0;
  e->dead = 0;
  e->lock_waiters = 0;
  e->cond_waiters = 0;
  e->next = b->live;
  b->live = e;
  return e;
}

/* The entry of a gate no thread waits on goes back (b->mu held). */
static void gate_put(GateBucket *b, GateEnt *e) {
  GateEnt **p = &b->live;
  while (*p != e) {
    p = &(*p)->next;
  }
  *p = e->next;
  e->next = b->spare;
  b->spare = e;
}

/* The entry of the gate at w, queuing the gate when it is not yet (b->mu
 * held, the caller running, so w is valid). A gate queued here was
 * locked by a thread through the word: its lock moves into the entry. */
static GateEnt *gate_queue(GateBucket *b, uint64_t *w, uint64_t id) {
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  GateEnt *e;
  for (;;) {
    if (v & GATE_QUEUED) {
      e = gate_find(b, id);
      if (!e) {
        die_task("gate queued without its entry");
      }
      return e;
    }
    if (!(v & GATE_LOCKED)) {
      return NULL;
    }
    if (__atomic_compare_exchange_n(w, &v, v | GATE_QUEUED, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      e = gate_find(b, id);
      if (e && e->dead) {
        /* Its holder's task trapped (gate_dead): the lock is free. */
        e->dead = 0;
        return e;
      }
      e = gate_take(b, id);
      e->locked = 1;
      e->poisoned = (v & GATE_POISON) != 0;
      return e;
    }
  }
}

/* Takes the entry's lock, waiting natively while another thread holds it
 * (b->mu held on entry and on return; w is not read). */
static void gate_acquire(GateBucket *b, GateEnt *e) {
  while (e->locked) {
    pool_block();
    e->lock_waiters++;
    avant_gc_cond_wait(&e->lock_cv, &b->mu);
    e->lock_waiters--;
  }
  e->locked = 1;
}

uint64_t avant_gate_new(void) {
  pthread_once(&gate_once, gate_init);
  return __atomic_add_fetch(&gate_next, 1, __ATOMIC_RELAXED) << 3;
}

/* 1 when the gate is poisoned. */
static int gate_lock_slow(uint64_t *w) {
  uint64_t id = __atomic_load_n(w, __ATOMIC_RELAXED) & ~GATE_BITS;
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  int poisoned;
  pthread_mutex_lock(&b->mu);
  for (;;) {
    uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
    if (!(v & (GATE_LOCKED | GATE_QUEUED))) {
      /* Unlocked meanwhile, or poisoned and free. */
      if (__atomic_compare_exchange_n(w, &v, v | GATE_LOCKED, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        pthread_mutex_unlock(&b->mu);
        held_push(id);
        return (v & GATE_POISON) != 0;
      }
      continue;
    }
    e = gate_queue(b, w, id);
    if (e) {
      break;
    }
  }
  gate_acquire(b, e);
  poisoned = e->poisoned;
  pthread_mutex_unlock(&b->mu);
  pool_resume();
  held_push(id);
  return poisoned;
}

void avant_gate_lock(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  if (!(v & GATE_BITS) && __atomic_compare_exchange_n(w, &v, v | GATE_LOCKED, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    held_push(v);
    return;
  }
  (void)gate_lock_slow(w);
}

int32_t avant_gate_lock_with(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  if (!(v & GATE_BITS) && __atomic_compare_exchange_n(w, &v, v | GATE_LOCKED, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    held_push(v);
    return 0;
  }
  return gate_lock_slow(w);
}

/* Whether the gate is poisoned (Mutex.poisoned): the word's bit, or the
 * entry of a gate whose holder's task trapped and that no lock took yet. */
int32_t avant_gate_poisoned(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
  uint64_t id = v & ~GATE_BITS;
  GateBucket *b;
  GateEnt *e;
  int32_t r = 0;
  if (v & GATE_POISON) {
    return 1;
  }
  if (!(v & (GATE_LOCKED | GATE_QUEUED))) {
    return 0;
  }
  b = gate_bucket(id);
  pthread_mutex_lock(&b->mu);
  e = gate_find(b, id);
  if (e && e->poisoned) {
    r = 1;
  }
  pthread_mutex_unlock(&b->mu);
  return r;
}

/* Clears the poison of a gate the calling thread holds (Mutex.recover,
 * clear_poison, and a recover function's rebuild under `with`): the
 * word's bit, and the entry's flag when the gate is queued or was taken
 * through a dead entry. Under the bucket's mutex, so no thread queues the
 * gate meanwhile; the caller holds the lock, so the word stays locked or
 * queued and only this call clears its poison bit. */
void avant_gate_clear_poison(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t id = __atomic_load_n(w, __ATOMIC_RELAXED) & ~GATE_BITS;
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  uint64_t v;
  pthread_mutex_lock(&b->mu);
  e = gate_find(b, id);
  if (e) {
    e->poisoned = 0;
  }
  v = __atomic_load_n(w, __ATOMIC_RELAXED);
  while (v & GATE_POISON) {
    if (__atomic_compare_exchange_n(w, &v, v & ~GATE_POISON, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      break;
    }
  }
  pthread_mutex_unlock(&b->mu);
}

/* The task that held the gate numbered id trapped (run_job): the gate is
 * poisoned and its lock given up. A queued gate's entry is free at once
 * (a waiter takes it); a gate locked through its word alone gets a dead
 * entry, which the next lock, finding the word locked, queues and takes
 * (gate_queue). The word itself is not read: its address may have moved. */
static void gate_dead(uint64_t id) {
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  pthread_mutex_lock(&b->mu);
  e = gate_find(b, id);
  if (!e) {
    e = gate_take(b, id);
    e->dead = 1;
  }
  e->locked = 0;
  e->poisoned = 1;
  if (e->lock_waiters > 0) {
    pthread_cond_signal(&e->lock_cv);
  }
  if (e->cond_waiters > 0) {
    pthread_cond_broadcast(&e->cond_cv);
  }
  pthread_mutex_unlock(&b->mu);
}

static void gate_unlock_slow(uint64_t *w) {
  uint64_t id = __atomic_load_n(w, __ATOMIC_RELAXED) & ~GATE_BITS;
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  uint64_t v;
  pthread_mutex_lock(&b->mu);
  v = __atomic_load_n(w, __ATOMIC_RELAXED);
  if (!(v & GATE_QUEUED)) {
    if (!(v & GATE_LOCKED)) {
      die_task("gate unlocked without its lock");
    }
    /* Not queued (a poisoned gate's unlock, or a compare-and-swap that
     * failed spuriously: the word is read again under the mutex). */
    __atomic_store_n(w, id | (v & GATE_POISON), __ATOMIC_RELEASE);
    pthread_mutex_unlock(&b->mu);
    return;
  }
  e = gate_find(b, id);
  if (!e || !e->locked) {
    die_task("gate unlocked without its lock");
  }
  e->locked = 0;
  if (e->lock_waiters > 0) {
    pthread_cond_signal(&e->lock_cv);
  } else if (e->cond_waiters == 0) {
    /* Nobody waits: the gate is plain again (poisoned for good). */
    __atomic_store_n(w, id | (e->poisoned ? GATE_POISON : 0), __ATOMIC_RELEASE);
    gate_put(b, e);
  }
  pthread_mutex_unlock(&b->mu);
}

void avant_gate_unlock(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  held_pop();
  if ((v & GATE_BITS) == GATE_LOCKED && __atomic_compare_exchange_n(w, &v, v & ~GATE_BITS, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    return;
  }
  gate_unlock_slow(w);
}

void avant_gate_wait(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t id = __atomic_load_n(w, __ATOMIC_RELAXED) & ~GATE_BITS;
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  pthread_mutex_lock(&b->mu);
  e = gate_queue(b, w, id);
  if (!e || !e->locked) {
    die_task("gate wait without its lock");
  }
  e->cond_waiters++;
  e->locked = 0;
  if (e->lock_waiters > 0) {
    pthread_cond_signal(&e->lock_cv);
  }
  pool_block();
  avant_gc_cond_wait(&e->cond_cv, &b->mu);
  e->cond_waiters--;
  gate_acquire(b, e);
  pthread_mutex_unlock(&b->mu);
  pool_resume();
}

/* Waiters exist only on a queued gate; the caller holds the gate's lock,
 * so a waiter that queued it did so before this call (it holds the lock
 * while it queues). */
static void gate_wake(uint64_t *w, int all) {
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  GateBucket *b;
  GateEnt *e;
  if (!(v & GATE_QUEUED)) {
    return;
  }
  v &= ~GATE_BITS;
  b = gate_bucket(v);
  pthread_mutex_lock(&b->mu);
  e = gate_find(b, v);
  if (e && e->cond_waiters > 0) {
    if (all) {
      pthread_cond_broadcast(&e->cond_cv);
    } else {
      pthread_cond_signal(&e->cond_cv);
    }
  }
  pthread_mutex_unlock(&b->mu);
}

void avant_gate_signal(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  gate_wake(w, 0);
}

void avant_gate_broadcast(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  gate_wake(w, 1);
}

/* The 64-bit monotonic clock: now_ms and now_us
 * are Int readings that do not wrap. */
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
