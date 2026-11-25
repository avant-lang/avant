#!/usr/bin/env python3
"""The table of two instruction-count files of the second half (tools/la-half.sh table).

  python3 -I tools/la-half.py table HEAD.tsv BASE.tsv NAMES.tsv

HEAD.tsv and BASE.tsv are tools/la-count.sh's output (SLUG, instructions, the
stdout's md5, the exit status, the first line); NAMES.tsv gives each program's
name (SLUG, name). Prints the markdown table, then the summary lines. Exit 1
when a program did not compile or run, or printed another output on one side.
"""
import math
import sys

BEYOND = 0.001  # a count moved beyond 0.1% (the instruction counts repeat to about 0.02%)


def read(path):
    rows = {}
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4 and p[0]:
                rows[p[0]] = p
    return rows


def count(row):
    return int(row[1]) if row and row[1].isdigit() else None


def main(argv):
    if len(argv) != 5 or argv[1] != "table":
        sys.stderr.write(__doc__)
        return 2
    head, base = read(argv[2]), read(argv[3])
    names = {}
    with open(argv[4]) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) == 2:
                names[p[0]] = p[1]
    slugs = sorted(set(head) | set(base), key=lambda s: names.get(s, s).lower())
    out = ["| Task | Base instructions | Head instructions | Head / base | Output |", "| --- | ---: | ---: | ---: | --- |"]
    ratios, red, faster, slower = [], [], [], []
    for s in slugs:
        h, b = head.get(s), base.get(s)
        hc, bc = count(h), count(b)
        name = names.get(s, s)
        state = "same"
        if h is None or b is None:
            state = "only " + ("head" if b is None else "base")
        elif hc is None or bc is None:
            state = "not run (%s / %s)" % ("head " + h[1] if hc is None else "head ok", "base " + b[1] if bc is None else "base ok")
        elif h[3] != "0" or b[3] != "0":
            state = "status head %s, base %s" % (h[3], b[3])
        elif h[2] != b[2]:
            state = "differs"
        if state != "same":
            red.append("%s: %s" % (name, state))
        ratio = "-"
        if hc and bc:
            r = hc / bc
            ratio = "%.4f" % r
            if state == "same":
                ratios.append(r)
                if r > 1 + BEYOND:
                    slower.append((name, r))
                elif r < 1 - BEYOND:
                    faster.append((name, r))
        out.append("| %s | %s | %s | %s | %s |" % (name, bc if bc is not None else "-", hc if hc is not None else "-", ratio, state))
    out.append("")
    if ratios:
        g = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        out.append("%d programs counted on both sides; geometric mean of head / base %.4f; %d fewer instructions beyond 0.1%%, %d more." % (len(ratios), g, len(faster), len(slower)))
        if faster:
            out.append("Fewer: " + ", ".join("%s %.4f" % x for x in sorted(faster, key=lambda x: x[1])))
        if slower:
            out.append("More: " + ", ".join("%s %.4f" % x for x in sorted(slower, key=lambda x: -x[1])))
    else:
        out.append("No program counted on both sides.")
    if red:
        out.append("")
        out.append("Red (the output differs, or a program did not compile or run): " + "; ".join(red))
    else:
        out.append("Every output is the same on both sides, and every program ran.")
    print("\n".join(out))
    return 1 if red or not ratios else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
