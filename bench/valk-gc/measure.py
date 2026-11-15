#!/usr/bin/env python3
"""rounds for bench/valk-gc (called by `run.sh rounds`; see its header).

  measure.py rounds ROUNDS ROWS_TSV HEAD_TXT OUT_DIR

The loop of tools/rounds.py (the one copy): one unmeasured
warm-up of every row, then ROUNDS fresh processes per row, the units'
order rotating as rounds.py rotates it and the sides' order rotating
inside each unit. Each run is started by rounds.py's spawner (a small C
program compiled into the session's directory): the wall time is its
monotonic clock just before vfork and just after wait4, so no wrapper's
start-up is inside it (GNU time's was), and peak RSS, user and
system time are wait4's rusage of the program's own process.

ROWS_TSV (written by run.sh): one row a line, tab-separated: side, set,
row, unit (`all`, an iso entry with `+` for its blank, or `-` for a
tree), what to verify (`scenarios N...` or `checksum=C`), the environment
added to the run (space-separated NAME=VALUE, or `-`), then the command
and its arguments. HEAD_TXT: run.sh's lines for session.txt (the
toolchains, the rows' commands, each side's build directory and hashes).

Every run: stdin from /dev/null, a timeout, the output capped, the exit
status 0 and the output verified (every scenario asked for present once
with its verification value, `=== done ===`; or a tree's checksum and
nothing else): otherwise the session stops and the run is not a result.
One raw file per run, OUT_DIR/raw/round-R/ID.run, and the program's output
beside it, ID.out (tables.py reads `time_us` there).
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # no __pycache__ in tools/
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import rounds  # noqa: E402  the loop and the spawner (tools/rounds.py)

LIMIT = float(os.environ.get("VALK_GC_TIMEOUT", "180"))
NAMES = {1: "short-lived alloc", 2: "stable-heap forced collects", 3: "build long-lived chain",
         4: "free long-lived chain", 5: "mutate live links", 6: "short-lived churn with large live set",
         7: "tree churn", 8: "reverse live links"}
EXPECT = {1: "2", 2: "OK", 3: "OK", 4: "5000000", 5: "OK", 6: "OK", 7: "13106800", 8: "OK"}
NUMBER = {v: k for k, v in NAMES.items()}


def die(msg):
    sys.stderr.write("measure.py: %s\n" % msg)
    sys.exit(1)


def verify_scenarios(out, want):
    """run.sh's verify_output, the same rules: None when right, else why."""
    seen, timed, verified, cur, done = {}, {}, {}, None, False
    for line in out.decode("utf-8", "replace").splitlines():
        m = re.match(r"^=== (.*) ===$", line)
        if m:
            name = m.group(1)
            if name == "done":
                done, cur = True, None
            elif name not in NUMBER:
                return "unknown scenario: %s" % name
            else:
                cur = NUMBER[name]
                seen[cur] = seen.get(cur, 0) + 1
            continue
        if line.startswith("time_us: ") and cur is not None:
            timed[cur] = timed.get(cur, 0) + 1
        if line.startswith("verify: "):
            if cur is None:
                return "verify outside a scenario"
            got = line.split()[1] if len(line.split()) > 1 else ""
            if got != EXPECT[cur]:
                return "scenario %d (%s): verify %s, expected %s" % (cur, NAMES[cur], got, EXPECT[cur])
            verified[cur] = verified.get(cur, 0) + 1
    for k in want:
        if seen.get(k) != 1 or verified.get(k) != 1 or timed.get(k) != 1:
            return "scenario %d: missing or repeated" % k
    for k in seen:
        if k not in want:
            return "scenario %d: not asked for" % k
    if not done:
        return "no === done === line"
    return None


def load_rows(path):
    rows = []
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) < 7:
            die("%s: a row needs seven fields: %r" % (path, line))
        side, set_, row, unit, want, env, argv = f[0], f[1], f[2], f[3], f[4], f[5], f[6:]
        exe = argv[0] if os.sep in argv[0] else shutil.which(argv[0])
        if not exe or not os.access(exe, os.X_OK):
            die("row %s %s %s: %s is not an executable (run.sh build)" % (side, set_, row, argv[0]))
        e = dict(os.environ)
        if env != "-":
            for kv in env.split():
                k, v = kv.split("=", 1)
                e[k] = v
        rid = "%s--%s--%s--%s" % (side, set_, row, unit)
        rows.append({"id": rid, "argv": [os.path.abspath(exe)] + argv[1:], "cwd": HERE, "env": e,
                     "fields": {"side": side, "set": set_, "row": row, "unit": unit, "want": want, "env": env},
                     "unit_key": (set_, row, unit)})
    return rows


def cmd_rounds(nrounds, rows_tsv, head_txt, out):
    rows = load_rows(rows_tsv)
    if not rows:
        die("no row to measure")
    allow = os.environ.get("VALK_GC_ALLOW_GOVERNOR") == "1"
    lines = ["mode: run.sh rounds (bench/valk-gc/measure.py on tools/rounds.py's loop)",
             "rounds: %d (after one unmeasured warm-up of every row), %d processes a round" % (nrounds, len(rows)),
             "command per run: (cd bench/valk-gc; <spawner> <result> <program> <args>) < /dev/null > <file> 2> <file>, "
             "the row's variables added to the environment; wall: the spawner's CLOCK_MONOTONIC before vfork and after "
             "wait4; peak RSS, user, sys: wait4's rusage of the program"]
    lines += [l.rstrip("\n") for l in open(head_txt)]
    note, meta = rounds.session_open(out, allow, lines)
    spawn, sline = rounds.build_spawn(out)
    note(sline)

    def check(row, res, o, e):
        if res["exit"] != 0:
            return "exit %d; stderr: %s" % (res["exit"], e[-400:].decode("utf-8", "replace"))
        want = row["fields"]["want"]
        if want.startswith("checksum="):
            return None if o.decode("utf-8", "replace").strip() == want[9:] and o.count(b"\n") <= 1 else \
                "output %r, expected the checksum %s" % (o[:200], want[9:])
        return verify_scenarios(o, [int(x) for x in want.split()[1:]])

    # Rows of one unit stay together; the side that goes first rotates.
    groups, index = [], {}
    for r in rows:
        k = r["unit_key"]
        if k not in index:
            index[k] = len(groups)
            groups.append([])
        groups[index[k]].append(r)

    def order_of(r):
        out_rows = []
        for g in rounds.rotate(groups, nrounds, r):
            k = (r - 1) % len(g)
            out_rows += g[k:] + g[:k]
        return out_rows

    for r in rows:
        del r["unit_key"]
    rounds.measure(spawn, rows, nrounds, out, LIMIT, check, note, None, order_of, keep=True)
    rounds.session_close(note, meta)
    print("raw files in %s; tables: python3 %s %s" % (out, os.path.join(HERE, "tables.py"), os.path.dirname(out)))


def main():
    if len(sys.argv) == 6 and sys.argv[1] == "rounds":
        cmd_rounds(int(sys.argv[2]), sys.argv[3], sys.argv[4], os.path.abspath(sys.argv[5]))
        return
    die("usage: measure.py rounds ROUNDS ROWS_TSV HEAD_TXT OUT_DIR")


if __name__ == "__main__":
    main()
