#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "avant_rt.h"
#include "avant_gc_internal.h"

#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

/* A stack overflow's message, and a crash's. Each thread the runtime
 * registers (the main thread, a pool worker, an HTTP loop, a C thread at
 * its first callback: avant_stack_ensure, which every callback's thunk
 * calls) gets a small signal stack of its own (sigaltstack), and the
 * process a SIGSEGV and SIGBUS handler that runs on it: a fault at the end
 * of the thread's stack (in or just below its guard) is a stack overflow,
 * which prints
 *
 *   avant runtime: stack overflow in FUNCTION on the main thread: the
 *   calls nest too deep for its 8 MiB stack
 *
 * after what the program wrote to stdout, and ends the process with
 * status 1, as a trap that ends the program does; in a task too, since a
 * stack cannot be given back safely at any instruction.
 *
 * Any other fault the kernel raised (a null pointer or bad memory, in C
 * code: Avant's own code checks what could fault) prints
 *
 *   avant runtime: segmentation fault in FUNCTION on the main thread:
 *   address 0x0 is not mapped (a null pointer, most likely in C code)
 *
 * after the program's output as well, removes the private directory
 * (stopping the children it waits on) and then dies by the signal, as it
 * did before (status 139 for SIGSEGV, 135 for SIGBUS, a core dump where
 * the system keeps one): the program did not stop itself, its memory was
 * wrong. FUNCTION is the program's innermost function on the stack (the
 * one that called into C). A signal sent with kill or raise is not a
 * fault and has no message: it goes on as before, as does any fault when
 * a handler that was there before this one is not the runtime's own (a C
 * library's: it may handle the fault itself), which keeps it.
 *
 * What it costs a program that never faults: per registered thread one
 * mapping and one sigaltstack call, and two sigaction calls per process
 * (measured: a hello world's user instructions stay within their
 * run-to-run spread); nothing on any call, spawn or loop. It calls no C
 * function a program did not already link (exe_bias, alt_stack): one more
 * would move all of the program's code. The handler uses only
 * async-signal-safe calls (write, open, pread, close, sigaction, raise,
 * _exit), and fflush only when the fault is in the program's own code
 * (out_pending). FUNCTION is read from the executable's own symbol table
 * at the fault; when none is found the message leaves it out. */

#define ALT_SIZE (64u * 1024u)
#define NEAR_BELOW (1024u * 1024u + 64u * 1024u)
#define NEAR_ABOVE (16u * 1024u)
#define SCAN_WORDS 8192

static __thread uintptr_t st_lo;
static __thread uintptr_t st_hi;
static __thread int st_role;
static __thread void *st_alt;

static pthread_once_t st_once = PTHREAD_ONCE_INIT;
static uint8_t st_main_alt[ALT_SIZE] __attribute__((aligned(64)));
static int st_main_taken;
static pthread_key_t st_key;
static struct sigaction st_old;
static struct sigaction st_old_bus;
static struct sigaction st_old_fpe;
static struct sigaction st_old_ill;
static struct sigaction st_old_abrt;
static int st_installed;

static void on_fault(int sig, siginfo_t *si, void *ucv);

/* sigaltstack and mincore are made as system calls (the kernel's
 * stack_t is the C library's): a C function the program did not call
 * before adds an entry to its PLT and moves all of its code (exe_bias). */
static int alt_stack(const stack_t *ss, stack_t *old) {
  return (int)syscall(SYS_sigaltstack, ss, old);
}

static void st_thread_exit(void *alt) {
  stack_t ss;
  if (!alt) {
    return;
  }
  memset(&ss, 0, sizeof ss);
  ss.ss_flags = SS_DISABLE;
  if (alt_stack(&ss, NULL) == 0) {
    munmap((uint8_t *)alt - 4096, ALT_SIZE + 4096);
  }
  st_alt = NULL;
}

static void st_install(void) {
  struct sigaction sa;
  if (pthread_key_create(&st_key, st_thread_exit) != 0) {
    return;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, &st_old) == 0) {
    st_installed = 1;
  }
  (void)sigaction(SIGBUS, &sa, &st_old_bus);
  /* A division by zero or an illegal instruction in C code end the same
   * way: stdout's bytes first, then one line naming the signal and the
   * function (on_crash); a C library's abort() (a failed assertion, a
   * corrupted heap), which prints its own line, keeps stdout's bytes. */
  (void)sigaction(SIGFPE, &sa, &st_old_fpe);
  (void)sigaction(SIGILL, &sa, &st_old_ill);
  (void)sigaction(SIGABRT, &sa, &st_old_abrt);
}

/* The registering thread's stack [lo, hi) and whether it is a C thread
 * Avant did not start (avant_gc_roots.c register_thread). */
void avant_stack_thread_init(void *lo, void *hi, int foreign) {
  stack_t ss;
  stack_t old;
  uint8_t *m;
  st_lo = (uintptr_t)lo;
  st_hi = (uintptr_t)hi;
  if (foreign || st_role != AVANT_STACK_FOREIGN) {
    st_role = foreign ? AVANT_STACK_FOREIGN : AVANT_STACK_RUNTIME;
  }
  pthread_once(&st_once, st_install);
  if (!st_installed || st_alt || !lo) {
    return;
  }
  /* A signal stack the thread already has (a C library's, a sanitizer's)
   * stays: the handler runs on it as well. */
  if (alt_stack(NULL, &old) == 0 && !(old.ss_flags & SS_DISABLE)) {
    return;
  }
  /* The main thread's is the program's own (in .bss, no mapping): a
   * mapping made at startup, before the collector's and the C library's,
   * moves theirs, and measured, that alone made one LangArena task run
   * at one of several speeds (Template::Parse 1.34 to 2.96 s against the
   * base's 1.29 to 1.30 s); the main thread never exits through the key's
   * destructor. Another thread's is mapped, with a guard page below. */
  if ((pid_t)syscall(SYS_gettid) == getpid() && !st_main_taken) {
    st_main_taken = 1;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = st_main_alt;
    ss.ss_size = sizeof st_main_alt;
    if (alt_stack(&ss, NULL) == 0) {
      st_alt = st_main_alt;
    }
    return;
  }
  m = mmap(NULL, ALT_SIZE + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) {
    return;
  }
  (void)mprotect(m, 4096, PROT_NONE);
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = m + 4096;
  ss.ss_size = ALT_SIZE;
  if (alt_stack(&ss, NULL) != 0) {
    munmap(m, ALT_SIZE + 4096);
    return;
  }
  st_alt = m + 4096;
  (void)pthread_setspecific(st_key, st_alt);
}

/* A callback's entry (its thunk calls this, or avant_task_cb_enter in a
 * program that spawns or serves HTTP): a thread the collector does not
 * know yet (a C thread whose callback has not allocated) gets its signal
 * stack now, as a C thread; its registration, if it comes, keeps it. */
static __attribute__((noinline, cold)) void stack_ensure_slow(void);

void avant_stack_ensure(void) {
  /* Every callback's entry: the test alone, with no frame of locals (so
   * no stack protector), the rest out of line. */
  if (__builtin_expect(st_lo != 0, 1)) {
    return;
  }
  stack_ensure_slow();
}

static void stack_ensure_slow(void) {
  pthread_attr_t attr;
  void *addr = NULL;
  size_t sz = 0;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) {
    return;
  }
  if (pthread_attr_getstack(&attr, &addr, &sz) != 0) {
    addr = NULL;
  }
  pthread_attr_destroy(&attr);
  if (addr && sz) {
    avant_stack_thread_init(addr, (uint8_t *)addr + sz, 1);
  }
}

/* What the registered thread runs, for the message (avant_task.c's
 * workers, avant_http.c's loops). */
void avant_stack_role(int role) {
  st_role = role;
}

/* ---- the message, without stdio */

typedef struct {
  char b[512];
  size_t n;
} Msg;

static void put(Msg *m, const char *s) {
  while (*s && m->n < sizeof m->b - 1) {
    m->b[m->n++] = *s++;
  }
}

static void put_n(Msg *m, const char *s, size_t k) {
  while (k-- && *s && m->n < sizeof m->b - 1) {
    m->b[m->n++] = *s++;
  }
}

static void put_u(Msg *m, uint64_t v) {
  char d[24];
  int i = 0;
  do {
    d[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && i < 24);
  while (i && m->n < sizeof m->b - 1) {
    m->b[m->n++] = d[--i];
  }
}

/* ---- the executable's symbols, read at the fault */

typedef struct {
  int fd;
  uintptr_t bias;
  uint64_t text_lo, text_hi;
  uint64_t sym_off, sym_size, str_off, str_size;
} Exe;

static int rd(int fd, void *p, size_t n, uint64_t off) {
  return pread(fd, p, n, (off_t)off) == (ssize_t)n;
}

/* The load bias: where this unit's avant_stack_role is against where the
 * symbol table says it is. (getauxval's AT_PHDR would say it too, but a C
 * function the program did not call before adds an entry to its PLT,
 * which moves all of the program's code; measured, that alone makes one
 * LangArena task 8% slower.) */
static int exe_bias(Exe *e) {
  static const char want[] = "avant_stack_role";
  Elf64_Sym syms[64];
  char nm[sizeof want];
  uint64_t off;
  for (off = 0; off < e->sym_size; off += sizeof syms) {
    size_t n = sizeof syms;
    size_t i;
    if (e->sym_size - off < n) {
      n = (size_t)(e->sym_size - off);
    }
    if (!rd(e->fd, syms, n, e->sym_off + off)) {
      return 0;
    }
    for (i = 0; i < n / sizeof(Elf64_Sym); i++) {
      Elf64_Sym *y = &syms[i];
      if (ELF64_ST_TYPE(y->st_info) != STT_FUNC || y->st_name >= e->str_size) {
        continue;
      }
      if (rd(e->fd, nm, sizeof nm, e->str_off + y->st_name) && memcmp(nm, want, sizeof want) == 0) {
        e->bias = (uintptr_t)&avant_stack_role - (uintptr_t)y->st_value;
        return 1;
      }
    }
  }
  return 0;
}

static int exe_open(Exe *e) {
  Elf64_Ehdr eh;
  Elf64_Shdr sh;
  int i;
  memset(e, 0, sizeof *e);
  e->fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (e->fd < 0) {
    return 0;
  }
  if (!rd(e->fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
    return 0;
  }
  for (i = 0; i < eh.e_shnum; i++) {
    if (!rd(e->fd, &sh, sizeof sh, eh.e_shoff + (uint64_t)i * eh.e_shentsize)) {
      return 0;
    }
    if (sh.sh_type == SHT_SYMTAB && sh.sh_entsize == sizeof(Elf64_Sym)) {
      Elf64_Shdr strs;
      e->sym_off = sh.sh_offset;
      e->sym_size = sh.sh_size;
      if (!rd(e->fd, &strs, sizeof strs, eh.e_shoff + (uint64_t)sh.sh_link * eh.e_shentsize)) {
        return 0;
      }
      e->str_off = strs.sh_offset;
      e->str_size = strs.sh_size;
    }
    if ((sh.sh_flags & SHF_EXECINSTR) && sh.sh_type == SHT_PROGBITS) {
      if (!e->text_lo || sh.sh_addr < e->text_lo) {
        e->text_lo = sh.sh_addr;
      }
      if (sh.sh_addr + sh.sh_size > e->text_hi) {
        e->text_hi = sh.sh_addr + sh.sh_size;
      }
    }
  }
  return e->sym_size > 0 && exe_bias(e);
}

static int exe_has(const Exe *e, uintptr_t pc) {
  uint64_t a = (uint64_t)(pc - e->bias);
  return pc >= e->bias && a >= e->text_lo && a < e->text_hi;
}

/* The name of the program's own function holding pc (an `av.` symbol
 * that is not the compiler's helper, or main, which is the program's),
 * shown as written: `av.mod/depth` is `depth`, `av.mod/Node.clone` is
 * `Node.clone`. 0 when pc is in no such function. */
static int exe_name(const Exe *e, uintptr_t pc, char *out, size_t cap) {
  Elf64_Sym syms[64];
  uint64_t a = (uint64_t)(pc - e->bias);
  uint64_t off;
  char nm[256];
  if (!exe_has(e, pc)) {
    return 0;
  }
  for (off = 0; off < e->sym_size; off += sizeof syms) {
    size_t want = sizeof syms;
    size_t i, k;
    if (e->sym_size - off < want) {
      want = (size_t)(e->sym_size - off);
    }
    if (!rd(e->fd, syms, want, e->sym_off + off)) {
      return 0;
    }
    for (i = 0; i < want / sizeof(Elf64_Sym); i++) {
      Elf64_Sym *s = &syms[i];
      const char *p;
      if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || a < s->st_value || a >= s->st_value + (s->st_size ? s->st_size : 1)) {
        continue;
      }
      if (s->st_name >= e->str_size) {
        return 0;
      }
      memset(nm, 0, sizeof nm);
      if (pread(e->fd, nm, sizeof nm - 1, (off_t)(e->str_off + s->st_name)) <= 0) {
        return 0;
      }
      if (strcmp(nm, "main") == 0) {
        /* The program's main is the executable's C main. */
        memcpy(out, "main", 5);
        return cap > 5;
      }
      if (strncmp(nm, "av.", 3) != 0) {
        return 0;
      }
      p = strrchr(nm, '/');
      p = p ? p + 1 : nm + 3;
      if (strncmp(p, "__avant", 7) == 0) {
        return 0;
      }
      for (k = 0; p[k] && p[k] != '#' && k < cap - 1; k++) {
        out[k] = p[k];
      }
      out[k] = 0;
      return k > 0;
    }
  }
  return 0;
}

static uintptr_t ctx_pc(void *ucv) {
#if defined(__x86_64__)
  return ucv ? (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP] : 0;
#elif defined(__aarch64__)
  return ucv ? (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc : 0;
#else
  (void)ucv;
  return 0;
#endif
}

static uintptr_t ctx_sp(void *ucv) {
#if defined(__x86_64__)
  return ucv ? (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RSP] : 0;
#elif defined(__aarch64__)
  return ucv ? (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.sp : 0;
#else
  (void)ucv;
  return 0;
#endif
}

/* Whether the page holding p is mapped (a word of it may be read): the
 * stack below the frames still in use may not be, on the main thread. */
static int page_mapped(uintptr_t p) {
  unsigned char v;
  return syscall(SYS_mincore, (void *)(p & ~(uintptr_t)4095), (size_t)4096, &v) == 0;
}

/* The start of the executable's function holding pc (any function: the
 * runtime's C, a C file the program links), 0 when none. */
static uintptr_t exe_func(const Exe *e, uintptr_t pc) {
  Elf64_Sym syms[64];
  uint64_t a = (uint64_t)(pc - e->bias);
  uint64_t off;
  if (!exe_has(e, pc)) {
    return 0;
  }
  for (off = 0; off < e->sym_size; off += sizeof syms) {
    size_t want = sizeof syms;
    size_t i;
    if (e->sym_size - off < want) {
      want = (size_t)(e->sym_size - off);
    }
    if (!rd(e->fd, syms, want, e->sym_off + off)) {
      return 0;
    }
    for (i = 0; i < want / sizeof(Elf64_Sym); i++) {
      Elf64_Sym *y = &syms[i];
      if (ELF64_ST_TYPE(y->st_info) == STT_FUNC && y->st_size && a >= y->st_value && a < y->st_value + y->st_size) {
        return (uintptr_t)y->st_value + e->bias;
      }
    }
  }
  return 0;
}

/* Whether the instruction before the return address v is a direct call
 * of target (x86-64's call rel32). */
static int calls(const Exe *e, uintptr_t v, uintptr_t target) {
#if defined(__x86_64__)
  int32_t rel;
  if (!exe_has(e, v - 5) || *(const uint8_t *)(v - 5) != 0xe8) {
    return 0;
  }
  memcpy(&rel, (const void *)(v - 4), sizeof rel);
  return v + (uintptr_t)(intptr_t)rel == target;
#else
  (void)e;
  (void)v;
  (void)target;
  return 0;
#endif
}

/* The first word from sp up that is a return address into a function of
 * the program (one that calls target, when target is not 0). */
static int scan_name(const Exe *e, uintptr_t sp, uintptr_t target, char *out, size_t cap) {
  uintptr_t w;
  uintptr_t page = 0;
  int tries = 0;
  /* Below st_lo is the guard (a thread's, mapped but not readable) or
   * nothing yet (the main thread's); above it, a page may still be
   * unmapped on the main thread. */
  if (sp < st_lo) {
    sp = st_lo;
  }
  sp &= ~(uintptr_t)7;
  for (w = sp; w + 8 <= st_hi && w < sp + SCAN_WORDS * 8 && tries < 64; w += 8) {
    uintptr_t v;
    if ((w & ~(uintptr_t)4095) != page) {
      page = w & ~(uintptr_t)4095;
      if (!page_mapped(page)) {
        w = page + 4096 - 8;
        continue;
      }
    }
    v = *(const uintptr_t *)w;
    if (exe_has(e, v) && (!target || calls(e, v, target))) {
      tries++;
      if (exe_name(e, v, out, cap)) {
        return 1;
      }
    }
  }
  return 0;
}

/* The innermost function of the program: the one the fault is in; else,
 * when the fault is in a C function of the executable, the program's
 * function whose call of it is on the stack; else the first return
 * address on the stack (from the stack pointer up) into one. A C
 * function's frame may hold stale words that only look like return
 * addresses (an earlier call's), which name a function the thread was
 * running not long before: as good a hint for a stack overflow, the
 * first try for a fault in C is the call itself. */
static int fault_name(const Exe *e, uintptr_t pc, uintptr_t sp, char *out, size_t cap) {
  uintptr_t f;
  if (exe_name(e, pc, out, cap)) {
    return 1;
  }
  f = exe_func(e, pc);
  if (f && scan_name(e, sp, f, out, cap)) {
    return 1;
  }
  return scan_name(e, sp, 0, out, cap);
}

/* The handler that was there before this one, for sig. */
static struct sigaction *old_of(int sig) {
  if (sig == SIGBUS) {
    return &st_old_bus;
  }
  if (sig == SIGFPE) {
    return &st_old_fpe;
  }
  if (sig == SIGILL) {
    return &st_old_ill;
  }
  if (sig == SIGABRT) {
    return &st_old_abrt;
  }
  return &st_old;
}

/* Whether that handler is someone else's (a C library's, which may
 * handle the fault itself and then keeps it): not the default, not
 * ignored, not the private directory's cleanup. */
static int old_foreign(const struct sigaction *o) {
  if (o->sa_flags & SA_SIGINFO) {
    return o->sa_sigaction != NULL;
  }
  return o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN && o->sa_handler != avant_tmp_crash_handler();
}

/* The default action, after the private directory's cleanup (its own
 * handler is not installed when this one was there first), as that
 * handler does it: the signal raised again stays pending until this
 * handler returns, and then ends the process with the default action
 * (a signal sent with kill or raise has no faulting instruction to run
 * again). */
static void die_by(int sig) {
  struct sigaction sa;
  avant_tmp_on_crash();
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
  raise(sig);
}

static void chain(int sig, siginfo_t *si, void *ucv) {
  struct sigaction *o = old_of(sig);
  if (o->sa_flags & SA_SIGINFO) {
    if (o->sa_sigaction) {
      sigaction(sig, o, NULL);
      o->sa_sigaction(sig, si, ucv);
      return;
    }
  } else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) {
    sigaction(sig, o, NULL);
    o->sa_handler(sig);
    return;
  }
  die_by(sig);
}

/* stdout's buffered bytes, written with write(2), when the fault is
 * inside the C library. The program's writes go through avant_out_whole,
 * which flushes the buffer before a write that would not fit its free
 * space, so the C library never flushes the buffer in the middle of one
 * of them: [write base, write pointer) holds whole earlier writes, never
 * part of the one under way (copied before the pointer moves) and never
 * bytes already written out (a flush moves the pointer back as soon as
 * its write returns); the process ends next, so the buffer is not
 * flushed again. glibc's FILE fields; on another C library, nothing. */
static void out_pending(void) {
  avant_out_pending();
}

/* out_pending's work, for the end of a program by SIGTERM or SIGINT too
 * (avant_io.c's tmp_on_signal): the signal may stop a thread in the
 * middle of a write, as a fault may. */
void avant_out_pending(void) {
#if defined(__GLIBC__)
  FILE *f = stdout;
  const char *b = f->_IO_write_base;
  const char *p = f->_IO_write_ptr;
  if (!b || !p || p <= b || !f->_IO_buf_base || b < f->_IO_buf_base || p > f->_IO_buf_end) {
    return;
  }
  while (b < p) {
    ssize_t w = write(1, b, (size_t)(p - b));
    if (w <= 0) {
      return;
    }
    b += w;
  }
#endif
}

/* Where the thread is, for the messages: " on the main thread", " in a
 * task", ... */
static void put_thread(Msg *m) {
  if ((pid_t)syscall(SYS_gettid) == getpid()) {
    put(m, " on the main thread");
  } else if (st_role == AVANT_STACK_WORKER) {
    put(m, " in a task");
  } else if (st_role == AVANT_STACK_LOOP) {
    put(m, " in an HTTP handler");
  } else if (st_role == AVANT_STACK_FOREIGN) {
    put(m, " in a callback on a C thread");
  } else {
    put(m, " on a thread of the runtime");
  }
}

static void put_x(Msg *m, uint64_t v) {
  static const char dig[] = "0123456789abcdef";
  char d[16];
  int i = 0;
  put(m, "0x");
  do {
    d[i++] = dig[v & 15];
    v >>= 4;
  } while (v && i < 16);
  while (i && m->n < sizeof m->b - 1) {
    m->b[m->n++] = d[--i];
  }
}

/* The fault's function, read from the executable: FUNCTION's text in
 * name (0 when none), and whether the fault is in the program's own
 * code (*in_exe: the executable's text, the runtime's C included; *in_av:
 * a function of the program). */
static int crash_name(uintptr_t pc, void *ucv, char *name, size_t cap, int *in_exe, int *in_av) {
  Exe e;
  int named = 0;
  *in_exe = 0;
  *in_av = 0;
  if (exe_open(&e)) {
    *in_exe = exe_has(&e, pc);
    *in_av = exe_name(&e, pc, name, cap);
    named = *in_av || fault_name(&e, pc, ctx_sp(ucv), name, cap);
  }
  if (e.fd >= 0) {
    close(e.fd);
  }
  return named;
}

/* A fault that is not a stack overflow: the program's output, the line,
 * the private directory's cleanup, then the signal's default action. */
static void on_crash(int sig, siginfo_t *si, void *ucv) {
  uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
  Msg m;
  char name[200];
  int in_exe, in_av;
  int named;
  avant_exit_claim();
  named = crash_name(ctx_pc(ucv), ucv, name, sizeof name, &in_exe, &in_av);
  /* In C code (the C library's, the runtime's, a library the program
   * binds) the thread may hold stdout's lock in the middle of a write:
   * the bytes the buffer holds up to its write pointer go out instead
   * of a flush (out_pending). */
  if (in_av) {
    fflush(stdout);
  } else {
    out_pending();
  }
  m.n = 0;
  if (sig == SIGABRT) {
    /* abort() comes after its caller's own line (a failed assertion's, the
     * C library's "malloc(): corrupted ...", the runtime's fatal errors):
     * the program's output is kept, and no second line is added. */
    die_by(sig);
    return;
  }
  if (sig == SIGFPE || sig == SIGILL) {
    put(&m, sig == SIGFPE ? "avant runtime: arithmetic fault" : "avant runtime: illegal instruction");
    if (named) {
      put(&m, " in ");
      put_n(&m, name, sizeof name);
    }
    put_thread(&m);
    if (sig == SIGFPE) {
      put(&m, ": an integer division by zero or overflow, most likely in C code\n");
    } else {
      put(&m, ": address ");
      put_x(&m, a);
      put(&m, " holds an instruction this processor cannot run, most likely in C code\n");
    }
    (void)!write(2, m.b, m.n);
    die_by(sig);
    return;
  }
  put(&m, sig == SIGBUS ? "avant runtime: bus error" : "avant runtime: segmentation fault");
  if (named) {
    put(&m, " in ");
    put_n(&m, name, sizeof name);
  }
  put_thread(&m);
  put(&m, ": address ");
  put_x(&m, a);
  if (sig == SIGBUS) {
    put(&m, " cannot be read or written (a mapped file cut short, or a misaligned access, most likely in C code)\n");
  } else if (si->si_code == SEGV_ACCERR) {
    put(&m, " is protected (bad memory, most likely in C code)\n");
  } else if (a < 4096) {
    put(&m, " is not mapped (a null pointer, most likely in C code)\n");
  } else {
    put(&m, " is not mapped (bad memory, most likely in C code)\n");
  }
  (void)!write(2, m.b, m.n);
  die_by(sig);
}

static void on_fault(int sig, siginfo_t *si, void *ucv) {
  uintptr_t a = si ? (uintptr_t)si->si_addr : 0;
  Msg m;
  char name[200];
  int named;
  int in_exe, in_av;
  /* A signal sent (kill, raise: si_code <= 0) is not a fault, but for
   * SIGABRT, which abort() sends itself. */
  if (!si || (si->si_code <= 0 && sig != SIGABRT)) {
    chain(sig, si, ucv);
    return;
  }
  if (sig != SIGSEGV || !st_lo || a + NEAR_BELOW < st_lo || a >= st_lo + NEAR_ABOVE || a >= st_hi) {
    if (old_foreign(old_of(sig))) {
      chain(sig, si, ucv);
    } else {
      on_crash(sig, si, ucv);
    }
    return;
  }
  avant_exit_claim();
  named = crash_name(ctx_pc(ucv), ucv, name, sizeof name, &in_exe, &in_av);
  /* What the program printed comes first, as for a trap. Inside the C
   * library the thread may be in the middle of a write to stdout (its
   * lock held by this very thread), where a flush could write a line
   * twice: the bytes the buffer holds up to its write pointer go out
   * instead (out_pending). */
  if (in_exe) {
    fflush(stdout);
  } else {
    out_pending();
  }
  m.n = 0;
  put(&m, "avant runtime: stack overflow");
  if (named) {
    put(&m, " in ");
    put_n(&m, name, sizeof name);
  }
  if ((pid_t)syscall(SYS_gettid) == getpid()) {
    put(&m, " on the main thread: the calls nest too deep for its ");
  } else if (st_role == AVANT_STACK_WORKER) {
    put(&m, " in a task: the calls nest too deep for a pool worker's ");
  } else if (st_role == AVANT_STACK_LOOP) {
    put(&m, " in an HTTP handler: the calls nest too deep for an event loop's ");
  } else if (st_role == AVANT_STACK_FOREIGN) {
    put(&m, " in a callback on a C thread: the calls nest too deep for its ");
  } else {
    put(&m, " on a thread of the runtime: the calls nest too deep for its ");
  }
  if (st_hi - st_lo >= 1024u * 1024u) {
    put_u(&m, (uint64_t)((st_hi - st_lo + 512u * 1024u) / (1024u * 1024u)));
    put(&m, " MiB");
  } else {
    put_u(&m, (uint64_t)((st_hi - st_lo) / 1024u));
    put(&m, " KiB");
  }
  put(&m, " stack\n");
  (void)!write(2, m.b, m.n);
  avant_tmp_on_crash();
  _exit(1);
}
