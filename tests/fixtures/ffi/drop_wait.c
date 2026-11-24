/* tests/fixtures/ffi/drop_wait.av: the runtime's drop of a Buf's old
 * large storage (avant_gc_large_drop) waits for the world lock while a C
 * thread registers (its first callback) and collects. The runtime calls
 * avant_gc_test_drop_hook right before it takes the lock (step 0) and when
 * it is done (step 1); defined here, it makes that wait happen once, after
 * drop_wait_start: this thread goes native, as a contended lock's wait
 * does, lets the C thread run its callback, and comes back when the
 * callback has returned. The drop must then see a shared program and
 * leave the storage to the collector (step 1 says it did not free it). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <time.h>

void avant_gc_native_begin(void);
void avant_gc_native_end(void);

typedef int64_t (*drop_wait_cb_t)(int64_t);
static drop_wait_cb_t dw_cb;
static pthread_t dw_th;
static int64_t dw_got;
static volatile int dw_armed, dw_go, dw_done, dw_fired, dw_watch, dw_freed;

static void nap(void) {
  struct timespec ts = {0, 1000000};
  nanosleep(&ts, NULL);
}

static void *dw_run(void *a) {
  (void)a;
  while (!dw_go) {
    nap();
  }
  dw_got = dw_cb(1);
  dw_done = 1;
  return 0;
}

void avant_gc_test_drop_hook(int step, int done) {
  if (step == 1) {
    if (dw_watch == 1) {
      dw_watch = 2;
      dw_freed = done;
    }
    return;
  }
  if (!dw_armed || dw_fired) {
    return;
  }
  dw_fired = 1;
  dw_watch = 1;
  avant_gc_native_begin();
  dw_go = 1;
  while (!dw_done) {
    nap();
  }
  avant_gc_native_end();
}

int64_t drop_wait_start(drop_wait_cb_t f) {
  dw_cb = f;
  dw_armed = 1;
  return pthread_create(&dw_th, 0, dw_run, 0);
}

/* The callback's result, after the thread ended; -1 when the wait never
 * happened (no drop of a large storage reached the hook), and the result
 * plus 1000000000 when the drop after the wait freed the storage in the
 * shared program. */
int64_t drop_wait_join(void) {
  if (!dw_fired) {
    dw_go = 1;
  }
  avant_gc_native_begin();
  pthread_join(dw_th, 0);
  avant_gc_native_end();
  return dw_fired ? dw_got + (dw_freed ? 1000000000 : 0) : -1;
}
