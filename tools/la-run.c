/* la-run.c — a program run as tools/la-count.sh measures it: from a path
 * that does not depend on where the program lies, and with its exact peak
 * resident memory.
 *
 *   la-run [-R] [-t SECONDS] [-n NAME] [-p OUT] PROGRAM [ARG...]
 *
 * PROGRAM is started through a file descriptor (execveat of an O_PATH
 * descriptor numbered RUN_FD, so the kernel names it /dev/fd/RUN_FD in the
 * auxiliary vector) with NAME as its argv[0] (default PROGRAM's last path
 * component): the strings the kernel copies to the top of the new stack, and
 * whatever the program copies from them (argv() copies argv into the heap),
 * are then the same wherever PROGRAM lies. From two paths of different
 * lengths a program's stack starts at different offsets and its argv copy
 * has another size, so its instruction count could move by a whole
 * collector pass with the path (a collector benchmark's scenario read
 * 145.6 M or 138.1 M instructions by the length of its directory's name).
 *
 * With -p OUT, PROGRAM runs as a child and OUT gets one line:
 *
 *   EXACT_KB <SP> MAXRSS_KB <SP> READS <SP> ANON_KB
 *
 * MAXRSS_KB is the child's ru_maxrss, what GNU time's %M reads. The kernel
 * keeps a process's resident-page counters per CPU and folds each CPU's part
 * into the total only once it reaches a batch (64 pages, 256 KB, on a machine
 * of 32 CPUs or more), and the high-water mark behind ru_maxrss is taken from
 * the folded total: a peak read that way moves in steps of about 256 KB with
 * the phase of the batches, at equal work, and reads up to a batch per CPU
 * the process ran on below (or, after frees, above) the true peak. EXACT_KB
 * is the peak read exactly: the resident size of a process only falls in a
 * system call that unmaps or discards memory (munmap, madvise, brk, mremap,
 * shmdt, mmap over an existing mapping) or at its exit, and between such
 * calls it can only grow, so its largest value is its value just before one
 * of them or at exit_group. A seccomp filter installed before PROGRAM starts
 * stops each of those calls (every thread's) until this program has read
 * /proc/TID/statm, whose resident field the kernel sums exactly over the
 * CPUs, and lets the call go on. READS is the number of such readings, and
 * ANON_KB the largest of them less the file-backed and shared pages
 * (statm's third field): a program's resident file pages also depend on
 * the page cache (which pages of its binary and libraries are cached, in
 * which folio sizes, and what the kernel reclaims from them while it runs),
 * so two runs of one program can read a few pages apart in EXACT_KB, never
 * in ANON_KB. A
 * process killed by a signal has no final reading: its EXACT_KB is the
 * largest before its last such call. In a threaded program another thread
 * may fault pages in between a reading and the call that stopped for it;
 * those pages are counted at the next reading unless that call itself
 * unmaps them first, a window of one system call (no single-threaded
 * program has it).
 *
 * -t SECONDS ends PROGRAM by SIGALRM after that many seconds (an alarm set
 * before it starts, which an exec keeps), in place of timeout(1): under
 * `perf stat`, which counts every process of the command, timeout's own
 * two million instructions moved by about two thousand from run to run,
 * where this program's own start before the exec repeats to a few.
 * -R starts PROGRAM with address randomization off, as `setarch -R`.
 * Without -p this program becomes PROGRAM (no child). The exit status is
 * PROGRAM's (128 + n for a signal's death, with -p); 125 when this program
 * fails, 127 when PROGRAM cannot be started. Linux on x86-64 only (execveat,
 * the seccomp user notification: 5.5 or later).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The descriptor PROGRAM is started through: one number on every run, so
 * the name the kernel gives it is always the same string. */
#define RUN_FD 200

static void die(const char *what) {
  fprintf(stderr, "la-run: %s: %s\n", what, strerror(errno));
  exit(125);
}

static unsigned alarm_secs;

/* Become PROGRAM (FD, opened as RUN_FD; PATH its path) with ARGV, its alarm
 * set (SIGALRM at its default action, unblocked); returns only on failure.
 * A script cannot start through a close-on-exec descriptor (its interpreter
 * would open /dev/fd/RUN_FD after the exec closed it): it starts from its
 * path, which its count then depends on, as before. */
static void run(int fd, const char *path, char **argv) {
  extern char **environ;
  if (alarm_secs) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    signal(SIGALRM, SIG_DFL);
    alarm(alarm_secs);
  }
  syscall(SYS_execveat, fd, "", argv, environ, AT_EMPTY_PATH);
  if (errno == ENOENT) execve(path, argv, environ);
}

/* Stop every call that can lower the resident size; let the rest go. */
static int install_filter(void) {
  /* Each call's jump lands on the USER_NOTIF return (index 14). */
  struct sock_filter f[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_munmap, 9, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_madvise, 8, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_brk, 7, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mremap, 6, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_shmdt, 5, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 4, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    /* mmap: only over an existing mapping (MAP_FIXED, the low word of its
     * flags argument) can it drop resident pages. */
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[3])),
    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, MAP_FIXED, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog = { .len = sizeof f / sizeof f[0], .filter = f };
  return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

static void send_fd(int sock, int fd) {
  char c = 0;
  struct iovec io = { .iov_base = &c, .iov_len = 1 };
  union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
  struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
  struct cmsghdr *h = CMSG_FIRSTHDR(&m);
  h->cmsg_level = SOL_SOCKET;
  h->cmsg_type = SCM_RIGHTS;
  h->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(h), &fd, sizeof(int));
  if (sendmsg(sock, &m, 0) != 1) _exit(125);
}

static int recv_fd(int sock) {
  char c;
  struct iovec io = { .iov_base = &c, .iov_len = 1 };
  union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
  struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf };
  if (recvmsg(sock, &m, 0) != 1) return -1;
  struct cmsghdr *h = CMSG_FIRSTHDR(&m);
  if (!h || h->cmsg_type != SCM_RIGHTS) return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(h), sizeof(int));
  return fd;
}

/* The resident size of TID's process in KB, from /proc/TID/statm, and in
 * *ANON its part not backed by a file or shared; -1 when it cannot be read
 * (the task is gone). */
static long resident_kb(pid_t tid, long page_kb, long *anon) {
  char path[64], buf[256];
  snprintf(path, sizeof path, "/proc/%d/statm", (int)tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  long size, res, shared;
  if (sscanf(buf, "%ld %ld %ld", &size, &res, &shared) != 3) return -1;
  *anon = (res - shared) * page_kb;
  return res * page_kb;
}

int main(int argc, char **argv) {
  int norand = 0;
  const char *out = NULL, *name = NULL;
  int i = 1;
  for (; i < argc; i++) {
    if (strcmp(argv[i], "-R") == 0) norand = 1;
    else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) out = argv[++i];
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
    else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) alarm_secs = (unsigned)strtoul(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--") == 0) { i++; break; }
    else break;
  }
  if (i >= argc || argv[i][0] == '-') {
    fprintf(stderr, "usage: la-run [-R] [-t SECONDS] [-n NAME] [-p OUT] PROGRAM [ARG...]\n");
    return 125;
  }
  const char *prog = argv[i];
  static char path_buf[4096];
  int pfd = -1;
  if (strchr(prog, '/')) {
    snprintf(path_buf, sizeof path_buf, "%s", prog);
    pfd = open(prog, O_PATH | O_CLOEXEC);
  } else {
    /* A bare name is looked up on PATH, as execvp does. */
    const char *path = getenv("PATH");
    for (const char *d = path ? path : "/usr/bin:/bin"; pfd < 0 && *d;) {
      size_t n = strcspn(d, ":");
      if (snprintf(path_buf, sizeof path_buf, "%.*s/%s", (int)n, n ? d : ".", prog) < (int)sizeof path_buf && access(path_buf, X_OK) == 0)
        pfd = open(path_buf, O_PATH | O_CLOEXEC);
      d += n;
      if (*d == ':') d++;
    }
    if (pfd < 0) errno = ENOENT;
  }
  if (pfd < 0) { fprintf(stderr, "la-run: %s: %s\n", prog, strerror(errno)); return 127; }
  if (pfd != RUN_FD) {
    if (dup3(pfd, RUN_FD, O_CLOEXEC) != RUN_FD) die("dup3");
    close(pfd);
  }
  if (!name) {
    const char *slash = strrchr(prog, '/');
    name = slash ? slash + 1 : prog;
  }
  argv[i] = (char *)name;
  if (!out) {
    if (norand && personality(ADDR_NO_RANDOMIZE) == -1) die("personality");
    run(RUN_FD, path_buf, argv + i);
    fprintf(stderr, "la-run: %s: %s\n", prog, strerror(errno));
    return 127;
  }
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) die("socketpair");
  pid_t parent = getpid();
  pid_t child = fork();
  if (child < 0) die("fork");
  if (child == 0) {
    close(sv[0]);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(125);
    if (norand && personality(ADDR_NO_RANDOMIZE) == -1) _exit(125);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(125);
    int lfd = install_filter();
    if (lfd < 0) { fprintf(stderr, "la-run: seccomp: %s\n", strerror(errno)); _exit(125); }
    send_fd(sv[1], lfd);
    close(lfd);
    close(sv[1]);
    run(RUN_FD, path_buf, argv + i);
    fprintf(stderr, "la-run: %s: %s\n", prog, strerror(errno));
    _exit(127);
  }
  close(RUN_FD);
  close(sv[1]);
  int lfd = recv_fd(sv[0]);
  close(sv[0]);
  if (lfd < 0) {
    int st;
    waitpid(child, &st, 0);
    fprintf(stderr, "la-run: no seccomp listener from the child\n");
    return 125;
  }
  long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  struct seccomp_notif_sizes sz;
  if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sz) != 0) die("seccomp sizes");
  struct seccomp_notif *req = calloc(1, sz.seccomp_notif < sizeof *req ? sizeof *req : sz.seccomp_notif);
  struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp < sizeof *resp ? sizeof *resp : sz.seccomp_notif_resp);
  if (!req || !resp) die("calloc");
  long peak = 0, peak_anon = 0, reads = 0;
  for (;;) {
    struct pollfd p = { .fd = lfd, .events = POLLIN };
    int r = poll(&p, 1, -1);
    if (r < 0) { if (errno == EINTR) continue; die("poll"); }
    if (p.revents & POLLIN) {
      memset(req, 0, sz.seccomp_notif);
      if (ioctl(lfd, SECCOMP_IOCTL_NOTIF_RECV, req) != 0) {
        if (errno == EINTR || errno == ENOENT) continue;
        break;
      }
      long anon = 0, kb = resident_kb((pid_t)req->pid, page_kb, &anon);
      if (kb > peak) peak = kb;
      if (kb >= 0 && anon > peak_anon) peak_anon = anon;
      if (kb >= 0) reads++;
      memset(resp, 0, sz.seccomp_notif_resp);
      resp->id = req->id;
      resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
      /* ENOENT: the call was interrupted by a signal meanwhile. */
      ioctl(lfd, SECCOMP_IOCTL_NOTIF_SEND, resp);
      continue;
    }
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
  }
  int st;
  struct rusage ru;
  while (wait4(child, &st, 0, &ru) < 0) if (errno != EINTR) die("wait4");
  FILE *f = fopen(out, "w");
  if (!f) die(out);
  fprintf(f, "%ld %ld %ld %ld\n", peak, ru.ru_maxrss, reads, peak_anon);
  if (fclose(f) != 0) die(out);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return WEXITSTATUS(st);
}
