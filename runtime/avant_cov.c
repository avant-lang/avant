#include "avant_rt.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char kind;
  char *path;
  int line;
  char *name;
} CovSlot;

typedef struct {
  char *path;
  char *show;
  int stmts;
  int stmts_hit;
  int branches;
  int branches_hit;
  int funcs;
  int funcs_hit;
  int lines;
  int lines_hit;
  int *uncovered;
  int uncovered_n;
  int uncovered_cap;
} CovFile;

static int g_inited;
static int g_nslots;
static CovSlot *g_slots;
static uint64_t *g_hits;
static size_t g_hits_bytes;
static int g_hits_fd = -1;
static char *g_map_path;

static int use_color(void) {
  const char *env = getenv("AVANT_COVERAGE_COLOR");
  if (env && env[0] == '0' && env[1] == 0) {
    return 0;
  }
  if (env && env[0] == '1' && env[1] == 0) {
    return 1;
  }
  return isatty(1);
}

static int should_report(void) {
  const char *r = getenv("AVANT_COVERAGE_REPORT");
  if (r && r[0] == '0' && r[1] == 0) {
    return 0;
  }
  if (r && r[0] == '1' && r[1] == 0) {
    return 1;
  }

  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0) {
    return 1;
  }
  char buf[4096];
  ssize_t n = read(fd, buf, (ssize_t)sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return 1;
  }
  buf[n] = 0;
  char *p = buf;
  char *end = buf + n;
  while (p < end && *p) {
    p++;
  }
  if (p >= end || p + 1 >= end) {
    return 1;
  }
  p++;
  if (strcmp(p, "compile") == 0 || strcmp(p, "c") == 0 || strcmp(p, "dump") == 0 ||
      strcmp(p, "d") == 0 || strcmp(p, "bind") == 0 || strcmp(p, "b") == 0) {
    return 0;
  }
  return 1;
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (!p) {
    return NULL;
  }
  memcpy(p, s, n + 1);
  return p;
}

static const char *display_path(const char *path) {
  const char *p;
  if (!path || !path[0]) {
    return "<input>";
  }
  p = strstr(path, "/compiler/");
  if (p) {
    return p + 1;
  }
  p = strstr(path, "/tests/");
  if (p) {
    return p + 1;
  }
  p = strrchr(path, '/');
  if (p && p[1]) {
    return p + 1;
  }
  return path;
}

static int parse_map(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return -1;
  }

  char line[2048];
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return -1;
  }
  if (strncmp(line, "avant-cov ", 10) != 0) {
    fclose(f);
    return -1;
  }

  int cap = 64;
  int n = 0;
  CovSlot *slots = (CovSlot *)malloc((size_t)cap * sizeof(CovSlot));
  if (!slots) {
    fclose(f);
    return -1;
  }

  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = 0;
    }
    if (len == 0) {
      continue;
    }
    char *tab1 = strchr(line, '\t');
    if (!tab1 || tab1 != line + 1) {
      continue;
    }
    char kind = line[0];
    *tab1 = 0;
    char *path_s = tab1 + 1;
    char *tab2 = strchr(path_s, '\t');
    if (!tab2) {
      continue;
    }
    *tab2 = 0;
    char *line_s = tab2 + 1;
    char *tab3 = strchr(line_s, '\t');
    char *name_s = "";
    if (tab3) {
      *tab3 = 0;
      name_s = tab3 + 1;
    }
    if (n >= cap) {
      cap *= 2;
      CovSlot *grow = (CovSlot *)realloc(slots, (size_t)cap * sizeof(CovSlot));
      if (!grow) {
        fclose(f);
        free(slots);
        return -1;
      }
      slots = grow;
    }
    slots[n].kind = kind;
    slots[n].path = xstrdup(path_s);
    slots[n].line = atoi(line_s);
    slots[n].name = xstrdup(name_s ? name_s : "");
    if (!slots[n].path || !slots[n].name) {
      fclose(f);
      return -1;
    }
    n++;
  }
  fclose(f);
  g_slots = slots;
  g_nslots = n;
  return 0;
}

static int open_hits(const char *map_path) {
  size_t n = strlen(map_path);
  char *hits_path = (char *)malloc(n + 6);
  if (!hits_path) {
    return -1;
  }
  memcpy(hits_path, map_path, n);
  memcpy(hits_path + n, ".hits", 6);

  int fd = open(hits_path, O_RDWR | O_CREAT, 0644);
  free(hits_path);
  if (fd < 0) {
    return -1;
  }

  g_hits_bytes = (size_t)g_nslots * sizeof(uint64_t);
  if (g_hits_bytes == 0) {
    g_hits_bytes = sizeof(uint64_t);
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  if ((size_t)st.st_size < g_hits_bytes) {
    if (ftruncate(fd, (off_t)g_hits_bytes) != 0) {
      close(fd);
      return -1;
    }
  }

  void *map = mmap(NULL, g_hits_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    return -1;
  }
  g_hits = (uint64_t *)map;
  g_hits_fd = fd;
  return 0;
}

static int pct_i(int hit, int tot) {
  if (tot <= 0) {
    return 100;
  }
  return (int)(((100.0 * (double)hit) / (double)tot) + 0.5);
}

static int cmp_int(const void *a, const void *b) {
  int x = *(const int *)a;
  int y = *(const int *)b;
  return x - y;
}

static int cmp_file(const void *a, const void *b) {
  const CovFile *fa = (const CovFile *)a;
  const CovFile *fb = (const CovFile *)b;
  return strcmp(fa->show, fb->show);
}

static CovFile *find_file(CovFile **files, int *n, int *cap, const char *path) {
  int i;
  for (i = 0; i < *n; i++) {
    if (strcmp((*files)[i].path, path) == 0) {
      return &(*files)[i];
    }
  }
  if (*n >= *cap) {
    int next = *cap ? *cap * 2 : 8;
    CovFile *grow = (CovFile *)realloc(*files, (size_t)next * sizeof(CovFile));
    if (!grow) {
      return NULL;
    }
    *files = grow;
    *cap = next;
  }
  CovFile *f = &(*files)[*n];
  memset(f, 0, sizeof(*f));
  f->path = xstrdup(path);
  f->show = xstrdup(display_path(path));
  (*n)++;
  return f;
}

static void add_uncovered(CovFile *f, int line) {
  int i;
  if (line <= 0) {
    return;
  }
  for (i = 0; i < f->uncovered_n; i++) {
    if (f->uncovered[i] == line) {
      return;
    }
  }
  if (f->uncovered_n >= f->uncovered_cap) {
    int next = f->uncovered_cap ? f->uncovered_cap * 2 : 8;
    int *grow = (int *)realloc(f->uncovered, (size_t)next * sizeof(int));
    if (!grow) {
      return;
    }
    f->uncovered = grow;
    f->uncovered_cap = next;
  }
  f->uncovered[f->uncovered_n++] = line;
}

static void format_uncovered(const CovFile *f, char *out, size_t cap) {
  int i;
  size_t used = 0;
  out[0] = 0;
  if (f->uncovered_n == 0) {
    return;
  }
  qsort(f->uncovered, (size_t)f->uncovered_n, sizeof(int), cmp_int);
  i = 0;
  while (i < f->uncovered_n) {
    int start = f->uncovered[i];
    int end = start;
    while (i + 1 < f->uncovered_n && f->uncovered[i + 1] == end + 1) {
      i++;
      end = f->uncovered[i];
    }
    char chunk[64];
    if (start == end) {
      snprintf(chunk, sizeof(chunk), "%d", start);
    } else {
      snprintf(chunk, sizeof(chunk), "%d-%d", start, end);
    }
    size_t clen = strlen(chunk);
    if (used > 0) {
      if (used + 1 + clen + 1 >= cap) {
        if (used + 4 < cap) {
          memcpy(out + used, "...", 4);
        }
        return;
      }
      out[used++] = ',';
    } else if (clen + 1 >= cap) {
      memcpy(out, "...", 4);
      return;
    }
    memcpy(out + used, chunk, clen + 1);
    used += clen;
    i++;
  }
}

static void dash_repeat(int n) {
  int i;
  for (i = 0; i < n; i++) {
    fputc('-', stdout);
  }
}

static void space_repeat(int n) {
  int i;
  for (i = 0; i < n; i++) {
    fputc(' ', stdout);
  }
}

static void print_border(int file_w, int uncovered_w) {
  dash_repeat(file_w + 2);
  fputs("|---------|----------|---------|---------|", stdout);
  dash_repeat(uncovered_w + 1);
  fputc('\n', stdout);
}

static void print_pct(int hit, int tot, int color, int inner) {
  int pct = pct_i(hit, tot);
  const char *c = "";
  const char *r = "";
  int num_w = inner - 2;
  if (num_w < 1) {
    num_w = 1;
  }
  if (color) {
    if (pct >= 80) {
      c = "\033[32m";
    } else if (pct >= 50) {
      c = "\033[33m";
    } else {
      c = "\033[31m";
    }
    r = "\033[0m";
  }
  printf(" %s%*d%s ", c, num_w, pct, r);
}

static const char *lcov_sf_path(const char *path) {
  return display_path(path);
}

static const char *lcov_out_path(void) {
  const char *env = getenv("AVANT_COVERAGE_LCOV");
  if (env && env[0] == '0' && env[1] == 0) {
    return NULL;
  }
  if (env && env[0]) {
    return env;
  }
  return "lcov.info";
}

static void write_lcov(void) {
  FILE *out;
  int i;
  int file_n = 0;
  int file_cap = 0;
  CovFile *files = NULL;
  const char *path;
  const char *out_path = lcov_out_path();

  if (!out_path || !g_slots || g_nslots <= 0) {
    return;
  }

  for (i = 0; i < g_nslots; i++) {
    if (!find_file(&files, &file_n, &file_cap, g_slots[i].path ? g_slots[i].path : "")) {
      free(files);
      return;
    }
  }

  out = fopen(out_path, "w");
  if (!out) {
    for (i = 0; i < file_n; i++) {
      free(files[i].path);
      free(files[i].show);
      free(files[i].uncovered);
    }
    free(files);
    return;
  }

  for (i = 0; i < file_n; i++) {
    int s;
    int fn_tot = 0, fn_hit = 0;
    int br_tot = 0, br_hit = 0;
    int br_idx = 0;
    int lf = 0, lh = 0;
    path = lcov_sf_path(files[i].path ? files[i].path : "");
    fprintf(out, "TN:\n");
    fprintf(out, "SF:%s\n", path ? path : "<input>");

    for (s = 0; s < g_nslots; s++) {
      const char *sp = g_slots[s].path ? g_slots[s].path : "";
      if (strcmp(sp, files[i].path ? files[i].path : "") != 0) {
        continue;
      }
      if (g_slots[s].kind == 'F') {
        const char *name = g_slots[s].name && g_slots[s].name[0] ? g_slots[s].name : "fn";
        fprintf(out, "FN:%d,%s\n", g_slots[s].line, name);
      }
    }
    for (s = 0; s < g_nslots; s++) {
      uint64_t hit = (g_hits && s >= 0 && s < g_nslots) ? g_hits[s] : 0;
      const char *sp = g_slots[s].path ? g_slots[s].path : "";
      if (strcmp(sp, files[i].path ? files[i].path : "") != 0) {
        continue;
      }
      if (g_slots[s].kind == 'F') {
        const char *name = g_slots[s].name && g_slots[s].name[0] ? g_slots[s].name : "fn";
        fprintf(out, "FNDA:%llu,%s\n", (unsigned long long)hit, name);
        fn_tot++;
        if (hit) {
          fn_hit++;
        }
      }
    }
    fprintf(out, "FNF:%d\n", fn_tot);
    fprintf(out, "FNH:%d\n", fn_hit);

    for (s = 0; s < g_nslots; s++) {
      uint64_t hit = (g_hits && s >= 0 && s < g_nslots) ? g_hits[s] : 0;
      const char *sp = g_slots[s].path ? g_slots[s].path : "";
      if (strcmp(sp, files[i].path ? files[i].path : "") != 0) {
        continue;
      }
      if (g_slots[s].kind == 'B') {
        if (hit) {
          fprintf(out, "BRDA:%d,0,%d,%llu\n", g_slots[s].line, br_idx, (unsigned long long)hit);
          br_hit++;
        } else {
          fprintf(out, "BRDA:%d,0,%d,-\n", g_slots[s].line, br_idx);
        }
        br_idx++;
        br_tot++;
      }
    }
    fprintf(out, "BRF:%d\n", br_tot);
    fprintf(out, "BRH:%d\n", br_hit);

    for (s = 0; s < g_nslots; s++) {
      const char *sp = g_slots[s].path ? g_slots[s].path : "";
      int t;
      int found = 0;
      if (strcmp(sp, files[i].path ? files[i].path : "") != 0) {
        continue;
      }
      if (g_slots[s].kind != 'L') {
        continue;
      }
      for (t = 0; t < s; t++) {
        const char *tp = g_slots[t].path ? g_slots[t].path : "";
        if (g_slots[t].kind == 'L' && g_slots[t].line == g_slots[s].line &&
            strcmp(tp, sp) == 0) {
          found = 1;
          break;
        }
      }
      if (found) {
        continue;
      }
      {
        uint64_t sum = 0;
        int u;
        for (u = s; u < g_nslots; u++) {
          const char *up = g_slots[u].path ? g_slots[u].path : "";
          if (g_slots[u].kind == 'L' && g_slots[u].line == g_slots[s].line &&
              strcmp(up, sp) == 0) {
            if (g_hits && u >= 0 && u < g_nslots) {
              sum += g_hits[u];
            }
          }
        }
        fprintf(out, "DA:%d,%llu\n", g_slots[s].line, (unsigned long long)sum);
        lf++;
        if (sum) {
          lh++;
        }
      }
    }
    fprintf(out, "LF:%d\n", lf);
    fprintf(out, "LH:%d\n", lh);
    fprintf(out, "end_of_record\n");
  }

  fclose(out);
  for (i = 0; i < file_n; i++) {
    free(files[i].path);
    free(files[i].show);
    free(files[i].uncovered);
  }
  free(files);
}

static void print_report(void) {
  int i;
  int file_n = 0;
  int file_cap = 0;
  CovFile *files = NULL;
  int color = use_color();
  int file_w = 10;
  int uncovered_w = 19;
  int stmts = 0, stmts_hit = 0;
  int branches = 0, branches_hit = 0;
  int funcs = 0, funcs_hit = 0;
  int lines = 0, lines_hit = 0;

  if (!g_slots || g_nslots <= 0) {
    return;
  }

  for (i = 0; i < g_nslots; i++) {
    CovFile *f = find_file(&files, &file_n, &file_cap, g_slots[i].path ? g_slots[i].path : "");
    uint64_t hit = 0;
    int show_n;
    if (!f) {
      continue;
    }
    if (g_hits && i >= 0) {
      hit = g_hits[i];
    }
    switch (g_slots[i].kind) {
      case 'S':
        f->stmts++;
        stmts++;
        if (hit) {
          f->stmts_hit++;
          stmts_hit++;
        }
        break;
      case 'B':
        f->branches++;
        branches++;
        if (hit) {
          f->branches_hit++;
          branches_hit++;
        }
        break;
      case 'F':
        f->funcs++;
        funcs++;
        if (hit) {
          f->funcs_hit++;
          funcs_hit++;
        }
        break;
      case 'L':
        f->lines++;
        lines++;
        if (hit) {
          f->lines_hit++;
          lines_hit++;
        } else {
          add_uncovered(f, g_slots[i].line);
        }
        break;
      default:
        break;
    }
    show_n = (int)strlen(f->show ? f->show : "");
    if (show_n > file_w) {
      file_w = show_n;
    }
  }

  if (file_w > 36) {
    file_w = 36;
  }

  qsort(files, (size_t)file_n, sizeof(CovFile), cmp_file);

  for (i = 0; i < file_n; i++) {
    char buf[256];
    format_uncovered(&files[i], buf, sizeof(buf));
    int n = (int)strlen(buf);
    if (n > uncovered_w) {
      uncovered_w = n;
    }
  }
  if (uncovered_w < 19) {
    uncovered_w = 19;
  }
  if (uncovered_w > 48) {
    uncovered_w = 48;
  }

  fputc('\n', stdout);
  print_border(file_w, uncovered_w);
  printf(" %-*s | %% Stmts | %% Branch | %% Funcs | %% Lines | %-*s\n", file_w, "File",
         uncovered_w, "Uncovered Line #s");
  print_border(file_w, uncovered_w);

  printf(" %-*s |", file_w, "All files");
  print_pct(stmts_hit, stmts, color, 9);
  fputc('|', stdout);
  print_pct(branches_hit, branches, color, 10);
  fputc('|', stdout);
  print_pct(funcs_hit, funcs, color, 9);
  fputc('|', stdout);
  print_pct(lines_hit, lines, color, 9);
  fputc('|', stdout);
  space_repeat(uncovered_w + 1);
  fputc('\n', stdout);

  for (i = 0; i < file_n; i++) {
    char buf[256];
    char shown[40];
    const char *name = files[i].show ? files[i].show : "";
    int nlen = (int)strlen(name);
    format_uncovered(&files[i], buf, sizeof(buf));
    if (nlen > file_w) {
      memcpy(shown, name, (size_t)file_w);
      shown[file_w] = 0;
      name = shown;
    }
    printf(" %-*s |", file_w, name);
    print_pct(files[i].stmts_hit, files[i].stmts, color, 9);
    fputc('|', stdout);
    print_pct(files[i].branches_hit, files[i].branches, color, 10);
    fputc('|', stdout);
    print_pct(files[i].funcs_hit, files[i].funcs, color, 9);
    fputc('|', stdout);
    print_pct(files[i].lines_hit, files[i].lines, color, 9);
    fputc('|', stdout);
    printf(" %-*s\n", uncovered_w, buf);
  }
  print_border(file_w, uncovered_w);

  for (i = 0; i < file_n; i++) {
    free(files[i].path);
    free(files[i].show);
    free(files[i].uncovered);
  }
  free(files);
}

static void avant_cov_exit(void) {
  if (g_hits && g_hits != MAP_FAILED) {
    msync(g_hits, g_hits_bytes, MS_SYNC);
  }
  write_lcov();
  if (should_report()) {
    print_report();
    fflush(stdout);
  }
  if (g_hits && g_hits != MAP_FAILED) {
    munmap(g_hits, g_hits_bytes);
    g_hits = NULL;
  }
  if (g_hits_fd >= 0) {
    close(g_hits_fd);
    g_hits_fd = -1;
  }
}

void avant_cov_init(const char *map_path) {
  if (g_inited) {
    return;
  }
  g_inited = 1;
  if (!map_path || !map_path[0]) {
    return;
  }
  g_map_path = xstrdup(map_path);
  if (!g_map_path) {
    return;
  }
  if (parse_map(g_map_path) != 0) {
    return;
  }
  if (open_hits(g_map_path) != 0) {
    g_hits = (uint64_t *)calloc((size_t)(g_nslots > 0 ? g_nslots : 1), sizeof(uint64_t));
    g_hits_bytes = (size_t)(g_nslots > 0 ? g_nslots : 1) * sizeof(uint64_t);
    g_hits_fd = -1;
  }
  atexit(avant_cov_exit);
}

void avant_cov_hit(int32_t slot) {
  if (!g_hits || slot < 0 || slot >= g_nslots) {
    return;
  }
  __sync_fetch_and_add(&g_hits[slot], 1);
}
