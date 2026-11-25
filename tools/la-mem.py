#!/usr/bin/env python3
"""la-mem.py — two sides' peak memory of the LangArena programs, compared.

    python3 -I tools/la-mem.py BASE.mem.tsv HEAD.mem.tsv [--md] [--mixed]

Each file is tools/la-count.sh's OUT.mem.tsv (LA_MEM_RUNS=N): per program
its median, minimum and maximum peak RSS in KB over N runs, unbound and with
address randomization off, and the runs. Its first line names how the peaks
were read (`# peak: exact`, or `# peak: maxrss`, the kernel's ru_maxrss,
which moves in steps of about 256 KB at equal work; a file without that line
is maxrss, as la-count.sh wrote them before it read peaks exactly). Two files
of different methods are not compared (status 2) unless --mixed is given: a
step of the counter would read as a moved peak. Printed:

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
  side (its peak is not one value: more runs before its change is read);
- with the runs taken with address randomization on (the files' sixth
  field, la-count.sh's LA_MEM_ON_RUNS) on both sides, a program is `moved`
  only when those runs' ranges part too, the same way: with it off each
  binary keeps one set of mapping addresses, so a change that moves only
  where a mapping lands reads as a moved peak in every run; such a row
  reads `repeat (ASLR on does not confirm)`, and the average of the
  ASLR-on medians is printed beside the other.

Status 0; 2 on usage or a file of another shape.
"""
import sys

def median(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2

def load(p):
    """The file's method and its rows."""
    r = {}
    method = "maxrss"
    for line in open(p):
        if line.startswith("#"):
            if line.startswith("# peak: "):
                method = line[len("# peak: "):].strip()
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) < 5:
            sys.exit("la-mem.py: %s: not a la-count.sh mem file (%r)" % (p, line))
        on = [float(x) for x in f[5].split(",") if x] if len(f) > 5 else []
        r[f[0]] = (float(f[1]), float(f[2]), float(f[3]), [float(x) for x in f[4].split(",")], on)
    return method, r

def main(argv):
    md = "--md" in argv
    mixed = "--mixed" in argv
    args = [a for a in argv if a not in ("--md", "--mixed")]
    if len(args) != 2:
        sys.exit(__doc__.strip())
    (mb_, b), (mh_, h) = load(args[0]), load(args[1])
    if mb_ != mh_ and not mixed:
        print("la-mem.py: the base's peaks are %s and the head's %s: read both sides alike (la-count.sh's LA_MEM_METHOD), or pass --mixed" % (mb_, mh_), file=sys.stderr)
        sys.exit(2)
    method = mb_ if mb_ == mh_ else "base %s, head %s" % (mb_, mh_)
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
    out.append("peaks read: %s" % method)
    if all(b[s][4] and h[s][4] for s in tasks):
        aob = sum(median(b[s][4]) for s in tasks) / len(tasks)
        aoh = sum(median(h[s][4]) for s in tasks) / len(tasks)
        out.append("with ASLR on: base %.1f KB, head %.1f KB, head/base %.4f" % (aob, aoh, aoh / aob))
    out.append("the passes' own spread: base %.3f%%, head %.3f%% of the average (1%% of the base's average is %.0f KB)" % (100 * (max(pb) - min(pb)) / ab, 100 * (max(ph) - min(ph)) / ah, ab / 100))
    rows = []
    for s in both:
        mb, nb, xb, _, ob = b[s]
        mh, nh, xh, _, oh = h[s]
        wide = (xb - nb) > 0.01 * mb or (xh - nh) > 0.01 * mh
        if mb == mh and not wide:
            continue
        if xh < nb or nh > xb:
            v = "moved"
            if ob and oh and not ((xh < nb and max(oh) < min(ob)) or (nh > xb and min(oh) > max(ob))):
                v = "repeat (ASLR on does not confirm)"
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
