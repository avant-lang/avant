#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Conservative scans of the collecting thread's C stack (Stage 28 W12;
 * moved from avant_rt.c and the stackmap walker's file, which W12r
 * removed). The collector's roots are precise (root slots, pins, TLS
 * slots, the remembered set); these scans cover what runtime C frames and register saves may still hold:
 *   - gc_pin_c_stack at a young collection runtime C entered (W15a2,
 *     W15c): the young blocks a stack word names are not evacuated (S+);
 *   - gc_note_c_stack_blocks before a defragmenting full collection (W11c,
 *     C31, D213): mature blocks a stack word names are not moved.
 * E's young-copy scans (gc_rewrite_c_stack, which rewrote every exact
 * forwarded start left on the stack, W8c, W11, C37; and
 * gc_count_c_stack_nursery, the sm_left statistic) went with E in Stage 28
 * W15d: no collection rewrites a C stack word.
 * gc_collect() spills its callers' callee-saved registers first, so a
 * reference a caller keeps in one is on the scanned stack. Other threads'
 * C frames are not scanned (D191, Part 2). */

/* C31: defragmentation moves mature objects and rewrites only the
 * precise references to them (root slots, pins, runtime TLS slots, the
 * remembered set, heap fields). A runtime C frame on the collecting
 * thread can hold a reference across a full collection that the
 * collector cannot rewrite: before Stage 28 W16a a helper held its
 * arguments in C across its allocations (avant_str_concat's operands:
 * the caller rooted them, so they were marked, but the helper's own
 * copies were C values). Since W16a runtime C keeps them in handle scopes
 * (RC1, avant_gc.h), which defragmentation rewrites; this exclusion stays
 * as the safety net until gc-design.md section 4.3's proof (W16b). So
 * a defragmenting collection first notes every mature block that a word
 * of the collecting thread's stack names (exact start, interior, or a
 * scalar that happens to look like one; a false positive only skips a
 * candidate), from this function's frame to the stack top, and those
 * blocks (gc_heap.cstack_blocks) are not candidates. gc_collect() first
 * saves its callers' callee-saved registers into its own frame, so a
 * reference a caller keeps in one is on the scanned stack. Other
 * threads' C frames are D191's (Part 2). */
__attribute__((noinline)) void gc_note_c_stack_blocks(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = gc_thread.reg.stack_hi;
  uint8_t *lo = gc_thread.reg.stack_lo;
  gc_heap.cstack_blocks.n = 0;
  if (!hi || !p || p >= hi || (lo && p < lo)) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= hi; p += ALIGN) {
    uint8_t *w = *(uint8_t **)p;
    /* The word's block, and its header's: a payload pointer just past
     * an object that ends a block lies in the next one. */
    Block *b = block_of(w);
    Block *hb = w ? block_of(w - sizeof(AvantHeader)) : NULL;
    if (b && b->space == SPACE_MATURE) {
      gc_block_set_add(&gc_heap.cstack_blocks, b);
    }
    if (hb && hb != b && hb->space == SPACE_MATURE) {
      gc_block_set_add(&gc_heap.cstack_blocks, hb);
    }
  }
  gc_block_set_seal(&gc_heap.cstack_blocks);
}

/* S+'s pin scan (candidate S, GC_STICKY_COPY 0, moves nothing, so it
 * neither pins nor rewrites C stack words). */
#if GC_STICKY_COPY

/* Mostly-copying at the collections runtime C enters (Stage 28 W15a2,
 * gc-design.md §3.2, §4.3's first half; C37's class removed there; S+
 * since W15c): every 8-aligned word of the collecting thread's stack,
 * from this function's frame to the stack top, that names a young block
 * object (its exact start or an interior address, resolved through the
 * start bitmap) keeps that object's block in place for this collection
 * (SPLUS_KEPT: not evacuated; its survivors are promoted in place). No
 * word is changed, so an integer that happens to equal an object's
 * address only keeps the object alive (C37 was the rewrite replacing such
 * an integer). Words that are registered root
 * slots of this thread's frames are skipped: those are precise, and the
 * young copy relocates them. Called before any root is traced.
 * gc_collect() first saves its callers' callee-saved registers into its
 * own frame, so a reference a runtime C caller keeps in one is on the
 * scanned stack. Other threads' C frames are not scanned (D191, W17). */
__attribute__((noinline)) void gc_pin_c_stack(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = gc_thread.reg.stack_hi;
  uint8_t *lo = gc_thread.reg.stack_lo;
  size_t nwords;
  size_t nmap;
  size_t k;
  Frame *f;
  if (!hi || !p || p >= hi || (lo && p < lo)) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  nwords = (size_t)(hi - p) / ALIGN;
  nmap = (nwords + 63) / 64;
  if (nmap > gc_heap.pin_slots_cap) {
    gc_heap.pin_slots = gc_xrealloc(gc_heap.pin_slots, nmap * sizeof(uint64_t));
    gc_heap.pin_slots_cap = nmap;
  }
  memset(gc_heap.pin_slots, 0, nmap * sizeof(uint64_t));
  for (f = gc_thread.reg.head ? *gc_thread.reg.head : NULL; f; f = f->prev) {
    uint32_t i;
    for (i = 0; i < f->n; i++) {
      uint8_t *a = (uint8_t *)f->slots[i];
      if (a >= p && a < hi && ((uintptr_t)a & (ALIGN - 1)) == 0) {
        size_t w = (size_t)(a - p) / ALIGN;
        gc_heap.pin_slots[w / 64] |= 1ULL << (w % 64);
      }
    }
  }
  for (k = 0; k < nwords; k++) {
    void *w;
    Block *b = NULL;
    AvantHeader *ol = NULL;
    void *start;
    if ((gc_heap.pin_slots[k / 64] >> (k % 64)) & 1ULL) {
      continue;
    }
    w = ((void **)p)[k];
    start = gc_object_payload(w, &b, &ol);
    /* The young block object stays in place, and so does every survivor
     * of its block (the block is not evacuated). */
    if (start && b && (avant_header(start)->flags & AVANT_FLAG_YOUNG)) {
      b->kept = SPLUS_KEPT;
    }
  }
}

#endif /* GC_STICKY_COPY */
