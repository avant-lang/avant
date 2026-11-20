#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The mutator's side of the collector (moved from
 * avant_rt.c): thread registration (stack bounds, frame list heads), frames
 * and root slots (avant_gc_enter/leave, the root batch, reload), quiet
 * leaves, pins, defer regions (helpers that hold references in C), and the
 * world lock. */

/* Becomes 1 when a second thread registers, and stays 1. A single
 * mutator collects only at its own allocating call, so an interior
 * pointer used before the next call does not need a root. Another
 * thread can collect at any instruction. */
atomic_int g_avant_shared;

/* Whether a thread has registered yet: the first one is the main thread. */
static atomic_int g_first_registered;

/* The calling thread's collector state (avant_gc_internal.h). */
__thread GcThread gc_thread;

/* The calling thread's innermost frame (avant_gc.h): generated code's
 * frames and runtime C's handle scopes on one chain (it
 * was GcThread.frame). Generated code reads and writes it. It is defined
 * in avant_gc_alloc.c, beside the allocation path's thread-locals: all
 * six are placed nearest the thread pointer. */

/* Thread exit unregisters. A thread that exits
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

/* A frame avant_gc_enter allocates: the frame, then the thread's list of
 * all such frames (GcThread.frame_all). */
typedef struct CFrame {
  Frame f;
  struct CFrame *all;
} CFrame;

static void thread_exit_hook(void *unused) {
  CFrame *cf;
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
  /* No collection reads an unregistered thread's state: every frame
   * avant_gc_enter allocated (free, or still on the chain of a thread
   * that exited inside a callback) and its remembered buffer go
   * back to the C heap. */
  gc_thread.frame_free = NULL;
  while ((cf = gc_thread.frame_all)) {
    gc_thread.frame_all = cf->all;
    free(cf->f.slots);
    free(cf);
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
  gc_thread.reg.tid = (int)syscall(SYS_gettid);
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
  /* A thread registered by its first avant_gc_enter is a foreign one (the
   * handshake's rule 4 in avant_gc_safepoint.c: native between callbacks),
   * except the program's first thread (the main thread's first frame). */
  foreign = implicit && atomic_exchange(&g_first_registered, 1) != 0;
  if (!implicit) {
    atomic_store(&g_first_registered, 1);
  }
  gc_alloc_thread_init();
  gc_thread_registered(foreign);
  /* The thread's signal stack, on which a stack overflow is told
   * (avant_stack.c). */
  avant_stack_thread_init(gc_thread.reg.stack_lo, gc_thread.reg.stack_hi, foreign);
  /* Every registered thread, a foreign one too, allocates young in its
   * own young blocks (once a foreign thread, the
   * pool's workers and the HTTP loop threads allocated in the mature
   * space: the young cursor was one per process). */
}

void avant_gc_register_thread(void) {
  register_thread(0);
}

void avant_gc_unregister_thread(void) {
  uint32_t i;
  if (!gc_thread.reg.head) {
    return;
  }
  gc_alloc_thread_leave();
  gc_thread_unregistering();
  /* An unregistered thread holds no heap reference:
   * no collection reads its runtime-root slots now, so they are cleared,
   * and a later registration of this thread (a C thread may unregister
   * and call back again) starts with empty slots; before, they kept
   * naming objects the other threads' collections had freed. The slots
   * stay registered, and every runtime unit reads NULL as empty. */
  for (i = 0; i < gc_thread.nroots; i++) {
    *gc_thread.roots[i] = NULL;
  }
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
    CFrame *cf = calloc(1, sizeof(CFrame));
    if (!cf) {
      gc_die("out of memory");
    }
    cf->all = gc_thread.frame_all;
    gc_thread.frame_all = cf;
    f = &cf->f;
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
  if (gc_frame_scope(f) & AVANT_GC_SCOPE_OPEN) {
    /* A runtime C scope (RC1) is innermost: it was not closed, or a
     * generated frame was left from inside one. */
    gc_die("gc leave inside a runtime C scope");
  }
  if (!(gc_frame_scope(f) & AVANT_GC_FRAME_C)) {
    /* A generated function's record is innermost: its words after prev
     * are that function's (its slot locals in place), not a free-list
     * frame's. */
    gc_die("gc leave inside a generated function's frame (avant_gc_leave without its avant_gc_enter)");
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

/* The frame record in the caller's own stack. Generated
 * code does not take a frame from the free list: each frame function
 * keeps its record in its stack frame, { prev, desc } with its slot
 * locals in place after it (avant_gc.h), and stores desc itself. A
 * function that is not a thread's first frame always runs below another
 * record of its thread, and links and unlinks its record inline with no
 * test (prev = avant_gc_top, avant_gc_top = the record; avant_gc_top =
 * prev). A first-frame function (main, a C callback's thunk, a task's
 * thunk) links inline when the thread already has an innermost frame;
 * otherwise, and every frame in the call forms (AVANT_LLVM_CALL_FORMS=1),
 * it calls avant_gc_frame_enter, which does what avant_gc_enter does for
 * a thread's outermost frame (the heap's first use, the thread's
 * registration, rule 4's return to running for a foreign thread's
 * callback), notes the record as the thread's base_frame (the checked
 * mode's gc_verify_frames) and links it. At leave such a record, when it
 * is innermost and not outermost, is unlinked inline; anything else
 * calls avant_gc_frame_leave. A record of a compiler built before the
 * in-place layout ({ prev, slots, n, cap, scope } and the slot addresses)
 * enters the same way. The enters and roots statistics count the frames
 * that enter through a call (avant_gc_enter, avant_gc_frame_enter): in
 * the inline forms a thread's outermost frame, in the call forms every
 * frame. */
void avant_gc_frame_enter(Frame *f) {
  gc_heap.ctr.root.enters++;
  gc_heap.ctr.root.roots += gc_frame_slot_count(f);
  gc_init();
  if (!gc_thread.reg.head) {
    register_thread(1);
  }
  if (gc_thread.base_native && !avant_gc_top) {
    gc_thread.base_native = 0;
    avant_gc_native_end();
  }
  if (!avant_gc_top) {
    gc_thread.base_frame = f;
  }
  f->prev = avant_gc_top;
  avant_gc_top = f;
}

void avant_gc_frame_leave(Frame *f) {
  if (avant_gc_top != f) {
    if (!avant_gc_top) {
      gc_die("gc leave without enter");
    }
    if (gc_frame_scope(avant_gc_top) & AVANT_GC_SCOPE_OPEN) {
      gc_die("gc leave inside a runtime C scope");
    }
    gc_die("gc leave of a frame that is not innermost");
  }
  avant_gc_top = f->prev;
  if (gc_thread.base_frame == f) {
    gc_thread.base_frame = NULL;
  }
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

/* avant_gc_root and avant_gc_root_begin act on the
 * innermost frame, which must be one avant_gc_enter opened. Called by user
 * C without its own avant_gc_enter, the innermost frame is a generated
 * function's record (whose words after prev are that function's own) or
 * a runtime scope, whose slot array lives in a stack frame: growing it
 * would realloc a stack address, and filling it would overwrite the
 * caller's locals or registered slots. */
static void user_frame_check(Frame *f, const char *who) {
  if (!(gc_frame_scope(f) & AVANT_GC_FRAME_C)) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "%s without avant_gc_enter: the innermost frame is %s, not one this C code opened (call avant_gc_enter first, avant_gc_leave after)",
             who, (gc_frame_scope(f) & AVANT_GC_SCOPE_OPEN) ? "a runtime C scope" : "a generated function's frame record");
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
   * The old myc-llvm back end stored this return into the rooted alloca after
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

/* A pin sets PINNED in the object's header, and the inline path's
 * objects since its last sync are read at the sync as that path wrote
 * them (the check modes stop at any other header). A lookup syncs only
 * when the object has no start bit yet, and a shared module's inline
 * sequence sets its objects' bits itself, so the pins sync first. */
void avant_pin(void *payload) {
  if (!payload) {
    return;
  }
  gc_alloc_sync();
  pthread_mutex_lock(&gc_heap.world);
  (void)pin_locked(payload);
  pthread_mutex_unlock(&gc_heap.world);
}

avant_int avant_pin_count(void) {
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

/* Field pins (avant_rt.h): the pins that stores of
 * references into C-layout fields took, recorded by the field's address
 * so that `release` can give them back. A hash of FieldRec by address,
 * chained, under the world lock like the pins themselves. A record holds
 * every value Avant stored at that address since its last release that
 * took a pin (a value stored again right after itself shares an entry
 * with a count), and the last value Avant stored there (pinned or not: a
 * literal string is not a heap payload and takes no pin), which is how a
 * release knows whether the field still holds Avant's value.
 *
 * C changed the memory. When a store or a release
 * finds the field no longer holding the value Avant last stored there,
 * C changed the field or freed the struct and reused its memory (a new
 * struct at the same address joins the old record). Either way C may
 * still hold the earlier values (it took the old pointer, or the old
 * struct's owner kept it), so they stay pinned for the rest of the run:
 * a store starts the record afresh (a later release gives back only what
 * Avant stored since), and a release gives back nothing and leaves C's
 * value in the field. A missing release costs memory, never correctness.
 * Under AVANT_GC_VERIFY=1 a store that finds its field changed reports it
 * once per record (the first 32 fields in full) as a likely missing
 * `release`; a release that finds C's value is silent (C overwriting a
 * value Avant stored is a correct use, and the release was not
 * missing). */
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
  uint32_t warned;
} FieldRec;

static FieldRec **field_tab;
static size_t field_tab_size; /* buckets, a power of two (0: none yet) */
static size_t field_count;    /* records */
static uint64_t field_changed; /* stores that found their field changed by C (VERIFY's reports) */

/* R's field no longer holds Avant's last value (CUR is what it holds):
 * its values stay pinned; the record forgets them. */
static void field_keep_pinned(FieldRec *r, void *cur, int report) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < r->n; i++) {
    kept += r->vals[i].count;
  }
  if (report && gc_heap.set.verify && !r->warned) {
    r->warned = 1;
    if (field_changed++ < 32) {
      fprintf(stderr,
              "avant gc verify: a store into C-layout field %p found it no longer holding the value Avant last stored there "
              "(%p; it holds %p): C changed it, or freed the struct without `release` and reused its memory "
              "(a missing `release`?); %u earlier pin(s) stay\n",
              (void *)r->field, r->last, cur, kept);
    }
  }
  r->n = 0;
}

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
  gc_alloc_sync();
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
  } else if (*f != r->last) {
    field_keep_pinned(r, *f, 1);
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
  if (cur != r->last) {
    /* C changed the field: what Avant stored stays pinned. */
    field_keep_pinned(r, cur, 0);
  }
  for (uint32_t i = 0; i < r->n; i++) {
    for (uint32_t k = 0; k < r->vals[i].count; k++) {
      unpin_locked(r->vals[i].v);
    }
  }
  if (cur != NULL && cur == r->last) {
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

/* The constants computed before main (avant_rt.h). A constant's value
 * never changes and, deeply frozen, can never gain a reference, so once
 * it is computed nothing it reaches needs tracing, moving or a barrier:
 * every Array and Hash it reaches is frozen for good (the freeze a
 * task's capture takes, never thawed), then every heap object it
 * reaches is copied once into the constants' region, outside the
 * collector's heap, and the slot's reference words and the copies'
 * reference words are rewritten to the copies. To the collector a
 * region address is a static address, as a literal's is (the field
 * rule: gc_field_object fails it by one compare), so it never traces,
 * moves or frees a constant, and a collection's work is what it was
 * without them; the heap originals are garbage at the next collection.
 * Nothing in the region names a heap object (AVANT_GC_VERIFY=1 checks
 * it before every collection: gc_verify_consts), so nothing there needs
 * a root, a pin or a remembered entry. What the compiler tells LLVM
 * about a slot and its object (compiler/llvm/emit.av ll_const_facts)
 * holds: nothing writes a slot after the start-up, and nothing writes
 * or moves a region object (its arrays and hashes trap before a change).
 * The walks follow each object's reference words by its type map, once
 * per object; a static object (a literal) is no heap object and is
 * left where it is. The region is written only here, before main, on
 * the main thread. */
void avant_trap_note_const_frozen(void);

/* A map from a heap object to its copy (open addressing by address). */
typedef struct ConstMap {
  void **from;
  void **to;
  size_t cap;
  size_t n;
} ConstMap;

static void const_map_put(ConstMap *m, void *from, void *to);

static void const_map_grow(ConstMap *m) {
  ConstMap g;
  size_t i;
  g.cap = m->cap ? m->cap * 2 : 64;
  g.n = 0;
  g.from = (void **)calloc(g.cap, sizeof(void *));
  g.to = (void **)calloc(g.cap, sizeof(void *));
  if (!g.from || !g.to) {
    gc_die("out of memory");
  }
  for (i = 0; i < m->cap; i++) {
    if (m->from[i]) {
      const_map_put(&g, m->from[i], m->to[i]);
    }
  }
  free(m->from);
  free(m->to);
  *m = g;
}

static size_t const_map_at(const ConstMap *m, const void *p) {
  return (size_t)(((uintptr_t)p >> 3) * 0x9e3779b97f4a7c15ull) & (m->cap - 1);
}

static void const_map_put(ConstMap *m, void *from, void *to) {
  size_t i;
  if ((m->n + 1) * 2 >= m->cap) {
    const_map_grow(m);
  }
  i = const_map_at(m, from);
  while (m->from[i]) {
    i = (i + 1) & (m->cap - 1);
  }
  m->from[i] = from;
  m->to[i] = to;
  m->n++;
}


/* The region: chunks taken from malloc, never given back, and the list
 * of every object copied there (the verifier's walk). A payload is
 * 16-byte aligned, as a heap object's start may be. */
static uint8_t *g_const_cur;
static size_t g_const_left;
static void **g_const_objs;
static size_t g_const_objs_n;
static size_t g_const_objs_cap;
static uint64_t g_const_bytes;

static void *const_region_take(size_t payload) {
  size_t need = (sizeof(AvantHeader) + payload + 15 + 16) & ~(size_t)15;
  uint8_t *at;
  if (need > g_const_left) {
    size_t chunk = need > (size_t)(1u << 16) ? need : (size_t)(1u << 16);
    g_const_cur = (uint8_t *)aligned_alloc(16, chunk);
    if (!g_const_cur) {
      gc_die("out of memory");
    }
    g_const_left = chunk;
  }
  /* The header right before a 16-aligned payload. */
  at = g_const_cur + 16;
  g_const_cur += need;
  g_const_left -= need;
  g_const_bytes += need;
  if (g_const_objs_n == g_const_objs_cap) {
    g_const_objs_cap = g_const_objs_cap ? g_const_objs_cap * 2 : 64;
    g_const_objs = (void **)gc_xrealloc(g_const_objs, g_const_objs_cap * sizeof(void *));
  }
  g_const_objs[g_const_objs_n++] = at;
  return at;
}

/* The walk's buffers, kept from one constant to the next (start-up is
 * one thread, before main): the objects met, in order, and the map from
 * each to its copy (its key present, its copy NULL, while the walk
 * lists). Emptied after each constant by its own list. */
static ConstMap g_const_map;
static void **g_const_list;
static size_t g_const_list_cap;

static void **const_map_slot(ConstMap *m, const void *p) {
  size_t i = const_map_at(m, p);
  while (m->from[i] && m->from[i] != p) {
    i = (i + 1) & (m->cap - 1);
  }
  return &m->from[i];
}

/* Whether P joins the walk (a heap object not met yet). */
static int const_meet(void *p) {
  void **at;
  if ((g_const_map.n + 1) * 2 >= g_const_map.cap) {
    const_map_grow(&g_const_map);
  }
  at = const_map_slot(&g_const_map, p);
  if (*at) {
    return 0;
  }
  *at = p;
  g_const_map.to[at - g_const_map.from] = NULL;
  g_const_map.n++;
  return 1;
}

/* ROOT's copy in the region: every heap object it reaches is frozen
 * (an Array or a Hash: for good) and copied once, and the copies'
 * reference words are rewritten to the copies. */
static void *const_copy(void *root) {
  size_t n = 0;
  size_t k;
  void *out;
  if (!root || !gc_is_heap_payload(root, NULL, NULL)) {
    return root;
  }
  if (!g_const_list_cap) {
    g_const_list_cap = 64;
    g_const_list = (void **)gc_xrealloc(NULL, g_const_list_cap * sizeof(void *));
  }
  (void)const_meet(root);
  g_const_list[n++] = root;
  for (k = 0; k < n; k++) {
    void *obj = g_const_list[k];
    uint32_t id = avant_header(obj)->type_id;
    if (id == AVANT_TYPE_ARRAY_OBJ) {
      avant_value_freeze(obj, 1, 0);
      avant_trap_note_const_frozen();
    } else if (id == AVANT_TYPE_HASH) {
      avant_value_freeze(obj, 1, 1);
      avant_trap_note_const_frozen();
    }
    EACH_REF_SLOT(obj, slot, {
      void *q = *slot;
      if (q && gc_is_heap_payload(q, NULL, NULL) && const_meet(q)) {
        if (n == g_const_list_cap) {
          g_const_list_cap *= 2;
          g_const_list = (void **)gc_xrealloc(g_const_list, g_const_list_cap * sizeof(void *));
        }
        g_const_list[n++] = q;
      }
    });
  }
  for (k = 0; k < n; k++) {
    const AvantHeader *h = avant_header(g_const_list[k]);
    void *to = const_region_take((size_t)h->size);
    AvantHeader *nh = avant_header(to);
    nh->size = h->size;
    nh->type_id = h->type_id;
    /* No heap flag: a static object, as a literal is. */
    nh->flags = 0;
    memcpy(to, g_const_list[k], (size_t)h->size);
    g_const_map.to[const_map_slot(&g_const_map, g_const_list[k]) - g_const_map.from] = to;
  }
  for (k = 0; k < n; k++) {
    void *to = g_const_map.to[const_map_slot(&g_const_map, g_const_list[k]) - g_const_map.from];
    EACH_REF_SLOT(to, slot, {
      void *q = *slot;
      if (q && gc_is_heap_payload(q, NULL, NULL)) {
        void **at = const_map_slot(&g_const_map, q);
        if (!*at) {
          gc_die("avant gc: a constant's copy missed an object it reaches");
        }
        *slot = g_const_map.to[at - g_const_map.from];
      }
    });
  }
  out = g_const_map.to[const_map_slot(&g_const_map, root) - g_const_map.from];
  /* Every key goes: the table is empty again. The slots are found
   * first and emptied after, since emptying one in the middle of a probe
   * chain would hide the keys after it from the search. */
  for (k = 0; k < n; k++) {
    g_const_list[k] = (void *)const_map_slot(&g_const_map, g_const_list[k]);
  }
  for (k = 0; k < n; k++) {
    *(void **)g_const_list[k] = NULL;
  }
  g_const_map.n = 0;
  return out;
}

void avant_const_ready(void *slot, uint64_t words) {
  void **base = (void **)slot;
  uint64_t w;
  gc_init();
  /* The inline path's objects since its last sync are counted first:
   * the walk looks objects up by their start bits. */
  gc_alloc_sync();
  pthread_mutex_lock(&gc_heap.world);
  for (w = words; w; w &= w - 1) {
    void **ws = base + __builtin_ctzll(w);
    *ws = const_copy(*ws);
  }
  pthread_mutex_unlock(&gc_heap.world);
}

/* AVANT_GC_VERIFY=1, before every collection: no word of a constant's
 * copy names the collector's heap (a frozen value cannot gain one; a
 * word that did would name an object the collector may move or free). */
void gc_verify_consts(void) {
  size_t i;
  for (i = 0; i < g_const_objs_n; i++) {
    void *obj = g_const_objs[i];
    EACH_REF_SLOT(obj, slot, {
      void *q = *slot;
      if (q && (uint64_t)((uintptr_t)q - (uintptr_t)gc_heap.base) < gc_heap.committed) {
        fprintf(stderr, "avant gc verify: a constant's field names a heap object: holder %p (type %u), field %p, value %p\n", obj,
                (unsigned)avant_header(obj)->type_id, (void *)slot, q);
        gc_die("avant gc verify: a constant's field names a heap object");
      }
    });
  }
}

/* The bytes the constants' region holds (AVANT_GC_STATS). */
uint64_t gc_const_region_bytes(void) {
  return g_const_bytes;
}

/* Task roots (avant_gc.h): traced from
 * gc_heap.task_roots and rewritten when the record moves (before the
 * safepoint handshake the record was PINNED while rooted). */
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

/* RC4's no-GC region (avant_gc.h). The region's depth is
 * gc_heap.defer, the one counter defer regions used, so every rule that
 * keeps a collection out of a defer region keeps it out of this one
 * (gc_maybe_collect, the stress point, the heap limit in place and the
 * large-object space); its entry is not a collection point. The
 * outermost entry notes its caller for "heap limit in a no-GC region"
 * and "collection inside a no-GC region". */
static void nogc_begin(void *site) {
  gc_init();
  gc_fast_clear();
  /* The thread's own depth (the only one; the process-wide
   * gc_heap.defer is retired): this thread
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
 * (before, the outermost entry was a collection point
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
