#!/usr/bin/env python3
"""Median-of-N rounds with rotation: the one timing loop (O221, O485, O372).

FOR MILESTONES AND PUBLICATION, NEVER PER GATE: a gate's LangArena evidence
stays the single run of `measure.cr -- prod` and `tools/lr-table.sh avant`
(operating.md, "Measuring"); this mode costs N + 1 runs of every task.

  python3 tools/rounds.py langarena --rounds N --run-js RUN_JS --out DIR
          [--side NAME=BIN_DIR]... [--pin CPU] [--short SECONDS]
          [--timeout SECONDS] [--allow-governor] [TASK...]
  python3 tools/rounds.py table DIR

`tools/lr-table.sh rounds ...` calls the first form. BIN_DIR holds the
harness-built Avant binaries (langarena/tmp after `measure.cr -- prod`); with
no --side, the checkout's own langarena/tmp is the one side, named `avant`.
Several sides (a base and a new tree) are measured in the same rounds, their
order rotated by round. TASK names restrict the set (default: every task of
RUN_JS with a binary in every side).

The loop, lifted from bench/valk/measure.py's `time` (W-BENCH-SUITE) with
its kept-but-bad settled (O387); bench/valk/measure.py now calls it:

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
  start-up is in the wall time (GNU time's was, O387), and peak RSS, user
  and system time are wait4's rusage of that one process, so the RSS is
  the program's own peak, never a wrapper's (the per-task rounds scripts'
  /usr/bin/time around perf read perf's 17 MB for every small task, and a
  child of Python would read Python's 12 to 20 MB; O372);
- stdin from /dev/null, the output to a file, capped, under a timeout;
  every run's output is checked (the first line against RUN_JS's checksum):
  a wrong output, a non-zero exit or a timeout stops the session, it is not
  a result;
- one raw file per run: DIR/raw/round-R/SIDE--SLUG.run (and the spawn
  floor's in DIR/floor/raw);
- --pin CPU runs every single-threaded task on that CPU (the parent's
  affinity, which the child inherits; Matmul::T* are never pinned).

Very short tasks (O372): a LangArena task cannot repeat its work inside one
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
# and one started under perf or a harness reads theirs (O372). The spawner's
# own memory is under any program's (it reads /bin/true's 1.4 MB as 1.4 MB),
# and it reads the clock itself just before vfork and just after wait4, so
# its own start-up is outside the interval, which GNU time's was not (O387).
SPAWN_C = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
/* spawn RESULT PROGRAM ARGS...: RESULT gets
   "wall_ns maxrss_kb utime_us stime_us wait_status" (tools/rounds.py) */
int main(int argc, char **argv) {
  struct timespec a, b; struct rusage ru; int st; pid_t p; FILE *f;
  if (argc < 3) return 125;
  f = fopen(argv[1], "w");
  if (!f) return 125;
  clock_gettime(CLOCK_MONOTONIC, &a);
  p = vfork();
  if (p == 0) { execv(argv[2], argv + 2); _exit(127); }
  if (p < 0 || wait4(p, &st, 0, &ru) < 0) return 125;
  clock_gettime(CLOCK_MONOTONIC, &b);
  fprintf(f, "%lld %ld %lld %lld %d\n",
          (long long)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec), ru.ru_maxrss,
          (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec,
          (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec, st);
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


def run_once(spawn, argv, cwd, out_path, err_path, limit, pin=None):
    """Runs argv (argv[0] a path, relative to cwd or absolute) under the
    spawner. Returns a dict: wall_ns, exit (128 + n for a signal), rss_kb,
    user_s, sys_s, timed_out."""
    res_path = out_path + ".res"
    old = None
    if pin is not None:
        old = os.sched_getaffinity(0)
        os.sched_setaffinity(0, {pin})
    try:
        with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
            proc = subprocess.Popen([spawn, res_path] + argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=fo, stderr=fe,
                                    start_new_session=True)
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
    wall, rss, ut, st, status = (int(x) for x in open(res_path).read().split())
    os.remove(res_path)
    code = os.waitstatus_to_exitcode(status)
    return {"wall_ns": wall, "exit": code if code >= 0 else 128 - code, "rss_kb": rss,
            "user_s": ut / 1e6, "sys_s": st / 1e6, "timed_out": False}


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


def measure(spawn, rows, rounds, d, limit, check, note, pin=None, order_of=None, raw="raw"):
    """The loop. rows: dicts with id, argv, cwd, pin (bool: may be pinned),
    fields (written into the raw file). check(row, result, out, err) returns
    None when the run is a result, else the reason (the session stops).
    order_of(r) gives round r's order (default: rotate). One unmeasured
    warm-up of every row first; one raw file per run,
    d/RAW/round-R/FILE.run (FILE: the row's "file", else its id)."""
    scratch = os.path.join(d, "scratch")
    os.makedirs(scratch, exist_ok=True)
    fo, fe = os.path.join(scratch, "out"), os.path.join(scratch, "err")

    def one(row, where):
        res = run_once(spawn, row["argv"], row["cwd"], fo, fe, limit, pin if row.get("pin", True) else None)
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
            res, out, err = one(row, "round %d" % r)
            with open(os.path.join(rd, row.get("file", row["id"]) + ".run"), "w") as f:
                f.write("id\t%s\nround\t%d\nposition\t%d\nwall_ns\t%d\nrss_kb\t%d\nuser_s\t%.6f\nsys_s\t%.6f\n"
                        "exit\t%d\noutput_sha256\t%s\noutput\tok\n"
                        % (row["id"], r, pos, res["wall_ns"], res["rss_kb"], res["user_s"], res["sys_s"],
                           res["exit"], hashlib.sha256(out).hexdigest()))
                for k, v in row.get("fields", {}).items():
                    f.write("%s\t%s\n" % (k, v))
                f.write("first_line\t%s\n" % out.split(b"\n", 1)[0].decode("utf-8", "replace").strip())
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


def checkout_of(path):
    p = os.path.abspath(path)
    while p != "/":
        if os.path.exists(os.path.join(p, ".git")) and os.path.basename(p) != "langarena":
            return p
        p = os.path.dirname(p)
    return None


def cmd_langarena(argv):
    rounds, run_js, out, sides, pin, short, limit, allow, only = None, None, None, [], None, 0.2, 900.0, False, []
    a = list(argv)
    while a:
        x = a.pop(0)
        if x in ("--rounds", "--run-js", "--out", "--side", "--pin", "--short", "--timeout"):
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
            elif x == "--pin":
                pin = int(v)
            elif x == "--short":
                short = float(v)
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
    sel = [(n, c) for n, c in tasks if (not only or n in only)
           and all(os.access(os.path.join(b, slug_of(n)), os.X_OK) for _, b in sides)]
    if not sel:
        die("no task has a binary in every side")
    out = os.path.abspath(out)
    if os.path.exists(out):
        die("%s exists" % out)
    lines = ["mode: langarena rounds (tools/rounds.py), for milestones and publication, never per gate",
             "rounds: %d (after one unmeasured warm-up of every row)" % rounds,
             "run_js: %s" % os.path.abspath(run_js),
             "tasks: %d; sides: %s" % (len(sel), " ".join(n for n, _ in sides)),
             "pin: %s" % ("cpu %d (Matmul::T* unpinned)" % pin if pin is not None else "none"),
             "short: under %g s median is flagged" % short,
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
                p = os.path.join(bdir, slug_of(n))
                f.write("%s  %s--%s\n" % (hashlib.sha256(open(p, "rb").read()).hexdigest(), name, slug_of(n)))
    with open(os.path.join(out, "params.tsv"), "w") as f:
        f.write("short\t%g\nsides\t%s\n" % (short, " ".join(n for n, _ in sides)))
    # The spawn floor: the same loop on /bin/true, 2 * rounds runs.
    floor_rows = [{"id": "floor--true", "argv": ["/bin/true"], "cwd": out, "pin": True}]
    measure(spawn, floor_rows, 2 * rounds, os.path.join(out, "floor"), 10.0,
            lambda row, res, o, e: None if res["exit"] == 0 else "exit %d" % res["exit"], note, pin)
    rows = []
    for n, c in sel:
        for side, bdir in sides:
            rows.append({"id": "%s--%s" % (side, slug_of(n)), "argv": ["./" + slug_of(n)], "cwd": os.path.abspath(bdir),
                         "pin": not n.startswith("Matmul::T"), "fields": {"task": n, "side": side, "checksum": c}})
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


def summary(rs):
    w = sorted(int(r["wall_ns"]) / 1e9 for r in rs)
    m = sorted(int(r["rss_kb"]) for r in rs)
    med = statistics.median(w)
    return {"n": len(rs), "med": med, "min": w[0], "max": w[-1], "spread": (w[-1] - w[0]) / med * 100 if med else 0.0,
            "slow": sum(1 for x in w if x > med * 1.05),
            "rss": statistics.median(m), "rss_min": m[0], "rss_max": m[-1],
            "user": statistics.median(float(r["user_s"]) for r in rs), "sys": statistics.median(float(r["sys_s"]) for r in rs)}


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
    for t in order:
        cells, sums, flag = [], {}, ""
        for s in sides:
            rs = runs.get("%s--%s" % (s, slug_of(t)))
            if not rs:
                cells.append("— | — | — | — | — | —")
                continue
            x = summary(rs)
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


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(sys.argv) >= 2 else 2
    if sys.argv[1] == "langarena":
        return cmd_langarena(sys.argv[2:])
    if sys.argv[1] == "table" and len(sys.argv) == 3:
        write_tables(sys.argv[2])
        print(os.path.join(sys.argv[2], "rounds.md"))
        return 0
    die("usage: rounds.py langarena ... | table DIR (--help)")


if __name__ == "__main__":
    sys.exit(main())
