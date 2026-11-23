#!/usr/bin/env python3
"""A CI run's step times per unit of runner speed.

    python3 tools/ci-steps.py RUN.json[=REF_SECONDS] ...

RUN.json is GitHub's jobs listing of one workflow run (`gh api
repos/OWNER/REPO/actions/runs/RUN_ID/jobs > RUN.json`) or one job of it.
REF_SECONDS is the runner's speed reference that the job printed
(`.github/runner-speed.sh`: `runner speed reference: re2c make S s`); without
it the reference is the whole step `Build the bundled re2c` (its download
included, so noisier), and the table says so.

Prints one Markdown table per job name: each step's seconds in every run and
its time in units of that run's reference (seconds / reference), so runs on
runners of different speed compare by the units column. Several runs side by
side: one column pair per run, in the order given.
"""
import datetime
import json
import sys

REF_STEP = "Build the bundled re2c"


def seconds(step):
    a, b = step.get("started_at"), step.get("completed_at")
    if not a or not b:
        return None
    f = lambda t: datetime.datetime.fromisoformat(t.replace("Z", "+00:00"))
    return (f(b) - f(a)).total_seconds()


def jobs_of(path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return data.get("jobs", [data]) if isinstance(data, dict) else data


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if argv else 2
    runs = []
    for arg in argv:
        path, _, ref = arg.partition("=")
        runs.append((path, float(ref) if ref else None, jobs_of(path)))
    names = []
    for _, _, jobs in runs:
        for j in jobs:
            if j.get("name") not in names:
                names.append(j.get("name"))
    for name in names:
        cols, order = [], []
        for path, ref, jobs in runs:
            job = next((j for j in jobs if j.get("name") == name), None)
            steps = {}
            for s in (job or {}).get("steps", []):
                steps[s["name"]] = seconds(s)
                if s["name"] not in order:
                    order.append(s["name"])
            how = "re2c make"
            if ref is None:
                ref = steps.get(REF_STEP)
                how = "the whole re2c step"
            cols.append((path, ref, how, steps))
        print("### %s\n" % name)
        for path, ref, how, _ in cols:
            print("- %s: reference %s s (%s)" % (path, "%.2f" % ref if ref else "none", how))
        print()
        print("| Step | " + " | ".join("s | units" for _ in cols) + " |")
        print("| --- | " + " | ".join("---: | ---:" for _ in cols) + " |")
        for step in order:
            cells = []
            for _, ref, _, steps in cols:
                v = steps.get(step)
                cells.append("-" if v is None else "%.0f" % v)
                cells.append("-" if v is None or not ref else "%.2f" % (v / ref))
            print("| %s | %s |" % (step, " | ".join(cells)))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
