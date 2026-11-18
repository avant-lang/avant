#!/usr/bin/env python3
"""How much each LangArena task's time depends on where its code sits, from
timed rounds of the same program at several code shifts.

  python3 tools/layout-spread.py spread [--bar PCT] [--out DIR] [--sides S,...] SESSION...
  python3 tools/layout-spread.py compare [--bar PCT] BASE.tsv NEW.tsv [OUT.md]

SESSION is a tools/rounds.py LangArena session (its raw/round-R/SIDE--SLUG.run
files) whose sides are layouts of one program, as tools/placement-probe.sh
times them: side s<SHIFT> is the program with its code SHIFT bytes later,
side s0c<N> a copy of s0 (the same bytes in another file). Several sessions
(one per group of tasks) read as one; a task in two sessions takes the
runs of both. Only undisturbed runs count (rounds.py's rule). --sides
keeps only those sides (to compare with a session that timed fewer).

spread, per task:
  median      the median over the layouts of each layout's median: the
              task's time with its code at no particular address
  min, max    the fastest and the slowest layout's median
  spread %    (max - min) / median: what the address alone can move
  noise %     what one layout moves from run to run: the larger of the
              median over the layouts of (max - min) / median of their runs
              and, with copies, the range of s0's and its copies' medians
              over the median
  verdict     `layout` when the spread is over the bar (default 2%) and
              over the noise; `noisy` when the noise alone is over the bar;
              else `ok`
writes DIR/spread.tsv and DIR/spread.md (worst spread first) and prints
the summary line. Exit 1 when a task is `layout`, else 0; 2 on a usage
error or an unreadable session.

compare reads two spread.tsv files (a base and a change measured the same
way) and gives per task the ratio of the medians and both spreads, and
over the tasks both have: the geometric mean of the median ratios, the
count of `layout` tasks on each side and the mean spread on each side.
"""
import math
import os
import statistics
import sys


def die(msg):
    sys.stderr.write("layout-spread.py: %s\n" % msg)
    sys.exit(2)


def read_session(d, runs, keep=None):
    rd = os.path.join(d, "raw")
    if not os.path.isdir(rd):
        die("%s: no raw/ directory (not a rounds.py session)" % d)
    for r in sorted(os.listdir(rd)):
        for f in sorted(os.listdir(os.path.join(rd, r))):
            if not f.endswith(".run"):
                continue
            rec = dict(l.rstrip("\n").split("\t", 1) for l in open(os.path.join(rd, r, f)) if "\t" in l)
            if rec.get("output") != "ok" or rec.get("exit") != "0":
                die("not a result: %s" % os.path.join(rd, r, f))
            if rec.get("undisturbed", "yes") != "yes" or (keep and rec["side"] not in keep):
                continue
            runs.setdefault(rec["task"], {}).setdefault(rec["side"], []).append(int(rec["wall_ns"]) / 1e9)


def is_copy(side):
    return "c" in side[1:]


def shift_of(side):
    return int(side[1:].split("c")[0])


def spread_rows(runs, bar):
    rows = []
    for task, sides in runs.items():
        meds = {s: statistics.median(v) for s, v in sides.items() if v}
        lay = {s: m for s, m in meds.items() if not is_copy(s)}
        if len(lay) < 2:
            continue
        med = statistics.median(lay.values())
        lo_s = min(lay, key=lay.get)
        hi_s = max(lay, key=lay.get)
        spread = (lay[hi_s] - lay[lo_s]) / med * 100
        within = statistics.median((max(v) - min(v)) / statistics.median(v) * 100 for s, v in sides.items() if len(v) > 1) \
            if any(len(v) > 1 for v in sides.values()) else 0.0
        cp = [m for s, m in meds.items() if s == "s0" or (is_copy(s) and shift_of(s) == 0)]
        copies = (max(cp) - min(cp)) / med * 100 if len(cp) > 1 else 0.0
        noise = max(within, copies)
        verdict = "layout" if spread > bar and spread > noise else ("noisy" if noise > bar else "ok")
        nruns = sum(len(v) for v in sides.values())
        rows.append({"task": task, "median": med, "min": lay[lo_s], "max": lay[hi_s], "spread": spread,
                     "within": within, "copies": copies, "noise": noise, "verdict": verdict,
                     "fastest": lo_s, "slowest": hi_s, "layouts": len(lay), "runs": nruns,
                     "per_layout": " ".join("%s=%.4f" % (s, lay[s]) for s in sorted(lay, key=shift_of))})
    rows.sort(key=lambda r: -r["spread"])
    return rows


COLS = ["task", "median_s", "min_s", "max_s", "spread_pct", "noise_pct", "within_pct", "copies_pct", "verdict",
        "fastest", "slowest", "layouts", "runs", "per_layout"]


def cmd_spread(argv):
    bar, out, sessions, keep = 2.0, None, [], None
    while argv:
        x = argv.pop(0)
        if x == "--bar":
            bar = float(argv.pop(0))
        elif x == "--out":
            out = argv.pop(0)
        elif x == "--sides":
            keep = set(argv.pop(0).split(","))
        elif x.startswith("-"):
            die("unknown option %s" % x)
        else:
            sessions.append(x)
    if not sessions:
        die("spread needs at least one SESSION")
    runs = {}
    for d in sessions:
        read_session(d, runs, keep)
    rows = spread_rows(runs, bar)
    if not rows:
        die("no task with two layouts in %s" % " ".join(sessions))
    n_lay = sum(1 for r in rows if r["verdict"] == "layout")
    n_noisy = sum(1 for r in rows if r["verdict"] == "noisy")
    gm = math.exp(sum(math.log(r["median"]) for r in rows) / len(rows))
    summary = ("layout spread: %d tasks; %d over the bar (%.1f%%) and their noise, %d noisy at one layout; "
               "mean spread %.2f%%, mean noise %.2f%%; sum of medians %.3f s, geometric mean %.4f s"
               % (len(rows), n_lay, bar, n_noisy, statistics.mean(r["spread"] for r in rows),
                  statistics.mean(r["noise"] for r in rows), sum(r["median"] for r in rows), gm))
    if out:
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "spread.tsv"), "w") as f:
            f.write("# " + summary + "\n# sessions: " + " ".join(os.path.abspath(s) for s in sessions) + "\n")
            f.write("\t".join(COLS) + "\n")
            for r in rows:
                f.write("%s\t%.6f\t%.6f\t%.6f\t%.2f\t%.2f\t%.2f\t%.2f\t%s\t%s\t%s\t%d\t%d\t%s\n" % (
                    r["task"], r["median"], r["min"], r["max"], r["spread"], r["noise"], r["within"], r["copies"],
                    r["verdict"], r["fastest"], r["slowest"], r["layouts"], r["runs"], r["per_layout"]))
        md = ["# Layout spread", "", summary + ".", "",
              "Median: the median over the layouts of each layout's median (undisturbed runs only). Spread: (slowest layout's median − fastest's) / median. "
              "Noise: the larger of the median run-to-run range at one layout and the range of shift 0's copies. "
              "`layout`: spread over %.1f%% and over the noise; `noisy`: the noise alone over %.1f%%." % (bar, bar), "",
              "| Task | Median s | Fastest s | Slowest s | Spread % | Noise % | Verdict | Fastest at | Slowest at |",
              "| --- | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |"]
        for r in rows:
            md.append("| %s | %.4f | %.4f | %.4f | %.1f | %.1f | %s | %s | %s |" % (
                r["task"], r["median"], r["min"], r["max"], r["spread"], r["noise"], r["verdict"], r["fastest"], r["slowest"]))
        open(os.path.join(out, "spread.md"), "w").write("\n".join(md) + "\n")
    print(summary)
    return 1 if n_lay else 0


def read_tsv(p):
    rows = {}
    try:
        for line in open(p):
            if line.startswith("#") or line.startswith("task\t"):
                continue
            f = line.rstrip("\n").split("\t")
            rows[f[0]] = {"median": float(f[1]), "spread": float(f[4]), "noise": float(f[5]), "verdict": f[8]}
    except (OSError, IndexError, ValueError) as e:
        die("%s: %s" % (p, e))
    return rows


def cmd_compare(argv):
    bar = 2.0
    if argv[:1] == ["--bar"]:
        bar = float(argv[1])
        argv = argv[2:]
    if len(argv) not in (2, 3):
        die("compare needs BASE.tsv NEW.tsv [OUT.md]")
    a, b = read_tsv(argv[0]), read_tsv(argv[1])
    both = [t for t in a if t in b]
    if not both:
        die("no task in both")
    lines = ["| Task | Base median s | New median s | New / base | Base spread % | New spread % | Base noise % | New noise % |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    ratios = []
    for t in sorted(both, key=lambda t: -(a[t]["spread"])):
        r = b[t]["median"] / a[t]["median"]
        ratios.append(r)
        lines.append("| %s | %.4f | %.4f | %.4f | %.1f | %.1f | %.1f | %.1f |" % (
            t, a[t]["median"], b[t]["median"], r, a[t]["spread"], b[t]["spread"], a[t]["noise"], b[t]["noise"]))
    gm = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
    over = lambda rows: sum(1 for t in both if rows[t]["spread"] > bar and rows[t]["spread"] > rows[t]["noise"])
    summary = ("compare: %d tasks; median ratio new/base geometric mean %.4f (min %.3f, max %.3f); "
               "over the bar and noise: base %d, new %d; mean spread base %.2f%%, new %.2f%%; "
               "spread lower on %d tasks, higher on %d"
               % (len(both), gm, min(ratios), max(ratios), over(a), over(b),
                  statistics.mean(a[t]["spread"] for t in both), statistics.mean(b[t]["spread"] for t in both),
                  sum(1 for t in both if b[t]["spread"] < a[t]["spread"]), sum(1 for t in both if b[t]["spread"] > a[t]["spread"])))
    if len(argv) == 3:
        open(argv[2], "w").write("# Layout spread: %s against %s\n\n%s.\n\n%s\n" % (argv[1], argv[0], summary, "\n".join(lines)))
    print(summary)
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 0 if len(argv) >= 2 else 2
    if argv[1] == "spread":
        return cmd_spread(argv[2:])
    if argv[1] == "compare":
        return cmd_compare(argv[2:])
    die("unknown command %s (spread, compare)" % argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
