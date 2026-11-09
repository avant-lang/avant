#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* Threads stop at safepoints (Stage 28 W17a, gc-design.md section 4.4;
 * closes D191's window). Every registered thread is in one of three
 * states (ThreadReg.state):
 *
 *   running      it may read and write the heap; it stops only at a
 *                safepoint: a collecting call (avant_alloc's slow path,
 *                the entry of gc_collect), avant_gc_poll (generated
 *                code's loop back-edges in shared programs), or the end
 *                of a native region;
 *   at safepoint it waits in gc_safepoint_slow while a collection is
 *                requested; its references are all in root slots (frame
 *                slots, runtime C's handle scopes, runtime-held slots);
 *   native       it runs C that touches no heap object it has not pinned
 *                (a blocking wait, the HTTP loop's poll); returning to
 *                running waits while a collection is in progress.
 *
 * The handshake. A thread that collects (gc_collect) releases the world
 * lock, raises avant_gc_requested under hs_mu (when another thread holds
 * the request it stops at a safepoint instead and returns 0), waits until
 * every other registered thread is at a safepoint or native, takes the
 * world lock again and collects; then gc_resume_world clears the request
 * and wakes the stopped threads. Single-threaded programs run the same
 * protocol with nobody to wait for (one lock and one store per
 * collection), so a thread that registers during a collection waits for
 * its end.
 *
 * The five no-deadlock rules (section 4.4) as kept here:
 *   1. The collector holds no runtime lock while it waits: gc_collect
 *      releases the world lock before gc_stop_world, never holds
 *      rem_mu, and hs_mu is released by the condition wait.
 *   2. A thread blocked in a runtime wait is native (gc_lock,
 *      gc_cond_wait, avant_gc_native_begin/end around the pool's, the
 *      join's, .parallel's, the gates' and the I/O waits). A brief lock
 *      whose holder never reaches a safepoint while it holds it (the
 *      world lock in never-collecting entries, rem_mu, the registry
 *      locks of avant_http.c and avant_re.c) stays a plain lock: a thread
 *      blocked on it waits only for a running thread that will release it
 *      without stopping, and making it native would let a collection move
 *      what the caller of a never-collecting entry holds in registers.
 *   3. The HTTP loops run us_loop_run native and return to running for
 *      each dispatch into Avant (avant_http.c).
 *   4. A foreign thread (registered by its first avant_gc_enter) is
 *      native between callbacks: its outermost avant_gc_leave begins a
 *      native region and its next avant_gc_enter ends it.
 *   5. Leaf functions do not poll (V15): the wait for a numeric leaf
 *      loop is measured (pause_hs_max_ns) and accepted (Q13).
 *
 * The flag and the states use sequentially consistent atomics, so a
 * thread that turns running sees a request raised before the collector
 * looked at its state, or the collector sees it running and waits
 * (Dekker); every change the collector waits for is announced under
 * hs_mu. */

atomic_int avant_gc_requested;

static uint64_t hs_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hs_announce(void) {
  pthread_mutex_lock(&gc_heap.hs_mu);
  pthread_cond_broadcast(&gc_heap.hs_stopped_cv);
  pthread_mutex_unlock(&gc_heap.hs_mu);
}

/* Waits under hs_mu, as STATE, until no collection is requested; the
 * caller holds hs_mu and leaves the state to set afterwards. */
static void hs_wait_locked(int state) {
  atomic_store(&gc_thread.reg.state, state);
  pthread_cond_broadcast(&gc_heap.hs_stopped_cv);
  while (atomic_load(&avant_gc_requested)) {
    pthread_cond_wait(&gc_heap.hs_resume_cv, &gc_heap.hs_mu);
  }
}

void gc_safepoint_slow(void) {
  if (!gc_thread.reg.head || gc_thread.nogc || gc_thread.collector || gc_thread.native_on) {
    return;
  }
  pthread_mutex_lock(&gc_heap.hs_mu);
  if (atomic_load(&avant_gc_requested)) {
    hs_wait_locked(GC_SAFEPOINT);
  }
  atomic_store(&gc_thread.reg.state, GC_RUNNING);
  pthread_mutex_unlock(&gc_heap.hs_mu);
}

void avant_gc_poll(void) {
  gc_safepoint_slow();
}

static int others_running(void) {
  ThreadReg *t;
  for (t = gc_heap.threads; t; t = t->next) {
    if (t != &gc_thread.reg && atomic_load(&t->state) == GC_RUNNING) {
      return 1;
    }
  }
  return 0;
}

int gc_stop_world(void) {
  uint64_t t0;
  int others;
  pthread_mutex_lock(&gc_heap.hs_mu);
  if (atomic_load(&avant_gc_requested)) {
    /* Another thread is collecting: stop here until it is done. A thread
     * that is not registered (no frame yet) cannot be told apart from
     * the others; it waits too. */
    hs_wait_locked(GC_SAFEPOINT);
    atomic_store(&gc_thread.reg.state, GC_RUNNING);
    pthread_mutex_unlock(&gc_heap.hs_mu);
    return 0;
  }
  atomic_store(&avant_gc_requested, 1);
  gc_thread.collector = 1;
  others = gc_heap.threads && (gc_heap.threads != &gc_thread.reg || gc_heap.threads->next);
  if (others) {
    t0 = hs_now_ns();
    while (others_running()) {
      pthread_cond_wait(&gc_heap.hs_stopped_cv, &gc_heap.hs_mu);
    }
    {
      uint64_t dt = hs_now_ns() - t0;
      gc_heap.ctr.pause.hs_n++;
      gc_heap.ctr.pause.hs_ns += dt;
      if (dt > gc_heap.ctr.pause.hs_max_ns) {
        gc_heap.ctr.pause.hs_max_ns = dt;
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.hs_mu);
  return 1;
}

void gc_resume_world(void) {
  pthread_mutex_lock(&gc_heap.hs_mu);
  gc_thread.collector = 0;
  atomic_store(&avant_gc_requested, 0);
  pthread_cond_broadcast(&gc_heap.hs_resume_cv);
  pthread_mutex_unlock(&gc_heap.hs_mu);
}

/* Registration (avant_gc_roots.c calls these with the thread's ThreadReg
 * filled): a new thread joins the list as running, but never while a
 * collection is requested (it waits for the end, under hs_mu, so the
 * collector's view of the list does not change under it). */
void gc_thread_registered(int foreign) {
  gc_thread.reg.owner = &gc_thread;
  gc_thread.foreign = foreign;
  atomic_store(&gc_thread.reg.state, GC_RUNNING);
  pthread_mutex_lock(&gc_heap.hs_mu);
  while (atomic_load(&avant_gc_requested)) {
    pthread_cond_wait(&gc_heap.hs_resume_cv, &gc_heap.hs_mu);
  }
  pthread_mutex_lock(&gc_heap.threads_mu);
  gc_thread.reg.next = gc_heap.threads;
  gc_heap.threads = &gc_thread.reg;
  if (gc_thread.reg.next) {
    atomic_store_explicit(&g_avant_shared, 1, memory_order_release);
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
  pthread_mutex_unlock(&gc_heap.hs_mu);
}

/* Unregistration: the thread leaves the list under hs_mu (a collector
 * waiting for it is woken: it no longer counts). It holds no heap
 * reference once it unregisters. */
void gc_thread_unregistering(void) {
  pthread_mutex_lock(&gc_heap.hs_mu);
  pthread_mutex_lock(&gc_heap.threads_mu);
  {
    ThreadReg **pp = &gc_heap.threads;
    while (*pp) {
      if (*pp == &gc_thread.reg) {
        *pp = gc_thread.reg.next;
        break;
      }
      pp = &(*pp)->next;
    }
  }
  gc_thread.reg.head = NULL;
  gc_thread.reg.next = NULL;
  pthread_mutex_unlock(&gc_heap.threads_mu);
  pthread_cond_broadcast(&gc_heap.hs_stopped_cv);
  pthread_mutex_unlock(&gc_heap.hs_mu);
}

/* Native regions (rule 2). They nest; the outermost begin and end change
 * the state. Inside the thread's own no-GC region, or on an unregistered
 * thread, the state does not change (a region holds references in C that
 * no collection may move, so its thread must not let one run). */
void avant_gc_native_begin(void) {
  if (gc_thread.native++ > 0) {
    return;
  }
  if (!gc_thread.reg.head || gc_thread.nogc || gc_thread.collector) {
    gc_thread.native_on = 0;
    return;
  }
  gc_thread.native_on = 1;
  atomic_store(&gc_thread.reg.state, GC_NATIVE);
  if (atomic_load(&avant_gc_requested)) {
    hs_announce();
  }
}

/* The end of the outermost native region; M, when not NULL, is a mutex
 * the thread holds, released while it waits for a collection to end
 * (the collector may need it: the world lock) and held again before
 * returning. */
static void native_end_holding(pthread_mutex_t *m) {
  if (gc_thread.native <= 0) {
    gc_die("native end without begin");
  }
  if (--gc_thread.native > 0) {
    return;
  }
  if (!gc_thread.native_on) {
    return;
  }
  gc_thread.native_on = 0;
  for (;;) {
    atomic_store(&gc_thread.reg.state, GC_RUNNING);
    if (!atomic_load(&avant_gc_requested)) {
      return;
    }
    /* A collection is requested or running: back to native, and wait. */
    if (m) {
      pthread_mutex_unlock(m);
    }
    pthread_mutex_lock(&gc_heap.hs_mu);
    hs_wait_locked(GC_NATIVE);
    pthread_mutex_unlock(&gc_heap.hs_mu);
    if (m) {
      pthread_mutex_lock(m);
    }
  }
}

void avant_gc_native_end(void) {
  native_end_holding(NULL);
}

void gc_lock(pthread_mutex_t *m) {
  if (pthread_mutex_trylock(m) == 0) {
    return;
  }
  avant_gc_native_begin();
  pthread_mutex_lock(m);
  native_end_holding(m);
}

void gc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m) {
  avant_gc_native_begin();
  pthread_cond_wait(cv, m);
  native_end_holding(m);
}

__attribute__((visibility("hidden"))) void avant_gc_lock(pthread_mutex_t *m) {
  gc_lock(m);
}

__attribute__((visibility("hidden"))) void avant_gc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m) {
  gc_cond_wait(cv, m);
}

__attribute__((visibility("hidden"))) int avant_gc_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *m,
                                                                 const struct timespec *deadline) {
  int rc;
  avant_gc_native_begin();
  rc = pthread_cond_timedwait(cv, m, deadline);
  native_end_holding(m);
  return rc;
}
