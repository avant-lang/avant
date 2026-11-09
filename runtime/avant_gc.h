#ifndef AVANT_GC_H
#define AVANT_GC_H

#include "avant_rt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

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
 * compile error. g_avant_shared, avant_alloc_c, the no-GC region (avant_gc_nogc_begin/end) and the
 * handle scopes (AvantGcFrame, avant_gc_top, AVANT_GC_SCOPE) are the
 * names here avant_rt.h does not ship.
 *
 * What the collector does at these calls, as the callers rely on it
 * today (a description of the current behavior, not a new contract; W13
 * audits it):
 *   - avant_alloc may collect. A young collection (S+, Stage 28 W15c)
 *     promotes young objects in place and copies the survivors of the
 *     young blocks it evacuates, rewriting root slots, runtime TLS slots
 *     and the fields of the objects it scanned; it never reads or
 *     rewrites a word of the C stack (the conservative scans retired in
 *     Stage 28 W16b, gc-design.md section 4.3: runtime C roots what it
 *     holds, RC1 below), and pinned objects never move. A full collection
 *     may evacuate sparse mature blocks holding no pinned object; it
 *     rewrites every precise reference. Every other registered thread is
 *     stopped at a safepoint or native meanwhile (Stage 28 W17a's
 *     handshake, avant_gc_safepoint.c; it closed D191's window).
 *   - avant_gc_defer_enter/leave (avant_gc_nogc_begin/end) bracket a
 *     region in which no collection runs (regions nest); the entry
 *     collects nothing (RC4 below; Stage 28 W16a).
 *   - A store of a reference into a heap object is preceded by
 *     avant_barrier on that object (D187), or by
 *     avant_barrier_slot(object, slot) when the slot is known (Stage 28
 *     W16t): a mature large pointer buffer (a carded object) is then
 *     scanned at a young collection only in the 256-byte cards a store
 *     marked; avant_barrier on such a buffer counts every card.
 *   - avant_pin/avant_unpin keep an object at its address while C holds
 *     it (counted). avant_alloc_in_mature(1) makes the calling thread
 *     allocate in the mature space, which a young collection never
 *     moves, until avant_alloc_in_mature(0) (no runtime unit uses it
 *     since Stage 28 W17b: the pool's workers, the HTTP loop threads and
 *     foreign threads allocate young in their own young blocks).
 *   - A thread that runs Avant code registers itself
 *     (avant_gc_register_thread) before its first allocation and
 *     unregisters when no Avant frame of it is active; a thread with no
 *     Avant caller opens a base frame with avant_gc_enter/avant_gc_leave
 *     (the HTTP loop threads). A thread that exits still registered (a
 *     foreign C thread, registered by its first avant_gc_enter) is
 *     unregistered by its exit (a pthread key destructor, Stage 28 W17c).
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
 * The runtime-C contract, RC1-RC4 (Stage 28 W16a, gc-design.md section
 * 4.2; it replaces W14b1's defer discipline and W15a1's named
 * safepoints). A runtime C function is a mutator like generated code:
 * any call that may collect (an allocation, a helper that allocates, a
 * callback into Avant) may run a young collection that moves the young
 * objects it can (S+'s copy) and frees the unreached ones, or a full
 * collection that may evacuate mature blocks. The caller of a runtime
 * entry has rooted its own references (compiler/mir/gc.av); what the
 * function holds in C across such a call is its own business:
 *
 *   RC1. Handles. A heap reference held across a call that may collect
 *   is kept in a C local registered in a handle scope, and read from
 *   that local after the call:
 *       AVANT_GC_SCOPE(2);        a frame of 2 slots on avant_gc_top
 *       AVANT_GC_ROOT(0, &a);     slot 0 names the local a
 *       AVANT_GC_ROOT(1, &b);
 *       p = avant_alloc(...);     may collect, may move a and b
 *       memcpy(p, a, na);         a, b re-read from their locals
 *       AVANT_GC_UNSCOPE();       optional: the scope also ends with
 *                                 the C block that declared it
 *   The scope is a frame of the thread's chain (generated code's frames,
 *   the same slots, so the collector gains no root kind): a young
 *   collection relocates each slot (an interior address keeps its
 *   offset; a value that is no heap address is left alone), a
 *   defragmenting full collection rewrites it. Slots are registered in order (0, 1, ...), before the
 *   first call that may collect; at most one scope per C block (open a
 *   nested block for a second). It is thread-local, takes no lock and
 *   allocates nothing. The same in a shared program since Stage 28
 *   W17a: other threads collect only while this one is stopped at a
 *   safepoint or native (gc-design.md section 4.4), so a scope no longer
 *   holds a no-GC region there (until W17a it did, D191, and took the
 *   world lock twice; O111).
 *
 *   RC2. No raw derived pointer across a collecting call. An interior
 *   pointer (s + start) or a buffer's data pointer (h->keys, a->buf,
 *   b->data) is recomputed from its handle after every call that may
 *   collect; a function that needs one across such a call roots the
 *   derived pointer itself (a slot may hold an interior address). Fresh
 *   objects the function allocated and has not published yet are
 *   handles too (hash grow's new buffers; split's array being filled).
 *
 *   RC3. Barriers (D187). Every store of a reference into a heap object
 *   is followed by avant_barrier on that object (or avant_barrier_at on
 *   the stored-to address, or avant_barrier_slot with the slot) before
 *   the next call that may collect (C34: avant_hash_inc_slice and
 *   array_grow lost young keys and elements without one). A store into
 *   an element buffer that knows its slot uses avant_barrier_slot (card
 *   marking, Stage 28 W16t); a bulk copy or fill barriers the buffer
 *   with avant_barrier, which counts every card.
 *
 *   RC4. No-GC regions, only where C cannot root. Where references live
 *   inside a C library's own data structures while it calls back or
 *   allocates (none in the runtime since Stage 28 W17a, whose handshake
 *   retired the HTTP/WS sends' regions; see the checklist), the
 *   function brackets the region with avant_gc_nogc_begin/end (regions
 *   nest; one process-wide depth, under the world lock). Allocation
 *   inside is allowed and counted against the young budget and the heap
 *   limit (C32: past the limit the full collection runs after the
 *   region, unless the region alone exceeds it); the policy's
 *   collection request is not lost: the budget stays spent, so the
 *   first collection point after the region runs it. A collection
 *   requested explicitly inside (avant_gc_collect) is deferred to that
 *   point too, and under AVANT_GC_VERIFY=1 any collection attempted
 *   inside dies naming the region's function ("collection inside a
 *   no-GC region (...)"). avant_gc_defer_enter/leave, the ABI names
 *   user --cc C may call, are the same region; generated code never
 *   emits either (mir.md section 5.8).
 *
 *   A function that allocates but holds no reference across the
 *   allocation (its inputs are C memory, or it reads nothing after it)
 *   needs no scope; tools/gc-lint.sh asks such a function for a comment
 *   "gc-lint: holds nothing" naming why.
 *
 *   How it is checked (gc-design.md section 4.2): AVANT_GC_STRESS=1 runs
 *   a young collection before every runtime C allocation that moves
 *   every young object S+ can move and fills freed memory with 0xdb
 *   (the stress mode, avant_gc_collect.c), so a function that breaks
 *   RC1 or RC2 reads poison or an old copy at once; AVANT_GC_VERIFY=1's
 *   remembered-set check catches RC3; tools/gc-lint.sh fails the gate
 *   when an allocating call stands in a function with neither a scope,
 *   a region nor the comment.
 *
 *   The checklist (gc-audit.md section 13's table, moved here; Stage 28
 *   W16a converted every row; "C" converted to RC1-RC3, "N" nothing
 *   held, "R4" a no-GC region kept, with its reason):
 *     row (unit)                                   verdict
 *     avant_str_concat, _cat_i32, _from_i32_bytes  C (operands, array)
 *     dup_bytes_at, slice, strip (avant_str.c)     C (text + offset, RC2)
 *     avant_str_split, _fields, chars_build (C35)  C (array filled in
 *                                                  place, str_array_new)
 *     avant_str_repeat_byte, avant_hash_new        N (integer inputs)
 *     array push_slot/_ptr/_i32, reserve,
 *       array_grow (W16a item 2)                   C (scope on growth)
 *     avant_array_copy                             C
 *     buf_append_slow, buf_grow (item 2)           C
 *     avant_buf_new, _to_str, _ensure,
 *       avant_bytes_to_buf                         C
 *     hash grow, setters, inc_slice (item 2)       C
 *     inc_str, inc_i32k                            C
 *     keys/vals (hash_collect; was items[])        C (array first, RC2)
 *     avant_json_parse (audit 12 shape 2)          C (yyjson allocates
 *                                                  from its own pool)
 *     avant_json_f64s, copy_str                    C
 *     base64 encode/decode, zlib                   C
 *     sha256 (avant_codec.c)                       N (digest in C)
 *     avant_argv, avant_dir_list                   C (array filled in
 *                                                  place)
 *     io dup_bytes, avant_llvm.c avant_string      N (C memory)
 *     avant_spawn (avant_task.c; C28, R1)          C (the record is a
 *                                                  task root, added
 *                                                  before the first
 *                                                  collecting call, read
 *                                                  from the root; W16b,
 *                                                  W17a)
 *     avant_join, fs_copy (W17a: a native wait)    C (scope)
 *     HTTP make_string/_bytes, packed headers      C
 *     HTTP/WS send, close, respond                 N (no safepoint
 *                                                  inside: no thread
 *                                                  collects meanwhile;
 *                                                  their regions went
 *                                                  in W17a)
 *     HTTP client fetch                            the request pinned
 *                                                  for its native loop
 *     HTTP server state in srv (C memory) across
 *       dispatch                                   pinned for the serve
 *                                                  (W16a: with one loop
 *                                                  the program may stay
 *                                                  unshared and S+
 *                                                  could move it)
 *     g_last_key (avant_hash.c), g_json_pool
 *       (avant_json.c, W16a)                       runtime roots
 *                                                  (avant_gc_add_root)
 *
 *   One young budget (gc-design.md section 5.1): avant_nursery_held
 *   counts every young byte, the nursery's and the young large
 *   objects', helper allocations included; a collection is due when it
 *   reaches avant_young_bytes.
 *
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
 *   (D220) also barrier the buffer (since Stage 28 W15a1 every large
 *   object is born young, except a mature-allocating thread's, and an
 *   edge to a young large object is remembered like any young edge).
 *   AVANT_GC_VERIFY checks the generational invariant in this wording
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
 *                          generation's budget in MiB, fixed; 0 is the
 *                          adaptive budget (4 MiB to start, within 4 to
 *                          64 MiB; gc-design.md section 5.2)
 *   AVANT_HEAP_MAX_MB      number, 0..2^30, default 0: the heap limit in
 *                          MiB ("heap limit" past it); 0 is no limit
 *   AVANT_HEAP_RESERVE_GB  number, 1..2^20, default 64: the heap's
 *                          address reservation in GiB, halved on refusal
 *                          down to 1 (W14a1)
 *   AVANT_NURSERY          switch, default 1: the young generation; 0
 *                          is full, non-moving collections only
 *   AVANT_GC_DEFRAG        switch, default 1: mature evacuation in full
 *                          collections
 *   AVANT_GC_VERIFY        switch, default 0: the collector's checkers
 *                          (avant_gc_verify.c)
 *   AVANT_GC_STATS         switch, default 0: one statistics line on
 *                          stderr at exit
 *   AVANT_GC_STRESS        switch, default 0: the full stress mode
 *                          (testing; W14b1, full since Stage 28 W16a):
 *                          a young collection before every runtime C
 *                          allocation (unshared programs, outside no-GC
 *                          regions); every young collection evacuates
 *                          every young block nothing names imprecisely
 *                          (mixed blocks too), and freed memory (free
 *                          young lines, evacuated blocks, freed large
 *                          runs) is filled with 0xdb
 *
 * Retired (W14b2; setting one is reported and has no effect):
 * AVANT_NURSERY_DISCARD, AVANT_GC_REM_FRONTIER, AVANT_GC_YOUNG_SIZING,
 * AVANT_GC_COPY_RESERVE; and AVANT_GC_STACK_REWRITE (Stage 28 W16b, with
 * the conservative C-stack scans; it had no effect since W15d). Removed earlier: AVANT_NURSERY_STACK (W14a1),
 * AVANT_NURSERY_STACKMAP (W12r).
 */

void *avant_alloc(uint64_t nbytes, uint32_t type_id);
void avant_alloc_in_mature(int32_t on);
void avant_barrier(void *obj);
void avant_barrier_slot(void *obj, void *slot);
void avant_gc_defer_enter(void);
void avant_gc_defer_leave(void);
/* Runtime-internal (Stage 28 W14b2, O37): avant_alloc_c below is an
 * entry of avant_rt.o that only the runtime's other objects call, as are
 * avant_gc_nogc_begin/end and avant_gc_top (below). avant_rt.h does not
 * declare them, and their visibility is hidden: no linked image exports
 * them (the second section of avant_rt.exports). avant_gc_quiet_defer_enter
 * went in Stage 28 W16a with the regions it opened. */
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
extern atomic_int avant_gc_requested;
void avant_gc_poll(void);
void avant_gc_native_begin(void);
void avant_gc_native_end(void);
/* Runtime-internal (Stage 28 W17a): a blocking lock or condition wait at
 * a collecting call (rule 2 of gc-design.md section 4.4): an uncontended
 * lock is taken at once; a contended one, and every wait, is native, so
 * a collection does not wait for this thread, and the thread returns to
 * running only when no collection is in progress (the mutex is held
 * again on return). A collection may run meanwhile: the caller holds
 * every heap reference it needs after the call in a handle scope (RC1)
 * and reads no heap object while waiting. A wait inside a no-GC region
 * stays running. timedwait returns pthread_cond_timedwait's result. */
__attribute__((visibility("hidden"))) void avant_gc_lock(pthread_mutex_t *m);
__attribute__((visibility("hidden"))) void avant_gc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m);
__attribute__((visibility("hidden"))) int avant_gc_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *m,
                                                                 const struct timespec *deadline);
/* avant_str_generation() (avant_rt.h) reads g_avant_str_gen, which every
 * collection bumps. */
extern _Atomic uint32_t g_avant_str_gen;
extern atomic_int g_avant_shared;

/* RC4's no-GC regions (the contract above; Stage 28 W16a). Hidden: only
 * the runtime's other objects call them; user --cc C has the ABI names
 * avant_gc_defer_enter/leave, which open and close the same region. */
__attribute__((visibility("hidden"))) void avant_gc_nogc_begin(void);
__attribute__((visibility("hidden"))) void avant_gc_nogc_end(void);
/* A helper about to replace a young large object it allocated before
 * (avant_json_parse's pool) runs the due young collection first when the
 * new one alone fills the young budget (avant_gc_alloc.c; Stage 28 W16a).
 * It may collect. Hidden. */
__attribute__((visibility("hidden"))) void avant_gc_young_before(uint64_t bytes);

/* Task roots (Stage 28 W16b; C28, R1): a spawned task's capture record
 * is rooted by the task, from avant_spawn until the task's function has
 * returned, instead of pinned by the generated code until the join. A
 * root is a node in the task's own C memory (O(1) to add and drop, no
 * pin entry); every collection traces OBJ from it and rewrites the root
 * when the record moves (Stage 28 W17a; until then the record was held in
 * place while rooted, because the worker read it while other threads
 * collected, D191); the worker reads the record from the root. After the
 * drop the record is an ordinary object: a finished task's record dies
 * with its last reference, joined or not. Hidden; add and drop take the
 * world lock and never collect. */
typedef struct AvantGcTaskRoot {
  void *obj;
  struct AvantGcTaskRoot *prev;
  struct AvantGcTaskRoot *next;
} AvantGcTaskRoot;
__attribute__((visibility("hidden"))) void avant_gc_task_root_add(AvantGcTaskRoot *r, void *obj);
__attribute__((visibility("hidden"))) void avant_gc_task_root_drop(AvantGcTaskRoot *r);

/* RC1's handle scopes (the contract above; Stage 28 W16a). A frame of the
 * thread's chain, the type every frame has (avant_gc_internal.h calls it
 * Frame): generated code's frames come from avant_gc_enter's free list,
 * a scope's lives in the C function's own stack frame. scope is 0 for a
 * generated-code frame, AVANT_GC_SCOPE_OPEN for an open scope (the
 * NOGC bit, a scope's no-GC region in a shared program, went in W17a).
 * avant_gc_top is the thread's innermost frame (hidden: no linked image
 * exports it; user C opens frames with avant_gc_enter and
 * avant_gc_root_begin, the ABI generated code uses). */
typedef struct AvantGcFrame {
  struct AvantGcFrame *prev;
  void ***slots;
  uint32_t n;
  uint32_t cap;
  uint32_t scope;
} AvantGcFrame;
#define AVANT_GC_SCOPE_OPEN 1u
extern __thread __attribute__((visibility("hidden"))) AvantGcFrame *avant_gc_top;

static inline void avant_gc_scope_open_(AvantGcFrame *f, void ***slots, uint32_t cap) {
  f->prev = avant_gc_top;
  f->slots = slots;
  f->n = 0;
  f->cap = cap;
  f->scope = AVANT_GC_SCOPE_OPEN;
  avant_gc_top = f;
}

static inline void avant_gc_scope_close_(AvantGcFrame *f) {
  if (f->scope) {
    avant_gc_top = f->prev;
    f->scope = 0;
  }
}

/* AVANT_GC_SCOPE(n) declares the scope gc_scope_ with n slots and opens
 * it; the scope closes when the declaring C block ends (cleanup) or at
 * AVANT_GC_UNSCOPE(), whichever comes first. AVANT_GC_ROOT(i, &x)
 * registers the local x as slot i (in order: 0, 1, ...). */
#define AVANT_GC_SCOPE(N)                                                           \
  void **gc_scope_slots_[(N)];                                                      \
  AvantGcFrame gc_scope_ __attribute__((cleanup(avant_gc_scope_close_)));           \
  avant_gc_scope_open_(&gc_scope_, gc_scope_slots_, (N))
#define AVANT_GC_ROOT(I, P)                                                         \
  do {                                                                              \
    gc_scope_slots_[(I)] = (void **)(P);                                            \
    gc_scope_.n = (uint32_t)(I) + 1u;                                               \
  } while (0)
#define AVANT_GC_UNSCOPE() avant_gc_scope_close_(&gc_scope_)

/* Runtime C allocates through avant_alloc_c (Stage 28 W14b1): the same
 * allocation as avant_alloc, plus the stress point of AVANT_GC_STRESS=1
 * (a young collection before it, outside defer regions, in unshared
 * programs). Every runtime unit that includes this header calls it under
 * the name avant_alloc; generated code and user C (avant_rt.h) keep
 * calling avant_alloc itself. */
__attribute__((visibility("hidden"))) void *avant_alloc_c(uint64_t nbytes, uint32_t type_id);
#define avant_alloc(nbytes, type_id) avant_alloc_c((nbytes), (type_id))

#endif
