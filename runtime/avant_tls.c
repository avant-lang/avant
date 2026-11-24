#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <elf.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The thread-locals generated code reads in its hot loops: the
 * allocation cursor and its limits (avant_gc_bump, avant_gc_limit,
 * avant_gc_limit_mt), the start bits' base and table (avant_gc_base,
 * avant_gc_starts) and the frame chain's top (avant_gc_top). Generated
 * code and the runtime read them at a fixed offset from the thread
 * pointer (%fs:-N, local-exec), and the offset matters: on the measuring
 * machine (Zen 1), the collector benchmark's allocation loop took 60%
 * more cycles at equal instructions once those offsets passed 1 KiB
 * (the cursor at -0x3f0 ran at full speed, at -0x410 slow), whatever
 * the variables' addresses and pages.
 *
 * A thread-local block lays its variables out in link order and ends at
 * the thread pointer, so the variables linked last are the nearest.
 * This object holds only these six, as ordinary .tbss definitions, and
 * the compiler links it after every other object of a program (the
 * program's own, the runtime's, a C package's, a --lib file): no
 * thread-local of another object comes between them and the thread
 * pointer, with GNU ld, mold and lld alike. (They used to be thread-local
 * common symbols in the collector's unit, which GNU ld and mold place
 * after every .tbss but lld places in .bss, outside the thread-local
 * block, with no error: every program lld linked wrote outside its
 * thread-local block from its first frame on.) The order below is the
 * one GNU ld gave the common symbols, so a program GNU ld links reads
 * them at the same offsets as before. tests/cases/test_tls_place.av
 * checks the offsets in a built program, with each linker installed. */
__asm__(".section .tbss,\"awT\",@nobits\n\t"
        ".p2align 3\n\t"
        ".globl avant_gc_starts\n\t.type avant_gc_starts, @object\n\t.size avant_gc_starts, 8\n"
        "avant_gc_starts:\n\t.zero 8\n\t"
        ".globl avant_gc_bump\n\t.type avant_gc_bump, @object\n\t.size avant_gc_bump, 8\n"
        "avant_gc_bump:\n\t.zero 8\n\t"
        ".globl avant_gc_top\n\t.type avant_gc_top, @object\n\t.size avant_gc_top, 8\n"
        "avant_gc_top:\n\t.zero 8\n\t"
        ".globl avant_gc_base\n\t.type avant_gc_base, @object\n\t.size avant_gc_base, 8\n"
        "avant_gc_base:\n\t.zero 8\n\t"
        ".globl avant_gc_limit\n\t.type avant_gc_limit, @object\n\t.size avant_gc_limit, 8\n"
        "avant_gc_limit:\n\t.zero 8\n\t"
        ".globl avant_gc_limit_mt\n\t.type avant_gc_limit_mt, @object\n\t.size avant_gc_limit_mt, 8\n"
        "avant_gc_limit_mt:\n\t.zero 8\n\t"
        ".previous\n");

extern __thread char avant_gc_starts __attribute__((tls_model("local-exec")));
extern __thread char avant_gc_bump __attribute__((tls_model("local-exec")));
extern __thread char avant_gc_top __attribute__((tls_model("local-exec")));
extern __thread char avant_gc_base __attribute__((tls_model("local-exec")));
extern __thread char avant_gc_limit __attribute__((tls_model("local-exec")));
extern __thread char avant_gc_limit_mt __attribute__((tls_model("local-exec")));

/* The executable's own ELF header, which GNU ld, gold, mold and lld
 * define. Read instead of getauxval(AT_PHDR): a C function the program
 * did not call before adds an entry to its PLT, which moves all of its
 * code (avant_stack.c says what that cost). */
extern const ElfW(Ehdr) __ehdr_start __attribute__((weak, visibility("hidden")));

/* A link that put these six outside the program's thread-local block (a
 * linker or a link line this object's placement does not hold for) would
 * have every access write over whatever lies there, from the first
 * frame on. Checked once, before main: each must lie inside the block,
 * which ends at the thread pointer and is the PT_TLS segment's size
 * rounded up to its alignment. */
__attribute__((constructor)) static void avant_tls_check(void) {
  const ElfW(Ehdr) *eh = &__ehdr_start;
  const ElfW(Phdr) *ph;
  uintptr_t tp;
  uintptr_t size = 0;
  const char *vars[6];
  const char *names[6] = {"avant_gc_starts", "avant_gc_bump", "avant_gc_top", "avant_gc_base", "avant_gc_limit", "avant_gc_limit_mt"};
  int i;
  if (!eh) {
    return;
  }
  ph = (const ElfW(Phdr) *)((const char *)eh + eh->e_phoff);
  for (i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_TLS) {
      uintptr_t al = ph[i].p_align ? ph[i].p_align : 1;
      size = (ph[i].p_memsz + al - 1) / al * al;
    }
  }
  tp = (uintptr_t)__builtin_thread_pointer();
  vars[0] = &avant_gc_starts;
  vars[1] = &avant_gc_bump;
  vars[2] = &avant_gc_top;
  vars[3] = &avant_gc_base;
  vars[4] = &avant_gc_limit;
  vars[5] = &avant_gc_limit_mt;
  for (i = 0; i < 6; i++) {
    uintptr_t a = (uintptr_t)vars[i];
    if (a + 8 > tp || tp - a > size) {
      fprintf(stderr,
              "avant runtime: the linker placed the runtime's thread-local %s outside the program's thread-local "
              "block (%ld bytes from the thread pointer; the block is %lu bytes), so the program cannot run: "
              "link it as the avant compiler does, with runtime/build/avant_tls.o after every other object\n",
              names[i], (long)(a - tp), (unsigned long)size);
      exit(1);
    }
  }
}
