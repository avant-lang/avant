/* sched_getaffinity and CPU_COUNT (O700). */
#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/*
 * Bounded pool shared by spawn and .parallel (D35, Q38-A).
 * The bound is the program's chosen thread count (avant_chosen_threads:
 * --threads, else the CPUs the process may use): at most that many tasks run at
 * once. Workers never touch g_world. Join or .parallel from a worker
 * waits for a pool thread, so a full pool deadlocks; those calls die.
 *
 * A task waiting on a gate gives its run back (Stage 29 W8Cb, O567,
 * D355): a worker that waits in a Channel's send or recv or for a Mutex
 * (pool_block, from the gate functions below) stops counting as running,
 * so a queued task may start, on an idle worker or, when every worker
 * is waiting, on a new one, up to POOL_THREAD_LIMIT threads (or the
 * bound, if larger); past the limit the program is told once on
 * standard error and the queued task waits. When the wait ends, the task
 * waits for a run (pool_resume) before it goes on, so the bound holds.
 * Before, as many waiting tasks as the pool had threads kept a queued
 * task from ever starting, and the program hung with no message.
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
/* This worker's task waits on a gate in the current gate call. */
static __thread int pool_blocked;

static void die_task(const char *msg) {
  fprintf(stderr, "avant runtime: %s\n", msg);
  abort();
}

/* .join or .parallel reached from a pool worker through a call (the
 * checker refuses one written in a spawn block or a .parallel body): a
 * misuse, so it ends like the runtime's traps (trap_head and trap_tail
 * in avant_rt_traps.c, whose form avant_re.c's traps repeat): the
 * program's stdout flushed, one line naming what happened and the
 * rule, status 1 (Stage 29 W-FIX1f, O234: it aborted, status 134).
 * die_task stays for the pool's own failures. what is ".join" or
 * ".parallel". */
__attribute__((noreturn, cold)) static void trap_on_worker(const char *what) {
  fflush(stdout);
  fprintf(stderr,
          "avant runtime: %s called from a pool worker: .join and .parallel cannot be called inside a spawn block "
          "or a .parallel body, directly or through a call\n",
          what);
  exit(1);
}

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
            "thread to be free (--threads N bounds the tasks that run at once, and a task that waits lets a queued "
            "one start while threads remain)\n",
            pool_threads, pool_limit);
  }
}

static void enqueue(Job *job) {
  job->next = NULL;
  if (queue_tail) {
    queue_tail->next = job;
  } else {
    queue_head = job;
  }
  queue_tail = job;
  pool_kick();
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

/* The program's one chosen thread count: the --threads flag when the
 * generated startup recorded one, else the CPUs the process may use. The
 * pool and the HTTP server's loop count both read this one value.
 * AVANT_THREADS, deprecated for one release by Q125-A, is not read. */
static long g_flag_threads;

void avant_set_flag_threads(long n) {
  g_flag_threads = n;
}

/* The CPUs this process may run on (Stage 29 W8Cc, O700): its affinity
 * mask (sched_getaffinity, CPU_COUNT), which `taskset`, a container's
 * cpuset and numactl set; the online processor count only when the mask
 * cannot be read. A mask past CPU_SETSIZE is read with a set sized to
 * it. A cgroup's CPU quota (cpu.max) is not read (O700's record). The
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
  if (g_flag_threads > 0) {
    return (int32_t)g_flag_threads;
  }
  return avant_usable_cpus();
}

static long pool_count(void) {
  return (long)avant_chosen_threads();
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
  n = pool_count();
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
    queue_head = job->next;
    if (!queue_head) {
      queue_tail = NULL;
    }
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
    trap_on_worker(".join");
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
    trap_on_worker(".parallel");
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

/* The gates under std.sync's Channel and Mutex (Stage 29 W8Ca, O532;
 * W8Cb, O690). A gate is a word in its object (the `gate` field), and
 * the gate functions take the object and the word's byte offset in it,
 * so the word's address w is computed in the call. The word is the
 * gate's number (avant_gate_new) shifted left by two, and two bits:
 * GATE_LOCKED, the lock of an uncontended gate, and GATE_QUEUED, set
 * while a thread waits on the gate, when the gate's state is an entry of
 * a table of GATE_BUCKETS buckets keyed by the number.
 *
 * Uncontended, a lock and an unlock are one compare-and-swap each on the
 * word and nothing else: no table, no mutex (O690: W8Ca's table took a
 * bucket's mutex and searched it on every lock and unlock, 222
 * instructions per lock-unlock where the leaking pthread gate of the
 * base took 78). A thread that finds the gate locked, or that waits on
 * it (avant_gate_wait), sets GATE_QUEUED under the bucket's mutex and
 * moves the gate's state into an entry (made then, from the bucket's
 * spare list); from then on every operation on the gate goes through
 * the entry, until an unlock that leaves no waiter clears the word back
 * to the plain number and gives the entry back. So a gate allocates
 * nothing and nothing has to be freed when its object dies: a program
 * that makes a million channels holds flat memory (before W8Ca every gate
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
 * (Stage 28 W17a, D258's rule 2): the lock's holder runs Avant code (a
 * Mutex.with block, a channel operation) and may stop at a safepoint. A
 * bucket's mutex is held only inside these functions and never across a
 * safepoint, so it is a plain lock (D258's amendment: a short lock whose
 * holder never stops while holding it); a native wait releases it, and
 * so does the return from native while a collection runs
 * (native_end_holding). avant_gate_wait gives up the gate's lock and
 * waits on cond_cv; like a condition variable it may return without a
 * signal, and every caller loops on its condition. */
#define GATE_BUCKETS 1024
#define GATE_LOCKED 1ull
#define GATE_QUEUED 2ull
#define GATE_BITS 3ull

typedef struct GateEnt {
  uint64_t id;
  int locked;
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
  return &gate_buckets[((id >> 2) * 0x9E3779B97F4A7C15ull) >> 54];
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
      e = gate_take(b, id);
      e->locked = 1;
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
  return __atomic_add_fetch(&gate_next, 1, __ATOMIC_RELAXED) << 2;
}

static void gate_lock_slow(uint64_t *w) {
  uint64_t id = __atomic_load_n(w, __ATOMIC_RELAXED) & ~GATE_BITS;
  GateBucket *b = gate_bucket(id);
  GateEnt *e;
  pthread_mutex_lock(&b->mu);
  for (;;) {
    uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
    if (!(v & GATE_BITS)) {
      /* Unlocked meanwhile. */
      if (__atomic_compare_exchange_n(w, &v, v | GATE_LOCKED, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        pthread_mutex_unlock(&b->mu);
        return;
      }
      continue;
    }
    e = gate_queue(b, w, id);
    if (e) {
      break;
    }
  }
  gate_acquire(b, e);
  pthread_mutex_unlock(&b->mu);
  pool_resume();
}

void avant_gate_lock(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
  if (!(v & GATE_BITS) && __atomic_compare_exchange_n(w, &v, v | GATE_LOCKED, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return;
  }
  gate_lock_slow(w);
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
    /* Not queued after all (a compare-and-swap that failed spuriously
     * cannot, but the word is read again under the mutex). */
    __atomic_store_n(w, id, __ATOMIC_RELEASE);
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
    /* Nobody waits: the gate is plain again. */
    __atomic_store_n(w, id, __ATOMIC_RELEASE);
    gate_put(b, e);
  }
  pthread_mutex_unlock(&b->mu);
}

void avant_gate_unlock(void *obj, uint64_t off) {
  uint64_t *w = (uint64_t *)((char *)obj + off);
  uint64_t v = __atomic_load_n(w, __ATOMIC_RELAXED);
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

/* The 64-bit monotonic clock (Stage 29 W4b1, D295 R6): now_ms and now_us
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
