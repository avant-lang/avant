#!/usr/bin/env python3
"""The floor (D296), re-derived from a wave's LR table.

  python3 tools/floor.py TABLE.md [--runtime RUNTIME.tsv]
                         [--base BASE_TABLE.md] [--base-label LABEL]

TABLE.md is the table `tools/lr-table.sh table` writes (a wave's
`la/table.md`). The floor is every task of its "Combined" section whose
"vs Crystal t/m" or "vs Go t/m" ratio, in time or in memory, is above 1.00:
a task where Avant is behind Crystal or Go. One Markdown row per such task,
in the combined table's order (worst first): the ratios behind ("time" ones
first, then "memory" ones; Crystal before Go), and the task's time and
memory multipliers against the best language measured.

--base BASE_TABLE.md adds a column with the base's ratios for the same task
("not on the floor" when it was not there), and the line "N tasks. Left the
list: ... Joined it: ...". The column is headed by --base-label, else by a
label taken from the base's path: the part after `records/` or `results/`
without the trailing `la/table.md` (`.../records/2026-11-13-valk-t/final/
la/table.md` gives `2026-11-13-valk-t/final`), else `base`.

--runtime RUNTIME.tsv (the harness's `la/runtime.tsv`) adds the LangArena
totals: the task count, whether every checksum is ok, the wall sum and the
average peak RSS.

A task whose Crystal or Go column is not a number (a failed run, `—`) is not
judged; it is listed after the table as "Not judged". Exit 0, or 1 when an
input has no combined table, 2 on a usage error.

Replaces the `floor.py` that each record copied from the one before (O353):
it reproduces VALK-T's `floor-final.txt` and `floor-base.txt` and W-FIX1f2's
`floor-table.txt` byte for byte, and the table of W-FIX1f's `floor.md` (with
--base-label W-FIX1c).
"""
import csv
import os
import re
import sys


def usage(msg):
    sys.stderr.write("floor.py: %s\n" % msg)
    sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
    sys.exit(2)


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def read(path):
    """Ordered list of (task, behind, time_x, mem_x) of the combined table, and the tasks not judged."""
    rows, unjudged = [], []
    sec = None
    try:
        f = open(path, encoding="utf-8")
    except OSError as e:
        sys.stderr.write("floor.py: %s: %s\n" % (path, e.strerror))
        sys.exit(1)
    with f:
        for line in f:
            if line.startswith("## "):
                sec = line.strip()
                continue
            if not (sec and sec.startswith("## Combined")):
                continue
            if not line.startswith("| ") or line.startswith("| #") or line.startswith("| ---"):
                continue
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if len(c) < 12:
                continue
            task = c[1]
            pairs = []
            for name, cell in (("Crystal", c[10]), ("Go", c[11])):
                p = [num(x) for x in cell.split("/")]
                pairs.append((name, p))
            if any(len(p) != 2 or None in p for _, p in pairs):
                unjudged.append(task)
                continue
            behind = ["time %.2f× %s" % (p[0], n) for n, p in pairs if p[0] > 1.0]
            behind += ["memory %.2f× %s" % (p[1], n) for n, p in pairs if p[1] > 1.0]
            rows.append((task, behind, c[5], c[9]))
    if not rows and not unjudged:
        sys.stderr.write("floor.py: %s: no row in a \"## Combined\" section\n" % path)
        sys.exit(1)
    return rows, unjudged


def label_of(path):
    p = os.path.abspath(path)
    m = re.search(r"/(?:records|results)/(.+)$", p)
    if not m:
        return "base"
    rest = re.sub(r"/?(la/)?table\.md$", "", m.group(1))
    return rest or "base"


def fmt(x):
    v = num(x)
    return "%.2f" % v if v is not None else x


def main(argv):
    table = runtime = base = label = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        if a in ("--runtime", "--base", "--base-label"):
            if not args:
                usage("%s needs a value" % a)
            v = args.pop(0)
            if a == "--runtime":
                runtime = v
            elif a == "--base":
                base = v
            else:
                label = v
        elif a.startswith("-"):
            usage("unknown option %s" % a)
        elif table is None:
            table = a
        else:
            usage("one TABLE.md only")
    if table is None:
        usage("TABLE.md is required")
    if label is not None and base is None:
        usage("--base-label needs --base")
    new, unjudged = read(table)
    floor = [r for r in new if r[1]]
    out = []
    if base is None:
        out.append("| Task | Behind on | Time × best | Memory × best |")
        out.append("| --- | --- | ---: | ---: |")
        for task, behind, tx, mx in floor:
            out.append("| %s | %s | %s | %s |" % (task, "; ".join(behind), tx, mx))
        out.append("")
        out.append("%d tasks." % len(floor))
    else:
        brows, _ = read(base)
        b = {t: behind for t, behind, _, _ in brows}
        blist = [t for t, behind, _, _ in brows if behind]
        if label is None:
            label = label_of(base)
        out.append("| Task | Behind on | Time × best | Memory × best | %s |" % label)
        out.append("| --- | --- | ---: | ---: | --- |")
        for task, behind, tx, mx in floor:
            was = "; ".join(b[task]) if b.get(task) else "not on the floor"
            out.append("| %s | %s | %s | %s | %s |" % (task, "; ".join(behind), fmt(tx), fmt(mx), was))
        out.append("")
        names = [r[0] for r in floor]
        out.append("%d tasks. Left the list: %s. Joined it: %s." % (
            len(floor), ", ".join(t for t in blist if t not in names) or "none",
            ", ".join(t for t in names if t not in blist) or "none"))
    if unjudged:
        out.append("Not judged (a Crystal or Go column is not a number): %s." % ", ".join(unjudged))
    if runtime is not None:
        w = m = 0.0
        n = 0
        ok = True
        with open(runtime, encoding="utf-8") as f:
            for r in csv.DictReader(f, delimiter="\t"):
                w += float(r["wall_s"])
                m += float(r["rss_mb"])
                n += 1
                ok = ok and r["checksum_ok"] == "true"
        if n == 0:
            sys.stderr.write("floor.py: %s: no task row\n" % runtime)
            return 1
        out.append("LangArena: %d tasks, checksums ok: %s, wall sum %.2f s, average peak RSS %.2f MB" % (n, ok, w, m / n))
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
