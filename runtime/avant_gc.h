#ifndef AVANT_GC_H
#define AVANT_GC_H

#include "avant_rt.h"

#include <stdatomic.h>

/*
 * The collector's interface to the rest of the runtime (Stage 28 W12).
 *
 * The collector lives in runtime/avant_gc_*.c (avant_gc_internal.h lists
 * the files and is private to them). A runtime unit outside the
 * collector (strings, arrays, hashes, Buf, JSON, HTTP, I/O, tasks,
 * codecs) includes this header and calls only what it declares. Every
 * function here is also declared, identically, by the shipped
 * avant_rt.h, which generated code uses; the declarations are repeated
 * so this file states the collector's surface on its own, and a
 * mismatch with avant_rt.h is a compile error. g_avant_shared is the one
 * name here avant_rt.h does not ship.
 *
 * What the collector does at these calls, as the callers rely on it
 * today (a description of the current behavior, not a new contract; W13
 * audits it):
 *   - avant_alloc may collect. A young collection moves nursery objects
 *     and rewrites root slots, pins, runtime TLS slots and, by default,
 *     every word of the collecting thread's stack that names a moved
 *     object's start exactly (avant_gc_stack.c). A full collection may
 *     evacuate sparse mature blocks, except blocks a word of the
 *     collecting thread's stack names (C31). Other threads' C frames are
 *     not scanned (D191).
 *   - avant_gc_defer_enter/leave bracket a region in which no collection
 *     runs (regions nest). With the nursery off (AVANT_NURSERY=0) the
 *     outermost enter may run a full collection first.
 *   - A store of a reference into a heap object is preceded by
 *     avant_barrier on that object (D187).
 *   - avant_pin/avant_unpin keep an object at its address while C holds
 *     it (counted). avant_alloc_in_mature(1) makes the calling thread
 *     allocate in the mature space, which a young collection never
 *     moves, until avant_alloc_in_mature(0) (task-pool workers, HTTP
 *     loop threads, the JSON parser's pool).
 *   - A thread that runs Avant code registers itself
 *     (avant_gc_register_thread) before its first allocation and
 *     unregisters when no Avant frame of it is active; a thread with no
 *     Avant caller opens a base frame with avant_gc_enter/avant_gc_leave
 *     (the HTTP loop threads).
 *   - g_avant_shared is 0 until a second thread registers and 1 from
 *     then on: while it is 0, the one mutator may skip the world lock
 *     (avant_array.c, avant_str.c).
 *   - avant_gc_heap_goal_collect may run a full collection; it is called
 *     only where the caller's C frames hold no references (the HTTP
 *     dispatch).
 *
 * What the collector calls back: avant_hash_walk_tls (avant_hash.c), so
 * the runtime's thread-local reference slots are roots that a moving
 * collection rewrites.
 */

void *avant_alloc(uint64_t nbytes, uint32_t type_id);
void avant_alloc_in_mature(int32_t on);
void avant_barrier(void *obj);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);
void avant_gc_quiet_defer_enter(void);
void avant_pin(void *payload);
void avant_unpin(void *payload);
int avant_is_heap(void *payload);
void avant_world_lock(void);
void avant_world_unlock(void);
void avant_gc_register_thread(void);
void avant_gc_unregister_thread(void);
void avant_gc_enter(void);
void avant_gc_leave(void);
void avant_gc_heap_goal_collect(void);
/* avant_str_generation() (avant_rt.h) reads g_avant_str_gen, which every
 * collection bumps. */
extern _Atomic uint32_t g_avant_str_gen;
extern atomic_int g_avant_shared;

/* Runtime C allocates through avant_alloc_c (Stage 28 W14b1): the same
 * allocation as avant_alloc, plus the stress point of AVANT_GC_STRESS=1
 * (a young collection before it, outside defer regions, in unshared
 * programs). Every runtime unit that includes this header calls it under
 * the name avant_alloc; generated code and user C (avant_rt.h) keep
 * calling avant_alloc itself. */
void *avant_alloc_c(uint64_t nbytes, uint32_t type_id);
#define avant_alloc(nbytes, type_id) avant_alloc_c((nbytes), (type_id))

#endif
