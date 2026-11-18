#!/usr/bin/env python3
"""The LangArena tasks whose time moved while their instruction count did
not: worth one layout experiment (tools/layout-retime.sh) before the time
is believed.

  python3 tools/layout-flag.py HEAD_LR BASE_LR HEAD_INS BASE_INS [OUT.tsv]

HEAD_LR and BASE_LR are two LR re-measures (`tools/lr-table.sh avant`'s
lr-avant.tsv: task, status, wall_s, ...): the head's and the base's.
HEAD_INS and BASE_INS are instruction counts of the same stitched programs
(`tools/la-count.sh`'s ins.tsv, one line per program: SLUG, count, ...):
the head's C' and the base's (the base cache's LangArena part). A task (its
slug: the name with `::` as `_`, lowercased) is flagged when both times are
ok and differ by more than 3% and 0.02 s, while the two counts differ by at
most 0.1%: the code the task runs is the same amount of work, so where the
code or its data sits is the first suspect (a PLT entry, a function or a
mapping added before it can move a task several percent on some machines).

Prints one line: `layout: N of M tasks ...`, naming each flagged task with
its time change and count change; with OUT.tsv, writes one line per flagged
task (task, slug, base s, head s, time %, count %). Exit 0, 2 on a usage
error or an unreadable input.
"""
import sys

TIME_REL, TIME_ABS, INS_REL = 0.03, 0.02, 0.001


def lr(path):
    times = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or line.startswith("task\t"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) >= 3 and p[1] == "ok":
                try:
                    times[p[0]] = float(p[2])
                except ValueError:
                    pass
    return times


def ins(path):
    counts = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 2 and p[1].isdigit():
                counts[p[0]] = int(p[1])
    return counts


def main(argv):
    if len(argv) not in (5, 6):
        sys.stderr.write(__doc__.split("\n\n")[1] + "\n")
        return 2
    try:
        ht, bt, hi, bi = lr(argv[1]), lr(argv[2]), ins(argv[3]), ins(argv[4])
    except OSError as e:
        sys.stderr.write("layout-flag.py: %s\n" % e)
        return 2
    rows, judged = [], 0
    for task in ht:
        slug = task.replace("::", "_").lower()
        if task not in bt or slug not in hi or slug not in bi or bi[slug] == 0:
            continue
        judged += 1
        a, b = bt[task], ht[task]
        dt, di = b / a - 1, hi[slug] / bi[slug] - 1
        if abs(b - a) > TIME_ABS and abs(dt) > TIME_REL and abs(di) <= INS_REL:
            rows.append((task, slug, a, b, dt * 100, di * 100))
    rows.sort(key=lambda r: -abs(r[4]))
    if len(argv) == 6:
        with open(argv[5], "w", encoding="utf-8") as f:
            f.write("task\tslug\tbase_s\thead_s\ttime_pct\tins_pct\n")
            for r in rows:
                f.write("%s\t%s\t%.3f\t%.3f\t%+.1f\t%+.3f\n" % r)
    rule = "time beyond 3% and 0.02 s, instructions within 0.1%"
    if rows:
        named = ", ".join("%s %+.1f%% (instructions %+.3f%%)" % (r[0], r[4], r[5]) for r in rows)
        print("layout: %d of %d tasks layout? (%s): %s; one layout experiment each before it is believed (tools/layout-retime.sh)" % (len(rows), judged, rule, named))
    else:
        print("layout: none of %d tasks (%s)" % (judged, rule))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
