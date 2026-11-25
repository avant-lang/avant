#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} AvantArray;

static int32_t g_argc;
static char **g_argv;
static int g_argv_set;
static void *g_argv_arr;

/* gc-lint: holds nothing (every caller passes C memory: argv, file and
 * environment text, directory names, paths). */
static char *dup_bytes(const char *s, size_t n) {
  char *p = avant_str_alloc((uint64_t)n);
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = 0;
  avant_str_tag(p);
  return p;
}

/* An array of N strings filled in place (RC1): the array
 * is a handle of the caller's scope, its buffer is published before the
 * first string is allocated, and each string is stored with a barrier
 * (RC3) before the next allocation. */
/* gc-lint: holds nothing (the array is read through the caller's handle *AP after the allocation). */
static void io_array_buf(AvantArray **ap, int32_t n) {
  char **buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
  avant_barrier(*ap);
  (*ap)->buf = buf;
  (*ap)->cap = n;
}

static void io_array_put(AvantArray *a, char *p) {
  char **slot = &((char **)a->buf)[a->size++];
  /* The barrier before the store (RC5). */
  avant_barrier_slot_young(a->buf, slot);
  *slot = p;
}

static char *cstr_dup(const char *s) {
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (!p) {
    return NULL;
  }
  memcpy(p, s, n + 1);
  return p;
}

/* The runtime's one program flag, --avant-threads=N (the thread pool's
 * size, 1 to AVANT_POOL_SIZE_MAX): the generated startup calls this
 * before anything can start the pool or the HTTP server, so scanning
 * here records the size in time, and removing the flag from the array
 * means the program's own argv() never sees it. Every other argument,
 * --threads included, is the program's; scanning stops at a literal
 * "--". A bare --avant-threads or a bad value stops the program at
 * startup: one line, status 1. */
static void avant_threads_bad(const char *a) {
  fprintf(stderr, "avant runtime: %s: the thread pool's size is --avant-threads=N, N from 1 to %d\n", a,
          AVANT_POOL_SIZE_MAX);
  exit(1);
}

static void scan_threads_flag(void) {
  int32_t i;
  int32_t w = 1;
  for (i = 1; i < g_argc; i++) {
    const char *a = g_argv[i] ? g_argv[i] : "";
    if (strcmp(a, "--") == 0) {
      for (; i < g_argc; i++) {
        g_argv[w++] = g_argv[i];
      }
      break;
    }
    if (strcmp(a, "--avant-threads") == 0) {
      avant_threads_bad(a);
    }
    if (strncmp(a, "--avant-threads=", 16) == 0) {
      const char *val = a + 16;
      char *end = NULL;
      unsigned long long v;
      if (*val < '0' || *val > '9') {
        avant_threads_bad(a);
      }
      v = strtoull(val, &end, 10);
      if (*end != '\0' || v < 1 || v > (unsigned long long)AVANT_POOL_SIZE_MAX) {
        avant_threads_bad(a);
      }
      avant_set_flag_threads((long)v);
      continue;
    }
    g_argv[w++] = g_argv[i];
  }
  g_argc = w;
  if (g_argc > 0) {
    g_argv[g_argc] = NULL;
  }
}

void avant_io_init_argv(int32_t argc, void *argv) {
  g_argc = argc < 0 ? 0 : argc;
  g_argv = (char **)argv;
  g_argv_set = 1;
  scan_threads_flag();
}

void *avant_argv(void) {
  if (g_argv_arr) {
    return g_argv_arr;
  }

  int32_t n = g_argv_set ? g_argc : 0;
  if (n < 0) {
    n = 0;
  }

  AvantArray *a = NULL;
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &a);
  a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (n > 0) {
    int32_t i;
    io_array_buf(&a, n);
    for (i = 0; i < n; i++) {
      const char *s = g_argv && g_argv[i] ? g_argv[i] : "";
      char *p = dup_bytes(s, strlen(s));
      io_array_put(a, p);
    }
  }
  avant_pin(a);
  g_argv_arr = a;
  return a;
}

/* The error of the last failed prelude file or process call on this
 * thread: errno, the stage a process failed in
 * (1 its output file, 2 its program), and the signal that killed one. */
static _Thread_local int32_t g_sys_errno;
static _Thread_local int32_t g_sys_stage;
static _Thread_local int32_t g_sys_signal;

/* The size a file reports is only a first buffer size: /proc files report
 * 0 and sysfs attributes report 4096 whatever they hold, so the read runs
 * to end of file. Trusting the size made /proc read empty and made the
 * test runner's processor count fall back to 1 on kernels whose sysfs
 * reports 4096. stdio only. read_all returns the
 * bytes in C memory (*n of them, NUL-terminated), or NULL with errno set;
 * open_read makes a directory EISDIR. */
static FILE *open_read(const char *path) {
  struct stat st;
  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  if (fstat(fileno(f), &st) == 0 && S_ISDIR(st.st_mode)) {
    fclose(f);
    errno = EISDIR;
    return NULL;
  }
  return f;
}

/* Reads and closes f (C memory only, so it may run native). */
static char *read_all(FILE *f, size_t *np) {
  size_t cap = 4096;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    /* A file past the String ceiling is an IoError (EFBIG) before
     * anything is read. */
    if (sz > (long)AVANT_SIZE_MAX) {
      fclose(f);
      errno = EFBIG;
      return NULL;
    }
    if (sz > 0) {
      cap = (size_t)sz + 1;
    }
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    rewind(f);
  }
  char *raw = (char *)malloc(cap + 1);
  if (!raw) {
    fclose(f);
    errno = ENOMEM;
    return NULL;
  }
  size_t n = 0;
  for (;;) {
    if (n == cap) {
      /* A file whose size was not known (a pipe, /proc) stops at the
       * ceiling the same way. */
      if (cap > (size_t)AVANT_SIZE_MAX) {
        free(raw);
        fclose(f);
        errno = EFBIG;
        return NULL;
      }
      size_t ncap = cap * 2;
      char *grown = (char *)realloc(raw, ncap + 1);
      if (!grown) {
        free(raw);
        fclose(f);
        errno = ENOMEM;
        return NULL;
      }
      raw = grown;
      cap = ncap;
    }
    size_t got = fread(raw + n, 1, cap - n, f);
    n += got;
    if (got == 0) {
      if (ferror(f)) {
        int e = errno ? errno : EIO;
        free(raw);
        fclose(f);
        errno = e;
        return NULL;
      }
      break;
    }
  }
  if (n > (size_t)AVANT_SIZE_MAX) {
    free(raw);
    fclose(f);
    errno = EFBIG;
    return NULL;
  }
  raw[n] = 0;
  fclose(f);
  *np = n;
  return raw;
}

/* The prelude's file_read: the text, or NULL
 * with avant_sys_errno() telling why. */
/* gc-lint: holds nothing (the text read is C memory; the path is not read after dup_bytes). */
char *avant_sys_read(const char *path) {
  size_t n = 0;
  char *raw;
  FILE *f = open_read(path ? path : "");
  if (!f) {
    g_sys_errno = errno;
    return NULL;
  }
  avant_gc_native_begin();
  raw = read_all(f, &n);
  g_sys_errno = raw ? 0 : errno;
  avant_gc_native_end();
  if (!raw) {
    return NULL;
  }
  char *heap = dup_bytes(raw, n);
  free(raw);
  return heap;
}

/* The prelude's file_write: the bytes written, or -1 with
 * avant_sys_errno() telling why (opening, writing or closing). */
avant_int avant_sys_write(const char *path, const char *body) {
  FILE *f;
  size_t n;
  if (!body) {
    body = "";
  }
  n = avant_str_n(body);
  /* The count is an Avant Int: a body past the ceiling (a C string) is
   * EFBIG, never a wrapped count. */
  if (n > (size_t)AVANT_SIZE_MAX) {
    g_sys_errno = EFBIG;
    return -1;
  }
  f = fopen(path ? path : "", "wb");
  if (!f) {
    g_sys_errno = errno;
    return -1;
  }
  if (n && fwrite(body, 1, n, f) != n) {
    g_sys_errno = errno ? errno : EIO;
    fclose(f);
    return -1;
  }
  if (fclose(f) != 0) {
    g_sys_errno = errno;
    return -1;
  }
  g_sys_errno = 0;
  return (int32_t)n;
}

/* An entry point's failure: `fn main: Void |
 * E` or `Int | E` returned a failure, whose text ("${e}") goes to stderr
 * with a newline, after what the program wrote to stdout; the result, 1,
 * is the exit status the generated main returns. */
avant_int avant_main_fail(const char *text) {
  /* A `fun` of the program's own entry lib: the text arrives as C reads a
   * String (terminated; the bytes of a slice in place), so its length is
   * its terminator's. */
  size_t n = text ? strlen(text) : 0;
  fflush(stdout);
  if (n) {
    fwrite(text, 1, n, stderr);
  }
  fputc('\n', stderr);
  fflush(stderr);
  return 1;
}

/* One write of the program's to stdout, whole: N bytes from P, then a
 * newline when NL, under one hold of stdout's lock (another thread's
 * flush between the text and its newline would write the line without
 * its end). The C library copies a write longer than the buffer's free
 * space in two parts: the part that fits, then a flush of the full
 * buffer, then the rest. A stack overflow (or SIGTERM, SIGINT) inside
 * that flush ends the process while the buffer holds the first part, and
 * the handler, which writes the buffer up to its write pointer
 * (avant_out_pending), wrote the line cut short. So the buffer is
 * flushed first, while it holds only whole earlier writes, and the write
 * then goes into the buffer at once; a write longer than the buffer
 * itself goes out with write(2) instead, in one piece after the buffer's
 * bytes. The buffer between the write base and the write pointer then
 * always holds whole writes: an interrupted write is either all there or
 * not there at all. glibc's FILE fields; on another C library, the
 * plain stdio writes. Not before the stream has a buffer: its first write
 * allocates one (the smallest glibc picks is a terminal's 1024 bytes),
 * so a first write longer than that goes out directly as well. */
void avant_out_whole(const char *p, size_t n, int nl) {
  FILE *f = stdout;
  size_t all = n + (nl ? 1u : 0u);
  flockfile(f);
#if defined(__GLIBC__)
  /* The free space up to the buffer's end (a line-buffered stream keeps
   * its write end at the buffer's start). */
  if (f->_IO_write_ptr > f->_IO_write_base && (size_t)(f->_IO_buf_end - f->_IO_write_ptr) < all) {
    (void)fflush_unlocked(f);
  }
  if (all > (f->_IO_buf_base ? (size_t)(f->_IO_buf_end - f->_IO_buf_base) : 1024u) && f->_IO_write_ptr == f->_IO_write_base) {
    const char *q = p;
    size_t left = n;
    int part = 0;
    while (part < 2) {
      while (left) {
        ssize_t w = write(f->_fileno, q, left);
        if (w < 0 && errno == EINTR) {
          continue;
        }
        if (w <= 0) {
          f->_flags |= _IO_ERR_SEEN;
          funlockfile(f);
          return;
        }
        q += w;
        left -= (size_t)w;
      }
      part++;
      q = "\n";
      left = (part == 1 && nl) ? 1u : 0u;
    }
    /* The stream's own count of its position no longer holds. */
    f->_offset = -1;
    funlockfile(f);
    return;
  }
#endif
  if (n) {
    fwrite_unlocked(p, 1, n, f);
  }
  if (nl) {
    putc_unlocked('\n', f);
  }
  funlockfile(f);
}

/* print, eputs and eprint (puts keeps its own writers in avant_str.c):
 * one value's text, a newline when HOW has bit 1, to stderr when it has
 * bit 2. A write to stderr first flushes what stdout holds, so the two
 * streams keep their order when they go to one place (a terminal, `2>&1`);
 * stdout stays buffered otherwise. Every write is checked as puts's is
 * (out_check below). */
static void out_check(int at_exit);

static void out_bytes(const char *p, size_t n, int32_t how) {
  if (how & 2) {
    fflush(stdout);
    if (n) {
      fwrite(p, 1, n, stderr);
    }
    if (how & 1) {
      putc('\n', stderr);
    }
  } else {
    avant_out_whole(p, n, how & 1);
  }
  if (ferror_unlocked(stdout) || ferror_unlocked(stderr)) {
    out_check(0);
  }
}

void avant_out_str(const char *s, int32_t how) {
  size_t n;
  s = avant_str_bytes(s, &n);
  out_bytes(s, n, how);
}

void avant_out_i64(int64_t v, int32_t how) {
  char buf[24];
  char *o = buf + sizeof(buf);
  uint64_t u = v < 0 ? 0u - (uint64_t)v : (uint64_t)v;
  do {
    *--o = (char)('0' + (int)(u % 10u));
    u /= 10u;
  } while (u);
  if (v < 0) {
    *--o = '-';
  }
  out_bytes(o, (size_t)(buf + sizeof(buf) - o), how);
}

void avant_out_u64(uint64_t v, int32_t how) {
  char buf[24];
  char *o = buf + sizeof(buf);
  do {
    *--o = (char)('0' + (int)(v % 10u));
    v /= 10u;
  } while (v);
  out_bytes(o, (size_t)(buf + sizeof(buf) - o), how);
}

void avant_out_f64(double v, int32_t how) {
  char buf[48];
  int k = avant_fmt_float_text(buf, v);
  out_bytes(buf, (size_t)k, how);
}

void avant_out_bool(int32_t v, int32_t how) {
  out_bytes(v ? "true" : "false", v ? 4 : 5, how);
}

/* exit(status): the generated code has checked 0..255 (a status outside
 * traps at its site); exit runs the atexit work: stdout flushed and
 * checked, the private temporary directory removed. One exit ends the
 * process (avant_exit_claim): a task's exit while another thread traps or
 * exits, or while main returns, waits for good. The end of main in a
 * program with tasks comes here too. */
void avant_exit(int32_t status) {
  avant_exit_claim();
  exit((int)status);
}

avant_int avant_sys_errno(void) {
  return g_sys_errno;
}

avant_int avant_sys_stage(void) {
  return g_sys_stage;
}

avant_int avant_sys_signal(void) {
  return g_sys_signal;
}

/* The text of an errno value ("No such file or directory"). */
/* gc-lint: holds nothing (C memory only). */
char *avant_sys_strerror(avant_int code) {
  char buf[256];
  const char *t = strerror_r(avant_c_int(code), buf, sizeof buf);
  return dup_bytes(t, strlen(t));
}

/* The name of a signal ("Killed"). */
/* gc-lint: holds nothing (C memory only). */
char *avant_sys_strsignal(avant_int sig) {
  const char *t = strsignal(avant_c_int(sig));
  if (!t) {
    t = "unknown signal";
  }
  return dup_bytes(t, strlen(t));
}

static void free_argv(char **argv) {
  int32_t i;
  if (!argv) {
    return;
  }
  for (i = 0; argv[i]; i++) {
    free(argv[i]);
  }
  free(argv);
}

/* argv for execv: path, then the Avant Array(String) args; NULL on OOM. */
static char **build_argv(const char *path, void *args) {
  AvantArray *extra = (AvantArray *)args;
  int32_t extra_n = 0;
  char **extra_buf = NULL;
  int32_t i;
  if (extra && extra->size > 0) {
    extra_n = extra->size;
    extra_buf = (char **)extra->buf;
  }
  char **argv = (char **)calloc((size_t)extra_n + 2, sizeof(char *));
  if (!argv) {
    return NULL;
  }
  argv[0] = cstr_dup(path);
  if (!argv[0]) {
    free(argv);
    return NULL;
  }
  for (i = 0; i < extra_n; i++) {
    /* An element is an Avant String (a view's bytes are its base's). */
    size_t n = 0;
    const char *s = avant_str_bytes(extra_buf ? extra_buf[i] : NULL, &n);
    argv[i + 1] = (char *)malloc(n + 1);
    if (argv[i + 1]) {
      memcpy(argv[i + 1], s, n);
      argv[i + 1][n] = 0;
    }
    if (!argv[i + 1]) {
      free_argv(argv);
      return NULL;
    }
  }
  return argv;
}

/* In the forked child: report why it could not start on errfd (when
 * there is one: a stage and errno) and leave with 127. */
static void child_fail(int errfd, int32_t stage) {
  if (errfd >= 0) {
    int32_t msg[2];
    ssize_t w;
    msg[0] = stage;
    msg[1] = errno;
    w = write(errfd, msg, sizeof msg);
    (void)w;
  }
  _exit(127);
}

static void child_redirect(const char *file, int fd_a, int fd_b, int errfd) {
  int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    child_fail(errfd, 1);
  }
  if (dup2(fd, fd_a) < 0) {
    child_fail(errfd, 1);
  }
  if (fd_b >= 0 && dup2(fd, fd_b) < 0) {
    child_fail(errfd, 1);
  }
  if (fd > STDERR_FILENO) {
    close(fd);
  }
}

/* In the forked child: optional stdout file, and stderr to err_path when
 * given, else to the stdout file; a job also gets its own process
 * group, /dev/null stdin, and default signal dispositions, so the runner
 * can stop the whole job and a job never reads the terminal. */
static void child_stdin_null(void) {
  int devnull = open("/dev/null", O_RDONLY);
  if (devnull >= 0) {
    dup2(devnull, STDIN_FILENO);
    if (devnull > STDERR_FILENO) {
      close(devnull);
    }
  }
}

/* job: a runner job. no_stdin: stdin from /dev/null, for a tool the
 * compiler runs: myc-llvm reads all of a non-terminal stdin before
 * its arguments, so an inherited pipe or socket made it hang or take the
 * input as a source file. */
static void child_exec(const char *path, char **argv, const char *out_path, const char *err_path, int job, int no_stdin, int errfd) {
  if (job) {
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
  }
  if (job || no_stdin) {
    child_stdin_null();
  }
  if (err_path && err_path[0]) {
    if (out_path && out_path[0]) {
      child_redirect(out_path, STDOUT_FILENO, -1, errfd);
    }
    child_redirect(err_path, STDERR_FILENO, -1, errfd);
  } else if (out_path && out_path[0]) {
    child_redirect(out_path, STDOUT_FILENO, STDERR_FILENO, errfd);
  }
  /* The prelude's process_run looks a name without '/' up on PATH; the
   * runtime's internal runs name their programs. */
  if (errfd >= 0) {
    execvp(path, argv);
  } else {
    execv(path, argv);
  }
  child_fail(errfd, 2);
}

static int32_t exit_code_of(int st) {
  if (WIFEXITED(st)) {
    return WEXITSTATUS(st);
  }
  if (WIFSIGNALED(st)) {
    return 128 + WTERMSIG(st);
  }
  return 1;
}

/* The children blocking runs wait on, one slot per waiting thread (each
 * task in a process.run has its own), and the live jobs (each the leader
 * of its own process group). The scratch signal handler reads these to
 * stop children before it removes the directory. A slot is claimed and
 * given back with atomics, so the handler, which only reads, sees each
 * pid whole; g_waits_hi bounds its scan.
 *
 * A slot is claimed before the fork, holding WAIT_STARTING, and takes the
 * child's pid as soon as fork returns: a crash, trap or signal on another
 * thread while a child is being started finds the claimed slot and waits
 * a bounded time for its pid, so no child is started that the handler
 * does not know of. g_stopping closes the other side: the handler sets it
 * before it reads the slots, and a thread that claims a slot after that
 * does not fork (each side's write comes before its read, both
 * sequentially consistent, so one of them sees the other). From the
 * claim to the pid the starting thread blocks the signals the handler
 * runs on, so the handler never runs on the one thread that would fill
 * the slot it waits for; the child gets the thread's mask back before
 * it runs anything. */
#define JOB_MAX 1024
#define WAIT_MAX 1100
#define WAIT_STARTING ((pid_t)-1)
static pid_t g_waits[WAIT_MAX];
static int32_t g_waits_hi;
static int32_t g_stopping;
static __thread int32_t t_wait_slot = -1;
static __thread int32_t t_job_starting;

/* The signals that end the program through tmp_on_signal; a crash's
 * signals are synchronous and are never blocked. */
static void start_signals(sigset_t *set) {
  sigemptyset(set);
  sigaddset(set, SIGINT);
  sigaddset(set, SIGTERM);
  sigaddset(set, SIGHUP);
  sigaddset(set, SIGPIPE);
  sigaddset(set, SIGQUIT);
}

/* Before a fork: blocks the handler's signals on this thread (the old
 * mask in *old) and tells whether the program is stopping, in which case
 * the caller must not fork and gives the mask back (start_end). */
static int start_begin(sigset_t *old) {
  sigset_t set;
  start_signals(&set);
  pthread_sigmask(SIG_BLOCK, &set, old);
  if (__atomic_load_n(&g_stopping, __ATOMIC_SEQ_CST)) {
    pthread_sigmask(SIG_SETMASK, old, NULL);
    return 0;
  }
  return 1;
}

static void start_end(const sigset_t *old) {
  pthread_sigmask(SIG_SETMASK, old, NULL);
}

/* In the child, first: the mask the starting thread had. */
static void start_child(const sigset_t *old) {
  pthread_sigmask(SIG_SETMASK, old, NULL);
}

/* Claims this thread's slot before the fork (start_begin's signals
 * blocked): 0 when the program is stopping, and then the caller does not
 * fork. With every slot taken the run goes on without one (the pool has
 * at most 1024 threads, so never in practice): that child is not stopped
 * by a crash. */
static int wait_claim(void) {
  int32_t i;
  t_wait_slot = -1;
  for (i = 0; i < WAIT_MAX; i++) {
    pid_t z = 0;
    if (__atomic_compare_exchange_n(&g_waits[i], &z, WAIT_STARTING, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      int32_t hi = __atomic_load_n(&g_waits_hi, __ATOMIC_RELAXED);
      while (hi < i + 1 && !__atomic_compare_exchange_n(&g_waits_hi, &hi, i + 1, 1, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      }
      t_wait_slot = i;
      break;
    }
  }
  if (__atomic_load_n(&g_stopping, __ATOMIC_SEQ_CST)) {
    if (t_wait_slot >= 0) {
      __atomic_store_n(&g_waits[t_wait_slot], 0, __ATOMIC_SEQ_CST);
      t_wait_slot = -1;
    }
    return 0;
  }
  return 1;
}

/* After the fork: the child's pid in the claimed slot, or the slot given
 * back when the fork failed (pid < 0). */
static void wait_begin(pid_t pid) {
  if (t_wait_slot >= 0) {
    __atomic_store_n(&g_waits[t_wait_slot], pid > 0 ? pid : 0, __ATOMIC_SEQ_CST);
    if (pid <= 0) {
      t_wait_slot = -1;
    }
  }
}

static void wait_end(void) {
  if (t_wait_slot >= 0) {
    __atomic_store_n(&g_waits[t_wait_slot], 0, __ATOMIC_SEQ_CST);
    t_wait_slot = -1;
  }
}
static volatile pid_t g_jobs[JOB_MAX];
static volatile int32_t g_njobs;
static int32_t g_job_status;
static int32_t g_job_signal;

/* The environment changes of a child, from an Avant Array(String) (NULL or
 * empty: none), made before the fork (no allocation in the child): an entry
 * NAME=VALUE sets NAME, an entry without '=' unsets it. The program is then
 * started directly, never through env(1): env reads any word with '=' before
 * the program as an assignment, so a program whose path holds '=' (a
 * TMPDIR such as .../AVANT_NURSERY=0/...) would not run and env would print
 * the environment instead, with status 0. */
static char **build_envs(void *env) {
  AvantArray *e = (AvantArray *)env;
  if (!e || e->size <= 0) {
    return NULL;
  }
  return build_argv("", env);
}

static void child_envs(char **envs) {
  int32_t i;
  if (!envs) {
    return;
  }
  for (i = 1; envs[i]; i++) {
    if (strchr(envs[i], '=')) {
      putenv(envs[i]);
    } else {
      unsetenv(envs[i]);
    }
  }
}

static int32_t run_child_env(const char *path, void *args, void *env, const char *out_path, const char *err_path, int no_stdin) {
  char **argv;
  char **envs;
  pid_t pid;
  int st = 0;
  sigset_t mask;
  if (!path) {
    return 1;
  }
  argv = build_argv(path, args);
  if (!argv) {
    return 1;
  }
  envs = build_envs(env);
  if (!envs && env && ((AvantArray *)env)->size > 0) {
    free_argv(argv);
    return 1;
  }
  fflush(stdout);
  fflush(stderr);
  if (!start_begin(&mask)) {
    pid = -1;
  } else if (!wait_claim()) {
    start_end(&mask);
    pid = -1;
  } else {
    pid = fork();
    if (pid == 0) {
      start_child(&mask);
      child_envs(envs);
      child_exec(path, argv, out_path, err_path, 0, no_stdin, -1);
    }
    wait_begin(pid);
    start_end(&mask);
  }
  if (pid < 0) {
    free_argv(argv);
    if (envs) {
      free_argv(envs);
    }
    return 1;
  }
  free_argv(argv);
  if (envs) {
    free_argv(envs);
  }
  /* The process wait is native (rule 2). */
  avant_gc_native_begin();
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      wait_end();
      avant_gc_native_end();
      return 1;
    }
  }
  avant_gc_native_end();
  wait_end();
  return exit_code_of(st);
}

static int32_t run_child(const char *path, void *args, const char *out_path, const char *err_path, int no_stdin) {
  return run_child_env(path, args, NULL, out_path, err_path, no_stdin);
}

/* The prelude's process_run and process_run_out: a program name without '/' is
 * looked up on PATH; stdout and
 * stderr go to out_path when it is not empty. The exit status of a
 * process that exited (0..255); -1 when it could not start
 * (avant_sys_stage(): 1 its output file, 2 its program; avant_sys_errno()
 * why); -2 when a signal killed it (avant_sys_signal()). The child
 * reports a failed start on a close-on-exec pipe, so "could not start"
 * is never confused with a program that exits 127. */
avant_int avant_sys_run(const char *path, void *args, const char *out_path) {
  char **argv;
  pid_t pid;
  int st = 0;
  int fds[2];
  int32_t msg[2];
  ssize_t got = 0;
  sigset_t mask;
  int e;
  g_sys_errno = 0;
  g_sys_stage = 0;
  g_sys_signal = 0;
  argv = build_argv(path ? path : "", args);
  if (!argv) {
    g_sys_errno = ENOMEM;
    g_sys_stage = 2;
    return -1;
  }
  if (pipe2(fds, O_CLOEXEC) != 0) {
    g_sys_errno = errno;
    g_sys_stage = 2;
    free_argv(argv);
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  if (!start_begin(&mask)) {
    pid = -1;
    errno = ECANCELED;
  } else if (!wait_claim()) {
    start_end(&mask);
    pid = -1;
    errno = ECANCELED;
  } else {
    pid = fork();
    if (pid == 0) {
      start_child(&mask);
      close(fds[0]);
      child_exec(argv[0], argv, out_path, NULL, 0, 0, fds[1]);
    }
    e = errno;
    wait_begin(pid);
    start_end(&mask);
    errno = e;
  }
  if (pid < 0) {
    g_sys_errno = errno;
    g_sys_stage = 2;
    close(fds[0]);
    close(fds[1]);
    free_argv(argv);
    return -1;
  }
  close(fds[1]);
  free_argv(argv);
  /* The pipe read and the process wait are native (a native wait
   * touches C memory only). */
  avant_gc_native_begin();
  for (;;) {
    got = read(fds[0], msg, sizeof msg);
    if (got >= 0 || errno != EINTR) {
      break;
    }
  }
  close(fds[0]);
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      wait_end();
      g_sys_errno = errno;
      g_sys_stage = 2;
      avant_gc_native_end();
      return -1;
    }
  }
  avant_gc_native_end();
  wait_end();
  if (got == (ssize_t)sizeof msg) {
    g_sys_stage = msg[0];
    g_sys_errno = msg[1];
    return -1;
  }
  if (WIFSIGNALED(st)) {
    g_sys_signal = WTERMSIG(st);
    return -2;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* `avant run` starts the program with this: the
 * exit status, 128 + n for signal n, 127 when it could not start, which
 * the command passes on as its own. */
avant_int avant_fs_run(const char *path, void *args) {
  return run_child(path, args, NULL, NULL, 0);
}

/* Starts a job without waiting: path with args, stdout and stderr to
 * out_path, stdin /dev/null, its own process group. Returns its pid, or
 * -1 when it cannot start. Internal to the test runner. */
avant_int avant_job_start(const char *path, void *args, const char *out_path) {
  return avant_job_start_env(path, args, NULL, out_path);
}

/* avant_job_start with environment changes (build_envs: NAME=VALUE sets,
 * NAME unsets), the program started directly. */
avant_int avant_job_start_env(const char *path, void *args, void *env, const char *out_path) {
  char **argv;
  char **envs;
  pid_t pid;
  int32_t n;
  sigset_t mask;
  if (!path || g_njobs >= JOB_MAX) {
    return -1;
  }
  argv = build_argv(path, args);
  if (!argv) {
    return -1;
  }
  envs = build_envs(env);
  if (!envs && env && ((AvantArray *)env)->size > 0) {
    free_argv(argv);
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  /* The job's entry is claimed before the fork, as a blocking run's slot
   * is (wait_claim), and takes the pid once its process group exists (the
   * handler stops a job by its group). Jobs are started and waited for on
   * one thread, so only the handler reads the entry meanwhile. */
  pid = -1;
  if (start_begin(&mask)) {
    n = g_njobs;
    __atomic_store_n(&g_jobs[n], WAIT_STARTING, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_njobs, n + 1, __ATOMIC_SEQ_CST);
    t_job_starting = 1;
    if (!__atomic_load_n(&g_stopping, __ATOMIC_SEQ_CST)) {
      pid = fork();
      if (pid == 0) {
        start_child(&mask);
        child_envs(envs);
        child_exec(path, argv, out_path, NULL, 1, 1, -1);
      }
    }
    if (pid > 0) {
      setpgid(pid, pid);
      __atomic_store_n(&g_jobs[n], pid, __ATOMIC_SEQ_CST);
    } else {
      __atomic_store_n(&g_njobs, n, __ATOMIC_SEQ_CST);
      __atomic_store_n(&g_jobs[n], 0, __ATOMIC_SEQ_CST);
    }
    t_job_starting = 0;
    start_end(&mask);
  }
  free_argv(argv);
  if (envs) {
    free_argv(envs);
  }
  return pid > 0 ? (int32_t)pid : -1;
}

/* Waits for any started job to finish. Returns its pid, or -1 when no job
 * is running; avant_job_status then gives its exit code (128+n for a
 * signal) and avant_job_signal the signal that ended it (0 when it
 * exited), so the runner can tell `exit 141` from SIGPIPE. */
avant_int avant_job_wait(void) {
  for (;;) {
    int st = 0;
    int32_t i;
    pid_t pid;
    if (g_njobs == 0) {
      return -1;
    }
    /* The process wait is native (rule 2). */
    avant_gc_native_begin();
    pid = waitpid(-1, &st, 0);
    avant_gc_native_end();
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    for (i = 0; i < g_njobs; i++) {
      if (g_jobs[i] == pid) {
        g_jobs[i] = g_jobs[g_njobs - 1];
        g_njobs = g_njobs - 1;
        g_job_status = exit_code_of(st);
        g_job_signal = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
        return (int32_t)pid;
      }
    }
  }
}

/* avant_job_wait that gives up after ms milliseconds: the pid of the job
 * that finished, 0 when none did in that time, -1 when no job is running.
 * The runner uses it to look at the clock between jobs' ends. The wait
 * polls waitpid with short sleeps (1 ms, doubling to 20 ms): a job's end
 * is noticed within 20 ms, and a runner with a few dozen jobs spends
 * nothing visible. */
avant_int avant_job_wait_for(avant_int ms) {
  struct timespec pause = {0, 1000 * 1000};
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    int st = 0;
    int32_t i;
    pid_t pid;
    struct timespec t1;
    if (g_njobs == 0) {
      return -1;
    }
    pid = waitpid(-1, &st, WNOHANG);
    if (pid < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (pid > 0) {
      for (i = 0; i < g_njobs; i++) {
        if (g_jobs[i] == pid) {
          g_jobs[i] = g_jobs[g_njobs - 1];
          g_njobs = g_njobs - 1;
          g_job_status = exit_code_of(st);
          g_job_signal = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
          return (int32_t)pid;
        }
      }
      continue;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if ((int64_t)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 >= ms) {
      return 0;
    }
    /* The process wait is native (rule 2): the sleep is too. */
    avant_gc_native_begin();
    nanosleep(&pause, NULL);
    avant_gc_native_end();
    if (pause.tv_nsec < 20 * 1000 * 1000) {
      pause.tv_nsec *= 2;
    }
  }
}

/* Sends signal sig to the process group of a running job (each job is the
 * leader of its own group, so its children are reached too): 0 when sent,
 * -1 when pid is not a running job. */
avant_int avant_job_stop(avant_int pid, avant_int sig) {
  int32_t i;
  for (i = 0; i < g_njobs; i++) {
    if (g_jobs[i] == (pid_t)pid) {
      return kill(-(pid_t)pid, (int)sig) == 0 ? 0 : -1;
    }
  }
  return -1;
}

avant_int avant_job_status(void) {
  return g_job_status;
}

avant_int avant_job_signal(void) {
  return g_job_signal;
}

/* A blocking run of a tool the compiler starts (myc-llvm, cc, re2c, the
 * bind helper, git): like process_run, with stdout (and stderr, unless
 * err_path names its own file) to out_path when given, and stdin from
 * /dev/null. Internal (no prelude name). */
avant_int avant_tool_run(const char *path, void *args, const char *out_path, const char *err_path) {
  return run_child(path, args, out_path, err_path, 1);
}

/* A blocking run with stdout and stderr in separate files: what
 * std.testing's run_golden and rejects and bind's output split asked of
 * sh. Like process_run, it forwards a signal to its child. Internal. */
avant_int avant_process_run_split(const char *path, void *args, const char *out_path, const char *err_path) {
  return run_child(path, args, out_path, err_path, 0);
}

/* avant_process_run_split with environment changes (build_envs), the
 * program started directly. */
avant_int avant_process_run_split_env(const char *path, void *args, void *env, const char *out_path, const char *err_path) {
  return run_child_env(path, args, env, out_path, err_path, 0);
}

/* Internal file helpers, bound with lib/fun, no prelude name: the runner, the
 * harness, std.testing, and the compiler use them
 * instead of starting /bin/rm, chmod, mkdir, or sh -c pwd. Each returns 0
 * on success, 1 otherwise. */

/* rm -f: 0 when the path is gone, including when it never existed. */
avant_int avant_fs_unlink(const char *path) {
  if (!path || !path[0]) {
    return 0;
  }
  if (unlink(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return 1;
}

/* chmod +x for the usual case: execute wherever read is granted, so the
 * umask that shaped the read bits shapes the execute bits too. */
avant_int avant_fs_chmod_x(const char *path) {
  struct stat st;
  mode_t add = 0;
  if (!path || stat(path, &st) != 0) {
    return 1;
  }
  if (st.st_mode & S_IRUSR) {
    add |= S_IXUSR;
  }
  if (st.st_mode & S_IRGRP) {
    add |= S_IXGRP;
  }
  if (st.st_mode & S_IROTH) {
    add |= S_IXOTH;
  }
  return chmod(path, (st.st_mode & 07777) | add) == 0 ? 0 : 1;
}

/* mkdir -p, mode 0777 under the umask. */
avant_int avant_fs_mkdir_p(const char *path) {
  char buf[4096];
  size_t n;
  size_t i;
  struct stat st;
  if (!path || !path[0]) {
    return 1;
  }
  n = strlen(path);
  if (n >= sizeof buf) {
    return 1;
  }
  memcpy(buf, path, n + 1);
  for (i = 1; i <= n; i++) {
    if (buf[i] == '/' || buf[i] == 0) {
      char keep = buf[i];
      buf[i] = 0;
      if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return 1;
      }
      buf[i] = keep;
    }
  }
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : 1;
}

/* cp: copies src to dst byte for byte, with src's permission bits, through
 * a temporary beside dst renamed into place, so dst is never half
 * written (a gate keeps the fixed point's next compiler this way). It
 * copies through stdio with a 64 KB buffer. (An Avant program's own
 * fn read or fn write cannot replace libc's: Avant functions are emitted
 * internal, under an av. prefix.) */
avant_int avant_fs_copy(const char *src, const char *dst) {
  char tmp[4096];
  char buf[65536];
  struct stat st;
  FILE *in;
  FILE *out;
  size_t n;
  int bad = 0;
  if (!src || !dst || snprintf(tmp, sizeof tmp, "%s.part-%ld", dst, (long)getpid()) >= (int)sizeof tmp) {
    return 1;
  }
  if (stat(src, &st) != 0) {
    return 1;
  }
  in = fopen(src, "rb");
  if (!in) {
    return 1;
  }
  out = fopen(tmp, "wb");
  if (!out) {
    fclose(in);
    return 1;
  }
  /* The copy is native (rule 2): C buffers only; dst is
   * read again after it, from a handle (a collection may move it). */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &dst);
  avant_gc_native_begin();
  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      bad = 1;
      break;
    }
  }
  if (ferror(in)) {
    bad = 1;
  }
  fclose(in);
  if (fclose(out) != 0) {
    bad = 1;
  }
  avant_gc_native_end();
  if (bad || chmod(tmp, st.st_mode & 07777) != 0 || rename(tmp, dst) != 0) {
    unlink(tmp);
    return 1;
  }
  return 0;
}

/* The identity of a file for the safepoint probe cache: device,
 * inode, size, and change time to the nanosecond, or "" when it cannot be
 * read. Replacing or rewriting the file changes it. */
char *avant_file_identity(const char *path) {
  struct stat st;
  char buf[160];
  int n;
  if (!path || stat(path, &st) != 0) {
    return dup_bytes("", 0);
  }
  n = snprintf(buf, sizeof buf, "%llu:%llu:%lld:%lld.%09ld", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, (long long)st.st_size, (long long)st.st_ctim.tv_sec, (long)st.st_ctim.tv_nsec);
  if (n < 0 || (size_t)n >= sizeof buf) {
    return dup_bytes("", 0);
  }
  return dup_bytes(buf, (size_t)n);
}

/* Sets an environment variable for this process and the children it
 * starts. Internal plumbing (the probe cache), no prelude name. */
void avant_env_set(const char *name, const char *value) {
  if (name && name[0] && value) {
    setenv(name, value, 1);
  }
}

/* pwd -P: the physical working directory, or "." when it cannot be read. */
/* gc-lint: holds nothing (C memory only). */
char *avant_fs_cwd(void) {
  char *cwd = getcwd(NULL, 0);
  char *out;
  if (!cwd) {
    return dup_bytes(".", 1);
  }
  out = dup_bytes(cwd, strlen(cwd));
  free(cwd);
  return out;
}

/* gc-lint: holds nothing (C memory only; the name is not read after dup_bytes). */
char *avant_env_get(const char *name, int32_t *ok) {
  if (ok) {
    *ok = 0;
  }
  if (!name) {
    return NULL;
  }
  /* A terminated name (avant_file_exists). */
  name = avant_str_flat(name);
  const char *s = getenv(name);
  if (!s) {
    return NULL;
  }
  size_t n = strlen(s);
  char *heap = dup_bytes(s, n);
  if (ok) {
    *ok = 1;
  }
  return heap;
}

/* gc-lint: holds nothing (the flat copy is its first call; only it is read after). */
int32_t avant_file_exists(const char *path) {
  if (!path) {
    return 0;
  }
  /* A terminated path: a view's bytes are copied first (it allocates;
   * nothing else is held). */
  path = avant_str_flat(path);
  return access(path, F_OK) == 0 ? 1 : 0;
}

static int cmp_names(const void *a, const void *b) {
  const char *sa = *(char *const *)a;
  const char *sb = *(char *const *)b;
  return strcmp(sa, sb);
}

/* The names in directory path, except . and .., sorted, in C memory:
 * 0 with *out and *np set, or the errno that stopped the listing (a name that
 * cannot be stored is ENOMEM, never a shorter list). */
/* gc-lint: no reference word (names is malloc'ed C memory). */
static int list_names(const char *path, char ***out, int32_t *np) {
  char **names = NULL;
  int32_t n = 0;
  int32_t cap = 0;
  int err = 0;
  struct dirent *ent;
  DIR *d = opendir(path);
  *out = NULL;
  *np = 0;
  if (!d) {
    return errno;
  }
  for (;;) {
    errno = 0;
    ent = readdir(d);
    if (!ent) {
      err = errno;
      break;
    }
    const char *name = ent->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
      continue;
    }
    if (n >= cap) {
      /* An Array of names stays within the ceiling. */
      if (cap > AVANT_SIZE_MAX / 2) {
        err = EOVERFLOW;
        break;
      }
      int32_t next = cap == 0 ? 16 : cap * 2;
      char **grown = (char **)realloc(names, (size_t)next * sizeof(char *));
      if (!grown) {
        err = ENOMEM;
        break;
      }
      names = grown;
      cap = next;
    }
    size_t len = strlen(name);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
      err = ENOMEM;
      break;
    }
    memcpy(copy, name, len + 1);
    names[n] = copy;
    n += 1;
  }
  closedir(d);
  if (err) {
    int32_t i;
    for (i = 0; i < n; i++) {
      free(names[i]);
    }
    free(names);
    return err;
  }
  if (n > 1) {
    qsort(names, (size_t)n, sizeof(char *), cmp_names);
  }
  *out = names;
  *np = n;
  return 0;
}

/* An Array(String) of the n names, which it frees. */
static void *names_array(char **names, int32_t n) {
  AvantArray *a = NULL;
  /* RC1: the array is a handle across the allocations. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &a);
  a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  if (n > 0) {
    int32_t i;
    io_array_buf(&a, n);
    for (i = 0; i < n; i++) {
      char *p = dup_bytes(names[i], strlen(names[i]));
      io_array_put(a, p);
      free(names[i]);
    }
  }
  free(names);
  return a;
}

/* The prelude's dir_list: the sorted names, or
 * NULL with avant_sys_errno() telling why (not found, not a directory,
 * permission denied, out of memory). */
/* gc-lint: holds nothing (the path is read before names_array allocates). */
void *avant_sys_list(const char *path) {
  char **names;
  int32_t n;
  int err = list_names(path ? path : "", &names, &n);
  g_sys_errno = err;
  if (err) {
    return NULL;
  }
  return names_array(names, n);
}

/* ---- Scratch directory ------------------------------------------
 *
 * One private directory per process, created on first use with mkdtemp
 * under $TMPDIR (else /tmp) and named avant-XXXXXX. Every temporary path
 * the compiler, the test runner, and std.testing produce lives inside it.
 * TMPDIR is then exported, so every child (nested Avant processes and
 * third-party tools alike: myc-llvm, cc, re2c, git) nests inside it, and
 * one removal at the top cleans the whole tree. An empty .avant-scratch
 * marker in each directory tells a child that it is nested.
 *
 * Removal runs at exit (atexit covers normal and error exits, traps
 * included) and on SIGINT, SIGTERM, SIGHUP and SIGPIPE, after which the
 * signal is re-raised so the status stays 128+n. Only the creating process removes:
 * a forked child that never execs must not delete its parent's tree. A
 * crash or kill -9 leaves one directory. AVANT_KEEP_TMP=1 (set by the
 * compiler's --keep-tmp) keeps it; the top-level process prints the path.
 *
 * The removal walk uses only async-signal-safe calls (openat, getdents64,
 * unlinkat, close, lseek), so the signal handler and atexit share it. */

#define TMP_PATH_MAX 4064

static pthread_mutex_t g_tmp_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_tmp_dir[TMP_PATH_MAX];
static int g_tmp_ready;
static int g_tmp_keep;
static int g_tmp_nested;
static pid_t g_tmp_owner;
static unsigned g_tmp_seq;

struct tmp_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

static int tmp_is_dot(const char *n) {
  return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

/* Empties the directory open at fd, depth-first. Deleting while reading
 * can skip entries, so each level re-reads from the start until a pass
 * finds nothing left to delete. */
static void tmp_empty_dir(int fd, int depth) {
  char buf[2048];
  int again = 1;
  int passes = 0;
  if (depth > 64) {
    return;
  }
  while (again && passes < 16) {
    again = 0;
    passes++;
    lseek(fd, 0, SEEK_SET);
    for (;;) {
      long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
      long off = 0;
      if (n <= 0) {
        break;
      }
      while (off < n) {
        struct tmp_dirent64 *d = (struct tmp_dirent64 *)(buf + off);
        off += d->d_reclen;
        if (tmp_is_dot(d->d_name)) {
          continue;
        }
        again = 1;
        if (unlinkat(fd, d->d_name, 0) == 0) {
          continue;
        }
        if (errno == EISDIR || errno == EPERM) {
          int sub = openat(fd, d->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          if (sub >= 0) {
            tmp_empty_dir(sub, depth + 1);
            close(sub);
          }
          unlinkat(fd, d->d_name, AT_REMOVEDIR);
        }
      }
    }
  }
}

/* rm -rf: a file or a symbolic link is unlinked; a directory is
 * emptied depth-first without following links, then removed. 0 when the
 * path is gone. */
avant_int avant_fs_remove_tree(const char *path) {
  int fd;
  if (!path || !path[0]) {
    return 0;
  }
  if (unlink(path) == 0 || errno == ENOENT) {
    return 0;
  }
  if (errno != EISDIR && errno != EPERM) {
    return 1;
  }
  fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return 1;
  }
  tmp_empty_dir(fd, 0);
  close(fd);
  if (rmdir(path) == 0 || errno == ENOENT) {
    return 0;
  }
  return 1;
}

/* A child that outlives this process's own work (a job's shell writing its
 * status file after a signal reached only us) can add a file between the
 * last emptying pass and rmdir. Once rmdir succeeds nothing can recreate
 * the directory, so empty and rmdir again, pausing 10 ms, for up to about
 * a second. nanosleep is async-signal-safe. */
static void tmp_remove(void) {
  int attempt;
  struct timespec pause = {0, 10 * 1000 * 1000};
  for (attempt = 0; attempt < 100; attempt++) {
    int fd = open(g_tmp_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
      tmp_empty_dir(fd, 0);
      close(fd);
    }
    if (rmdir(g_tmp_dir) == 0 || errno == ENOENT) {
      return;
    }
    if (errno != ENOTEMPTY && errno != EEXIST) {
      return;
    }
    nanosleep(&pause, NULL);
  }
}

static void tmp_say_kept(void) {
  static const char a[] = "avant: kept temporary directory ";
  size_t n = strlen(g_tmp_dir);
  ssize_t w;
  w = write(2, a, sizeof a - 1);
  w = write(2, g_tmp_dir, n);
  w = write(2, "\n", 1);
  (void)w;
}

static void tmp_finish(void) {
  if (!g_tmp_ready || getpid() != g_tmp_owner) {
    return;
  }
  g_tmp_ready = 0;
  if (g_tmp_keep) {
    if (!g_tmp_nested) {
      tmp_say_kept();
    }
    return;
  }
  tmp_remove();
}

/* Stops what this process started before it ends by a crash, a trap
 * or a signal, and before its directory goes (avant_stop_children):
 * SIGTERM to every job's group and to every child a blocking run waits
 * on (one per waiting task), then reap them for up to about two seconds,
 * then SIGKILL whatever is left and reap briefly. Async-signal-safe
 * calls only. It first marks the program as stopping, so no thread
 * starts a child after the slots are read, then reads the waited pids
 * and the jobs once, before the first kill; a slot or a job another
 * thread is starting a child for (WAIT_STARTING) is waited for, about two
 * seconds at most, until it holds the pid (that thread has the handler's
 * signals blocked and is between its fork and its next store). */
static pid_t start_settled(volatile pid_t *at) {
  struct timespec pause = {0, 1000 * 1000};
  int round;
  pid_t p = __atomic_load_n(at, __ATOMIC_SEQ_CST);
  for (round = 0; p == WAIT_STARTING && round < 2000; round++) {
    nanosleep(&pause, NULL);
    p = __atomic_load_n(at, __ATOMIC_SEQ_CST);
  }
  return p;
}

static void tmp_stop_children(void) {
  struct timespec pause = {0, 10 * 1000 * 1000};
  static pid_t kids[WAIT_MAX];
  static pid_t jobs[JOB_MAX];
  int32_t i;
  int32_t nk = 0;
  int32_t n = 0;
  int32_t hi;
  int32_t nj;
  int round;
  __atomic_store_n(&g_stopping, 1, __ATOMIC_SEQ_CST);
  hi = __atomic_load_n(&g_waits_hi, __ATOMIC_SEQ_CST);
  for (i = 0; i < hi && i < WAIT_MAX; i++) {
    pid_t p = __atomic_load_n(&g_waits[i], __ATOMIC_SEQ_CST);
    if (p == WAIT_STARTING && i != t_wait_slot) {
      p = start_settled(&g_waits[i]);
    }
    if (p > 0) {
      kids[nk++] = p;
    }
  }
  nj = __atomic_load_n(&g_njobs, __ATOMIC_SEQ_CST);
  for (i = 0; i < nj && i < JOB_MAX; i++) {
    pid_t p = t_job_starting ? __atomic_load_n(&g_jobs[i], __ATOMIC_SEQ_CST) : start_settled(&g_jobs[i]);
    if (p > 0) {
      jobs[n++] = p;
    }
  }
  if (n == 0 && nk == 0) {
    return;
  }
  for (i = 0; i < n; i++) {
    kill(-jobs[i], SIGTERM);
  }
  for (i = 0; i < nk; i++) {
    kill(kids[i], SIGTERM);
  }
  for (round = 0; round < 250; round++) {
    int st;
    pid_t got;
    int live = 0;
    while ((got = waitpid(-1, &st, WNOHANG)) > 0) {
    }
    for (i = 0; i < n; i++) {
      if (kill(-jobs[i], 0) == 0) {
        live = 1;
      }
    }
    for (i = 0; i < nk; i++) {
      if (kill(kids[i], 0) == 0) {
        live = 1;
      }
    }
    if (!live) {
      return;
    }
    if (round == 200) {
      for (i = 0; i < n; i++) {
        kill(-jobs[i], SIGKILL);
      }
      for (i = 0; i < nk; i++) {
        kill(kids[i], SIGKILL);
      }
    }
    nanosleep(&pause, NULL);
  }
}

/* Standard output and standard error. A write that
 * fails is never silent: puts checks the stream after every write
 * (avant_out_check), and at exit a failed flush, or an error a write
 * left on the stream, stops the program with a message naming the
 * stream, status 1, as a trap does. */
static int g_out_failed;

__attribute__((noreturn)) static void out_fail(const char *stream, int e, int at_exit) {
  g_out_failed = 1;
  fprintf(stderr, "avant runtime: cannot write to %s: %s\n", stream, e ? strerror(e) : "an earlier write was lost");
  if (at_exit) {
    _exit(1);
  }
  exit(1);
}

/* stdout's lock for the exit's flush, waited for a second at most. Two
 * flushes of one buffer at once write its bytes twice: a task that traps
 * flushes stdout on its own thread (the trap is told at once, after the
 * program's output) while main returns and the exit flushes it. So the
 * exit takes the lock, flushes, and keeps it to the end of the process:
 * no other thread writes or flushes stdout after the exit's flush. A
 * thread that keeps the lock longer (a write blocked on a full pipe)
 * gets the old behavior, an unlocked flush, so the exit cannot hang.
 * The lock is recursive: this thread's later stdio (glibc's own exit
 * flush, another exit handler) goes on. Returns whether it is held. */
static int out_lock_at_exit(void) {
  struct timespec pause = {0, 1000000};
  int i;
  for (i = 0; i < 1000; i++) {
    if (ftrylockfile(stdout) == 0) {
      return 1;
    }
    nanosleep(&pause, NULL);
  }
  return 0;
}

/* The stream's error flag set by this write or an earlier one (printf
 * included): flushing again what is buffered names the reason. At exit
 * the flush holds stdout's lock when it can (out_lock_at_exit). */
static void out_check(int at_exit) {
  int e = 0;
  int bad;
  if (g_out_failed) {
    return;
  }
  if (at_exit) {
    (void)out_lock_at_exit();
  }
  bad = at_exit ? fflush_unlocked(stdout) != 0 : fflush(stdout) != 0;
  if (bad) {
    e = errno;
  } else if (!ferror_unlocked(stdout)) {
    if (ferror_unlocked(stderr)) {
      out_fail("standard error", 0, at_exit);
    }
    return;
  }
  out_fail("standard output", e, at_exit);
}

/* puts's check, after its write (avant_str.c). The unlocked read of the
 * flag is the fast path: a write that failed sets it for good. */
void avant_out_check(void) {
  if (ferror_unlocked(stdout)) {
    out_check(0);
  }
}

static void out_check_at_exit(void) {
  out_check(1);
}

/* An Avant program's SIGPIPE has the default action whatever its parent
 * left: an inherited SIG_IGN (a CI
 * runner, a shell's trap '' PIPE) or a blocked SIGPIPE is reset
 * before main, so `prog | head` ends the program the same way under any
 * parent, status 141, and tmp_catch below then removes the private
 * directory on it. std.http's sockets never raise it (every socket write
 * carries MSG_NOSIGNAL: uSockets' bsd_send and bsd_write2). The exit
 * check is registered first, so it runs after tmp_finish. */
static void tmp_catch(int sig);

/* The process the program started as: the children it waits on are
 * stopped only by it, never by a forked child that has not exec'd yet
 * (avant_stop_children). */
static pid_t g_io_pid;

/* `avant test`'s report that a test process ran: when the runner names a
 * file in AVANT_TEST_RESULTS, the program writes `exit` to it as it exits
 * (normally or by exit()), so a job that exits 0 without having been this
 * program (a launcher that ran nothing, as env(1) given a path holding
 * '=') is told from a pass. The variable is removed from the process's
 * environment at start, so the programs it runs do not write the file. */
static char g_test_results[4096];

static void test_results_at_exit(void) {
  int fd = open(g_test_results, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd >= 0) {
    ssize_t w = write(fd, "exit\n", 5);
    (void)w;
    close(fd);
  }
}

__attribute__((constructor)) static void avant_io_start(void) {
  struct sigaction sa;
  sigset_t set;
  const char *results = getenv("AVANT_TEST_RESULTS");
  if (results && results[0] && strlen(results) < sizeof g_test_results) {
    memcpy(g_test_results, results, strlen(results) + 1);
    atexit(test_results_at_exit);
  }
  unsetenv("AVANT_TEST_RESULTS");
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPIPE, &sa, NULL);
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  sigprocmask(SIG_UNBLOCK, &set, NULL);
  atexit(out_check_at_exit);
  g_io_pid = getpid();
  /* A program ended by SIGTERM or SIGINT (kill, Ctrl-C) keeps what it
   * printed: the handler writes stdout's buffer before the default
   * action (tmp_on_signal). Every signal that ends it (these, SIGHUP, a
   * closed pipe) stops the children it waits on first, whether or not
   * it made its private directory. Only a default action is replaced. */
  tmp_catch(SIGINT);
  tmp_catch(SIGTERM);
  tmp_catch(SIGHUP);
  tmp_catch(SIGPIPE);
}

/* Every way a program ends other than its main returning (a crash, a
 * trap, a signal) stops the children it waits on, with or without its
 * private directory: a child left running keeps the program's pipes
 * open (`prog | cat` would wait on it). Only the process the program
 * started as stops them. */
void avant_stop_children(void) {
  if (g_io_pid != 0 && getpid() == g_io_pid) {
    tmp_stop_children();
  }
}

/* The private directory's cleanup on a crash, from a fault handler that
 * ends the process itself (avant_stack.c): the same work as tmp_on_signal
 * before its re-raise. */
void avant_tmp_on_crash(void) {
  avant_stop_children();
  tmp_finish();
}

/* A signal that ends the program (SIGTERM, SIGINT, SIGHUP, a closed
 * pipe, a crash the fault handler does not take): what the program
 * printed and stdout still buffers is written first, as a crash's is
 * (avant_out_pending: the bytes up to the buffer's write pointer, with
 * write(2), since the signal may stop a write half way), then the
 * private directory's cleanup, then the signal's default action. */
static void tmp_on_signal(int sig) {
  struct sigaction sa;
  if (sig != SIGPIPE) {
    avant_out_pending();
  }
  avant_stop_children();
  tmp_finish();
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
  raise(sig);
}

/* The cleanup's handler, so the fault handler (avant_stack.c) can tell
 * it from a C library's own when it was installed first. */
void (*avant_tmp_crash_handler(void))(int) {
  return tmp_on_signal;
}

/* An ignored signal stays ignored (nohup, a parent's choice; SIGPIPE is
 * reset to the default before, avant_io_start); a handler
 * someone else installed stays theirs. Only the default is replaced. */
static void tmp_catch(int sig) {
  struct sigaction old;
  struct sigaction sa;
  if (sigaction(sig, NULL, &old) != 0) {
    return;
  }
  if (old.sa_handler != SIG_DFL || (old.sa_flags & SA_SIGINFO)) {
    return;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = tmp_on_signal;
  sigfillset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
}

/* std.process.notify: the signals a program asked to see (SIGTERM and
 * SIGINT, so a server can close and drain, and the others it names). The
 * handler writes the signal's number to a pipe, the one thing a handler
 * may do with an Avant program's state (it cannot send on a Channel):
 * one pipe for each notify call, and the process.Signals that call
 * returns reads it in recv (avant_sys_signal_wait, waiting natively, on
 * whichever task calls it), with no reader task and no channel. Such
 * a signal no longer ends the program: its default action is replaced,
 * the scratch directory's handler (tmp_catch) included, and the program
 * decides (the directory is still removed at exit). The crash signals
 * are not given here (avant_sys_signal_notify refuses them). */
#define SIG_SLOTS 16
typedef struct SigSlot {
  int fd[2];
  volatile uint32_t mask; /* bit n: signal n */
} SigSlot;
static SigSlot g_sig_slots[SIG_SLOTS];
static volatile int g_sig_nslots;
static pthread_mutex_t g_sig_mu = PTHREAD_MUTEX_INITIALIZER;

/* Each notify call's pipe that asked for sig gets its number. */
static void sig_on_notify(int sig) {
  int saved = errno;
  unsigned char b = (unsigned char)sig;
  int i;
  int n = g_sig_nslots;
  for (i = 0; i < n; i++) {
    if (g_sig_slots[i].mask & (1u << sig)) {
      ssize_t w = write(g_sig_slots[i].fd[1], &b, 1);
      (void)w; /* a full pipe drops it: 65536 signals are waiting */
    }
  }
  errno = saved;
}

/* A notify call's slot for the signals of mask (bit n: signal n): its
 * pipe, then each signal's handler. -1 for a signal that is not a
 * catchable one or is a crash signal the runtime keeps, -2 when the
 * slots (16) or pipes are used up. */
avant_int avant_sys_signal_notify(avant_int mask) {
  static const int refused[] = {SIGKILL, SIGSTOP, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP};
  struct sigaction sa;
  int slot;
  int sig;
  size_t k;
  if (mask <= 1 || (uint64_t)mask >= (1ull << 32)) {
    return -1;
  }
  for (k = 0; k < sizeof refused / sizeof refused[0]; k++) {
    if (mask & (1ll << refused[k])) {
      return -1;
    }
  }
  pthread_mutex_lock(&g_sig_mu);
  slot = g_sig_nslots;
  if (slot >= SIG_SLOTS || pipe(g_sig_slots[slot].fd) != 0) {
    pthread_mutex_unlock(&g_sig_mu);
    return -2;
  }
  fcntl(g_sig_slots[slot].fd[0], F_SETFD, FD_CLOEXEC);
  fcntl(g_sig_slots[slot].fd[1], F_SETFD, FD_CLOEXEC);
  fcntl(g_sig_slots[slot].fd[1], F_SETFL, O_NONBLOCK);
  g_sig_slots[slot].mask = (uint32_t)mask;
  __atomic_store_n(&g_sig_nslots, slot + 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_sig_mu);
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = sig_on_notify;
  sa.sa_flags = SA_RESTART;
  sigfillset(&sa.sa_mask);
  for (sig = 1; sig < 32; sig++) {
    if (mask & (1ll << sig)) {
      sigaction(sig, &sa, NULL);
    }
  }
  return slot;
}

/* The next signal of the slot's, its number; waits natively (the
 * collector runs without this thread, and the task stops counting as
 * running in the pool). */
avant_int avant_sys_signal_wait(avant_int slot) {
  unsigned char b = 0;
  ssize_t r;
  if (slot < 0 || slot >= g_sig_nslots) {
    return 0;
  }
  avant_task_wait_begin();
  avant_gc_native_begin();
  do {
    r = read(g_sig_slots[slot].fd[0], &b, 1);
  } while (r < 0 && errno == EINTR);
  avant_gc_native_end();
  avant_task_wait_end();
  return r == 1 ? (avant_int)b : 0;
}

static int tmp_env_on(const char *name) {
  const char *v = getenv(name);
  return v && v[0] && !(v[0] == '0' && v[1] == 0);
}

static void tmp_die(const char *what, const char *path) {
  fprintf(stderr, "avant: cannot %s %s: %s\n", what, path, strerror(errno));
  exit(1);
}

static void tmp_create(void) {
  const char *base = getenv("TMPDIR");
  char marker[TMP_PATH_MAX + 32];
  size_t n;
  int fd;
  if (!base || !base[0]) {
    base = "/tmp";
  }
  n = strlen(base);
  while (n > 1 && base[n - 1] == '/') {
    n--;
  }
  if (snprintf(marker, sizeof marker, "%.*s/.avant-scratch", (int)n, base) >= (int)sizeof marker) {
    errno = ENAMETOOLONG;
    tmp_die("use TMPDIR", base);
  }
  g_tmp_nested = access(marker, F_OK) == 0;
  if (snprintf(g_tmp_dir, sizeof g_tmp_dir, "%.*s/avant-XXXXXX", (int)n, base) >= (int)sizeof g_tmp_dir) {
    errno = ENAMETOOLONG;
    tmp_die("use TMPDIR", base);
  }
  if (!mkdtemp(g_tmp_dir)) {
    tmp_die("create a temporary directory under", base);
  }
  g_tmp_owner = getpid();
  g_tmp_keep = tmp_env_on("AVANT_KEEP_TMP");
  g_tmp_ready = 1;
  atexit(tmp_finish);
  tmp_catch(SIGINT);
  tmp_catch(SIGTERM);
  tmp_catch(SIGHUP);
  /* A closed pipe on standard output (`prog | head`) is an ordinary way
   * for a command-line program to end: the
   * directory goes and the program still dies by SIGPIPE, status 141
   * (SIGPIPE keeps its default action; avant_io_start resets an
   * inherited SIG_IGN). std.http's sockets never raise it (MSG_NOSIGNAL). */
  tmp_catch(SIGPIPE);
  /* A crash removes the directory too: the handler
   * cleans up and re-raises the signal with its default action, so the
   * process still dies by it (a core dump included). SIGSEGV and SIGBUS
   * already have the fault handler of avant_stack.c (installed when the
   * main thread registers, on the thread's signal stack), which names the
   * fault or the stack overflow and cleans up the same way; these calls
   * leave it in place. */
  tmp_catch(SIGSEGV);
  tmp_catch(SIGBUS);
  tmp_catch(SIGFPE);
  tmp_catch(SIGILL);
  tmp_catch(SIGABRT);
  snprintf(marker, sizeof marker, "%s/.avant-scratch", g_tmp_dir);
  fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0) {
    close(fd);
  }
  setenv("TMPDIR", g_tmp_dir, 1);
}

/* The process's scratch directory, created on first call. */
char *avant_tmp_dir(void) {
  pthread_mutex_lock(&g_tmp_lock);
  if (!g_tmp_ready || g_tmp_owner != getpid()) {
    tmp_create();
  }
  pthread_mutex_unlock(&g_tmp_lock);
  return dup_bytes(g_tmp_dir, strlen(g_tmp_dir));
}

/* A fresh, created directory inside the scratch directory (std.testing's
 * temp_dir). */
char *avant_tmp_new_dir(void) {
  char path[TMP_PATH_MAX + 32];
  pthread_mutex_lock(&g_tmp_lock);
  if (!g_tmp_ready || g_tmp_owner != getpid()) {
    tmp_create();
  }
  for (;;) {
    g_tmp_seq++;
    snprintf(path, sizeof path, "%s/d%u", g_tmp_dir, g_tmp_seq);
    if (mkdir(path, 0700) == 0) {
      break;
    }
    if (errno != EEXIST) {
      tmp_die("create a directory", path);
    }
  }
  pthread_mutex_unlock(&g_tmp_lock);
  return dup_bytes(path, strlen(path));
}

/* --keep-tmp: keep this process's directory and every nested one. */
void avant_tmp_keep(void) {
  pthread_mutex_lock(&g_tmp_lock);
  g_tmp_keep = 1;
  setenv("AVANT_KEEP_TMP", "1", 1);
  pthread_mutex_unlock(&g_tmp_lock);
}

/* A failed check fails the process. std.testing's
 * Test and the suite's harness call avant_test_fail at every failed check;
 * a program that then ends with status 0 (a void main, a main that
 * returns 0, a main that never calls finish) exits 1 instead. The handler
 * is registered before main, so it runs after every exit handler the
 * runtime registers later (statistics, the verifier, coverage, the
 * temporary directory), flushes stdio as exit would, and ends the process
 * with status 1. A non-zero status is kept. Any thread may fail a check. */
static int g_test_failed;

void avant_test_fail(void) {
  __atomic_store_n(&g_test_failed, 1, __ATOMIC_RELAXED);
}

/* std.testing's report lines (testing_write): every byte of S written to
 * standard output (a short write goes
 * on, an interrupted one is retried); a write that fails stops the
 * process as puts does (out_fail: "avant runtime: cannot write to
 * standard output: REASON", status 1). Before this, testing_write
 * called write(2) itself and ignored its result, so a test process whose
 * standard output failed lost its report lines and still exited 0 when
 * every check passed. */
void avant_test_write(const char *s, avant_int n) {
  size_t off = 0;
  while (n > 0 && off < (size_t)n) {
    ssize_t k = write(1, s + off, (size_t)n - off);
    if (k < 0 && errno == EINTR) {
      continue;
    }
    if (k <= 0) {
      out_fail("standard output", k < 0 ? errno : EIO, 0);
    }
    off += (size_t)k;
  }
}

static void test_status_at_exit(int status, void *arg) {
  (void)arg;
  if (status == 0 && __atomic_load_n(&g_test_failed, __ATOMIC_RELAXED)) {
    fflush(NULL);
    _exit(1);
  }
}

__attribute__((constructor)) static void test_status_init(void) {
  on_exit(test_status_at_exit, NULL);
}

/* ---- Shared with avant_sys.c ----------------------------------------
 *
 * avant_sys.c (the prelude's second part, linked only by a program that
 * calls one of its functions) uses these helpers of this file. */

char *avant_io_dup(const char *s, size_t n) {
  return dup_bytes(s, n);
}

FILE *avant_io_open_read(const char *path) {
  return open_read(path);
}

int avant_io_list_names(const char *path, char ***out, int32_t *np) {
  return list_names(path, out, np);
}

void *avant_io_names_array(char **names, int32_t n) {
  return names_array(names, n);
}

char **avant_io_build_argv(const char *path, void *args) {
  return build_argv(path, args);
}

void avant_io_free_argv(char **argv) {
  free_argv(argv);
}

__attribute__((noreturn)) void avant_io_child_fail(int errfd, int32_t stage) {
  child_fail(errfd, stage);
  _exit(127);
}

/* A blocking run's start, for avant_sys.c's: before the fork,
 * avant_io_start_claim blocks the handler's signals on this thread and
 * claims its wait slot (0: the program is stopping, nothing is claimed and
 * the run must not fork); in the child, first, avant_io_start_child; in
 * the parent, avant_io_start_done with fork's result, which puts the pid
 * in the slot (or gives the slot back) and the mask back. */
int avant_io_start_claim(sigset_t *old) {
  if (!start_begin(old)) {
    return 0;
  }
  if (!wait_claim()) {
    start_end(old);
    return 0;
  }
  return 1;
}

void avant_io_start_child(const sigset_t *old) {
  start_child(old);
}

void avant_io_start_done(int32_t pid, const sigset_t *old) {
  int e = errno;
  wait_begin((pid_t)pid);
  start_end(old);
  errno = e;
}

/* The end of a blocking run's wait (pid 0): the scratch directory's
 * signal handler no longer stops that child. */
void avant_io_wait_child(int32_t pid) {
  if (pid <= 0) {
    wait_end();
  }
}
