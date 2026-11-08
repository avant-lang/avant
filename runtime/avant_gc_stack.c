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
 *   - gc_rewrite_c_stack after a young copy (W8c's and W11's rule): every
 *     exact forwarded start left on the stack is rewritten to the copy;
 *   - gc_count_c_stack_nursery: counts words still naming from-space (the
 *     sm_left statistic, checked by AVANT_GC_VERIFY);
 *   - gc_note_c_stack_blocks before a defragmenting full collection (W11c,
 *     C31, D213): mature blocks a stack word names are not moved.
 * gc_collect() spills its callers' callee-saved registers first, so a
 * reference a caller keeps in one is on the scanned stack. Other threads'
 * C frames are not scanned (D191, Part 2). */

/* C31: defragmentation moves mature objects and rewrites only the
 * precise references to them (root slots, pins, runtime TLS slots, the
 * remembered set, heap fields). A runtime C frame on the collecting
 * thread can hold a reference across a full collection that the
 * collector cannot rewrite: with AVANT_NURSERY=0 the outermost
 * avant_gc_defer_enter collects while the helper holds its arguments
 * (avant_str_concat's operands: the caller rooted them, so they are
 * marked, but the helper's own copies are C values), and a helper that
 * calls avant_alloc outside defer holds its arguments across the full
 * collection gc_maybe_collect may run. Such an object must not move. So
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

/* Candidate E, and S's variant S+ for the pin scan, from here (Stage 28
 * W15b, W15c): S's young collections move nothing, so they neither pin
 * nor rewrite C stack words; S+ copies and pins, never rewrites. */
#if GC_CANDIDATE == GC_CANDIDATE_E || GC_STICKY_COPY

/* Mostly-copying at the collections runtime C enters (Stage 28 W15a2,
 * gc-design.md §3.2, §4.3's first half; C37's class removed there): every
 * 8-aligned word of the collecting thread's stack, from this function's
 * frame to the stack top, that names a nursery object (its exact start or
 * an interior address, resolved through the start bitmap) keeps that
 * object in place for this collection and promotes it in place
 * (gc_keep_conservative). No word is changed, so an integer that happens
 * to equal an object's address only keeps the object alive (C37 was the
 * rewrite replacing such an integer). Words that are registered root
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
#if GC_CANDIDATE == GC_CANDIDATE_E
    if (start && b && b->space == SPACE_NURSERY) {
      gc_keep_conservative(start);
    }
#else
    /* S+: the young block object stays in place, and so does every
     * survivor of its block (the block is not evacuated). */
    if (start && b && (avant_header(start)->flags & AVANT_FLAG_YOUNG)) {
      b->kept = SPLUS_KEPT;
    }
#endif
  }
}

#endif /* GC_CANDIDATE == GC_CANDIDATE_E || GC_STICKY_COPY */
#if GC_CANDIDATE == GC_CANDIDATE_E

static void gc_rewrite_if_forwarded(void **slot) {
  void *to = gc_forwarded(*slot);
  if (to) {
    *slot = to;
  }
}

/*
 * Copies of a forwarded pointer survive a young copy outside the
 * precise roots: callee-saved register saves in C frames on the call
 * chain into the collector, and runtime C locals. Rewrite every exact
 * forwarded start left on this thread's stack, with the same predicate
 * gc_count_c_stack_nursery verifies, so no stack word still names
 * from-space after a young copy. Interiors are not touched (only an
 * exact start's header holds the forward, gc_forwarded), and a word that was not forwarded keeps its
 * bits. AVANT_GC_STACK_REWRITE=0 is the kill switch (gc_heap.set.c_stack_rewrite).
 */
void gc_rewrite_c_stack(void) {
  uint8_t *p;
  uint8_t *hi;
  uint8_t *lo;
  if (!gc_heap.set.c_stack_rewrite) {
    return;
  }
  p = (uint8_t *)__builtin_frame_address(0);
  lo = gc_thread.reg.stack_lo;
  hi = gc_thread.reg.stack_hi;
  if (!hi || !p) {
    return;
  }
  if (lo && p < (uint8_t *)lo) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= (uint8_t *)hi; p += ALIGN) {
    void *w = *(void **)p;
    Block *b = NULL;
    if (!gc_is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      gc_rewrite_if_forwarded((void **)p);
    }
  }
}

void gc_count_c_stack_nursery(void) {
  uint8_t *p;
  uint8_t *hi;
  uint8_t *lo;
  if (!gc_heap.set.c_stack_pass) {
    return;
  }
  p = (uint8_t *)__builtin_frame_address(0);
  lo = gc_thread.reg.stack_lo;
  hi = gc_thread.reg.stack_hi;
  if (!hi || !p) {
    return;
  }
  if (lo && p < lo) {
    return;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= hi; p += ALIGN) {
    void *w = *(void **)p;
    Block *b = NULL;
    if (!gc_is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      /* Copied to to-space, but this stack word still names from-space. */
      if (gc_forwarded(w)) {
        gc_heap.ctr.stack_left++;
      }
    }
  }
}

#endif /* GC_CANDIDATE == GC_CANDIDATE_E */
