#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* std.fs, std.process and the prelude's second part
 * (compiler/prelude_src.av, prelude_more_source): standard input and
 * files read line by line, the file system's basics, the environment and
 * a child process's output. A program links this object only when it
 * imports one of those modules or calls read_line, so no other program
 * carries it. It shares avant_io.c's helpers (the avant_io_*
 * functions below); its own error state is read with
 * avant_sys_more_errno and avant_sys_more_signal, as avant_io.c's is with
 * avant_sys_errno and avant_sys_signal. */

/* avant_io.c's, for this object. */
char *avant_io_dup(const char *s, size_t n);
FILE *avant_io_open_read(const char *path);
int avant_io_list_names(const char *path, char ***out, int32_t *np);
void *avant_io_names_array(char **names, int32_t n);
char **avant_io_build_argv(const char *path, void *args);
void avant_io_free_argv(char **argv);
__attribute__((noreturn)) void avant_io_child_fail(int errfd, int32_t stage);
void avant_io_wait_child(int32_t pid);
int avant_io_start_claim(sigset_t *old);
void avant_io_start_child(const sigset_t *old);
void avant_io_start_done(int32_t pid, const sigset_t *old);

static _Thread_local int32_t g_sys_errno;
static _Thread_local int32_t g_sys_signal;

avant_int avant_sys_more_errno(void) {
  return g_sys_errno;
}

avant_int avant_sys_more_signal(void) {
  return g_sys_signal;
}

/* ---- Lines (read_line, FileReader) --------------------------------------
 *
 * A reader is a FILE * the prelude holds as an Int; -1 is standard input.
 * A line comes without its "\n" (and a "\r" before it); the last line of
 * a file that does not end in a newline is a line too. NULL at the end of
 * the input (avant_sys_more_errno() 0) or on a read error (its errno). The read
 * blocks, so it runs native; the line is C memory until it is copied. */
static _Thread_local char *g_line;
static _Thread_local size_t g_line_cap;
static int g_out_tty = -1;

/* gc-lint: holds nothing (the line is C memory until avant_io_dup). */
char *avant_sys_read_line(avant_int handle) {
  FILE *f = handle == -1 ? stdin : (FILE *)(intptr_t)handle;
  ssize_t n;
  int e;
  if (!f) {
    g_sys_errno = EBADF;
    return NULL;
  }
  if (handle == -1) {
    /* A prompt printed without a newline shows before the program waits
     * for its answer, when a person is reading stdout. */
    if (g_out_tty < 0) {
      g_out_tty = isatty(STDOUT_FILENO) ? 1 : 0;
    }
    if (g_out_tty) {
      fflush(stdout);
    }
  }
  avant_gc_native_begin();
  errno = 0;
  n = getline(&g_line, &g_line_cap, f);
  e = n < 0 && ferror(f) ? (errno ? errno : EIO) : 0;
  avant_gc_native_end();
  if (n < 0) {
    g_sys_errno = e;
    return NULL;
  }
  if (n > 0 && g_line[n - 1] == '\n') {
    n -= 1;
    if (n > 0 && g_line[n - 1] == '\r') {
      n -= 1;
    }
  }
  if ((uint64_t)n > (uint64_t)AVANT_SIZE_MAX) {
    g_sys_errno = EFBIG;
    return NULL;
  }
  g_sys_errno = 0;
  return avant_io_dup(g_line, (size_t)n);
}

/* file_open: the reader of a file (a directory is EISDIR), or 0 with
 * avant_sys_more_errno() telling why. */
avant_int avant_sys_open(const char *path) {
  FILE *f = avant_io_open_read(path ? path : "");
  if (!f) {
    g_sys_errno = errno;
    return 0;
  }
  g_sys_errno = 0;
  return (avant_int)(intptr_t)f;
}

void avant_sys_close(avant_int handle) {
  if (handle != 0 && handle != -1) {
    fclose((FILE *)(intptr_t)handle);
  }
}

/* ---- File-system basics (dir_exists, dir_make, file_delete, file_size,
 * dir_walk). Each sets avant_sys_more_errno(). */

avant_int avant_sys_is_dir(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
}

/* The directory and its missing parents, mode 0777 under the umask; a
 * directory that exists already is success, anything else there EEXIST. */
avant_int avant_sys_make_dir(const char *path) {
  char buf[4096];
  size_t n;
  size_t i;
  struct stat st;
  n = path ? strlen(path) : 0;
  if (n == 0) {
    g_sys_errno = ENOENT;
    return -1;
  }
  if (n >= sizeof buf) {
    g_sys_errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(buf, path, n + 1);
  for (i = 1; i <= n; i++) {
    if (buf[i] == '/' || buf[i] == 0) {
      char keep = buf[i];
      buf[i] = 0;
      if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
        g_sys_errno = errno;
        return -1;
      }
      buf[i] = keep;
    }
  }
  if (stat(path, &st) != 0) {
    g_sys_errno = errno;
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    g_sys_errno = EEXIST;
    return -1;
  }
  g_sys_errno = 0;
  return 0;
}

/* A file (or a link) removed; a directory is EISDIR. */
avant_int avant_sys_delete(const char *path) {
  if (unlink(path ? path : "") != 0) {
    g_sys_errno = errno;
    return -1;
  }
  g_sys_errno = 0;
  return 0;
}

/* std.process's env_set: setenv(3), replacing a value there; -1 for a
 * name that is empty or holds '=' (EINVAL). A string from Avant may hold
 * a zero byte, which would cut the name: refused too. */
avant_int avant_sys_env_set(const char *name, const char *value) {
  if (!name || !name[0] || strchr(name, '=') || avant_str_n(name) != strlen(name)) {
    g_sys_errno = EINVAL;
    return -1;
  }
  if (setenv(name, value ? value : "", 1) != 0) {
    g_sys_errno = errno;
    return -1;
  }
  g_sys_errno = 0;
  return 0;
}

/* std.fs's rename: from moved to to, replacing a file there (rename(2)). */
avant_int avant_sys_rename(const char *from, const char *to) {
  if (rename(from ? from : "", to ? to : "") != 0) {
    g_sys_errno = errno;
    return -1;
  }
  g_sys_errno = 0;
  return 0;
}

/* std.fs's append: body added to the end of the file, created when it is
 * not there; the bytes written, or -1 (avant_sys_more_errno). */
avant_int avant_sys_append(const char *path, const char *body) {
  FILE *f;
  size_t n;
  if (!body) {
    body = "";
  }
  n = avant_str_n(body);
  if (n > (size_t)AVANT_SIZE_MAX) {
    g_sys_errno = EFBIG;
    return -1;
  }
  f = fopen(path ? path : "", "ab");
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
  return (avant_int)n;
}

/* The size in bytes of what path names, or -1. */
avant_int avant_sys_size(const char *path) {
  struct stat st;
  if (stat(path ? path : "", &st) != 0) {
    g_sys_errno = errno;
    return -1;
  }
  g_sys_errno = 0;
  return (avant_int)st.st_size;
}

/* dir_walk: every name below a directory, as a path relative to it, a
 * directory before what it holds, the names of each directory sorted. A
 * link to a directory is listed and not followed, so a cycle cannot loop.
 * In C memory until the array is built: 0, or the errno that stopped it. */
typedef struct {
  char **names;
  int32_t n;
  int32_t cap;
} WalkList;

static int walk_push(WalkList *w, const char *rel) {
  size_t len = strlen(rel);
  char *copy;
  if (w->n >= w->cap) {
    if (w->cap > AVANT_SIZE_MAX / 2) {
      return EOVERFLOW;
    }
    int32_t next = w->cap == 0 ? 64 : w->cap * 2;
    char **grown = (char **)realloc(w->names, (size_t)next * sizeof(char *));
    if (!grown) {
      return ENOMEM;
    }
    w->names = grown;
    w->cap = next;
  }
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return ENOMEM;
  }
  memcpy(copy, rel, len + 1);
  w->names[w->n] = copy;
  w->n += 1;
  return 0;
}

/* gc-lint: no reference word (the list is malloc'ed C memory). */
static int walk_dir(WalkList *w, const char *root, const char *rel) {
  char full[4096];
  char sub[4096];
  char **names;
  int32_t n;
  int32_t i;
  int err;
  struct stat st;
  if (snprintf(full, sizeof full, "%s%s%s", root, rel[0] ? "/" : "", rel) >= (int)sizeof full) {
    return ENAMETOOLONG;
  }
  err = avant_io_list_names(full, &names, &n);
  if (err) {
    return err;
  }
  for (i = 0; i < n && !err; i++) {
    if (snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", names[i]) >= (int)sizeof sub) {
      err = ENAMETOOLONG;
      break;
    }
    err = walk_push(w, sub);
    if (err) {
      break;
    }
    if (snprintf(full, sizeof full, "%s/%s", root, sub) >= (int)sizeof full) {
      err = ENAMETOOLONG;
      break;
    }
    if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
      err = walk_dir(w, root, sub);
    }
  }
  for (i = 0; i < n; i++) {
    free(names[i]);
  }
  free(names);
  return err;
}

/* gc-lint: holds nothing (the path is read before names_array allocates). */
void *avant_sys_walk(const char *path) {
  WalkList w = {NULL, 0, 0};
  int err;
  int32_t i;
  /* The walk is native: the path is copied out of the heap first. */
  char *root = strdup(path ? path : "");
  if (!root) {
    g_sys_errno = ENOMEM;
    return NULL;
  }
  avant_gc_native_begin();
  err = walk_dir(&w, root, "");
  avant_gc_native_end();
  free(root);
  g_sys_errno = err;
  if (err) {
    for (i = 0; i < w.n; i++) {
      free(w.names[i]);
    }
    free(w.names);
    return NULL;
  }
  return avant_io_names_array(w.names, w.n);
}

/* process_output: runs the program as process_run does, with the bytes of
 * input on its stdin (then the end of input) and its stdout and stderr
 * captured, read as they come so that neither pipe can fill and stall the
 * child. The status as avant_sys_run gives it (-1 with
 * avant_sys_more_errno(), -2 with avant_sys_more_signal()); the captured bytes
 * wait in C memory for avant_sys_captured. A child that stops reading its
 * input early is no error (the rest of the input is dropped, and the
 * SIGPIPE the write raises is blocked and taken back here). An output past
 * the String ceiling is EFBIG. */
static _Thread_local char *g_cap[2];
static _Thread_local size_t g_cap_n[2];

static void cap_free(void) {
  int i;
  for (i = 0; i < 2; i++) {
    free(g_cap[i]);
    g_cap[i] = NULL;
    g_cap_n[i] = 0;
  }
}

/* Moves fd onto target in the child (dup2 clears close-on-exec; an fd
 * that is already target keeps it, so it is cleared by hand). */
static void child_move(int fd, int target, int errfd) {
  if (fd == target) {
    if (fcntl(fd, F_SETFD, 0) != 0) {
      avant_io_child_fail(errfd, 2);
    }
    return;
  }
  if (dup2(fd, target) < 0) {
    avant_io_child_fail(errfd, 2);
  }
}

typedef struct {
  char *p;
  size_t n;
  size_t cap;
  int big;
} CapBuf;

static int cap_read(int fd, CapBuf *b) {
  char chunk[65536];
  ssize_t got = read(fd, chunk, sizeof chunk);
  if (got < 0) {
    return errno == EINTR || errno == EAGAIN ? 1 : 0;
  }
  if (got == 0) {
    return 0;
  }
  if (b->big || b->n + (size_t)got > (size_t)AVANT_SIZE_MAX) {
    b->big = 1;
    return 1;
  }
  if (b->n + (size_t)got > b->cap) {
    size_t next = b->cap ? b->cap * 2 : 65536;
    while (next < b->n + (size_t)got) {
      next *= 2;
    }
    char *grown = (char *)realloc(b->p, next);
    if (!grown) {
      b->big = 2;
      return 1;
    }
    b->p = grown;
    b->cap = next;
  }
  memcpy(b->p + b->n, chunk, (size_t)got);
  b->n += (size_t)got;
  return 1;
}

/* gc-lint: holds nothing (the input is copied to C memory before the fork; the path and the arguments are read before it). */
avant_int avant_sys_capture(const char *path, void *args, const char *input) {
  char **argv;
  pid_t pid;
  int st = 0;
  int fds[2];
  int in_p[2];
  int out_p[2];
  int err_p[2];
  int32_t msg[2];
  ssize_t got = 0;
  size_t in_n = input ? avant_str_n(input) : 0;
  size_t in_at = 0;
  char *in_c = NULL;
  CapBuf bufs[2];
  sigset_t pipe_set;
  sigset_t old_set;
  sigset_t pending;
  sigset_t start_mask;
  int had_pipe;
  int i;
  memset(bufs, 0, sizeof bufs);
  cap_free();
  g_sys_errno = 0;
  g_sys_signal = 0;
  if (in_n) {
    in_c = (char *)malloc(in_n);
    if (!in_c) {
      g_sys_errno = ENOMEM;
      return -1;
    }
    memcpy(in_c, input, in_n);
  }
  argv = avant_io_build_argv(path ? path : "", args);
  if (!argv) {
    free(in_c);
    g_sys_errno = ENOMEM;
    return -1;
  }
  if (pipe2(fds, O_CLOEXEC) != 0 || pipe2(in_p, O_CLOEXEC) != 0 || pipe2(out_p, O_CLOEXEC) != 0 || pipe2(err_p, O_CLOEXEC) != 0) {
    /* The descriptors opened so far stay with the process: rare enough
     * (EMFILE) that the leak of up to six descriptors is accepted. */
    g_sys_errno = errno;
    avant_io_free_argv(argv);
    free(in_c);
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  if (avant_io_start_claim(&start_mask)) {
    pid = fork();
    if (pid == 0) {
      avant_io_start_child(&start_mask);
    } else {
      avant_io_start_done(pid, &start_mask);
    }
  } else {
    pid = -1;
    errno = ECANCELED;
  }
  if (pid < 0) {
    g_sys_errno = errno;
    close(fds[0]);
    close(fds[1]);
    close(in_p[0]);
    close(in_p[1]);
    close(out_p[0]);
    close(out_p[1]);
    close(err_p[0]);
    close(err_p[1]);
    avant_io_free_argv(argv);
    free(in_c);
    return -1;
  }
  if (pid == 0) {
    close(fds[0]);
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
    child_move(in_p[0], STDIN_FILENO, fds[1]);
    child_move(out_p[1], STDOUT_FILENO, fds[1]);
    child_move(err_p[1], STDERR_FILENO, fds[1]);
    execvp(argv[0], argv);
    avant_io_child_fail(fds[1], 2);
  }
  close(fds[1]);
  close(in_p[0]);
  close(out_p[1]);
  close(err_p[1]);
  avant_io_free_argv(argv);
  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
  sigpending(&pending);
  had_pipe = sigismember(&pending, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
  /* The pipes and the process wait are native: C memory only. */
  avant_gc_native_begin();
  for (;;) {
    got = read(fds[0], msg, sizeof msg);
    if (got >= 0 || errno != EINTR) {
      break;
    }
  }
  close(fds[0]);
  if (got == (ssize_t)sizeof msg) {
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
  } else {
    struct pollfd pf[3];
    int open_out = 1;
    int open_err = 1;
    int open_in = 1;
    if (in_n == 0) {
      close(in_p[1]);
      open_in = 0;
    } else {
      fcntl(in_p[1], F_SETFL, fcntl(in_p[1], F_GETFL) | O_NONBLOCK);
    }
    while (open_out || open_err || open_in) {
      int k = 0;
      int at_out = -1;
      int at_err = -1;
      int at_in = -1;
      if (open_out) {
        pf[k].fd = out_p[0];
        pf[k].events = POLLIN;
        at_out = k++;
      }
      if (open_err) {
        pf[k].fd = err_p[0];
        pf[k].events = POLLIN;
        at_err = k++;
      }
      if (open_in) {
        pf[k].fd = in_p[1];
        pf[k].events = POLLOUT;
        at_in = k++;
      }
      if (poll(pf, (nfds_t)k, -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (at_out >= 0 && pf[at_out].revents) {
        if (!cap_read(out_p[0], &bufs[0])) {
          close(out_p[0]);
          open_out = 0;
        }
      }
      if (at_err >= 0 && pf[at_err].revents) {
        if (!cap_read(err_p[0], &bufs[1])) {
          close(err_p[0]);
          open_err = 0;
        }
      }
      if (at_in >= 0 && pf[at_in].revents) {
        ssize_t w = write(in_p[1], in_c + in_at, in_n - in_at);
        if (w > 0) {
          in_at += (size_t)w;
        }
        if (in_at == in_n || (w < 0 && errno != EINTR && errno != EAGAIN)) {
          close(in_p[1]);
          open_in = 0;
        }
      }
    }
    if (open_out) {
      close(out_p[0]);
    }
    if (open_err) {
      close(err_p[0]);
    }
    if (open_in) {
      close(in_p[1]);
    }
  }
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      st = -1;
      break;
    }
  }
  avant_gc_native_end();
  avant_io_wait_child(0);
  if (!had_pipe) {
    struct timespec zero = {0, 0};
    sigpending(&pending);
    if (sigismember(&pending, SIGPIPE)) {
      sigtimedwait(&pipe_set, NULL, &zero);
    }
  }
  pthread_sigmask(SIG_SETMASK, &old_set, NULL);
  free(in_c);
  if (got == (ssize_t)sizeof msg) {
    free(bufs[0].p);
    free(bufs[1].p);
    g_sys_errno = msg[1];
    return -1;
  }
  for (i = 0; i < 2; i++) {
    g_cap[i] = bufs[i].p;
    g_cap_n[i] = bufs[i].n;
  }
  if (st == -1) {
    cap_free();
    g_sys_errno = ECHILD;
    return -1;
  }
  if (bufs[0].big || bufs[1].big) {
    int oom = bufs[0].big == 2 || bufs[1].big == 2;
    cap_free();
    g_sys_errno = oom ? ENOMEM : EFBIG;
    return -1;
  }
  if (WIFSIGNALED(st)) {
    g_sys_signal = WTERMSIG(st);
    cap_free();
    return -2;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Appends what the last capture read from the child's stdout (which 1)
 * or stderr (2) to buf and frees it: process_output_into, which writes
 * into the caller's buffers instead of allocating two Strings. 0, or
 * EFBIG when buf would pass its ceiling (nothing appended). */
avant_int avant_sys_captured_into(avant_int which, void *buf) {
  int i = which == 2 ? 1 : 0;
  int32_t at = (int32_t)avant_buf_size(buf);
  size_t n = g_cap_n[i];
  uint8_t *dst;
  if (n > (size_t)INT32_MAX - (size_t)at) {
    free(g_cap[i]);
    g_cap[i] = NULL;
    g_cap_n[i] = 0;
    return EFBIG;
  }
  /* RC1/RC2: the growth may collect; the Buf is a handle, its bytes
   * read after it. */
  AVANT_GC_SCOPE(1);
  AVANT_GC_ROOT(0, &buf);
  avant_buf_ensure(buf, at + (int32_t)n);
  dst = avant_buf_bytes(buf);
  if (n > 0) {
    memcpy(dst + at, g_cap[i], n);
  }
  avant_buf_set_size(buf, at + (int32_t)n);
  free(g_cap[i]);
  g_cap[i] = NULL;
  g_cap_n[i] = 0;
  return 0;
}

/* The bytes the last capture on this thread read: 1 its stdout, 2 its
 * stderr, as a String; the C copy is freed. */
/* gc-lint: holds nothing (C memory only). */
char *avant_sys_captured(avant_int which) {
  int i = which == 2 ? 1 : 0;
  char *p = avant_io_dup(g_cap[i] ? g_cap[i] : "", g_cap_n[i]);
  free(g_cap[i]);
  g_cap[i] = NULL;
  g_cap_n[i] = 0;
  return p;
}

