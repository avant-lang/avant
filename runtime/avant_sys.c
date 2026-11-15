#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The prelude's second part (compiler/prelude_src.av,
 * prelude_more_source): standard input and files read line by line, the
 * file system's basics. A program links
 * this object only when it calls one of those functions, so no other
 * program carries it. It shares avant_io.c's helpers (the avant_io_*
 * functions below); its own error state is read with
 * avant_sys_more_errno, as avant_io.c's is with avant_sys_errno. */

/* avant_io.c's, for this object. */
char *avant_io_dup(const char *s, size_t n);
FILE *avant_io_open_read(const char *path);
int avant_io_list_names(const char *path, char ***out, int32_t *np);
void *avant_io_names_array(char **names, int32_t n);

static _Thread_local int32_t g_sys_errno;

avant_int avant_sys_more_errno(void) {
  return g_sys_errno;
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
