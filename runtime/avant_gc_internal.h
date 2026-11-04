#ifndef AVANT_GC_INTERNAL_H
#define AVANT_GC_INTERNAL_H

#include "avant_rt.h"

#include <stddef.h>
#include <stdint.h>

/* Private surface shared by the collector translation units
 * (avant_rt.c, avant_gc_stackmap.c). Not a public header: user
 * programs and other runtime units see avant_rt.h only. The units
 * are folded into one avant_rt.o by runtime/build.sh, so the seed's
 * link table does not change when a unit is split out. */

#define ALIGN 8
#define LINE_SIZE 256
#define LINES_PER_BLOCK 128
#define BLOCK_SIZE (LINE_SIZE * LINES_PER_BLOCK)
#define START_WORDS 64
#define BLOCK_HASH 4096
#define LARGE_HASH 4096
#define LOS_LIMIT 8192
#define SCALAR_LOS_MIN 6144
#define YOUNG_BYTES_DEFAULT (16u * 1024u * 1024u)
#define FULL_BYTES (32u * 1024u * 1024u)
#define MAX_TYPES 1024
#define CACHE_BLOCKS 2

#define SPACE_MATURE 0
#define SPACE_NURSERY 1
#define SPACE_FROM 2
#define SPACE_TO 3

typedef struct Block {
  struct Block *hash_next;
  struct Block *next;
  uint8_t *base;
  uint8_t space;
  uint16_t search;
  uint8_t *bump;
  uint8_t *limit;
  uint8_t line_mark[LINES_PER_BLOCK];
  uint64_t start[START_WORDS];
} Block;

typedef struct Large {
  struct Large *next;
  struct Large *hash_next;
  AvantHeader header;
} Large;

typedef struct Frame {
  struct Frame *prev;
  void ***slots;
  uint32_t n;
  uint32_t cap;
} Frame;

typedef struct ThreadReg {
  struct ThreadReg *next;
  Frame **head;
  void *stack_lo;
  void *stack_hi;
} ThreadReg;

extern __thread ThreadReg g_self_reg;
extern int g_saw_pinned_nursery;
extern int g_stack_scan;
extern int g_stackmap_walk;
extern uint64_t g_live;
extern uint64_t g_sm_walks;
extern uint64_t g_sm_hits;
extern uint64_t g_sm_leftover;
extern uint32_t g_sm_nsites;

void work_push(void *p);
size_t obj_total(const AvantHeader *h);
int is_heap_payload(void *p, Block **ob, Large **ol);
void *fwd_get(void *from);
void fwd_put(void *from, void *to);
void *copy_nursery(void *p);
void mark_object_lines(void *p);
void copy_from_slot(void **slot);

void sm_init(void);
void copy_from_c_stack(void);
void copy_from_stackmaps(void);
void rewrite_from_stackmaps(void);
void rewrite_c_stack(void);
void count_c_stack_nursery(void);

#endif
