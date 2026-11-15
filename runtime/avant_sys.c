#define _GNU_SOURCE
#include "avant_rt.h"
#include "avant_gc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The prelude's second part (compiler/prelude_src.av,
 * prelude_more_source): standard input and files read line by line. A
 * program links
 * this object only when it calls one of those functions, so no other
 * program carries it. It shares avant_io.c's helpers (the avant_io_*
 * functions below); its own error state is read with
 * avant_sys_more_errno, as avant_io.c's is with avant_sys_errno. */

/* avant_io.c's, for this object. */
char *avant_io_dup(const char *s, size_t n);
FILE *avant_io_open_read(const char *path);

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
