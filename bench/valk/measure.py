#!/usr/bin/env python3
"""check, counts and time for bench/valk (called by run.sh; see its header).

Reads $VALK_BENCH_OUT/rows.tsv (written by `run.sh build`): one row per
line, id, program, row, set, binary, working directory, arguments.

Every run: stdin from /dev/null, a timeout, the output capped, and the
output compared with expected/<program>.txt (a wrong output, a non-zero
exit or a timeout stops the session: such a run is not a result).

  check        every row once, untimed. Writes check/<id>.out and
               check/summary.tsv.
  counts       every row once under `perf stat -e instructions:u`.
               Writes counts/<id>.perf and counts/instructions.tsv.
  time ROUNDS  one unmeasured warm-up of every row, then ROUNDS fresh
               processes per row. The order rotates: round r starts
               r * (rows // ROUNDS) rows further into the list, and every
               other round walks it backwards. Wall time is
               time.monotonic_ns() around the process (GNU time is the
               process started, so its start-up is inside the interval:
               the same for every row); peak RSS, user and system time
               are GNU time's. One raw file per run:
               time/<session>/round-<r>/<id>.run.
"""
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
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


def cmd_counts(rows):
    d = os.path.join(OUT, "counts")
    os.makedirs(d, exist_ok=True)
    lines = []
    for row in rows:
        perf = os.path.join(d, fname(row["id"]) + ".perf")
        _, code, out, err = run(row, ["perf", "stat", "-x,", "-e", "instructions:u", "-o", perf])
        verify(row, code, out, err, "counts")
        count = None
        with open(perf) as f:
            for line in f:
                parts = line.split(",")
                if len(parts) > 2 and parts[2].startswith("instructions"):
                    count = int(parts[0])
        if count is None:
            die("no instruction count for %s (%s)" % (row["id"], perf))
        lines.append("%s\t%d" % (row["id"], count))
        print(lines[-1])
    with open(os.path.join(d, "instructions.tsv"), "w") as f:
        f.write("id\tinstructions:u\n" + "\n".join(lines) + "\n")


def governors():
    seen = {}
    base = "/sys/devices/system/cpu"
    for name in sorted(os.listdir(base)):
        p = os.path.join(base, name, "cpufreq", "scaling_governor")
        if name.startswith("cpu") and os.path.exists(p):
            g = open(p).read().strip()
            seen[g] = seen.get(g, 0) + 1
    return seen


def cmd_time(rows, rounds):
    gov = governors()
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
    note("rounds: %d (after one unmeasured warm-up of every row)" % rounds)
    note("rows: %d (%s)" % (len(rows), " ".join(sorted({r["set"] for r in rows}))))
    note("governor: %s" % ", ".join("%s x%d" % kv for kv in gov.items()))
    note("kernel: %s" % os.uname().release)
    note("loadavg before: %s" % open("/proc/loadavg").read().strip())
    if dry or list(gov) != ["performance"]:
        note("DISTURBED, NOT EVIDENCE: a dry run (VALK_BENCH_ALLOW_GOVERNOR), the machine was not reserved")
    wrapper_file = os.path.join(d, "gnu-time.tmp")
    wrapper = ["/usr/bin/time", "-f", "%e %M %U %S", "-o", wrapper_file]
    note("command per run: (cd <dir>; /usr/bin/time -f '%e %M %U %S' -o <file> <binary> <args>) < /dev/null, wall by time.monotonic_ns() around it")

    for row in rows:  # the warm-up
        _, code, out, err = run(row, wrapper)
        verify(row, code, out, err, "the warm-up")
    note("warm-up done: %s" % open("/proc/loadavg").read().strip())

    n = len(rows)
    step = max(1, n // rounds)
    for r in range(1, rounds + 1):
        order = rows[(r - 1) * step % n:] + rows[:(r - 1) * step % n]
        if r % 2 == 0:
            order = order[::-1]
        rd = os.path.join(d, "round-%d" % r)
        os.makedirs(rd)
        for pos, row in enumerate(order, 1):
            wall, code, out, err = run(row, wrapper)
            verify(row, code, out, err, "round %d" % r)
            elapsed, rss, user, system = open(wrapper_file).read().split()[-4:]
            with open(os.path.join(rd, fname(row["id"]) + ".run"), "w") as f:
                f.write("id\t%s\nround\t%d\nposition\t%d\nwall_ns\t%d\nrss_kb\t%s\n"
                        "gnu_time_elapsed_s\t%s\nuser_s\t%s\nsys_s\t%s\nexit\t%d\noutput_sha256\t%s\noutput\tok\n"
                        % (row["id"], r, pos, wall, rss, elapsed, user, system, code, hashlib.sha256(out).hexdigest()))
        note("round %d: first %s, %s; loadavg %s" % (r, order[0]["id"], "backwards" if r % 2 == 0 else "forwards",
                                                     open("/proc/loadavg").read().split()[0]))
    os.remove(wrapper_file)
    note("loadavg after: %s" % open("/proc/loadavg").read().strip())
    note("governor after: %s" % ", ".join("%s x%d" % kv for kv in governors().items()))
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
