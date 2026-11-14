#!/usr/bin/env python3
"""check, counts and time for bench/valk (called by run.sh; see its header).

Reads $VALK_BENCH_OUT/rows.tsv (written by `run.sh build`): one row per
line, id, program, row, set, binary, working directory, arguments.

Every run: stdin from /dev/null, a timeout, the output capped, and the
output compared with expected/<program>.txt (a wrong output, a non-zero
exit or a timeout stops the session: such a run is not a result).

  check        every row once, untimed. Writes check/<id>.out and
               check/summary.tsv.
  counts       every row VALK_BENCH_COUNT_RUNS times (default 3) under
               `perf stat -e instructions:u` (every thread of the process),
               and once more with `--no-inherit` (the main thread only,
               with its task-clock). Writes counts/<id>.<n>.perf,
               counts/<id>.main.perf and counts/instructions.tsv: the
               median count, its minimum and maximum (a row whose work
               varies by run, such as Rust's lru, shows its range), the
               main thread's count, its share of the process's CPU time in
               that run, and "threaded" when the share is under 95% (a
               collector or worker threads, as in Go's allocating rows:
               such a row's count includes work its wall time overlaps).
  time ROUNDS  the loop of tools/rounds.py (the one copy, O221): one
               unmeasured warm-up of every row, then ROUNDS fresh processes
               per row. The order rotates: round r starts (r - 1) *
               (rows // ROUNDS) rows further into the list, and every other
               round walks it backwards from there. Each run is started by
               the spawner of tools/rounds.py (a small C program compiled
               into the session's directory): wall time is its monotonic
               clock just before vfork and just after wait4, so no wrapper's
               start-up is inside (GNU time's was, O387); peak RSS, user
               and system time are wait4's rusage of the program's own
               process. One raw file per run:
               time/<session>/round-<r>/<id>.run.
"""
import hashlib
import os
import resource
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ in tools/
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import rounds  # noqa: E402  the loop and the spawner (tools/rounds.py)
OUT = os.environ["VALK_BENCH_OUT"]
LIMIT = float(os.environ.get("VALK_BENCH_TIMEOUT", "120"))
CAP = 50_000_000
ALL_SETS = ["shipped", "switch", "levelled", "v3", "lto"]


def die(msg):
    sys.stderr.write("measure.py: %s\n" % msg)
    sys.exit(1)


def load_rows():
    sets = os.environ.get("VALK_BENCH_SETS", "").split() or ALL_SETS
    only = os.environ.get("VALK_BENCH_ROWS", "").split()
    rows = []
    with open(os.path.join(OUT, "rows.tsv")) as f:
        for line in f:
            rid, program, row, rset, binary, cwd, args = line.rstrip("\n").split("\t")
            if rset not in sets or (only and rid not in only):
                continue
            rows.append({"id": rid, "program": program, "row": row, "set": rset,
                         "binary": binary, "cwd": cwd, "args": args.split()})
    if not rows:
        die("no rows selected")
    return rows


def expected(program):
    with open(os.path.join(HERE, "expected", program + ".txt"), "rb") as f:
        return f.read()


def fname(rid):
    return rid.replace(":", "--")


def run(row, wrapper):
    """Runs one row under `wrapper` (a command prefix). Returns (wall_ns, exit, stdout)."""
    cmd = wrapper + [row["binary"]] + row["args"]
    t0 = time.monotonic_ns()
    proc = subprocess.Popen(cmd, cwd=row["cwd"], stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = proc.communicate(timeout=LIMIT)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        die("timeout after %gs: %s" % (LIMIT, row["id"]))
    t1 = time.monotonic_ns()
    if len(out) > CAP or len(err) > CAP:
        die("output over the cap: %s" % row["id"])
    return t1 - t0, proc.returncode, out, err


def verify(row, code, out, err, where):
    if code != 0:
        die("%s: exit %d in %s; stderr: %s" % (row["id"], code, where, err[-400:].decode("utf-8", "replace")))
    if out != expected(row["program"]):
        bad = os.path.join(OUT, "wrong-output-" + fname(row["id"]) + ".out")
        with open(bad, "wb") as f:
            f.write(out)
        die("%s: wrong output in %s (kept in %s)" % (row["id"], where, bad))


def cmd_check(rows):
    d = os.path.join(OUT, "check")
    os.makedirs(d, exist_ok=True)
    lines = []
    failed = 0
    for row in rows:
        _, code, out, err = run(row, [])
        with open(os.path.join(d, fname(row["id"]) + ".out"), "wb") as f:
            f.write(out)
        try:
            ok = code == 0 and out == expected(row["program"])
        except FileNotFoundError:
            ok = False
        failed += 0 if ok else 1
        lines.append("%s\t%d\t%s\t%s" % (row["id"], code, hashlib.sha256(out).hexdigest()[:16], "ok" if ok else "DIFFERENT"))
        print(lines[-1])
    with open(os.path.join(d, "summary.tsv"), "w") as f:
        f.write("id\texit\toutput sha256 (16)\tagainst expected/<program>.txt\n" + "\n".join(lines) + "\n")
    print("check: %d rows, %d not ok" % (len(rows), failed))
    sys.exit(1 if failed else 0)


def perf_count(path):
    with open(path) as f:
        for line in f:
            parts = line.split(",")
            if len(parts) > 2 and parts[2].startswith("instructions"):
                return int(parts[0])
    return None


def cmd_counts(rows):
    d = os.path.join(OUT, "counts")
    os.makedirs(d, exist_ok=True)
    n = int(os.environ.get("VALK_BENCH_COUNT_RUNS", "3"))
    if n < 1:
        die("VALK_BENCH_COUNT_RUNS must be at least 1")
    lines = []
    for row in rows:
        counts = []
        for i in range(1, n + 1):
            perf = os.path.join(d, "%s.%d.perf" % (fname(row["id"]), i))
            _, code, out, err = run(row, ["perf", "stat", "-x,", "-e", "instructions:u", "-o", perf])
            verify(row, code, out, err, "counts")
            c = perf_count(perf)
            if c is None:
                die("no instruction count for %s (%s)" % (row["id"], perf))
            counts.append(c)
        # The main thread alone, and its share of the process's CPU time in
        # the same run: perf's task-clock of the main thread against the
        # rusage of perf and the program together (perf's own share is about
        # 1%). Comparing counts of different runs cannot tell threads from a
        # program whose work varies by run (Rust's lru).
        perf = os.path.join(d, fname(row["id"]) + ".main.perf")
        r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        _, code, out, err = run(row, ["perf", "stat", "--no-inherit", "-x,", "-e", "instructions:u,task-clock", "-o", perf])
        r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        verify(row, code, out, err, "counts (main thread)")
        main_c = perf_count(perf)
        main_ms = None
        with open(perf) as f:
            for line in f:
                parts = line.split(",")
                if len(parts) > 2 and parts[2].startswith("task-clock"):
                    main_ms = float(parts[0])
        if main_c is None or main_ms is None:
            die("no main-thread count for %s (%s)" % (row["id"], perf))
        cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
        share = main_ms / 1000 / cpu if cpu > 0 else 1.0
        med = int(statistics.median(counts))
        threaded = "threaded" if share < 0.95 else ""
        lines.append("%s\t%d\t%d\t%d\t%d\t%d\t%.3f\t%s" % (row["id"], med, min(counts), max(counts), n, main_c, share, threaded))
        print(lines[-1])
    with open(os.path.join(d, "instructions.tsv"), "w") as f:
        f.write("id\tinstructions:u (median, every thread)\tmin\tmax\truns\tinstructions:u (main thread, --no-inherit)"
                "\tmain thread's share of the CPU time\tthreaded\n" + "\n".join(lines) + "\n")


def cmd_time(rows, nrounds):
    gov = rounds.governors()
    dry = bool(os.environ.get("VALK_BENCH_ALLOW_GOVERNOR"))
    if list(gov) != ["performance"] and not dry:
        die("governor is %s, not performance: no timed work (VALK_BENCH_ALLOW_GOVERNOR=1 for a dry run)" % gov)
    session = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    d = os.path.join(OUT, "time", session)
    os.makedirs(d)
    meta = open(os.path.join(d, "session.txt"), "w")

    def note(s):
        meta.write(s + "\n")
        meta.flush()
        print(s)

    note("session: %s" % session)
    note("rounds: %d (after one unmeasured warm-up of every row)" % nrounds)
    note("rows: %d (%s)" % (len(rows), " ".join(sorted({r["set"] for r in rows}))))
    note("governor: %s" % ", ".join("%s x%d" % kv for kv in gov.items()))
    note("kernel: %s" % os.uname().release)
    note("loadavg before: %s" % open("/proc/loadavg").read().strip())
    if dry or list(gov) != ["performance"]:
        note("DISTURBED, NOT EVIDENCE: a dry run (VALK_BENCH_ALLOW_GOVERNOR), the machine was not reserved")
    spawn, sline = rounds.build_spawn(d)
    note(sline)
    note("command per run: (cd <dir>; <spawner> <result> <binary> <args>) < /dev/null; wall by the spawner's monotonic clock "
         "around vfork and wait4; peak RSS, user and system time from wait4's rusage of the program (tools/rounds.py)")

    def check(row, res, out, err):
        if res["exit"] != 0:
            return "exit %d; stderr: %s" % (res["exit"], err[-400:].decode("utf-8", "replace"))
        if out != expected(row["program"]):
            return "wrong output"
        return None

    rrows = [{"id": r["id"], "file": fname(r["id"]), "program": r["program"], "argv": [r["binary"]] + r["args"], "cwd": r["cwd"]} for r in rows]
    rounds.measure(spawn, rrows, nrounds, d, LIMIT, check, note, raw="")
    note("loadavg after: %s" % open("/proc/loadavg").read().strip())
    note("governor after: %s" % ", ".join("%s x%d" % kv for kv in rounds.governors().items()))
    meta.close()
    with open(os.path.join(OUT, "time", "latest"), "w") as f:
        f.write(session + "\n")


def main():
    if len(sys.argv) < 2:
        die("usage: measure.py check | counts | time ROUNDS")
    rows = load_rows()
    if sys.argv[1] == "check":
        cmd_check(rows)
    elif sys.argv[1] == "counts":
        cmd_counts(rows)
    elif sys.argv[1] == "time":
        cmd_time(rows, int(sys.argv[2]) if len(sys.argv) > 2 else 7)
    else:
        die("unknown command %s" % sys.argv[1])


if __name__ == "__main__":
    main()
