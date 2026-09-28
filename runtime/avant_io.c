#include "avant_rt.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

void avant_io_init_argv(int32_t argc, void *argv) {
  g_argc = argc < 0 ? 0 : argc;
  g_argv = (char **)argv;
  g_argv_set = 1;
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
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  size_t n = (size_t)sz;
  char *raw = (char *)malloc(n + 1);
  if (!raw) {
    fclose(f);
    return NULL;
  }
  if (n && fread(raw, 1, n, f) != n) {
    free(raw);
    fclose(f);
    return NULL;
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

static int32_t run_child(const char *path, void *args, const char *out_path) {
  if (!path) {
    return 1;
  }

  AvantArray *extra = (AvantArray *)args;
  int32_t extra_n = 0;
  char **extra_buf = NULL;
  if (extra && extra->size > 0) {
    extra_n = extra->size;
    extra_buf = (char **)extra->buf;
  }

  int32_t argc = extra_n + 1;
  char **argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
  if (!argv) {
    return 1;
  }

  argv[0] = cstr_dup(path);
  if (!argv[0]) {
    free(argv);
    return 1;
  }
  int32_t i;
  for (i = 0; i < extra_n; i++) {
    const char *s = extra_buf && extra_buf[i] ? extra_buf[i] : "";
    argv[i + 1] = cstr_dup(s);
    if (!argv[i + 1]) {
      int32_t j;
      for (j = 0; j <= i; j++) {
        free(argv[j]);
      }
      free(argv);
      return 1;
    }
  }
  argv[argc] = NULL;

  fflush(stdout);
  fflush(stderr);

  pid_t pid = fork();
  if (pid < 0) {
    for (i = 0; i < argc; i++) {
      free(argv[i]);
    }
    free(argv);
    return 1;
  }
  if (pid == 0) {
    if (out_path && out_path[0]) {
      int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd < 0) {
        _exit(127);
      }
      if (dup2(fd, STDOUT_FILENO) < 0) {
        _exit(127);
      }
      if (dup2(fd, STDERR_FILENO) < 0) {
        _exit(127);
      }
      if (fd > STDERR_FILENO) {
        close(fd);
      }
    }
    execv(path, argv);
    _exit(127);
  }

  for (i = 0; i < argc; i++) {
    free(argv[i]);
  }
  free(argv);

  int st = 0;
  if (waitpid(pid, &st, 0) < 0) {
    return 1;
  }
  if (WIFEXITED(st)) {
    return WEXITSTATUS(st);
  }
  if (WIFSIGNALED(st)) {
    return 128 + WTERMSIG(st);
  }
  return 1;
}

int32_t avant_process_run(const char *path, void *args) {
  return run_child(path, args, NULL);
}

int32_t avant_process_run_out(const char *path, void *args, const char *out_path) {
  return run_child(path, args, out_path);
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
