#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Conservative scans of the collecting thread's C stack (Stage 28 W12;
 * moved from avant_rt.c and avant_gc_stackmap.c). The collector's roots are
 * precise (root slots, pins, TLS slots, the remembered set); these scans
 * cover what runtime C frames and register saves may still hold:
 *   - copy_from_c_stack (AVANT_NURSERY_STACK=1 only, a diagnostic): words
 *     that name nursery objects are rewritten or pinned in place;
 *   - rewrite_c_stack after a young copy (W8c's and W11's rule): every
 *     exact forwarded start left on the stack is rewritten to the copy;
 *   - count_c_stack_nursery: counts words still naming from-space (the
 *     sm_left statistic, checked by AVANT_GC_VERIFY);
 *   - gc_note_c_stack_blocks before a defragmenting full collection (W11c,
 *     C31, D213): mature blocks a stack word names are not moved.
 * gc_collect() spills its callers' callee-saved registers first, so a
 * reference a caller keeps in one is on the scanned stack. Other threads'
 * C frames are not scanned (D191, Part 2). */

int g_stack_scan;

/* C31: defragmentation moves mature objects and rewrites only the
 * precise references to them (root slots, pins, runtime TLS slots, the
 * remembered set, heap fields). A runtime C frame on the collecting
 * thread can hold a reference across a full collection that the
 * collector cannot rewrite: with AVANT_NURSERY=0 the outermost
 * avant_gc_defer_enter collects while the helper holds its arguments
 * (avant_str_concat's operands: the caller rooted them, so they are
 * marked, but the helper's own copies are C values), and a helper that
 * calls avant_alloc outside defer holds its arguments across the full
 * collection maybe_collect may run. Such an object must not move. So a
 * defragmenting collection first notes every mature block that a word
 * of the collecting thread's stack names (exact start, interior, or a
 * scalar that happens to look like one; a false positive only skips a
 * candidate), from this function's frame to the stack top, and those
 * blocks are not candidates. gc_collect() first saves its callers'
 * callee-saved registers into its own frame, so a reference a caller
 * keeps in one is on the scanned stack. Other threads' C frames are
 * D191's (Part 2). */
BlockSet g_cstack_blocks;

__attribute__((noinline)) void gc_note_c_stack_blocks(void) {
  uint8_t *p = (uint8_t *)__builtin_frame_address(0);
  uint8_t *hi = g_self_reg.stack_hi;
  uint8_t *lo = g_self_reg.stack_lo;
  g_cstack_blocks.n = 0;
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
      gc_block_set_add(&g_cstack_blocks, b);
    }
    if (hb && hb != b && hb->space == SPACE_MATURE) {
      gc_block_set_add(&g_cstack_blocks, hb);
    }
  }
  gc_block_set_seal(&g_cstack_blocks);
}

/*
 * LLVM spill slots hold heap pointers that avant_gc_root never sees.
 * After precise copy, from-space object-starts on the live C stack are
 * rewritten to to-space when the object was already forwarded. Words
 * that only appear here are pinned in place (D34-shaped) so we do not
 * treat a packed i32 pair as a root that gets copied. Do not walk
 * interiors: that smashed compile-compiler.
 */
static void pin_nursery_in_place(void *p) {
  if (fwd_get(p)) {
    return;
  }
  AvantHeader *h = avant_header(p);
  if (h->flags & AVANT_FLAG_PINNED) {
    copy_nursery(p);
    return;
  }
  fwd_put(p, p);
  g_saw_pinned_nursery = 1;
  if (!(h->flags & AVANT_FLAG_MARKED)) {
    h->flags |= AVANT_FLAG_MARKED;
    mark_object_lines(p);
    g_live += obj_total(h);
    work_push(p);
  }
}

void copy_from_c_stack(void) {
  uint8_t *sp;
  uint8_t *hi;
  uint8_t *p;
  uint8_t *lo;
  if (!g_stack_scan) {
    return;
  }
  sp = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
  if (!hi || !sp || sp >= (uint8_t *)hi) {
    return;
  }
  if (lo && sp < (uint8_t *)lo) {
    return;
  }
  p = (uint8_t *)((uintptr_t)sp & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= (uint8_t *)hi; p += ALIGN) {
    void *w = *(void **)p;
    Block *b = NULL;
    if (!is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      void *to = fwd_get(w);
      if (to) {
        *(void **)p = to;
      } else {
        pin_nursery_in_place(w);
      }
    }
  }
}

void gc_rewrite_if_forwarded(void **slot) {
  void *w = *slot;
  void *to = fwd_get(w);
  if (to && to != w) {
    *slot = to;
  }
}

/*
 * The frame walk above rewrites only frames with a stackmap site.
 * Copies of a forwarded pointer also survive outside those frames:
 * callee-saved register saves in C frames on the call chain into the
 * collector, and spills of struct-typed temps the statepoint did not
 * name. Rewrite every exact forwarded start left on this thread's
 * stack, with the same predicate count_c_stack_nursery verifies, so
 * no stack word still names from-space after a young copy. Interiors
 * are not touched (fwd_get keys on object starts), and a word that
 * was not forwarded keeps its bits. AVANT_GC_STACK_REWRITE=0 is the
 * kill switch.
 */
void rewrite_c_stack(void) {
  static int enabled = -1;
  uint8_t *p;
  uint8_t *hi;
  uint8_t *lo;
  if (!g_stackmap_walk) {
    return;
  }
  if (enabled < 0) {
    const char *v = getenv("AVANT_GC_STACK_REWRITE");
    enabled = !(v && v[0] == '0' && v[1] == 0);
  }
  if (!enabled) {
    return;
  }
  p = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
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
    if (!is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      gc_rewrite_if_forwarded((void **)p);
    }
  }
}

void count_c_stack_nursery(void) {
  uint8_t *p;
  uint8_t *hi;
  uint8_t *lo;
  if (!g_stackmap_walk) {
    return;
  }
  p = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
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
    if (!is_heap_payload(w, &b, NULL)) {
      continue;
    }
    if (b && b->space == SPACE_NURSERY) {
      void *to = fwd_get(w);
      /* Copied to to-space, but this stack word still names from-space. */
      if (to && to != w) {
        g_sm_leftover++;
      }
    }
  }
}
