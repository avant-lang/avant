/* la-peak.c — a program's exact peak resident memory.
 *
 *   la-peak [-R] -o OUT PROGRAM [ARG...]
 *
 * Runs PROGRAM as its child and writes to OUT one line:
 *
 *   EXACT_KB <SP> MAXRSS_KB <SP> READS
 *
 * MAXRSS_KB is the child's ru_maxrss, what GNU time's %M reads. The kernel
 * keeps a process's resident-page counters per CPU and folds each CPU's part
 * into the total only once it reaches a batch (64 pages, 256 KB, on a machine
 * of 32 CPUs or more), and the high-water mark behind ru_maxrss is taken from
 * the folded total: a peak read that way moves in steps of about 256 KB with
 * the phase of the batches, at equal work. EXACT_KB is the peak read exactly:
 * the resident size of a process only falls in a system call that unmaps or
 * discards memory (munmap, madvise, brk, mremap, shmdt, mmap over an existing
 * mapping) or at its exit, and between such calls it can only grow, so its
 * largest value is its value just before one of them or at exit_group. A
 * seccomp filter installed before PROGRAM starts stops each of those calls
 * (every thread's) until this program has read /proc/TID/statm, whose resident
 * field this kernel sums exactly over the CPUs, and lets the call go on. READS
 * is the number of such readings. A process killed by a signal has no final
 * reading: its EXACT_KB is the largest before its last such call.
 *
 * In a threaded program another thread may fault pages in between a reading
 * and the call that stopped for it; those pages are counted at the next
 * reading unless that call itself unmaps them first, a window of one system
 * call (no single-threaded program has it).
 *
 * -R starts PROGRAM with address randomization off, as `setarch -R`. The exit
 * status is PROGRAM's (128 + n for a signal's death); 125 when this program
 * fails. Linux on x86-64 only (the seccomp user notification, 5.5 or later).
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
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *what) {
  fprintf(stderr, "la-peak: %s: %s\n", what, strerror(errno));
  exit(125);
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

/* The resident size of TID's process in KB, from /proc/TID/statm; -1 when
 * it cannot be read (the task is gone). */
static long resident_kb(pid_t tid, long page_kb) {
  char path[64], buf[256];
  snprintf(path, sizeof path, "/proc/%d/statm", (int)tid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  long size, res;
  if (sscanf(buf, "%ld %ld", &size, &res) != 2) return -1;
  return res * page_kb;
}

int main(int argc, char **argv) {
  int norand = 0;
  const char *out = NULL;
  int i = 1;
  for (; i < argc; i++) {
    if (strcmp(argv[i], "-R") == 0) norand = 1;
    else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    else if (strcmp(argv[i], "--") == 0) { i++; break; }
    else break;
  }
  if (!out || i >= argc) {
    fprintf(stderr, "usage: la-peak [-R] -o OUT PROGRAM [ARG...]\n");
    return 125;
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
    if (lfd < 0) { fprintf(stderr, "la-peak: seccomp: %s\n", strerror(errno)); _exit(125); }
    send_fd(sv[1], lfd);
    close(lfd);
    close(sv[1]);
    execvp(argv[i], argv + i);
    fprintf(stderr, "la-peak: %s: %s\n", argv[i], strerror(errno));
    _exit(127);
  }
  close(sv[1]);
  int lfd = recv_fd(sv[0]);
  close(sv[0]);
  if (lfd < 0) {
    int st;
    waitpid(child, &st, 0);
    fprintf(stderr, "la-peak: no seccomp listener from the child\n");
    return 125;
  }
  long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  struct seccomp_notif_sizes sz;
  if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sz) != 0) die("seccomp sizes");
  struct seccomp_notif *req = calloc(1, sz.seccomp_notif < sizeof *req ? sizeof *req : sz.seccomp_notif);
  struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp < sizeof *resp ? sizeof *resp : sz.seccomp_notif_resp);
  if (!req || !resp) die("calloc");
  long peak = 0, reads = 0;
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
      long kb = resident_kb((pid_t)req->pid, page_kb);
      if (kb > peak) peak = kb;
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
  fprintf(f, "%ld %ld %ld\n", peak, ru.ru_maxrss, reads);
  if (fclose(f) != 0) die(out);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return WEXITSTATUS(st);
}
