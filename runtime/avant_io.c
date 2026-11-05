#define _GNU_SOURCE
#include "avant_rt.h"

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

static char *dup_bytes(const char *s, size_t n) {
  avant_gc_defer_enter();
  char *p = (char *)avant_alloc((uint64_t)n + 1, AVANT_TYPE_BYTES);
  if (n) {
    memcpy(p, s, n);
  }
  p[n] = 0;
  avant_str_tag(p);
  avant_gc_defer_leave();
  return p;
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

/* The program flag --threads (Q125-A): the generated startup calls
 * this before anything can start the pool or the HTTP server, so
 * scanning here records the override in time, and removing the flag
 * from the array means the program's own argv() never sees it. Both
 * spellings are one implementation; scanning stops at a literal "--".
 * A malformed value dies loudly. */
static void scan_threads_flag(void) {
  int32_t i;
  int32_t w = 1;
  for (i = 1; i < g_argc; i++) {
    const char *a = g_argv[i] ? g_argv[i] : "";
    const char *val = NULL;
    int consumed = 0;
    if (strcmp(a, "--") == 0) {
      for (; i < g_argc; i++) {
        g_argv[w++] = g_argv[i];
      }
      break;
    }
    if (strcmp(a, "--threads") == 0) {
      if (i + 1 >= g_argc) {
        fprintf(stderr, "avant runtime: --threads must be a positive integer\n");
        abort();
      }
      val = g_argv[i + 1];
      consumed = 2;
    } else if (strncmp(a, "--threads=", 10) == 0) {
      val = a + 10;
      consumed = 1;
    }
    if (val) {
      char *end = NULL;
      unsigned long long v = strtoull(val, &end, 10);
      if (end == val || *end != '\0' || v < 1 || v > 1073741824ull) {
        fprintf(stderr, "avant runtime: --threads must be a positive integer\n");
        abort();
      }
      avant_set_flag_threads((long)v);
      i += consumed - 1;
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

  avant_gc_defer_enter();
  AvantArray *a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  char **buf = NULL;
  if (n > 0) {
    buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
    int32_t i;
    for (i = 0; i < n; i++) {
      const char *s = g_argv && g_argv[i] ? g_argv[i] : "";
      buf[i] = dup_bytes(s, strlen(s));
    }
  }
  a->buf = buf;
  a->size = n;
  a->cap = n;
  avant_pin(a);
  g_argv_arr = a;
  avant_gc_defer_leave();
  return a;
}

/* The size a file reports is only a first buffer size: /proc files report
 * 0 and sysfs attributes report 4096 whatever they hold, so the read runs
 * to end of file. Trusting the size made /proc read empty and made the
 * test runner's processor count fall back to 1 on kernels whose sysfs
 * reports 4096 (Wave T close). stdio only (Q160). */
char *avant_file_read(const char *path, int32_t *ok) {
  if (ok) {
    *ok = 0;
  }
  if (!path) {
    return NULL;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  size_t cap = 4096;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
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
    return NULL;
  }
  size_t n = 0;
  for (;;) {
    if (n == cap) {
      size_t ncap = cap * 2;
      char *grown = (char *)realloc(raw, ncap + 1);
      if (!grown) {
        free(raw);
        fclose(f);
        return NULL;
      }
      raw = grown;
      cap = ncap;
    }
    size_t got = fread(raw + n, 1, cap - n, f);
    n += got;
    if (got == 0) {
      if (ferror(f)) {
        free(raw);
        fclose(f);
        return NULL;
      }
      break;
    }
  }
  raw[n] = 0;
  fclose(f);

  char *heap = dup_bytes(raw, n);
  free(raw);
  if (ok) {
    *ok = 1;
  }
  return heap;
}

int32_t avant_file_write(const char *path, const char *body) {
  if (!path) {
    return 1;
  }
  if (!body) {
    body = "";
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    return 1;
  }
  size_t n = avant_str_n(body);
  if (n && fwrite(body, 1, n, f) != n) {
    fclose(f);
    return 1;
  }
  if (fclose(f) != 0) {
    return 1;
  }
  return 0;
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
    const char *s = extra_buf && extra_buf[i] ? extra_buf[i] : "";
    argv[i + 1] = cstr_dup(s);
    if (!argv[i + 1]) {
      free_argv(argv);
      return NULL;
    }
  }
  return argv;
}

static void child_redirect(const char *file, int fd_a, int fd_b) {
  int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    _exit(127);
  }
  if (dup2(fd, fd_a) < 0) {
    _exit(127);
  }
  if (fd_b >= 0 && dup2(fd, fd_b) < 0) {
    _exit(127);
  }
  if (fd > STDERR_FILENO) {
    close(fd);
  }
}

/* In the forked child: optional stdout file, and stderr to err_path when
 * given, else to the stdout file; a job (D131) also gets its own process
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

/* job: a runner job (D131). no_stdin: stdin from /dev/null, for a tool the
 * compiler runs (Wave D): myc-llvm reads all of a non-terminal stdin before
 * its arguments, so an inherited pipe or socket made it hang or take the
 * input as a source file. */
static void child_exec(const char *path, char **argv, const char *out_path, const char *err_path, int job, int no_stdin) {
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
      child_redirect(out_path, STDOUT_FILENO, -1);
    }
    child_redirect(err_path, STDERR_FILENO, -1);
  } else if (out_path && out_path[0]) {
    child_redirect(out_path, STDOUT_FILENO, STDERR_FILENO);
  }
  execv(path, argv);
  _exit(127);
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

/* The child a blocking process_run waits on, and the live jobs (each the
 * leader of its own process group). The scratch signal handler reads
 * these to stop children before it removes the directory (D131). */
#define JOB_MAX 1024
static volatile pid_t g_wait_child;
static volatile pid_t g_jobs[JOB_MAX];
static volatile int32_t g_njobs;
static int32_t g_job_status;

static int32_t run_child(const char *path, void *args, const char *out_path, const char *err_path, int no_stdin) {
  char **argv;
  pid_t pid;
  int st = 0;
  if (!path) {
    return 1;
  }
  argv = build_argv(path, args);
  if (!argv) {
    return 1;
  }
  fflush(stdout);
  fflush(stderr);
  pid = fork();
  if (pid < 0) {
    free_argv(argv);
    return 1;
  }
  if (pid == 0) {
    child_exec(path, argv, out_path, err_path, 0, no_stdin);
  }
  free_argv(argv);
  g_wait_child = pid;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      g_wait_child = 0;
      return 1;
    }
  }
  g_wait_child = 0;
  return exit_code_of(st);
}

/* Starts a job without waiting: path with args, stdout and stderr to
 * out_path, stdin /dev/null, its own process group. Returns its pid, or
 * -1 when it cannot start. Internal to the test runner (D131). */
int32_t avant_job_start(const char *path, void *args, const char *out_path) {
  char **argv;
  pid_t pid;
  if (!path || g_njobs >= JOB_MAX) {
    return -1;
  }
  argv = build_argv(path, args);
  if (!argv) {
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  pid = fork();
  if (pid < 0) {
    free_argv(argv);
    return -1;
  }
  if (pid == 0) {
    child_exec(path, argv, out_path, NULL, 1, 1);
  }
  free_argv(argv);
  setpgid(pid, pid);
  g_jobs[g_njobs] = pid;
  g_njobs = g_njobs + 1;
  return (int32_t)pid;
}

/* Waits for any started job to finish. Returns its pid, or -1 when no job
 * is running; avant_job_status then gives its exit code (128+n for a
 * signal). */
int32_t avant_job_wait(void) {
  for (;;) {
    int st = 0;
    int32_t i;
    pid_t pid;
    if (g_njobs == 0) {
      return -1;
    }
    pid = waitpid(-1, &st, 0);
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
        return (int32_t)pid;
      }
    }
  }
}

int32_t avant_job_status(void) {
  return g_job_status;
}

int32_t avant_process_run(const char *path, void *args) {
  return run_child(path, args, NULL, NULL, 0);
}

int32_t avant_process_run_out(const char *path, void *args, const char *out_path) {
  return run_child(path, args, out_path, NULL, 0);
}

/* A blocking run of a tool the compiler starts (myc-llvm, cc, re2c, the
 * bind helper, git): like process_run, with stdout (and stderr, unless
 * err_path names its own file) to out_path when given, and stdin from
 * /dev/null. Internal (no prelude name, D132). */
int32_t avant_tool_run(const char *path, void *args, const char *out_path, const char *err_path) {
  return run_child(path, args, out_path, err_path, 1);
}

/* A blocking run with stdout and stderr in separate files (D140): what
 * std.testing's run_golden and rejects and bind's output split asked of
 * sh. Like process_run, it forwards a signal to its child. Internal. */
int32_t avant_process_run_split(const char *path, void *args, const char *out_path, const char *err_path) {
  return run_child(path, args, out_path, err_path, 0);
}

/* Internal file helpers (D140), bound with lib/fun, no prelude name
 * (D132): the runner, the harness, std.testing, and the compiler use them
 * instead of starting /bin/rm, chmod, mkdir, or sh -c pwd. Each returns 0
 * on success, 1 otherwise. */

/* rm -f: 0 when the path is gone, including when it never existed. */
int32_t avant_fs_unlink(const char *path) {
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
int32_t avant_fs_chmod_x(const char *path) {
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
int32_t avant_fs_mkdir_p(const char *path) {
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
 * written (D138: a gate keeps the fixed point's next compiler). It uses
 * stdio, not read()/write(): an Avant program's own fn read or fn write is
 * a global symbol of the same name and would replace libc's for every
 * caller in the process. */
int32_t avant_fs_copy(const char *src, const char *dst) {
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
  if (bad || chmod(tmp, st.st_mode & 07777) != 0 || rename(tmp, dst) != 0) {
    unlink(tmp);
    return 1;
  }
  return 0;
}

/* The identity of a file for the safepoint probe cache (D139): device,
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
 * starts. Internal plumbing (the probe cache, D139), no prelude name. */
void avant_env_set(const char *name, const char *value) {
  if (name && name[0] && value) {
    setenv(name, value, 1);
  }
}

/* pwd -P: the physical working directory, or "." when it cannot be read. */
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

char *avant_env_get(const char *name, int32_t *ok) {
  if (ok) {
    *ok = 0;
  }
  if (!name) {
    return NULL;
  }
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

int32_t avant_file_exists(const char *path) {
  if (!path) {
    return 0;
  }
  return access(path, F_OK) == 0 ? 1 : 0;
}

static int cmp_names(const void *a, const void *b) {
  const char *sa = *(char *const *)a;
  const char *sb = *(char *const *)b;
  return strcmp(sa, sb);
}

void *avant_dir_list(const char *path) {
  avant_gc_defer_enter();
  AvantArray *a = (AvantArray *)avant_alloc(sizeof(AvantArray), AVANT_TYPE_ARRAY_OBJ);
  a->buf = NULL;
  a->size = 0;
  a->cap = 0;
  if (!path) {
    avant_gc_defer_leave();
    return a;
  }

  DIR *d = opendir(path);
  if (!d) {
    avant_gc_defer_leave();
    return a;
  }

  char **names = NULL;
  int32_t n = 0;
  int32_t cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
      continue;
    }
    if (n >= cap) {
      int32_t next = cap == 0 ? 16 : cap * 2;
      char **grown = (char **)realloc(names, (size_t)next * sizeof(char *));
      if (!grown) {
        break;
      }
      names = grown;
      cap = next;
    }
    size_t len = strlen(name);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
      break;
    }
    memcpy(copy, name, len + 1);
    names[n] = copy;
    n += 1;
  }
  closedir(d);

  if (n > 1) {
    qsort(names, (size_t)n, sizeof(char *), cmp_names);
  }

  char **buf = NULL;
  if (n > 0) {
    buf = (char **)avant_alloc((uint64_t)n * sizeof(char *), AVANT_TYPE_PTRS);
    int32_t i;
    for (i = 0; i < n; i++) {
      buf[i] = dup_bytes(names[i], strlen(names[i]));
      free(names[i]);
    }
  }
  free(names);
  a->buf = buf;
  a->size = n;
  a->cap = n;
  avant_gc_defer_leave();
  return a;
}

/* ---- Scratch directory (D127) ------------------------------------------
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
 * included) and on SIGINT, SIGTERM, and SIGHUP, after which the signal is
 * re-raised so the status stays 128+n. Only the creating process removes:
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

/* rm -rf (D140): a file or a symbolic link is unlinked; a directory is
 * emptied depth-first without following links, then removed. 0 when the
 * path is gone. */
int32_t avant_fs_remove_tree(const char *path) {
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

/* Stops what this process started before the directory goes (D131):
 * SIGTERM to every job's group and to the child a blocking process_run
 * waits on, then reap them for up to about two seconds, then SIGKILL
 * whatever is left and reap briefly. Async-signal-safe calls only. */
static void tmp_stop_children(void) {
  struct timespec pause = {0, 10 * 1000 * 1000};
  int32_t i;
  int round;
  pid_t child = g_wait_child;
  int32_t n = g_njobs;
  if (n == 0 && child <= 0) {
    return;
  }
  for (i = 0; i < n; i++) {
    kill(-g_jobs[i], SIGTERM);
  }
  if (child > 0) {
    kill(child, SIGTERM);
  }
  for (round = 0; round < 250; round++) {
    int st;
    pid_t got;
    int live = 0;
    while ((got = waitpid(-1, &st, WNOHANG)) > 0) {
    }
    for (i = 0; i < n; i++) {
      if (kill(-g_jobs[i], 0) == 0) {
        live = 1;
      }
    }
    if (child > 0 && kill(child, 0) == 0) {
      live = 1;
    }
    if (!live) {
      return;
    }
    if (round == 200) {
      for (i = 0; i < n; i++) {
        kill(-g_jobs[i], SIGKILL);
      }
      if (child > 0) {
        kill(child, SIGKILL);
      }
    }
    nanosleep(&pause, NULL);
  }
}

static void tmp_on_signal(int sig) {
  struct sigaction sa;
  if (g_tmp_ready && getpid() == g_tmp_owner) {
    tmp_stop_children();
  }
  tmp_finish();
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, NULL);
  raise(sig);
}

/* An ignored signal stays ignored (nohup, a parent's choice); a handler
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
 * temp_dir, D128). */
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
