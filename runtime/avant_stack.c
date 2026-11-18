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

/* A stack overflow's message. Each thread the runtime registers (the main
 * thread, a pool worker, an HTTP loop, a C thread at its first callback
 * that allocates, or at its first callback in a program that spawns or
 * serves HTTP: avant_stack_ensure) gets a small signal stack of its own
 * (sigaltstack), and the process a
 * SIGSEGV handler that runs on it: a fault at the end of the thread's
 * stack (in or just below its guard) is a stack overflow, which prints
 *
 *   avant runtime: stack overflow in FUNCTION on the main thread: the
 *   calls nest too deep for its 8 MiB stack
 *
 * after what the program wrote to stdout, and ends the process with
 * status 1, as a trap that ends the program does; in a task too, since a
 * stack cannot be given back safely at any instruction. Any other fault
 * goes on as before: the handler that was there (the private directory's
 * cleanup, or a C library's own), else the default action, a crash.
 *
 * What it costs a program that never overflows: per registered thread one
 * mapping and one sigaltstack call (measured: a hello world's user
 * instructions stay within their run-to-run spread); nothing on any call,
 * spawn or loop. It calls no C function a program did not already link
 * (exe_bias, alt_stack): one more would move all of the program's code. The
 * handler uses only async-signal-safe calls (write, open, pread, close,
 * sigaction, _exit). FUNCTION is read from the executable's own symbol
 * table at the fault (the innermost function of the program on the stack);
 * when none is found the message leaves it out. */

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

/* A callback's entry on a thread the collector does not know yet (a C
 * thread whose callback has not allocated): the thread gets its signal
 * stack now, as a C thread; its registration, if it comes, keeps it. */
void avant_stack_ensure(void) {
  pthread_attr_t attr;
  void *addr = NULL;
  size_t sz = 0;
  if (st_lo) {
    return;
  }
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
 * that is not the compiler's helper), shown as written: `av.mod/depth`
 * is `depth`, `av.mod/Node.clone` is `Node.clone`. 0 when pc is in no
 * such function. */
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

/* The innermost function of the program: the one the fault is in, else
 * the first return address on the stack (from the stack pointer up) into
 * one; a word that only looks like one names a function the thread was
 * running not long before, which is as good a hint. */
static int fault_name(const Exe *e, uintptr_t pc, uintptr_t sp, char *out, size_t cap) {
  uintptr_t w;
  uintptr_t page = 0;
  int tries = 0;
  if (exe_name(e, pc, out, cap)) {
    return 1;
  }
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
    if (exe_has(e, v)) {
      tries++;
      if (exe_name(e, v, out, cap)) {
        return 1;
      }
    }
  }
  return 0;
}

static void chain(int sig, siginfo_t *si, void *ucv) {
  if (st_old.sa_flags & SA_SIGINFO) {
    if (st_old.sa_sigaction) {
      sigaction(sig, &st_old, NULL);
      st_old.sa_sigaction(sig, si, ucv);
      return;
    }
  } else if (st_old.sa_handler != SIG_DFL && st_old.sa_handler != SIG_IGN) {
    sigaction(sig, &st_old, NULL);
    st_old.sa_handler(sig);
    return;
  }
  /* The default action, after the private directory's cleanup (its own
   * handler is not installed when this one was there first), as that
   * handler does it: the signal raised again stays pending until this
   * handler returns, and then ends the process with the default action
   * (a signal sent with kill or raise has no faulting instruction to run
   * again). */
  avant_tmp_on_crash();
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
  }
  raise(sig);
}

static void on_fault(int sig, siginfo_t *si, void *ucv) {
  uintptr_t a = si ? (uintptr_t)si->si_addr : 0;
  uintptr_t pc = ctx_pc(ucv);
  Msg m;
  Exe e;
  char name[200];
  int named = 0;
  int in_exe = 0;
  /* A fault the kernel raised (si_code > 0), not a signal sent. */
  if (!si || si->si_code <= 0 || !st_lo || a + NEAR_BELOW < st_lo || a >= st_lo + NEAR_ABOVE || a >= st_hi) {
    chain(sig, si, ucv);
    return;
  }
  avant_exit_claim();
  if (exe_open(&e)) {
    in_exe = exe_has(&e, pc);
    named = fault_name(&e, pc, ctx_sp(ucv), name, sizeof name);
  }
  if (e.fd >= 0) {
    close(e.fd);
  }
  /* What the program printed comes first, as for a trap; not when the
   * fault is inside the C library, which may be in the middle of a write
   * to stdout itself. */
  if (in_exe) {
    fflush(stdout);
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
