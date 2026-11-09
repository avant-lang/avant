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

/* Whether a thread has registered yet: the first one is the main thread
 * (Stage 28 W17a). */
static atomic_int g_first_registered;

/* The calling thread's collector state (avant_gc_internal.h). */
__thread GcThread gc_thread;

/* The calling thread's innermost frame (avant_gc.h): generated code's
 * frames and runtime C's handle scopes on one chain (Stage 28 W16a; it
 * was GcThread.frame). Generated code reads and writes it since W18. */
__thread Frame *avant_gc_top;

/* Thread exit unregisters (Stage 28 W17c, O126). A thread that exits
 * still registered (a foreign C thread after its last callback, or any
 * thread that never called avant_gc_unregister_thread) would leave its
 * ThreadReg in gc_heap.threads while its GcThread, thread-local, is freed
 * with the thread: every later collection would read it, and a later
 * thread on the same reused stack would register the same node again (a
 * cycle the handshake loops on forever). Every registration sets a
 * pthread key whose destructor runs at the thread's exit, for threads
 * Avant did not create too, while the thread's thread-local state is
 * still valid (glibc runs the key destructors before it frees the
 * thread). */
static pthread_key_t g_exit_key;
static pthread_once_t g_exit_once = PTHREAD_ONCE_INIT;

static void thread_exit_hook(void *unused) {
  Frame *f;
  (void)unused;
  if (gc_thread.reg.head) {
    /* Back to running (gc_thread_exiting waits while a collection is in
     * progress), then unregistered as avant_gc_unregister_thread does it:
     * the young hole's charge settled, the remembered buffer merged, the
     * thread out of the list. The thread's frames are gone with its
     * stack: the chain is dropped first (the thread is running, so no
     * collection reads it meanwhile). */
    gc_thread_exiting();
    avant_gc_top = NULL;
    avant_gc_unregister_thread();
  }
  /* No collection reads an unregistered thread's state: its free frames
   * and its remembered buffer go back to the C heap. */
  while ((f = gc_thread.frame_free)) {
    gc_thread.frame_free = f->prev;
    free(f->slots);
    free(f);
  }
  free(gc_thread.rembuf);
  gc_thread.rembuf = NULL;
  gc_thread.rembuf_n = 0;
  gc_thread.rembuf_cap = 0;
}

static void exit_key_make(void) {
  if (pthread_key_create(&g_exit_key, thread_exit_hook) != 0) {
    gc_die("cannot create the thread-exit key");
  }
}

static void register_thread(int implicit) {
  int foreign;
  if (gc_thread.reg.head) {
    return;
  }
  pthread_once(&g_exit_once, exit_key_make);
  if (pthread_setspecific(g_exit_key, &gc_thread) != 0) {
    gc_die("cannot set the thread-exit key");
  }
  gc_thread.reg.head = &avant_gc_top;
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
  /* A thread registered by its first avant_gc_enter is a foreign one
   * (rule 4 of gc-design.md section 4.4: native between callbacks),
   * except the program's first thread (the main thread's first frame). */
  foreign = implicit && atomic_exchange(&g_first_registered, 1) != 0;
  if (!implicit) {
    atomic_store(&g_first_registered, 1);
  }
  gc_alloc_thread_init();
  gc_thread_registered(foreign);
  /* Every registered thread, a foreign one too, allocates young in its
   * own young blocks (Stage 28 W17b; until then a foreign thread, the
   * pool's workers and the HTTP loop threads allocated in the mature
   * space: the young cursor was one per process, W17a's rule 4 proof). */
}

void avant_gc_register_thread(void) {
  register_thread(0);
}

void avant_gc_unregister_thread(void) {
  if (!gc_thread.reg.head) {
    return;
  }
  gc_alloc_thread_leave();
  gc_thread_unregistering();
}

/* A call used to calloc a frame and free it on leave. Hot recursion
 * (Brainfuck) and numeric methods spent the call on that pair. Frames
 * are kept on a thread-local freelist; the slot array is reused too. */
void avant_gc_enter(void) {
  gc_heap.ctr.root.enters++;
  gc_init();
  if (!gc_thread.reg.head) {
    register_thread(1);
  }
  if (gc_thread.base_native && !avant_gc_top) {
    /* Rule 4: a foreign thread's callback returns to running (a
     * safepoint if a collection is in progress). */
    gc_thread.base_native = 0;
    avant_gc_native_end();
  }
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
  f->prev = avant_gc_top;
  f->scope = AVANT_GC_FRAME_C;
  avant_gc_top = f;
}

void avant_gc_leave(void) {
  Frame *f = avant_gc_top;
  if (!f) {
    gc_die("gc leave without enter");
  }
  if (f->scope & AVANT_GC_SCOPE_OPEN) {
    /* A runtime C scope (RC1) is innermost: it was not closed, or a
     * generated frame was left from inside one. */
    gc_die("gc leave inside a runtime C scope");
  }
  avant_gc_top = f->prev;
  f->n = 0;
  f->prev = gc_thread.frame_free;
  gc_thread.frame_free = f;
  if (!avant_gc_top && gc_thread.foreign && !gc_thread.base_native) {
    /* Rule 4: a foreign thread is native between callbacks. */
    gc_thread.base_native = 1;
    avant_gc_native_begin();
  }
}

/* The frame record in the caller's own stack (Stage 28 W18). Generated
 * code since W18 does not take a frame from the free list: each frame
 * function keeps an AvantGcFrame in its stack frame, its slot array right
 * after it, and fills prev-free fields itself (slots, n = cap = the root
 * words, scope 0, then each slot's address). When the thread already has
 * an innermost frame, it links the record inline (prev = avant_gc_top,
 * avant_gc_top = the record); otherwise, and always in the call forms
 * (AVANT_LLVM_CALL_FORMS=1), it calls avant_gc_frame_enter, which does
 * what avant_gc_enter does for a thread's outermost frame (the heap's
 * first use, the thread's registration, rule 4's return to running for a
 * foreign thread's callback) and links it. At leave, a record that is
 * innermost and not outermost is unlinked inline (avant_gc_top = prev);
 * anything else calls avant_gc_frame_leave. A quiet region does not
 * clear the record's count: registering slots no collection reads is
 * harmless. The enters and roots statistics count the frames that
 * enter through a call (avant_gc_enter, avant_gc_frame_enter): in the
 * inline forms a thread's outermost frame, in the call forms every
 * frame. */
void avant_gc_frame_enter(Frame *f) {
  gc_heap.ctr.root.enters++;
  gc_heap.ctr.root.roots += f->n;
  gc_init();
  if (!gc_thread.reg.head) {
    register_thread(1);
  }
  if (gc_thread.base_native && !avant_gc_top) {
    gc_thread.base_native = 0;
    avant_gc_native_end();
  }
  f->prev = avant_gc_top;
  avant_gc_top = f;
}

void avant_gc_frame_leave(Frame *f) {
  if (avant_gc_top != f) {
    if (!avant_gc_top) {
      gc_die("gc leave without enter");
    }
    if (avant_gc_top->scope & AVANT_GC_SCOPE_OPEN) {
      gc_die("gc leave inside a runtime C scope");
    }
    gc_die("gc leave of a frame that is not innermost");
  }
  avant_gc_top = f->prev;
  if (!avant_gc_top && gc_thread.foreign && !gc_thread.base_native) {
    /* Rule 4: a foreign thread is native between callbacks. */
    gc_thread.base_native = 1;
    avant_gc_native_begin();
  }
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

/* O138 (Stage 28 W18M): avant_gc_root and avant_gc_root_begin act on the
 * innermost frame, which must be one avant_gc_enter opened. Called by user
 * C without its own avant_gc_enter, the innermost frame is a generated
 * function's record or a runtime scope, whose slot array lives in a stack
 * frame: growing it would realloc a stack address, and filling it would
 * overwrite the caller's registered slots. */
static void user_frame_check(Frame *f, const char *who) {
  if (!(f->scope & AVANT_GC_FRAME_C)) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "%s without avant_gc_enter: the innermost frame is %s, not one this C code opened (call avant_gc_enter first, avant_gc_leave after)",
             who, (f->scope & AVANT_GC_SCOPE_OPEN) ? "a runtime C scope" : "a generated function's frame record");
    gc_die(msg);
  }
}

void avant_gc_root(void *slot) {
  gc_heap.ctr.root.roots++;
  if (gc_thread.quiet) {
    return;
  }
  if (!avant_gc_top) {
    gc_die("gc root without enter");
  }
  Frame *f = avant_gc_top;
  uint32_t i;
  user_frame_check(f, "avant_gc_root");
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
  if (!avant_gc_top) {
    gc_die("gc root begin without enter");
  }
  Frame *f = avant_gc_top;
  user_frame_check(f, "avant_gc_root_begin");
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

/* One more pin of payload, under the world lock (taken by the caller).
 * Returns 0 when payload is not a heap payload (nothing is pinned). */
static int pin_locked(void *payload) {
  if (!gc_is_heap_payload(payload, NULL, NULL)) {
    return 0;
  }
  AvantHeader *h = avant_header(payload);
  if (h->flags & AVANT_FLAG_PINNED) {
    for (PinEnt *p = gc_heap.pins; p; p = p->next) {
      if (p->payload == payload) {
        p->count += 1;
        return 1;
      }
    }
  }
  PinEnt *e = (PinEnt *)malloc(sizeof(PinEnt));
  if (!e) {
    pthread_mutex_unlock(&gc_heap.world);
    gc_die("out of memory");
  }
  gc_hdr_or(h, AVANT_FLAG_PINNED);
  e->payload = payload;
  e->count = 1;
  e->next = gc_heap.pins;
  gc_heap.pins = e;
  return 1;
}

/* One pin of payload given back, under the world lock (taken by the
 * caller). A pointer that is not a pinned heap payload is ignored. */
static void unpin_locked(void *payload) {
  PinEnt **pp;
  if (!gc_is_heap_payload(payload, NULL, NULL)) {
    return;
  }
  AvantHeader *h = avant_header(payload);
  if ((h->flags & AVANT_FLAG_PINNED) == 0) {
    return;
  }
  pp = &gc_heap.pins;
  while (*pp) {
    if ((*pp)->payload == payload) {
      (*pp)->count -= 1;
      if ((*pp)->count > 0) {
        return;
      }
      PinEnt *dead = *pp;
      *pp = dead->next;
      free(dead);
      break;
    }
    pp = &(*pp)->next;
  }
  gc_hdr_and(h, (uint16_t)~AVANT_FLAG_PINNED);
}

void avant_pin(void *payload) {
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&gc_heap.world);
  (void)pin_locked(payload);
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
  if (!payload) {
    return;
  }
  pthread_mutex_lock(&gc_heap.world);
  unpin_locked(payload);
  pthread_mutex_unlock(&gc_heap.world);
}

/* Field pins (Stage 28 W16c, D266; avant_rt.h): the pins that stores of
 * references into C-layout fields took, recorded by the field's address
 * so that `release` can give them back. A hash of FieldRec by address,
 * chained, under the world lock like the pins themselves. A record holds
 * every value Avant stored at that address since its last release that
 * took a pin (a value stored again right after itself shares an entry
 * with a count), and the last value Avant stored there (pinned or not: a
 * literal string is not a heap payload and takes no pin), which is how a
 * release knows whether the field still holds Avant's value. */
typedef struct FieldVal {
  void *v;
  uint32_t count;
} FieldVal;

typedef struct FieldRec {
  struct FieldRec *next;
  void **field;
  void *last;
  FieldVal *vals;
  uint32_t n;
  uint32_t cap;
} FieldRec;

static FieldRec **field_tab;
static size_t field_tab_size; /* buckets, a power of two (0: none yet) */
static size_t field_count;    /* records */

static size_t field_hash(void **field) {
  uint64_t x = (uint64_t)(uintptr_t)field >> 3;
  x *= 0x9E3779B97F4A7C15ull;
  return (size_t)(x >> 32) & (field_tab_size - 1);
}

static void *field_alloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) {
    pthread_mutex_unlock(&gc_heap.world);
    gc_die("out of memory");
  }
  return p;
}

static void field_grow(void) {
  size_t old = field_tab_size;
  FieldRec **ot = field_tab;
  field_tab_size = old ? old * 2 : 64;
  field_tab = (FieldRec **)field_alloc(field_tab_size * sizeof(FieldRec *));
  for (size_t i = 0; i < old; i++) {
    FieldRec *r = ot[i];
    while (r) {
      FieldRec *nx = r->next;
      size_t b = field_hash(r->field);
      r->next = field_tab[b];
      field_tab[b] = r;
      r = nx;
    }
  }
  free(ot);
}

void avant_pin_field(void *field, void *payload) {
  void **f = (void **)field;
  pthread_mutex_lock(&gc_heap.world);
  int pinned = payload ? pin_locked(payload) : 0;
  if (field_count >= field_tab_size) {
    field_grow();
  }
  size_t b = field_hash(f);
  FieldRec *r = field_tab[b];
  while (r && r->field != f) {
    r = r->next;
  }
  if (!r) {
    r = (FieldRec *)field_alloc(sizeof(FieldRec));
    r->field = f;
    r->next = field_tab[b];
    field_tab[b] = r;
    field_count += 1;
  }
  r->last = payload;
  if (pinned) {
    if (r->n > 0 && r->vals[r->n - 1].v == payload) {
      r->vals[r->n - 1].count += 1;
    } else {
      if (r->n == r->cap) {
        uint32_t cap = r->cap ? r->cap * 2 : 4;
        FieldVal *nv = (FieldVal *)realloc(r->vals, cap * sizeof(FieldVal));
        if (!nv) {
          pthread_mutex_unlock(&gc_heap.world);
          gc_die("out of memory");
        }
        r->vals = nv;
        r->cap = cap;
      }
      r->vals[r->n].v = payload;
      r->vals[r->n].count = 1;
      r->n += 1;
    }
  }
  pthread_mutex_unlock(&gc_heap.world);
}

/* Gives back r's pins and nulls its field if the field still holds a
 * value Avant stored there; frees r (already unlinked). */
static void field_release(FieldRec *r) {
  void *cur = *r->field;
  int ours = cur != NULL && cur == r->last;
  for (uint32_t i = 0; i < r->n; i++) {
    if (r->vals[i].v == cur) {
      ours = cur != NULL;
    }
    for (uint32_t k = 0; k < r->vals[i].count; k++) {
      unpin_locked(r->vals[i].v);
    }
  }
  if (ours) {
    *r->field = NULL;
  }
  free(r->vals);
  free(r);
}

void avant_release_range(void *lo, uint64_t n) {
  uintptr_t a = (uintptr_t)lo;
  uintptr_t end = a + (uintptr_t)n;
  pthread_mutex_lock(&gc_heap.world);
  if (field_count == 0 || n == 0) {
    pthread_mutex_unlock(&gc_heap.world);
    return;
  }
  if (n / sizeof(void *) <= field_count) {
    /* A field or a struct: look up each pointer-aligned address. */
    uintptr_t p = (a + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    for (; p + sizeof(void *) <= end; p += sizeof(void *)) {
      FieldRec **rp = &field_tab[field_hash((void **)p)];
      while (*rp && (uintptr_t)(*rp)->field != p) {
        rp = &(*rp)->next;
      }
      if (*rp) {
        FieldRec *r = *rp;
        *rp = r->next;
        field_count -= 1;
        field_release(r);
      }
    }
  } else {
    /* A range larger than the table: walk the table. */
    for (size_t i = 0; i < field_tab_size; i++) {
      FieldRec **rp = &field_tab[i];
      while (*rp) {
        FieldRec *r = *rp;
        uintptr_t f = (uintptr_t)r->field;
        if (f >= a && f < end) {
          *rp = r->next;
          field_count -= 1;
          field_release(r);
        } else {
          rp = &r->next;
        }
      }
    }
  }
  pthread_mutex_unlock(&gc_heap.world);
}

/* Task roots (avant_gc.h; Stage 28 W16b, C28, R1): traced from
 * gc_heap.task_roots and rewritten when the record moves (Stage 28 W17a:
 * until then the record was PINNED while rooted, D191). */
void avant_gc_task_root_add(AvantGcTaskRoot *r, void *obj) {
  gc_init();
  r->obj = obj;
  r->prev = NULL;
  pthread_mutex_lock(&gc_heap.world);
  r->next = gc_heap.task_roots;
  if (r->next) {
    r->next->prev = r;
  }
  gc_heap.task_roots = r;
  pthread_mutex_unlock(&gc_heap.world);
}

void avant_gc_task_root_drop(AvantGcTaskRoot *r) {
  pthread_mutex_lock(&gc_heap.world);
  if (r->prev) {
    r->prev->next = r->next;
  } else {
    gc_heap.task_roots = r->next;
  }
  if (r->next) {
    r->next->prev = r->prev;
  }
  r->next = r->prev = NULL;
  r->obj = NULL;
  pthread_mutex_unlock(&gc_heap.world);
}

/* RC4's no-GC region (avant_gc.h; Stage 28 W16a). The region's depth is
 * gc_heap.defer, the one counter defer regions used, so every rule that
 * keeps a collection out of a defer region keeps it out of this one
 * (gc_maybe_collect, the stress point, C32's limit in place and the
 * large-object space); its entry is not a collection point. The
 * outermost entry notes its caller for "heap limit in a no-GC region"
 * and "collection inside a no-GC region". */
static void nogc_begin(void *site) {
  gc_init();
  gc_fast_clear();
  /* The thread's own depth (Stage 28 W17a; the only one since W17b,
   * which retired the process-wide gc_heap.defer, O125): this thread
   * never stops at a safepoint inside a region, nor turns native, nor
   * collects; another thread's collection waits for it to leave the
   * region and reach a safepoint. So no collection runs while the
   * region is open, and one thread's region no longer holds off the
   * other threads' collection policy. A region must therefore never
   * wait for another thread (that thread may be stopped for a
   * collection that waits for this one). */
  if (gc_thread.nogc == 0) {
    gc_thread.region_bytes = 0;
    gc_thread.region_site = site;
  }
  gc_thread.nogc += 1;
}

__attribute__((visibility("hidden"))) void avant_gc_nogc_begin(void) {
  nogc_begin(__builtin_return_address(0));
}

/* The ABI names user --cc C may call (avant_rt.h): the same region
 * (Stage 28 W16a; before, the outermost entry was a collection point
 * with the nursery off and under AVANT_GC_STRESS=1, and runtime helpers
 * bracketed their allocations with it). */
void avant_gc_defer_enter(void) {
  nogc_begin(__builtin_return_address(0));
}

void avant_gc_defer_leave(void) {
  avant_gc_nogc_end();
}

/* The region's end. A collection the policy asked for inside is still
 * due (the budget stays spent, a pending explicit or heap-limit request
 * stays set), so the first collection point after the region runs it;
 * the end itself collects nothing, so the function may still hold what
 * it built inside. */
__attribute__((visibility("hidden"))) void avant_gc_nogc_end(void) {
  if (gc_thread.nogc > 0) {
    gc_thread.nogc -= 1;
  }
}

void avant_world_lock(void) {
  gc_init();
  pthread_mutex_lock(&gc_heap.world);
}

void avant_world_unlock(void) {
  pthread_mutex_unlock(&gc_heap.world);
}
