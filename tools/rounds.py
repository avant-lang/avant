#!/usr/bin/env python3
"""Median-of-N rounds with rotation: the one timing loop.

FOR MILESTONES AND PUBLICATION, NEVER PER GATE: a gate's LangArena evidence
stays the single run of `measure.cr -- prod` and `tools/lr-table.sh avant`;
this mode costs N + 1 runs of every task.

  python3 tools/rounds.py langarena --rounds N --run-js RUN_JS --out DIR
          [--side NAME=BIN_DIR]... [--layouts S1,S2,...] [--pin CPU] [--short SECONDS]
          [--timeout SECONDS] [--allow-governor] [--cpu-tolerance F]
          [--max-involuntary N] [--max-outside CPUS] [--retries N] [TASK...]
  python3 tools/rounds.py table DIR
  python3 tools/rounds.py spawner DIR      (compiles the spawner into DIR, prints its path)
  python3 tools/rounds.py judge RESULT [THREADS]  (one spawner result by the undisturbed rule; THREADS the program's own)

`tools/lr-table.sh rounds ...` calls the first form. BIN_DIR holds the
harness-built Avant binaries (langarena/tmp after `measure.cr -- prod`); with
no --side, the checkout's own langarena/tmp is the one side, named `avant`.
Several sides (a base and a new tree) are measured in the same rounds, their
order rotated by round. TASK names restrict the set (default: every task of
RUN_JS with a binary in every side).

--layouts S1,S2,... times every task across code layouts: each BIN_DIR then
holds one directory per shift, BIN_DIR/S/SLUG, the same program with its
code S bytes later (tools/layout-cc.sh links them), and round r runs every
side's binary at the ((r - 1) mod K)-th shift (the warm-up at S1). With as
many rounds as shifts, every layout runs once per side, so a task's median
is over its layouts and its min-max spread covers what one address can move
it: one address no longer decides a published number. Each raw file names
its `layout`; binaries.sha256 lists every layout's binary (SIDE@S--SLUG).

The loop, lifted from bench/valk/measure.py's `time` with
its kept-but-bad settled; bench/valk/measure.py now calls it:

- the governor must be `performance` on every CPU (else it stops, unless
  --allow-governor, which marks the session "DISTURBED, NOT EVIDENCE");
- one unmeasured warm-up of every row, then N rounds of one fresh process
  per row; round r starts (r - 1) * (rows // N) rows (at least r - 1) into
  the list, every other round walks it backwards from there, and the
  sides' order rotates;
- the program is started by a small C spawner (compiled into DIR at the
  session's start, its sha256 in session.txt) as its only child, nothing
  else between (no GNU time, no perf, no harness): the spawner reads the
  monotonic clock just before vfork and just after wait4, so no wrapper's
  start-up is in the wall time (GNU time's was), and peak RSS, user
  and system time are wait4's rusage of that one process, so the RSS is
  the program's own peak, never a wrapper's (the per-task rounds scripts'
  /usr/bin/time around perf read perf's 17 MB for every small task, and a
  child of Python would read Python's 12 to 20 MB);
- stdin from /dev/null, the output to a file, capped, under a timeout;
  every run's output is checked (the first line against RUN_JS's checksum):
  a wrong output, a non-zero exit or a timeout stops the session, it is not
  a result;
- one raw file per run: DIR/raw/round-R/SIDE--SLUG.run (and the spawn
  floor's in DIR/floor/raw);
- --pin CPU runs every single-threaded task on that CPU (the parent's
  affinity, which the child inherits; Matmul::T* are never pinned);
- only undisturbed runs count: the spawner also records the task's
  voluntary and involuntary context switches and the CPU time the CPUs it
  may run on (with their SMT siblings) and the whole machine spent outside
  it, from /proc/stat. A run whose CPU time falls under its wall, whose
  involuntary switches are too many, or beside which other work ran (the
  thresholds and their measurements are at CPU_TOLERANCE below) is
  disturbed: it is run again at once, at most --retries times (default
  3), and kept beside the result as FILE.disturbed-N; a run still
  disturbed after the retries is the round's FILE.run marked
  `undisturbed no`. Medians are of the undisturbed runs only, for every
  side; rounds.md lists every disturbed run with its counters and names
  each row short of undisturbed runs (to re-run).

Very short tasks: a LangArena task cannot repeat its work inside one
process (its sizes are compiled in and it prints only a checksum), so a task
whose median is under --short seconds (default 0.2, as the LR table's
"<0.2 s" column) is flagged in the table, beside the spawn floor: the same
loop's median for /bin/true, the cost of starting and reaping a process.

Written by the script: DIR/session.txt (date, kernel, CPU, governor and load
before and after, the Python, every side's checkout HEAD and compiler
versions file, the binaries' sha256 in binaries.sha256), DIR/rounds.tsv and
DIR/rounds.md (per side and task: median, minimum, maximum and spread of
the wall; median, minimum and maximum peak RSS; never a best-of), and per
side DIR/SIDE.lr-avant.tsv, the medians in `tools/lr-table.sh`'s TSV form,
so `lr-table.sh table REF DIR/SIDE.lr-avant.tsv` gives the multiplier table
of the medians. `table DIR` rewrites rounds.tsv, rounds.md and the
lr-avant.tsv files from the raw files.
"""
import hashlib
import os
import platform
import re
import signal
import statistics
import subprocess
import sys
import time

CAP = 50_000_000


def die(msg):
    sys.stderr.write("rounds.py: %s\n" % msg)
    sys.exit(1)


# ---- the loop ---------------------------------------------------------------

def governors():
    seen = {}
    base = "/sys/devices/system/cpu"
    for name in sorted(os.listdir(base)):
        p = os.path.join(base, name, "cpufreq", "scaling_governor")
        if name.startswith("cpu") and os.path.exists(p):
            g = open(p).read().strip()
            seen[g] = seen.get(g, 0) + 1
    return seen


def loadavg():
    return open("/proc/loadavg").read().strip()


# The spawner: a small C program, compiled into each session's directory,
# that starts the measured program as its own child and reports what the
# kernel measured for it. It exists because the measuring process cannot be
# the parent: a child's peak RSS (ru_maxrss) starts from its parent's memory
# at the fork, so a program started by Python reads Python's 12 to 20 MB,
# and one started under perf or a harness reads theirs. The spawner's
# own memory is under any program's (it reads /bin/true's 1.4 MB as 1.4 MB),
# and it reads the clock itself just before vfork and just after wait4, so
# its own start-up is outside the interval, which GNU time's was not.
SPAWN_C = r"""
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
/* spawn RESULT PROGRAM ARGS...: RESULT gets "wall_ns maxrss_kb utime_us
   stime_us wait_status nvcsw nivcsw busy_mine busy_all cpus_mine hz"
   (tools/rounds.py): busy_mine and busy_all are the clock ticks every CPU
   the program may run on (this process's affinity, widened by each CPU's
   SMT siblings, which share its core), and every CPU of the machine, spent
   outside idle and iowait between vfork and wait4, from /proc/stat; the
   program's own CPU time is inside them. */
static void add_siblings(cpu_set_t *mine) {
  cpu_set_t base = *mine;
  int c;
  for (c = 0; c < CPU_SETSIZE; c++) {
    char path[128], list[256], *p;
    FILE *f;
    if (!CPU_ISSET(c, &base)) continue;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
    f = fopen(path, "r");
    if (!f) continue;
    if (fgets(list, sizeof list, f)) {
      for (p = list; *p;) {
        char *end; long a = strtol(p, &end, 10), b = a;
        if (end == p) break;
        if (*end == '-') { p = end + 1; b = strtol(p, &end, 10); }
        for (; a <= b && a < CPU_SETSIZE; a++) if (a >= 0) CPU_SET((int)a, mine);
        p = *end == ',' ? end + 1 : end;
        if (*p == '\n') break;
      }
    }
    fclose(f);
  }
}
static int busy(const cpu_set_t *mine, long long *b_mine, long long *b_all) {
  char line[512]; FILE *f = fopen("/proc/stat", "r");
  *b_mine = *b_all = 0;
  if (!f) return -1;
  while (fgets(line, sizeof line, f)) {
    int n; unsigned long long v[8] = {0}; long long b;
    if (strncmp(line, "cpu", 3) != 0) break;
    if (line[3] == ' ') {
      sscanf(line + 3, "%llu %llu %llu %llu %llu %llu %llu %llu", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7);
      *b_all = (long long)(v[0] + v[1] + v[2] + v[5] + v[6] + v[7]);
      continue;
    }
    if (sscanf(line + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu", &n, v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7) < 9) continue;
    b = (long long)(v[0] + v[1] + v[2] + v[5] + v[6] + v[7]);
    if (n >= 0 && n < CPU_SETSIZE && CPU_ISSET(n, mine)) *b_mine += b;
  }
  fclose(f);
  return 0;
}
int main(int argc, char **argv) {
  struct timespec a, b; struct rusage ru; int st; pid_t p; FILE *f; cpu_set_t mine;
  long long m0, a0, m1, a1;
  if (argc < 3) return 125;
  f = fopen(argv[1], "w");
  if (!f) return 125;
  if (sched_getaffinity(0, sizeof mine, &mine) != 0) return 125;
  add_siblings(&mine);
  if (busy(&mine, &m0, &a0) != 0) return 125;
  clock_gettime(CLOCK_MONOTONIC, &a);
  p = vfork();
  if (p == 0) { execv(argv[2], argv + 2); _exit(127); }
  if (p < 0 || wait4(p, &st, 0, &ru) < 0) return 125;
  clock_gettime(CLOCK_MONOTONIC, &b);
  if (busy(&mine, &m1, &a1) != 0) return 125;
  fprintf(f, "%lld %ld %lld %lld %d %ld %ld %lld %lld %d %ld\n",
          (long long)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec), ru.ru_maxrss,
          (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec,
          (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec, st,
          ru.ru_nvcsw, ru.ru_nivcsw, m1 - m0, a1 - a0, CPU_COUNT(&mine), sysconf(_SC_CLK_TCK));
  return fclose(f) == 0 ? 0 : 125;
}
"""


def build_spawn(d):
    """Compiles the spawner into d; returns its path and a line for the session file."""
    src, exe = os.path.join(d, "spawn.c"), os.path.join(d, "spawn")
    with open(src, "w") as f:
        f.write(SPAWN_C.lstrip("\n"))
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-O2", "-o", exe, src], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        die("cannot compile the spawner with %s: %s" % (cc, r.stderr.strip()[-400:]))
    ver = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.split("\n")[0]
    return exe, "spawner: %s (%s -O2; %s), sha256 %s" % (exe, cc, ver, hashlib.sha256(open(exe, "rb").read()).hexdigest())


def parse_result(f):
    """A spawner result line's integers as run_once's dict."""
    wall, rss, ut, st, status = f[:5]
    code = os.waitstatus_to_exitcode(status)
    res = {"wall_ns": wall, "exit": code if code >= 0 else 128 - code, "rss_kb": rss,
           "user_s": ut / 1e6, "sys_s": st / 1e6, "timed_out": False}
    if len(f) >= 11:
        nv, niv, b_mine, b_all, cpus, hz = f[5:11]
        cpu = res["user_s"] + res["sys_s"]
        res.update({"nvcsw": nv, "nivcsw": niv, "cpus": cpus,
                    # CPU time spent outside the program (idle and iowait excluded), on the CPUs it may run on and
                    # on the whole machine, over its wall; the program's own CPU time is taken out of both
                    "outside_mine_s": max(0.0, b_mine / hz - cpu), "outside_all_s": max(0.0, b_all / hz - cpu)})
    return res


def run_once(spawn, argv, cwd, out_path, err_path, limit, pin=None, env=None):
    """Runs argv (argv[0] a path, relative to cwd or absolute) under the
    spawner, with env (a complete environment; None: this process's).
    Returns a dict: wall_ns, exit (128 + n for a signal), rss_kb, user_s,
    sys_s, timed_out."""
    res_path = out_path + ".res"
    old = None
    if pin is not None:
        old = os.sched_getaffinity(0)
        os.sched_setaffinity(0, {pin})
    try:
        with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
            proc = subprocess.Popen([spawn, res_path] + argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=fo, stderr=fe,
                                    start_new_session=True, env=env)
            try:
                rc = proc.wait(timeout=limit)
                timed_out = False
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                rc = proc.wait()
                timed_out = True
    finally:
        if old is not None:
            os.sched_setaffinity(0, old)
    if timed_out:
        return {"wall_ns": 0, "exit": 137, "rss_kb": 0, "user_s": 0.0, "sys_s": 0.0, "timed_out": True}
    if rc != 0:
        die("the spawner failed (status %d) on %s" % (rc, " ".join(argv)))
    f = [int(x) for x in open(res_path).read().split()]
    os.remove(res_path)
    res = parse_result(f)
    return res


# The undisturbed rule: a timed run counts only when the program had the CPU
# for its whole wall time and nothing else ran beside it. Measured on this
# server (an EPYC 7351P, 16 cores, 2 threads each, boost off), idle and
# beside known competitors, on a short (0.25 s), a long (8 s) and two
# threaded tasks; the tools' change records the table:
# - a CPU shared with another process: the CPU time falls under the wall
#   (by 50% for a full time-share, by 2 to 20% on a saturated machine);
#   idle, the gap is at most 0.23% (process start and reaping);
# - a threaded program waiting for CPUs: its involuntary switches rise to
#   560 to 2,200 a second; idle, at most 134 a second (16 threads), and at
#   most 61 a second for a single thread;
# - a competitor on the core's other SMT thread, or anywhere on a busy
#   machine, slows a task by 15 to 57% with neither sign (the program has
#   its CPU): only the CPU time spent outside the program on the CPUs it may
#   run on and their SMT siblings shows it, one CPU's worth for one
#   competitor; idle, at most 0.53 CPUs on average (typically under 0.3).
# For CPU-bound programs only: a program that waits for input or sleeps is
# not timed here.
# A program whose own threads outnumber the CPUs it may run on (Matmul::T16
# in one NUMA node's 8 CPUs) preempts itself: idle, in a node slot, its
# involuntary switches reached 133 to 414 a second (16 threads on 8 CPUs)
# where the same program on the whole machine stays near 50; so the limit
# on involuntary switches is scaled by its threads over its CPUs for such a
# program (500 a second for 16 threads on 8 CPUs), its threads read from
# the task's configuration (cfg_threads). The other two signs, and the
# CPU time outside the program on its CPUs above all, are judged as for
# any program: another process on its CPUs still shows there.
CPU_TOLERANCE = 0.01      # wall - (user + sys) at most this fraction of the wall...
CPU_SLACK_S = 0.005       # ...or this many seconds, whichever is larger (process start and reaping)
MAX_INVOLUNTARY_PER_S = 250  # involuntary context switches per second of wall
MAX_OUTSIDE_CPUS = 0.75   # CPU time outside the program over its wall, on its CPUs and their siblings
RETRIES = 3               # a disturbed run is run again at once, at most this many times


def rule_from_env():
    """ROUNDS_RULE="tolerance=F,involuntary=N,outside=CPUS,retries=N" (any of them) overrides the thresholds, for
    the tools' own tests (and lr-table.sh, which judges through this file); the session file and every table
    state the rule a run used."""
    for part in filter(None, os.environ.get("ROUNDS_RULE", "").split(",")):
        k, _, v = part.partition("=")
        flag = {"tolerance": "--cpu-tolerance", "involuntary": "--max-involuntary", "outside": "--max-outside",
                "retries": "--retries"}.get(k.strip())
        if not flag:
            die("ROUNDS_RULE: unknown %r (tolerance, involuntary, outside, retries)" % k)
        set_rule(flag, v.strip())


def rule_text():
    return ("undisturbed when the CPU time is under the wall by at most %g%% or %g s, at most %g involuntary switches a second "
            "(times its threads over its CPUs when they outnumber them), "
            "at most %g CPUs busy outside the task on its CPUs and their SMT siblings; a disturbed run is run again at once, at most %d times"
            % (CPU_TOLERANCE * 100, CPU_SLACK_S, MAX_INVOLUNTARY_PER_S, MAX_OUTSIDE_CPUS, RETRIES))


def set_rule(flag, value):
    """--cpu-tolerance FRACTION, --max-involuntary PER_SECOND, --max-outside CPUS, --retries N: the rule's
    thresholds for this session (the defaults are the measured ones above)."""
    global CPU_TOLERANCE, MAX_INVOLUNTARY_PER_S, MAX_OUTSIDE_CPUS, RETRIES
    try:
        if flag == "--cpu-tolerance":
            CPU_TOLERANCE = float(value)
        elif flag == "--max-involuntary":
            MAX_INVOLUNTARY_PER_S = float(value)
        elif flag == "--max-outside":
            MAX_OUTSIDE_CPUS = float(value)
        else:
            RETRIES = int(value)
    except ValueError:
        die("%s needs a number, not %r" % (flag, value))


def disturbance(res, tol=None, slack=None, max_inv=None, max_out=None, threads=None):
    """None when the run is undisturbed, else the reason (the counters that broke the rule). threads: the
    program's own threads, when known (cfg_threads): above its CPUs, the limit on involuntary switches is
    scaled by threads / CPUs."""
    if "nivcsw" not in res:
        return None  # a result from before the counters: nothing to judge by
    tol = CPU_TOLERANCE if tol is None else tol
    slack = CPU_SLACK_S if slack is None else slack
    max_inv = MAX_INVOLUNTARY_PER_S if max_inv is None else max_inv
    max_out = MAX_OUTSIDE_CPUS if max_out is None else max_out
    wall = res["wall_ns"] / 1e9
    cpu = res["user_s"] + res["sys_s"]
    why = []
    if wall - cpu > max(tol * wall, slack):
        why.append("CPU time %.4f s under the wall %.4f s by %.1f%%" % (cpu, wall, (wall - cpu) / wall * 100 if wall else 0))
    own = ""
    if threads and res.get("cpus") and threads > res["cpus"]:
        own = "; %d threads on %d CPUs" % (threads, res["cpus"])
        max_inv = max_inv * threads / res["cpus"]
    if wall > 0 and res["nivcsw"] > max(10, max_inv * wall):  # at least 10 for a run of a few milliseconds
        why.append("%d involuntary context switches, %.0f a second (at most %d%s)" % (res["nivcsw"], res["nivcsw"] / wall, max_inv, own))
    if wall > 0 and res["outside_mine_s"] > max(max_out * wall, 0.05):  # 0.05 s: five clock ticks, the counters' grain
        why.append("%.2f CPUs busy outside the program on its CPUs (at most %g)" % (res["outside_mine_s"] / wall, max_out))
    return "; ".join(why) or None


def rotate(rows, rounds, r):
    """Round r's order (1-based): it starts at row (r - 1) * (rows // rounds)
    (at least one further each round) and every other round walks the list
    backwards from there. (bench/valk/measure.py reversed the rotated list,
    so with one row of step a backward round began where the round before
    it began: with two rows, the same row was always first.)"""
    n = len(rows)
    step = max(1, n // rounds)
    k = (r - 1) * step % n
    if r % 2 == 0:
        return [rows[(k - j) % n] for j in range(n)]
    return rows[k:] + rows[:k]


def session_open(d, allow_governor, lines):
    gov = governors()
    if list(gov) != ["performance"] and not allow_governor:
        die("governor is %s, not performance: no timed work (--allow-governor for a dry run)" % gov)
    os.makedirs(d, exist_ok=False)
    meta = open(os.path.join(d, "session.txt"), "w")

    def note(s):
        meta.write(s + "\n")
        meta.flush()
        print(s, flush=True)
    note("date: %s" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    note("kernel: %s" % os.uname().release)
    cpu = [l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")]
    note("cpu: %s x%d" % (cpu[0] if cpu else "?", len(cpu)))
    note("python: %s" % platform.python_version())
    note("governor: %s" % ", ".join("%s x%d" % kv for kv in gov.items()))
    note("loadavg before: %s" % loadavg())
    if list(gov) != ["performance"] or allow_governor:
        note("DISTURBED, NOT EVIDENCE: a dry run (--allow-governor); the numbers prove the runner, nothing else")
    for l in lines:
        note(l)
    return note, meta


def session_close(note, meta):
    note("loadavg after: %s" % loadavg())
    note("governor after: %s" % ", ".join("%s x%d" % kv for kv in governors().items()))
    meta.close()


def write_run(path, row, r, pos, res, out, why, attempt):
    """One run's raw file: the fields every reader knows, then the counters, the attempt and the verdict."""
    with open(path, "w") as f:
        f.write("id\t%s\nround\t%d\nposition\t%d\nwall_ns\t%d\nrss_kb\t%d\nuser_s\t%.6f\nsys_s\t%.6f\n"
                "exit\t%d\noutput_sha256\t%s\noutput\tok\n"
                % (row["id"], r, pos, res["wall_ns"], res["rss_kb"], res["user_s"], res["sys_s"],
                   res["exit"], hashlib.sha256(out).hexdigest()))
        for k, v in row.get("fields", {}).items():
            f.write("%s\t%s\n" % (k, v))
        f.write("first_line\t%s\n" % out.split(b"\n", 1)[0].decode("utf-8", "replace").strip())
        if "nivcsw" in res:
            f.write("nvcsw\t%d\nnivcsw\t%d\ncpus\t%d\noutside_mine_s\t%.3f\noutside_all_s\t%.3f\n"
                    % (res["nvcsw"], res["nivcsw"], res["cpus"], res["outside_mine_s"], res["outside_all_s"]))
            f.write("attempt\t%d\nundisturbed\t%s\n" % (attempt + 1, "no" if why else "yes"))
            if why:
                f.write("disturbed\t%s\n" % why)


def measure(spawn, rows, rounds, d, limit, check, note, pin=None, order_of=None, raw="raw", keep=False):
    """The loop. rows: dicts with id, argv, cwd, pin (bool: may be pinned),
    env (optional: the run's complete environment), fields (written into
    the raw file). check(row, result, out, err) returns None when the run
    is a result, else the reason (the session stops). order_of(r) gives
    round r's order (default: rotate). One unmeasured warm-up of every row
    first; one raw file per run, d/RAW/round-R/FILE.run (FILE: the row's
    "file", else its id), and with keep the run's standard output beside
    it as FILE.out (bench/valk-gc reads its programs' own timings there).
    A row with "layouts", a list of (name, cwd), runs round r from the
    (r - 1) % len-th of them (its name is the raw file's `layout`) and the
    warm-up from the first."""
    scratch = os.path.join(d, "scratch")
    os.makedirs(scratch, exist_ok=True)
    fo, fe = os.path.join(scratch, "out"), os.path.join(scratch, "err")

    def one(row, where):
        res = run_once(spawn, row["argv"], row["cwd"], fo, fe, limit, pin if row.get("pin", True) else None, row.get("env"))
        if os.path.getsize(fo) > CAP or os.path.getsize(fe) > CAP:
            die("output over the cap: %s in %s" % (row["id"], where))
        out, err = open(fo, "rb").read(), open(fe, "rb").read()
        if res["timed_out"]:
            die("timeout after %gs: %s in %s" % (limit, row["id"], where))
        why = check(row, res, out, err)
        if why:
            bad = os.path.join(d, "not-a-result-" + row["id"].replace(":", "-") + ".out")
            with open(bad, "wb") as f:
                f.write(out + b"\n--- stderr ---\n" + err[-4000:])
            die("%s: %s in %s (kept in %s)" % (row["id"], why, where, bad))
        return res, out, err

    for row in rows:
        one(row, "the warm-up")
    note("warm-up done: %s" % loadavg())
    for r in range(1, rounds + 1):
        order = order_of(r) if order_of else rotate(rows, rounds, r)
        rd = os.path.join(d, raw, "round-%d" % r)
        os.makedirs(rd)
        for pos, row in enumerate(order, 1):
            if row.get("layouts"):
                lname, lcwd = row["layouts"][(r - 1) % len(row["layouts"])]
                row = dict(row, cwd=lcwd, fields=dict(row.get("fields", {}), layout=lname))
            base = os.path.join(rd, row.get("file", row["id"]))
            for attempt in range(RETRIES + 1):
                res, out, err = one(row, "round %d" % r)
                why = disturbance(res, threads=row.get("threads"))
                last = why is None or attempt == RETRIES
                # a disturbed run that is run again is kept beside the result as FILE.disturbed-N (N from 1),
                # never as a .run file, so the readers of the .run files see one run per row and round
                path = base + (".run" if last else ".disturbed-%d" % (attempt + 1))
                write_run(path, row, r, pos, res, out, why, attempt)
                if why:
                    note("round %d: %s disturbed (%s)%s" % (r, row["id"], why, "; kept as not undisturbed" if last else "; run again"))
                if last:
                    break
            if keep:
                with open(os.path.join(rd, row.get("file", row["id"]) + ".out"), "wb") as f:
                    f.write(out)
        note("round %d: first %s, %s; loadavg %s" % (r, order[0]["id"], "backwards" if r % 2 == 0 else "forwards",
                                                     loadavg().split()[0]))
    for p in (fo, fe):
        os.remove(p)
    os.rmdir(scratch)


# ---- LangArena --------------------------------------------------------------

def tasks_of(run_js):
    """[(name, checksum)] in RUN_JS order (the same reading as lr-table.sh's)."""
    out, name = [], None
    for line in open(run_js):
        parts = line.split('"')
        if '"name":' in line and len(parts) > 3:
            name = parts[3]
        elif '"checksum":' in line and name is not None:
            out.append((name, "".join(ch for ch in parts[2] if ch.isdigit())))
    return out


def slug_of(name):
    return name.lower().replace("::", "_")


def threads_of(bdir, slug):
    """The task's own threads from the harness's configuration beside its binary (SLUG.cfg.av's cfg_threads,
    in BIN_DIR or its parent: a layout's directory sits below it), else None."""
    for d in (bdir, os.path.dirname(os.path.abspath(bdir))):
        p = os.path.join(d, slug + ".cfg.av")
        if os.path.exists(p):
            for line in open(p):
                m = re.match(r"\s*fn cfg_threads: Int \{\s*(\d+)\s*\}", line)
                if m:
                    return int(m.group(1))
            return None
    return None


def checkout_of(path):
    p = os.path.abspath(path)
    while p != "/":
        if os.path.exists(os.path.join(p, ".git")) and os.path.basename(p) != "langarena":
            return p
        p = os.path.dirname(p)
    return None


def cmd_langarena(argv):
    rounds, run_js, out, sides, pin, short, limit, allow, only = None, None, None, [], None, 0.2, 900.0, False, []
    layouts = []
    a = list(argv)
    while a:
        x = a.pop(0)
        if x in ("--rounds", "--run-js", "--out", "--side", "--layouts", "--pin", "--short", "--timeout",
                 "--cpu-tolerance", "--max-involuntary", "--max-outside", "--retries"):
            if not a:
                die("%s needs a value" % x)
            v = a.pop(0)
            if x == "--rounds":
                rounds = int(v)
            elif x == "--run-js":
                run_js = v
            elif x == "--out":
                out = v
            elif x == "--side":
                if "=" not in v:
                    die("--side needs NAME=BIN_DIR")
                sides.append(tuple(v.split("=", 1)))
            elif x == "--layouts":
                layouts = [p.strip() for p in v.split(",") if p.strip()]
                if not layouts or any(not p.isdigit() for p in layouts):
                    die("--layouts needs shifts in bytes, comma-separated (0,64,192)")
            elif x == "--pin":
                pin = int(v)
            elif x == "--short":
                short = float(v)
            elif x in ("--cpu-tolerance", "--max-involuntary", "--max-outside", "--retries"):
                set_rule(x, v)
            else:
                limit = float(v)
        elif x == "--allow-governor":
            allow = True
        elif x.startswith("-"):
            die("unknown option %s" % x)
        else:
            only.append(x)
    if rounds is None or rounds < 1 or not run_js or not out:
        die("langarena needs --rounds N (N >= 1), --run-js RUN_JS and --out DIR")
    if not os.path.isfile(run_js):
        die("RUN_JS %s: no such file" % run_js)
    if not sides:
        sides = [("avant", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "langarena", "tmp"))]
    for name, bdir in sides:
        if not os.path.isdir(bdir):
            die("side %s: %s is not a directory" % (name, bdir))
        if not name.replace("-", "").replace("_", "").isalnum():
            die("side name %s: letters, digits, - and _ only" % name)
    tasks = tasks_of(run_js)
    if not tasks:
        die("RUN_JS %s: lists no task" % run_js)
    known = {n for n, _ in tasks}
    for t in only:
        if t not in known:
            die("task %s: not in %s" % (t, run_js))
    def bins(b, n):
        """The task's binaries of one side: one, or one per layout."""
        return [os.path.join(b, lay, slug_of(n)) for lay in layouts] if layouts else [os.path.join(b, slug_of(n))]
    sel = [(n, c) for n, c in tasks if (not only or n in only)
           and all(os.access(p, os.X_OK) for _, b in sides for p in bins(b, n))]
    if not sel:
        die("no task has a binary in every side")
    out = os.path.abspath(out)
    if os.path.exists(out):
        die("%s exists" % out)
    lines = ["mode: langarena rounds (tools/rounds.py), for milestones and publication, never per gate",
             "rounds: %d (after one unmeasured warm-up of every row)" % rounds,
             "run_js: %s" % os.path.abspath(run_js),
             "tasks: %d; sides: %s" % (len(sel), " ".join(n for n, _ in sides)),
             "layouts: %s" % ("shifts %s; round r runs every side at the ((r - 1) mod %d)-th, the warm-up at the first"
                              % (" ".join(layouts), len(layouts)) if layouts else "one (the binaries as built)"),
             "pin: %s" % ("cpu %d (Matmul::T* unpinned)" % pin if pin is not None else "none"),
             "short: under %g s median is flagged" % short,
             "rule: " + rule_text(),
             "command per run: (cd <side>; <spawner> <result> ./<task>) < /dev/null > <file> 2> <file>; "
             "wall: the spawner's CLOCK_MONOTONIC before vfork and after wait4; peak RSS, user, sys: wait4's rusage of the task"]
    for name, bdir in sides:
        lines.append("side %s: %s" % (name, os.path.abspath(bdir)))
        co = checkout_of(bdir)
        if co:
            head = subprocess.run(["git", "-C", co, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
            lines.append("side %s checkout: %s HEAD %s" % (name, co, head or "?"))
            for c in ("avant-c-llvm", "avant-c-release"):
                vf = os.path.join(co, "bin", c + ".versions.txt")
                if os.path.exists(vf):
                    lines.append("side %s %s.versions.txt: %s" % (name, c, " | ".join(l.strip() for l in open(vf) if l.strip())))
            la = os.path.join(co, "langarena")
            if os.path.isdir(os.path.join(la, ".git")):
                h = subprocess.run(["git", "-C", la, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
                lines.append("side %s langarena HEAD: %s" % (name, h or "?"))
    note, meta = session_open(out, allow, lines)
    spawn, sline = build_spawn(out)
    note(sline)
    with open(os.path.join(out, "binaries.sha256"), "w") as f:
        for name, bdir in sides:
            for n, _ in sel:
                for i, p in enumerate(bins(bdir, n)):
                    f.write("%s  %s%s--%s\n" % (hashlib.sha256(open(p, "rb").read()).hexdigest(), name,
                                                "@" + layouts[i] if layouts else "", slug_of(n)))
    with open(os.path.join(out, "params.tsv"), "w") as f:
        f.write("short\t%g\nsides\t%s\n" % (short, " ".join(n for n, _ in sides)))
        if layouts:
            f.write("layouts\t%s\n" % " ".join(layouts))
    # The spawn floor: the same loop on /bin/true, 2 * rounds runs.
    floor_rows = [{"id": "floor--true", "argv": ["/bin/true"], "cwd": out, "pin": True}]
    measure(spawn, floor_rows, 2 * rounds, os.path.join(out, "floor"), 10.0,
            lambda row, res, o, e: None if res["exit"] == 0 else "exit %d" % res["exit"], note, pin)
    rows = []
    for n, c in sel:
        for side, bdir in sides:
            row = {"id": "%s--%s" % (side, slug_of(n)), "argv": ["./" + slug_of(n)], "cwd": os.path.abspath(bdir),
                   "pin": not n.startswith("Matmul::T"), "fields": {"task": n, "side": side, "checksum": c},
                   "threads": threads_of(bdir, slug_of(n))}
            if layouts:
                row["layouts"] = [(lay, os.path.abspath(os.path.join(bdir, lay))) for lay in layouts]
                row["cwd"] = row["layouts"][0][1]
            rows.append(row)
    # The sides' order rotates by round inside the rotating task order: rows
    # of one task stay together, and the side that goes first changes.
    per_task = [rows[i:i + len(sides)] for i in range(0, len(rows), len(sides))]

    def check(row, res, o, e):
        if res["exit"] != 0:
            return "exit %d" % res["exit"]
        got = o.split(b"\n", 1)[0].decode("utf-8", "replace").strip()
        if got != row["fields"]["checksum"]:
            return "checksum %r, RUN_JS %s" % (got, row["fields"]["checksum"])
        return None

    def order_of(r):
        k = (r - 1) % len(sides)
        return [row for g in rotate(per_task, rounds, r) for row in g[k:] + g[:k]]

    measure(spawn, rows, rounds, out, limit, check, note, pin, order_of)
    session_close(note, meta)
    write_tables(out)
    print("rounds: %s" % os.path.join(out, "rounds.md"))
    return 0


# ---- tables -----------------------------------------------------------------

def read_runs(d):
    runs = {}
    rd = os.path.join(d, "raw")
    for r in sorted(os.listdir(rd)) if os.path.isdir(rd) else []:
        for f in sorted(os.listdir(os.path.join(rd, r))):
            if not f.endswith(".run"):
                continue
            rec = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(rd, r, f)) if "\t" in l)
            if rec.get("output") != "ok" or rec.get("exit") != "0":
                die("not a result: %s" % os.path.join(rd, r, f))
            runs.setdefault(rec["id"], []).append(rec)
    return runs


def disturbed_runs(d):
    """Every disturbed run under d/raw (those run again, FILE.disturbed-N, and those kept as not undisturbed), as records."""
    got = []
    rd = os.path.join(d, "raw")
    for r in sorted(os.listdir(rd)) if os.path.isdir(rd) else []:
        for f in sorted(os.listdir(os.path.join(rd, r))):
            if ".disturbed-" in f or f.endswith(".run"):
                rec = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(rd, r, f)) if "\t" in l)
                if rec.get("undisturbed") == "no":
                    rec["file"] = os.path.join(r, f)
                    got.append(rec)
    return got


def undisturbed(rs):
    """The runs that count: a table reads the median of the undisturbed runs; a run from before the counters counts."""
    return [r for r in rs if r.get("undisturbed", "yes") == "yes"]


def summary(rs):
    rs = undisturbed(rs)
    if not rs:
        return None
    w = sorted(int(r["wall_ns"]) / 1e9 for r in rs)
    m = sorted(int(r["rss_kb"]) for r in rs)
    med = statistics.median(w)
    return {"n": len(rs), "med": med, "min": w[0], "max": w[-1], "spread": (w[-1] - w[0]) / med * 100 if med else 0.0,
            "slow": sum(1 for x in w if x > med * 1.05),
            "rss": statistics.median(m), "rss_min": m[0], "rss_max": m[-1],
            "user": statistics.median(float(r["user_s"]) for r in rs), "sys": statistics.median(float(r["sys_s"]) for r in rs)}


def disturbed_section(d, few):
    """The rounds.md lines that list every disturbed run with its counters, and the rows short of undisturbed runs."""
    bad = disturbed_runs(d)
    md = ["## Disturbed runs", "",
          "A run is undisturbed when its CPU time (user + system) is under its wall by at most %g%% of the wall or %g s, "
          "it had at most %d involuntary context switches a second, and the CPUs it may run on (with their SMT siblings) spent at most %g CPUs' worth outside it; a disturbed run was run again at once (at most %d times) "
          "and is kept beside the result (`FILE.disturbed-N`), never in a median. Outside CPU: the CPU time spent outside "
          "the program over its wall, on the CPUs it may run on and on the whole machine." % (CPU_TOLERANCE * 100, CPU_SLACK_S, MAX_INVOLUNTARY_PER_S, MAX_OUTSIDE_CPUS, RETRIES), ""]
    if not bad:
        md += ["None.", ""]
    else:
        md += ["| Run | Round | Wall s | User s | Sys s | Involuntary | Voluntary | Outside CPU s (its CPUs / machine) | Kept | Reason |",
               "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |"]
        for r in bad:
            md.append("| %s | %s | %.4f | %s | %s | %s | %s | %s / %s | %s | %s |" % (
                r.get("id", "?"), r.get("round", "?"), int(r.get("wall_ns", 0)) / 1e9, r.get("user_s"), r.get("sys_s"),
                r.get("nivcsw"), r.get("nvcsw"), r.get("outside_mine_s"), r.get("outside_all_s"),
                "not counted (out of retries)" if r["file"].endswith(".run") else "run again", r.get("disturbed", "")))
        md.append("")
    if few:
        md += ["**Rows short of undisturbed runs** (their median is over fewer runs; re-run them): " + "; ".join(few) + ".", ""]
    return md


def write_tables(d):
    params = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(d, "params.tsv")))
    short = float(params["short"])
    sides = params["sides"].split()
    runs = read_runs(d)
    fl = read_runs(os.path.join(d, "floor")).get("floor--true")
    floor = summary(fl) if fl else None
    order = []
    for rs in runs.values():
        t = rs[0]["task"]
        if t not in order:
            order.append(t)
    # RUN_JS order: the first round's positions are rotated, so sort by the slug's first appearance in binaries.sha256
    pos = {}
    bs = os.path.join(d, "binaries.sha256")
    if os.path.exists(bs):
        for i, l in enumerate(open(bs)):
            pos.setdefault(l.split("--", 1)[1].strip(), i)
    order.sort(key=lambda t: pos.get(slug_of(t), 0))
    disturbed = "DISTURBED" in open(os.path.join(d, "session.txt")).read()
    tsv = ["side\ttask\tn\twall_median_s\twall_min_s\twall_max_s\tspread_pct\tslow_runs\trss_median_kb\trss_min_kb\trss_max_kb\tuser_median_s\tsys_median_s\tshort"]
    md = ["# LangArena rounds%s" % (" — DISTURBED, NOT EVIDENCE" if disturbed else ""), "",
          "Median-of-N with rotation (`tools/rounds.py`; for milestones and publication, never per gate). Session: `session.txt`. "
          "Wall: a nanosecond clock read by the spawner just before it starts the task and just after it reaps it (no wrapper's start-up inside); "
          "spread = (max − min) / median; slow runs: runs more than 5% over the median (one or two slow runs make a wide spread, not a moved median). Peak RSS: wait4's rusage of the task's own process (no wrapper's memory). "
          "Median, minimum and maximum; never a best-of.", ""]
    if params.get("layouts"):
        lays = params["layouts"].split()
        md.append("Layouts: round r ran every side's binary with its code shifted by the ((r − 1) mod %d)-th of %s bytes "
                  "(the same instructions at another address), so a median is over the layouts and the min–max spread covers "
                  "what the address moves as well as the run-to-run noise. Each raw file names its `layout`." % (len(lays), ", ".join(lays)))
        md.append("")
    if floor:
        md.append("Spawn floor (the same loop on `/bin/true`, %d runs): median %.2f ms, min %.2f, max %.2f. "
                  "A row flagged short (median under %g s) is mostly start-up and noise at this resolution: "
                  "a LangArena task cannot repeat its work inside one process." % (floor["n"], floor["med"] * 1e3, floor["min"] * 1e3, floor["max"] * 1e3, short))
        md.append("")
    head = "| Task | " + " | ".join("%s median s | min | max | spread %% | slow runs | RSS MB (min–max)" % s for s in sides)
    if len(sides) > 1:
        head += " | " + " | ".join("%s / %s time | memory" % (s, sides[0]) for s in sides[1:])
    head += " | short |"
    ncol = head.count("|") - 1
    md.append(head)
    md.append("| --- |" + " ---: |" * (ncol - 2) + " :---: |")
    lr = {s: [] for s in sides}
    few = []
    for t in order:
        cells, sums, flag = [], {}, ""
        for s in sides:
            rs = runs.get("%s--%s" % (s, slug_of(t)))
            if not rs:
                cells.append("— | — | — | — | — | —")
                continue
            x = summary(rs)
            if len(undisturbed(rs)) < len(rs):
                few.append("%s %s: %d undisturbed of %d rounds" % (s, t, len(undisturbed(rs)), len(rs)))
            if x is None:
                cells.append("— | — | — | — | — | —")
                lr[s].append("%s\tdisturbed\t-\t-\t-\t0" % t)
                continue
            sums[s] = x
            if x["med"] < short:
                flag = "yes"
            cells.append("%.4f | %.4f | %.4f | %.1f | %d | %.1f (%.1f–%.1f)" % (x["med"], x["min"], x["max"], x["spread"], x["slow"],
                                                                             x["rss"] / 1024, x["rss_min"] / 1024, x["rss_max"] / 1024))
            tsv.append("%s\t%s\t%d\t%.6f\t%.6f\t%.6f\t%.2f\t%d\t%d\t%d\t%d\t%.4f\t%.4f\t%s" % (
                s, t, x["n"], x["med"], x["min"], x["max"], x["spread"], x["slow"], x["rss"], x["rss_min"], x["rss_max"],
                x["user"], x["sys"], "yes" if x["med"] < short else ""))
            lr[s].append("%s\tok\t%.4f\t%d\t-\t0" % (t, x["med"], x["rss"]))
        line = "| %s | %s" % (t, " | ".join(cells))
        if len(sides) > 1:
            b = sums.get(sides[0])
            rat = []
            for s in sides[1:]:
                x = sums.get(s)
                rat.append("%.3f | %.3f" % (x["med"] / b["med"], x["rss"] / b["rss"]) if x and b else "— | —")
            line += " | " + " | ".join(rat)
        md.append(line + " | %s |" % flag)
    md.append("")
    md += disturbed_section(d, few)
    open(os.path.join(d, "rounds.tsv"), "w").write("\n".join(tsv) + "\n")
    open(os.path.join(d, "rounds.md"), "w").write("\n".join(md))
    for s in sides:
        with open(os.path.join(d, "%s.lr-avant.tsv" % s), "w") as f:
            f.write("# label: Avant (%s, median of rounds)\n" % s)
            f.write("# governor: %s\n" % ", ".join("%s x%d" % kv for kv in governors().items()))
            f.write("# date: %s\n" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
            f.write("# method: tools/rounds.py median of rounds; wall by monotonic_ns around the process, RSS by wait4 rusage\n")
            f.write("task\tstatus\twall_s\trss_kb\tinprog_s\texit\n")
            f.write("\n".join(lr[s]) + ("\n" if lr[s] else ""))


def cmd_judge(res_path, threads=None):
    """One spawner result file judged by the rule (THREADS: the program's own
    threads, when known): prints its counters as tab-separated fields
    (user_s sys_s nvcsw nivcsw outside_cpus) and then `yes` or `no:
    REASON`; status 0 when undisturbed, 3 when disturbed."""
    try:
        f = [int(x) for x in open(res_path).read().split()]
    except (OSError, ValueError) as e:
        die("judge %s: %s" % (res_path, e))
    if len(f) < 11:
        die("judge %s: not a result of this spawner" % res_path)
    res = parse_result(f)
    why = disturbance(res, threads=threads)
    wall = f[0] / 1e9
    print("%.4f\t%.4f\t%d\t%d\t%.3f\t%s" % (res["user_s"], res["sys_s"], res["nvcsw"], res["nivcsw"],
                                         res["outside_mine_s"] / wall if wall else 0.0, "no: " + why if why else "yes"))
    return 3 if why else 0


def main():
    rule_from_env()
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(sys.argv) >= 2 else 2
    if sys.argv[1] == "langarena":
        return cmd_langarena(sys.argv[2:])
    if sys.argv[1] == "spawner" and len(sys.argv) == 3:
        os.makedirs(sys.argv[2], exist_ok=True)
        exe, line = build_spawn(sys.argv[2])
        print(exe)
        return 0
    if sys.argv[1] == "rule" and len(sys.argv) == 2:
        print(rule_text())
        return 0
    if sys.argv[1] == "judge" and len(sys.argv) in (3, 4):
        if len(sys.argv) == 4 and not sys.argv[3].isdigit():
            die("judge RESULT [THREADS]: THREADS is a number of threads, not %r" % sys.argv[3])
        return cmd_judge(sys.argv[2], int(sys.argv[3]) if len(sys.argv) == 4 and sys.argv[3] != "0" else None)
    if sys.argv[1] == "table" and len(sys.argv) == 3:
        write_tables(sys.argv[2])
        print(os.path.join(sys.argv[2], "rounds.md"))
        return 0
    die("usage: rounds.py langarena ... | table DIR (--help)")


if __name__ == "__main__":
    sys.exit(main())
