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
 * function here but the two runtime-internal entries at the end is also
 * declared, identically, by the shipped avant_rt.h, which generated
 * code uses; the declarations are repeated so this file states the
 * collector's surface on its own, and a mismatch with avant_rt.h is a
 * compile error. g_avant_shared, avant_gc_quiet_defer_enter,
 * avant_alloc_c and the safepoint (avant_gc_safepoint_slow,
 * avant_gc_safepoint_force, gc_safepoint_entry) are the names here
 * avant_rt.h does not ship.
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
 *   - A runtime unit that keeps a heap reference in a thread-local
 *     variable registers that slot once per thread with
 *     avant_gc_add_root before it first holds one (avant_hash.c's last
 *     key); the collections on that thread read and rewrite it like a
 *     root slot (Stage 28 W14b2, gc-design.md section 1.4; it replaced a
 *     callback from the collector into avant_hash.c).
 *
 * The runtime-C contract (Stage 28 W14b2 states it here; gc-design.md
 * section 4.2's RC1-RC4 and its handle scopes replace the first part in
 * W16):
 *
 *   The stress mode's contract, the defer discipline (W14b1). A runtime
 *   helper that holds a heap reference in C across a point where a
 *   collection may run keeps it inside one defer region
 *   (avant_gc_defer_enter ... avant_gc_defer_leave). Its outermost
 *   region begins before it forms a derived pointer (an interior pointer
 *   such as s + start, or a buffer's data pointer) or writes a stack
 *   buffer it will copy into the heap, because the region's entry is a
 *   collection point: with the nursery off it may run a full
 *   collection, and under AVANT_GC_STRESS=1 a young collection that
 *   moves what it reaches. An internal helper whose callers do either
 *   before calling it enters quietly (avant_gc_quiet_defer_enter: the
 *   same region, without the stress point), as does a helper whose entry
 *   is a safepoint (below): its region begins right after the safepoint,
 *   which already was its collection point. Fresh objects the helper
 *   allocated and has not yet published are held the same way, inside
 *   the region.
 *
 *   The named safepoints (Stage 28 W15a1, gc-design.md section 5.1, the
 *   interim until W16's handle scopes): a few helpers the call table
 *   calls *collects* begin with gc_safepoint_entry(a, ...), naming every
 *   heap reference argument the helper reads after it (one to three):
 *   the hash setters avant_hash_set_i32, _set_str, _set_i32k_i32,
 *   avant_hash_inc_slice, buf_append_slow (behind the avant_buf_push_*
 *   rows), and the array entries avant_array_push_slot, _push_ptr,
 *   _push_i32 and avant_array_reserve. When the young budget is spent
 *   (or under AVANT_GC_STRESS=1 always, or with a heap-limit collection
 *   pending) and only in an unshared program, outside every defer
 *   region and collection, the safepoint runs the collection the policy
 *   asks for (gc_maybe_collect; a young one under stress) with the named
 *   arguments as root slots of a frame of its own, so a moved argument is
 *   read back from that slot into the helper's parameter: the helper's
 *   C locals hold no stale reference after it, and the collection does
 *   not rewrite the C stack (it needs no conservative rewrite). The
 *   caller rooted its references across the call like any collecting
 *   call (compiler/mir/gc.av). The safepoint is the helper's first
 *   statement: before it forms a derived pointer or copies an argument
 *   into another local.
 *
 *   One young budget (gc-design.md section 5.1): avant_nursery_held
 *   counts every young byte, the nursery's and the young large
 *   objects', helper allocations included; a collection is due when it
 *   reaches avant_young_bytes.
 *
 *   Barriers (D187). Every store of a reference into a heap object is
 *   followed by avant_barrier on that object (or avant_barrier_at on the
 *   stored-to address) before the next point where a collection may run
 *   (C34's lesson: avant_hash_inc_slice and array_grow lost young keys
 *   and elements without one).
 *
 *   The young collection's three-way rule, R1' (gc-design.md section
 *   1.3, D219 (2)). A young collection scans one level of every
 *   non-young object that is named (a) by a root (a frame slot, a pin, a
 *   runtime root of avant_gc_add_root), (b) by a field of a young
 *   object, or (c) by a field of a remembered object; deeper non-young
 *   objects are followed only when they are remembered themselves. The
 *   generated code's fresh-store exemption relies on it (a buffer
 *   promoted in the middle of its fill is still found through its slot,
 *   mir.md section 5.4), and so did the runtime builders that fill a
 *   born-mature large reference buffer (array_grow, str_array_build,
 *   hash_array_str, avant_array_copy, avant_argv: a young array header
 *   (b) or a root (a) names the buffer; audit N2b), which since W13b
 *   (D220) also barrier the buffer. AVANT_GC_VERIFY checks the generational invariant in this wording
 *   before every young collection: every young reference held by a
 *   non-young object's field is held by a remembered object or by an
 *   object R1' names this cycle ("missing barrier: ..." otherwise).
 */

/*
 * The collector's settings (Stage 28 W14b2, gc-design.md section 1.7,
 * O40): the environment variables below, read once when the collector
 * starts (gc_init_slow, avant_gc_collect.c) into one settings struct.
 * One rule for all of them: unset or empty is the default; a switch is
 * on for exactly 1 and off for exactly 0; a number is decimal digits
 * only, in its range; any other value is reported on stderr at start-up
 * ("avant runtime: warning: ...") and the default applies.
 *
 *   AVANT_YOUNG_MB         number, 0..2^30, default 0: the young
 *                          generation's threshold in MiB, fixed; 0 is the
 *                          adaptive threshold (16 MiB to start)
 *   AVANT_HEAP_MAX_MB      number, 0..2^30, default 0: the heap limit in
 *                          MiB ("heap limit" past it); 0 is no limit
 *   AVANT_HEAP_RESERVE_GB  number, 1..2^20, default 64: the heap's
 *                          address reservation in GiB, halved on refusal
 *                          down to 1 (W14a1)
 *   AVANT_NURSERY          switch, default 1: the copying nursery; 0 is
 *                          full, non-moving collections only
 *   AVANT_GC_DEFRAG        switch, default 1: mature evacuation in full
 *                          collections
 *   AVANT_GC_STACK_REWRITE switch, default 1: the young copy's rewrite of
 *                          the collecting thread's C stack (with the
 *                          nursery; until gc-design.md section 4.3
 *                          retires it)
 *   AVANT_GC_VERIFY        switch, default 0: the collector's checkers
 *                          (avant_gc_verify.c)
 *   AVANT_GC_STATS         switch, default 0: one statistics line on
 *                          stderr at exit
 *   AVANT_GC_STRESS        switch, default 0: a young collection at every
 *                          runtime C allocation and every outermost defer
 *                          entry (testing; W14b1)
 *
 * Retired (W14b2; setting one is reported and has no effect):
 * AVANT_NURSERY_DISCARD, AVANT_GC_REM_FRONTIER, AVANT_GC_YOUNG_SIZING,
 * AVANT_GC_COPY_RESERVE. Removed earlier: AVANT_NURSERY_STACK (W14a1),
 * AVANT_NURSERY_STACKMAP (W12r).
 */

void *avant_alloc(uint64_t nbytes, uint32_t type_id);
void avant_alloc_in_mature(int32_t on);
void avant_barrier(void *obj);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);
/* Runtime-internal (Stage 28 W14b2, O37): avant_gc_quiet_defer_enter
 * and avant_alloc_c below are entries of avant_rt.o that only the
 * runtime's other objects call. avant_rt.h does not declare them, and
 * their visibility is hidden: no linked image exports them (the second
 * section of avant_rt.exports). */
__attribute__((visibility("hidden"))) void avant_gc_quiet_defer_enter(void);
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
void avant_gc_add_root(void **slot);
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
__attribute__((visibility("hidden"))) void *avant_alloc_c(uint64_t nbytes, uint32_t type_id);
#define avant_alloc(nbytes, type_id) avant_alloc_c((nbytes), (type_id))

/* The named safepoints (the contract above; Stage 28 W15a1). The test is
 * inline: the young budget spent, or avant_gc_safepoint_force (set under
 * AVANT_GC_STRESS=1, and while a heap-limit collection is pending). The
 * slow path takes the arguments by value in an array it roots, and
 * gc_safepoint_entry writes them back, so the helper's parameters name
 * the objects where the collection left them. */
extern __attribute__((visibility("hidden"))) uint32_t avant_gc_safepoint_force;
__attribute__((visibility("hidden"))) void avant_gc_safepoint_slow(void **roots, uint32_t n);
#define GC_SAFEPOINT_DUE_()                                                         \
  __builtin_expect(avant_nursery_held >= avant_young_bytes || avant_gc_safepoint_force != 0, 0)
#define GC_SAFEPOINT_1_(a)                                                          \
  do {                                                                              \
    if (GC_SAFEPOINT_DUE_()) {                                                      \
      void *gc_sp_r_[1] = {(void *)(a)};                                            \
      avant_gc_safepoint_slow(gc_sp_r_, 1);                                         \
      (a) = gc_sp_r_[0];                                                            \
    }                                                                               \
  } while (0)
#define GC_SAFEPOINT_2_(a, b)                                                       \
  do {                                                                              \
    if (GC_SAFEPOINT_DUE_()) {                                                      \
      void *gc_sp_r_[2] = {(void *)(a), (void *)(b)};                               \
      avant_gc_safepoint_slow(gc_sp_r_, 2);                                         \
      (a) = gc_sp_r_[0];                                                            \
      (b) = gc_sp_r_[1];                                                            \
    }                                                                               \
  } while (0)
#define GC_SAFEPOINT_3_(a, b, c)                                                    \
  do {                                                                              \
    if (GC_SAFEPOINT_DUE_()) {                                                      \
      void *gc_sp_r_[3] = {(void *)(a), (void *)(b), (void *)(c)};                  \
      avant_gc_safepoint_slow(gc_sp_r_, 3);                                         \
      (a) = gc_sp_r_[0];                                                            \
      (b) = gc_sp_r_[1];                                                            \
      (c) = gc_sp_r_[2];                                                            \
    }                                                                               \
  } while (0)
#define GC_SAFEPOINT_PICK_(_1, _2, _3, NAME, ...) NAME
#define gc_safepoint_entry(...)                                                     \
  GC_SAFEPOINT_PICK_(__VA_ARGS__, GC_SAFEPOINT_3_, GC_SAFEPOINT_2_, GC_SAFEPOINT_1_, _)(__VA_ARGS__)

#endif
