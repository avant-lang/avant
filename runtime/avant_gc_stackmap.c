#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_gc_internal.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The LLVM stackmap walker (TO BE REMOVED after the re-seed; Stage 28 W12,
 * D212, O12). Mapped spill slots in frames of code that carries
 * .llvm_stackmaps are precise roots of a young copy, and leftover
 * forwarded words in those frames are rewritten after it. Only binaries
 * the myc back end built carry the section: today that is B, the
 * compiler the myc seed builds. The new back end emits no statepoints, so
 * once the seed is a myc-free release nothing carries the section, every
 * walk finds no site, and this file, g_stackmap_walk's role as the gate of
 * the conservative rewrite in avant_gc_stack.c, and the sm_* statistics
 * go (O12). Internals stay static here. */

int g_stackmap_walk;

extern char __LLVM_StackMaps[] __attribute__((weak));

#define SM_DIRECT 2
#define SM_INDIRECT 3
#define DWARF_X64_RBP 6
#define DWARF_X64_RSP 7

typedef struct SmSite {
  uintptr_t site;
  const uint8_t *rec;
  uint32_t fsize;
} SmSite;

static const uint8_t *g_sm_sec;
static uintptr_t g_load_bias;
static SmSite *g_sm_sites;

static uint16_t sm_u16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

static uint32_t sm_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint64_t sm_u64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

static int32_t sm_i32(const uint8_t *p) {
  int32_t v;
  memcpy(&v, p, 4);
  return v;
}

static unsigned sm_record_size(const uint8_t *rec) {
  uint16_t nloc = sm_u16(rec + 14);
  unsigned loc_end = ((16u + 12u * nloc) + 7u) & ~7u;
  unsigned nlo_off = loc_end + 2u;
  uint16_t nlive = sm_u16(rec + nlo_off);
  unsigned rec_end = nlo_off + 2u + 4u * nlive;
  return (rec_end + 7u) & ~7u;
}

static void sm_apply_record(const uint8_t *rec, void *rbp, void *rsp) {
  uint16_t nloc = sm_u16(rec + 14);
  const uint8_t *loc = rec + 16;
  uint16_t i;
  for (i = 0; i < nloc; i++, loc += 12) {
    uint8_t kind = loc[0];
    uint16_t size = sm_u16(loc + 2);
    uint16_t reg = sm_u16(loc + 4);
    int32_t off = sm_i32(loc + 8);
    uint8_t *base;
    if (size != sizeof(void *)) {
      continue;
    }
    if (kind != SM_INDIRECT && kind != SM_DIRECT) {
      continue;
    }
    if (reg == DWARF_X64_RBP) {
      base = (uint8_t *)rbp;
    } else if (reg == DWARF_X64_RSP && rsp) {
      base = (uint8_t *)rsp;
    } else {
      continue;
    }
    copy_from_slot((void **)(base + off));
  }
}

static int sm_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
  int *seen = (int *)data;
  (void)size;
  if (*seen) {
    return 1;
  }
  g_load_bias = (uintptr_t)info->dlpi_addr;
  *seen = 1;
  return 1;
}

/*
 * LLVM emits __LLVM_StackMaps as a local section label. The runtime's
 * weak extern stays UND in dynsym, so the pointer is always NULL.
 * Find .llvm_stackmaps via ELF section headers of /proc/self/exe.
 */
static const uint8_t *sm_section_from_exe(void) {
  int fd;
  Elf64_Ehdr eh;
  Elf64_Shdr *sh;
  char *names;
  const uint8_t *sec = NULL;
  size_t bytes;
  uint32_t i;
  uint32_t strndx;

  fd = open("/proc/self/exe", O_RDONLY);
  if (fd < 0) {
    return NULL;
  }
  if (read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh) ||
      memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_shentsize != sizeof(Elf64_Shdr) ||
      eh.e_shoff == 0 || eh.e_shnum == 0) {
    close(fd);
    return NULL;
  }
  bytes = (size_t)eh.e_shnum * sizeof(Elf64_Shdr);
  sh = malloc(bytes);
  if (!sh) {
    close(fd);
    return NULL;
  }
  if (lseek(fd, (off_t)eh.e_shoff, SEEK_SET) != (off_t)eh.e_shoff ||
      (size_t)read(fd, sh, bytes) != bytes) {
    free(sh);
    close(fd);
    return NULL;
  }
  strndx = eh.e_shstrndx;
  if (strndx >= eh.e_shnum) {
    free(sh);
    close(fd);
    return NULL;
  }
  names = malloc(sh[strndx].sh_size + 1);
  if (!names) {
    free(sh);
    close(fd);
    return NULL;
  }
  if (lseek(fd, (off_t)sh[strndx].sh_offset, SEEK_SET) !=
          (off_t)sh[strndx].sh_offset ||
      (size_t)read(fd, names, sh[strndx].sh_size) != sh[strndx].sh_size) {
    free(names);
    free(sh);
    close(fd);
    return NULL;
  }
  names[sh[strndx].sh_size] = 0;
  for (i = 0; i < eh.e_shnum; i++) {
    uint32_t no = sh[i].sh_name;
    if (no >= sh[strndx].sh_size) {
      continue;
    }
    if (strcmp(names + no, ".llvm_stackmaps") != 0) {
      continue;
    }
    sec = (const uint8_t *)(g_load_bias + (uintptr_t)sh[i].sh_addr);
    break;
  }
  free(names);
  free(sh);
  close(fd);
  return sec;
}

static int sm_site_cmp(const void *a, const void *b) {
  const SmSite *x = (const SmSite *)a;
  const SmSite *y = (const SmSite *)b;
  if (x->site < y->site) {
    return -1;
  }
  if (x->site > y->site) {
    return 1;
  }
  return 0;
}

static void sm_index_records(void) {
  const uint8_t *sec = g_sm_sec;
  uint32_t nfn, nconst, nrec, fi, ri;
  const uint8_t *fnp;
  const uint8_t *rec;
  uint32_t cap;

  if (!sec || sec[0] != 3) {
    return;
  }
  nfn = sm_u32(sec + 4);
  nconst = sm_u32(sec + 8);
  nrec = sm_u32(sec + 12);
  if (nrec == 0 || nrec > 10000000u) {
    return;
  }
  fnp = sec + 16;
  rec = fnp + (unsigned)nfn * 24u + (unsigned)nconst * 8u;
  cap = nrec;
  g_sm_sites = malloc((size_t)cap * sizeof(SmSite));
  if (!g_sm_sites) {
    return;
  }
  ri = 0;
  for (fi = 0; fi < nfn; fi++) {
    uint64_t faddr = sm_u64(fnp + (unsigned)fi * 24u);
    uint64_t fcount = sm_u64(fnp + (unsigned)fi * 24u + 16u);
    uint64_t k;
    uintptr_t fbase = (uintptr_t)faddr;
    /*
     * R_X86_64_RELATIVE in .llvm_stackmaps (TEXTREL) rewrites faddr to
     * the runtime VA. If the loader left the ELF vaddr, add the bias.
     */
    if (g_load_bias && fbase < g_load_bias) {
      fbase += g_load_bias;
    }
    uint64_t fsize = sm_u64(fnp + (unsigned)fi * 24u + 8u);
    uint32_t fsize32 = 0;
    if (fsize && fsize != UINT64_MAX && fsize <= (1u << 20)) {
      fsize32 = (uint32_t)fsize;
    }
    for (k = 0; k < fcount && ri < nrec; k++, ri++) {
      uint32_t inst_off = sm_u32(rec + 8);
      /* LLVM records the return address (next insn after CALL). */
      g_sm_sites[g_sm_nsites].site = fbase + inst_off;
      g_sm_sites[g_sm_nsites].rec = rec;
      g_sm_sites[g_sm_nsites].fsize = fsize32;
      g_sm_nsites++;
      rec += sm_record_size(rec);
    }
  }
  if (g_sm_nsites) {
    qsort(g_sm_sites, g_sm_nsites, sizeof(SmSite), sm_site_cmp);
  }
}

void sm_init(void) {
  int seen = 0;
  if (g_sm_sec || !g_stackmap_walk) {
    return;
  }
  dl_iterate_phdr(sm_phdr_cb, &seen);
  if (__LLVM_StackMaps && (unsigned char)__LLVM_StackMaps[0] == 3) {
    g_sm_sec = (const uint8_t *)__LLVM_StackMaps;
  } else {
    g_sm_sec = sm_section_from_exe();
  }
  sm_index_records();
}

static SmSite *sm_find_site(void *ret) {
  uintptr_t retu = (uintptr_t)ret;
  uint32_t lo = 0;
  uint32_t hi;
  if (!g_sm_nsites) {
    return NULL;
  }
  hi = g_sm_nsites;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (g_sm_sites[mid].site < retu) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= g_sm_nsites || g_sm_sites[lo].site != retu) {
    return NULL;
  }
  return &g_sm_sites[lo];
}

static int sm_lookup_apply(void *ret, void *caller_rbp, void *callee_rbp) {
  SmSite *site;
  const uint8_t *rec;
  void *rsp;
  site = sm_find_site(ret);
  if (!site) {
    return 0;
  }
  rec = site->rec;
  /*
   * Callee prologue: push rbp; mov rsp, rbp. Caller's RSP at the CALL
   * (before pushing the return address, after any stack args) is
   * callee_rbp + 16. LLVM stack size is one number per function, so
   * rbp - fsize is wrong at callsites that push extra stack arguments.
   */
  rsp = callee_rbp ? (uint8_t *)callee_rbp + 16 : NULL;
  sm_apply_record(rec, caller_rbp, rsp);
  g_sm_hits++;
  return 1;
}

/*
 * After the work queue drains, every copied object is in the forwarding
 * table. Stackmap apply ran earlier as a *root* walk (copy_from_slot).
 * LLVM still leaves extra as(0) spills of the same pointer bits that
 * were never in gc-live. Rewrite those words in the matched myc frame
 * (LLVM stack size from the callsite RSP) so discard does not alias.
 * Exact fwd_get only: packed i32s that are not a copied payload stay.
 */
static void sm_rewrite_frame(void *rsp, uint32_t fsize) {
  uint8_t *p;
  uint8_t *end;
  uint8_t *hi;
  if (!rsp || fsize < sizeof(void *)) {
    return;
  }
  p = (uint8_t *)rsp;
  end = p + fsize;
  hi = g_self_reg.stack_hi;
  if (hi && end > hi) {
    end = hi;
  }
  p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)(ALIGN - 1));
  for (; p + sizeof(void *) <= end; p += ALIGN) {
    gc_rewrite_if_forwarded((void **)p);
  }
}

static int sm_lookup_rewrite(void *ret, void *caller_rbp, void *callee_rbp) {
  SmSite *site;
  void *rsp;
  (void)caller_rbp;
  site = sm_find_site(ret);
  if (!site) {
    return 0;
  }
  rsp = callee_rbp ? (uint8_t *)callee_rbp + 16 : NULL;
  sm_rewrite_frame(rsp, site->fsize);
  return 1;
}

static void walk_c_frames(int roots) {
  uint8_t *fp;
  uint8_t *hi;
  uint8_t *lo;
  uint32_t n;
  if (!g_stackmap_walk || !g_sm_nsites) {
    return;
  }
  fp = (uint8_t *)__builtin_frame_address(0);
  lo = g_self_reg.stack_lo;
  hi = g_self_reg.stack_hi;
  if (!fp || !hi) {
    return;
  }
  for (n = 0; n < 256 && fp; n++) {
    uint8_t *next;
    void *ret;
    if (lo && fp < lo) {
      break;
    }
    if (fp >= hi) {
      break;
    }
    if (((uintptr_t)fp & 7u) != 0) {
      break;
    }
    next = *(uint8_t **)fp;
    ret = *((void **)fp + 1);
    if (ret) {
      if (roots) {
        g_sm_walks++;
        sm_lookup_apply(ret, next, fp);
      } else {
        sm_lookup_rewrite(ret, next, fp);
      }
    }
    if (!next || next <= fp) {
      break;
    }
    fp = next;
  }
}

void copy_from_stackmaps(void) {
  /*
   * On with AVANT_NURSERY=1 unless AVANT_NURSERY_STACKMAP=0. Callsite
   * RSP is callee_rbp+16 (not LLVM stack size). Mapped slots are roots
   * (copy_from_slot). Leftover spills in the same frame are rewritten
   * after the work queue (rewrite_from_stackmaps).
   */
  walk_c_frames(1);
}

void rewrite_from_stackmaps(void) {
  walk_c_frames(0);
}

uint64_t g_sm_walks;
uint64_t g_sm_hits;
uint64_t g_sm_leftover;
uint32_t g_sm_nsites;
