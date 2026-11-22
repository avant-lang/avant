#!/usr/bin/env python3
"""la-mem.py — two sides' peak memory of the LangArena programs, compared.

    python3 -I tools/la-mem.py BASE.mem.tsv HEAD.mem.tsv [--md]

Each file is tools/la-count.sh's OUT.mem.tsv (LA_MEM_RUNS=N): per program
its median, minimum and maximum peak RSS in KB over N runs, unbound and with
address randomization off, and the runs. Printed:

- the average peak over the published tasks (every program but the
  pure-Avant twins, named `-pure`, `-common` or `-clever`) of each side, from the medians, and their ratio; its
  spread: the average over the tasks of each run index (run i of every
  task) is one complete pass, so the range of those passes' averages is the
  measured run-to-run variation of the average itself, and the ratio's
  spread is the head's lowest pass over the base's highest to the head's
  highest over the base's lowest;
- the verdict on the average: lower or higher when the ratio's whole
  spread is below or above 1, else within the spread;
- per program (twins included) whose median moved: the change, `moved`
  when the two sides' min–max ranges do not overlap, `repeat` when they
  overlap (the runs do not decide it: run that program again with more
  runs, LA_MEM_RUNS, or read its runs' modes);
- `repeat` too for a program whose own runs spread more than 1% on either
  side (its peak is not one value: more runs before its change is read).

Status 0; 2 on usage or a file of another shape.
"""
import sys

def load(p):
    r = {}
    for line in open(p):
        f = line.rstrip("\n").split("\t")
        if len(f) < 5:
            sys.exit("la-mem.py: %s: not a la-count.sh mem file (%r)" % (p, line))
        r[f[0]] = (float(f[1]), float(f[2]), float(f[3]), [float(x) for x in f[4].split(",")])
    return r

def main(argv):
    md = "--md" in argv
    args = [a for a in argv if a != "--md"]
    if len(args) != 2:
        sys.exit(__doc__.strip())
    b, h = load(args[0]), load(args[1])
    both = sorted(set(b) & set(h))
    tasks = [s for s in both if not s.endswith(("-pure", "-common", "-clever"))]
    if not tasks:
        sys.exit("la-mem.py: no program on both sides")
    avg = lambda d: sum(d[s][0] for s in tasks) / len(tasks)
    def passes(d):
        n = min(len(d[s][3]) for s in tasks)
        return [sum(d[s][3][i] for s in tasks) / len(tasks) for i in range(n)]
    ab, ah = avg(b), avg(h)
    pb, ph = passes(b), passes(h)
    lo, hi = min(ph) / max(pb), max(ph) / min(pb)
    verdict = "lower" if hi < 1 else "higher" if lo > 1 else "within the spread"
    out = []
    out.append("average peak over %d tasks: base %.1f KB, head %.1f KB, head/base %.4f (passes %.4f–%.4f): %s" % (len(tasks), ab, ah, ah / ab, lo, hi, verdict))
    out.append("the passes' own spread: base %.3f%%, head %.3f%% of the average (1%% of the base's average is %.0f KB)" % (100 * (max(pb) - min(pb)) / ab, 100 * (max(ph) - min(ph)) / ah, ab / 100))
    rows = []
    for s in both:
        mb, nb, xb, _ = b[s]
        mh, nh, xh, _ = h[s]
        wide = (xb - nb) > 0.01 * mb or (xh - nh) > 0.01 * mh
        if mb == mh and not wide:
            continue
        if xh < nb or nh > xb:
            v = "moved"
        else:
            v = "repeat"
        if wide:
            v = "repeat" if v == "repeat" else v + ", repeat (runs spread over 1%)"
        rows.append((s, mb, nb, xb, mh, nh, xh, v))
    if md:
        out.append("")
        out.append("| Program | Base KB (min–max) | Head KB (min–max) | Change | Verdict |")
        out.append("| --- | --- | --- | --- | --- |")
        for s, mb, nb, xb, mh, nh, xh, v in rows:
            out.append("| %s | %.0f (%.0f–%.0f) | %.0f (%.0f–%.0f) | %+.2f%% | %s |" % (s, mb, nb, xb, mh, nh, xh, 100 * (mh / mb - 1), v))
    else:
        for s, mb, nb, xb, mh, nh, xh, v in rows:
            out.append("%s\t%.0f (%.0f-%.0f)\t%.0f (%.0f-%.0f)\t%+.2f%%\t%s" % (s, mb, nb, xb, mh, nh, xh, 100 * (mh / mb - 1), v))
    rep = [r[0] for r in rows if "repeat" in r[7]]
    out.append("to run again: %s" % (" ".join(rep) if rep else "none"))
    print("\n".join(out))

if __name__ == "__main__":
    main(sys.argv[1:])
