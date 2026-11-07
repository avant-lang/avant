#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The mutator's side of the collector (Stage 28 W12, moved from
 * avant_rt.c): thread registration (stack bounds, frame list heads), frames
 * and root slots (avant_gc_enter/leave, the root batch, reload), quiet
 * leaves, pins, defer regions (helpers that hold references in C), and the
 * world lock. */

/* Becomes 1 when a second thread registers, and stays 1. A single
 * mutator collects only at its own allocating call, so an interior
 * pointer used before the next call does not need a root. Another
 * thread can collect at any instruction. */
atomic_int g_avant_shared;

/* The calling thread's collector state (avant_gc_internal.h). */
__thread GcThread gc_thread;

void avant_gc_register_thread(void) {
  if (gc_thread.reg.head) {
    return;
  }
  gc_thread.reg.head = &gc_thread.frame;
  {
    pthread_attr_t attr;
    void *addr = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      if (pthread_attr_getstack(&attr, &addr, &sz) == 0 && addr && sz) {
        gc_thread.reg.stack_lo = addr;
        gc_thread.reg.stack_hi = (uint8_t *)addr + sz;
      }
      pthread_attr_destroy(&attr);
    }
  }
  pthread_mutex_lock(&gc_heap.threads_mu);
  gc_thread.reg.next = gc_heap.threads;
  gc_heap.threads = &gc_thread.reg;
  if (gc_thread.reg.next) {
    atomic_store_explicit(&g_avant_shared, 1, memory_order_release);
  }
  pthread_mutex_unlock(&gc_heap.threads_mu);
}

void avant_gc_unregister_thread(void) {
  pthread_mutex_lock(&gc_heap.threads_mu);
  ThreadReg **pp = &gc_heap.threads;
  while (*pp) {
    if (*pp == &gc_thread.reg) {
      *pp = gc_thread.reg.next;
      break;
    }
    pp = &(*pp)->next;
  }
  gc_thread.reg.head = NULL;
  gc_thread.reg.next = NULL;
  pthread_mutex_unlock(&gc_heap.threads_mu);
}

/* A call used to calloc a frame and free it on leave. Hot recursion
 * (Brainfuck) and numeric methods spent the call on that pair. Frames
 * are kept on a thread-local freelist; the slot array is reused too. */
void avant_gc_enter(void) {
  gc_heap.ctr.root.enters++;
  gc_init();
  avant_gc_register_thread();
  Frame *f = gc_thread.frame_free;
  if (f) {
    gc_thread.frame_free = f->prev;
    f->n = 0;
  } else {
    f = calloc(1, sizeof(Frame));
    if (!f) {
      gc_die("out of memory");
    }
  }
  f->prev = gc_thread.frame;
  gc_thread.frame = f;
}

void avant_gc_leave(void) {
  Frame *f = gc_thread.frame;
  if (!f) {
    gc_die("gc leave without enter");
  }
  gc_thread.frame = f->prev;
  f->n = 0;
  f->prev = gc_thread.frame_free;
  gc_thread.frame_free = f;
}

/* A runtime unit registers a thread-local slot that holds a heap
 * reference (or NULL) once per thread, before the slot first holds one;
 * from then on every collection on this thread treats it as a root: it
 * marks what the slot names and rewrites the slot when the object moves.
 * The slot must live as long as the thread (a __thread variable). No
 * lock: only this thread registers its slots, and only a collection
 * running on this thread reads them. */
void avant_gc_add_root(void **slot) {
  uint32_t i;
  if (!slot) {
    return;
  }
  for (i = 0; i < gc_thread.nroots; i++) {
    if (gc_thread.roots[i] == slot) {
      return;
    }
  }
  if (gc_thread.nroots == GC_THREAD_ROOTS) {
    gc_die("too many runtime roots on one thread");
  }
  gc_thread.roots[gc_thread.nroots++] = slot;
}

void avant_gc_quiet_enter(void) {
  gc_thread.quiet++;
}

void avant_gc_quiet_leave(void) {
  if (gc_thread.quiet > 0) {
    gc_thread.quiet--;
  }
}

void avant_gc_root(void *slot) {
  gc_heap.ctr.root.roots++;
  if (gc_thread.quiet) {
    return;
  }
  if (!gc_thread.frame) {
    gc_die("gc root without enter");
  }
  Frame *f = gc_thread.frame;
  uint32_t i;
  /*
   * Codegen emits CALL :avant_gc_root at the first use site of a temp.
   * When that site is inside a loop (Array.push/pop), the same stack
   * slot was appended every iteration and the frame list grew without
   * bound (prod Words RSS). Same address is already a root.
   */
  for (i = 0; i < f->n; i++) {
    if (f->slots[i] == (void **)slot) {
      return;
    }
  }
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->slots = gc_xrealloc(f->slots, f->cap * sizeof(void **));
  }
  f->slots[f->n++] = (void **)slot;
}

void *avant_gc_root_begin(uint32_t count) {
  gc_heap.ctr.root.roots += count;
  if (!gc_thread.frame) {
    gc_die("gc root begin without enter");
  }
  Frame *f = gc_thread.frame;
  if (count > f->cap) {
    uint32_t cap = f->cap ? f->cap : 8;
    while (cap < count) {
      cap *= 2;
    }
    f->slots = gc_xrealloc(f->slots, cap * sizeof(void **));
    f->cap = cap;
  }
  f->n = gc_thread.quiet ? 0 : count;
  return f->slots;
}

__attribute__((noinline))
void *avant_gc_reload(void *slot) {
  gc_heap.ctr.root.reloads++;
  /*
   * Stage 17: myc-llvm stores this return into the rooted alloca after
   * a collecting CALL. The load is opaque to LLVM isel, so a pre-call
   * copy in an unrooted spill cannot replace it.
   */
  void *p = *(void *volatile *)slot;
  *(void *volatile *)slot = p;
  return p;
}

void avant_pin(void *payload) {
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&gc_heap.world);
  if (!is_heap_payload(payload, NULL, NULL)) {
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  AvantHeader *h = avant_header(payload);
  if (h->flags & AVANT_FLAG_PINNED) {
    for (PinEnt *p = gc_heap.pins; p; p = p->next) {
      if (p->payload == payload) {
        p->count += 1;
        pthread_mutex_unlock(&gc_heap.world);
        return;
      }
    }
  }
  PinEnt *e = (PinEnt *)malloc(sizeof(PinEnt));
  if (!e) {
    pthread_mutex_unlock(&gc_heap.world);
    gc_die("out of memory");
  }
  h->flags |= AVANT_FLAG_PINNED;
  e->payload = payload;
  e->count = 1;
  e->next = gc_heap.pins;
  gc_heap.pins = e;
  pthread_mutex_unlock(&gc_heap.world);
}

int avant_pin_count(void) {
  int n = 0;
  pthread_mutex_lock(&gc_heap.world);
  for (PinEnt *p = gc_heap.pins; p; p = p->next) {
    n += 1;
  }
  pthread_mutex_unlock(&gc_heap.world);
  return n;
}

void avant_unpin(void *payload) {
  PinEnt **pp;
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&gc_heap.world);
  if (!is_heap_payload(payload, NULL, NULL)) {
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  AvantHeader *h = avant_header(payload);
  if ((h->flags & AVANT_FLAG_PINNED) == 0) {
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  pp = &gc_heap.pins;
  while (*pp) {
    if ((*pp)->payload == payload) {
      (*pp)->count -= 1;
      if ((*pp)->count > 0) {
        pthread_mutex_unlock(&gc_heap.world);
        return;
      }
      PinEnt *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  h->flags = (uint16_t)(h->flags & ~AVANT_FLAG_PINNED);
  pthread_mutex_unlock(&gc_heap.world);
}

static void defer_enter(int collecting_row) {
  gc_init();
  gc_fast_clear();
  pthread_mutex_lock(&gc_heap.world);
  /*
   * Helpers wrap avant_alloc in defer so C-held pointers are not swept.
   * If every allocation in a loop is inside that window, gc_maybe_collect
   * in avant_alloc never fires (prod Base64 RSS 3.7 GB). Collect *before*
   * taking defer, while the caller has already stored the previous result
   * in a rooted local. Nested enter (hash grow, buf push inside json_gen)
   * must not collect: the outer helper still holds unmarked pointers.
   * Do not collect on leave — a helper that returns a heap pointer has
   * not yet STOREd it.
   *
   * Young copy must not run here: myc spilled heap args into roots and
   * then copied them into C registers. A moving collect would update the
   * slots and leave the C args dangling. Full collect stays non-moving
   * (AVANT_NURSERY=0), so it is still safe at this boundary.
   */
  if (gc_heap.defer == 0 && (gc_heap.set.nursery_off || (gc_heap.limit_pending && collecting_row))) {
    /* With the nursery on, only a collection C32 left pending runs
     * here, and not at a quiet row's entry: a full collection moves no
     * young object and keeps every block this thread's stack names
     * (C31), and the caller of a collecting row has rooted what it
     * holds. */
    gc_maybe_collect();
  }
  /* AVANT_GC_STRESS=1: a young collection at every outermost entry
   * (W14b1). It moves, so it proves that no helper holds a reference
   * the collector cannot see or rewrite when its region begins. */
  if (gc_heap.defer == 0 && gc_heap.set.stress && collecting_row) {
    gc_thread.c_entry++;
    gc_stress_point();
    gc_thread.c_entry--;
  }
  gc_heap.defer += 1;
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_gc_defer_enter(void) {
  defer_enter(1);
}

/* The outermost defer region of a helper the call table calls *quiet*
 * (compiler/mir/runtime.av: the hash setters, avant_hash_inc_slice and
 * the avant_buf_push_* rows). Its caller may hold a reference outside
 * its slots across the call (V11/V12 ask for slots only across
 * collecting calls), so no moving collection may run at its entry, and
 * AVANT_GC_STRESS=1 has no stress point here (Stage 28 W14b1). The rest
 * is avant_gc_defer_enter, including the full collection of the
 * nursery-off mode. W15 makes these rows collecting calls (gc-design.md
 * §5.1); then this entry goes. */
void avant_gc_quiet_defer_enter(void) {
  defer_enter(0);
}

void avant_gc_defer_leave(void) {
  pthread_mutex_lock(&gc_heap.world);
  if (gc_heap.defer > 0) {
    gc_heap.defer -= 1;
  }
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_world_lock(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
}

void avant_world_unlock(void) {
  pthread_mutex_unlock(&gc_heap.world);
}
