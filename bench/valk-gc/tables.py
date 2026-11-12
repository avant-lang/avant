#!/usr/bin/env python3
"""Regenerates every table of bench/valk-gc from the raw per-run files.

usage: python3 tables.py OUT > tables.md

OUT is the directory run.sh wrote (VALK_GC_OUT): OUT/raw (run.sh time)
and OUT/counts (run.sh counts). Nothing is measured here; a table is only
a reading of those files, so it can be rebuilt at any time. Standard
library only.
"""
import os
import re
import statistics
import sys

SCENARIOS = {
    1: "short-lived alloc",
    2: "stable-heap forced collects",
    3: "build long-lived chain",
    4: "free long-lived chain",
    5: "mutate live links",
    6: "short-lived churn with large live set",
    7: "tree churn",
    8: "reverse live links",
}
NUMBER = {name: n for n, name in SCENARIOS.items()}
SHORT = {1: "1 short-lived", 2: "2 stable collects", 3: "3 build chain", 4: "4 free chain",
         5: "5 mutate links", 6: "6 churn + live", 7: "7 tree churn", 8: "8 reverse links (added)"}

HEADLINE = ["avant", "avant-v3", "valk", "go", "d", "csharp", "crystal", "java"]
VARIANTS = ["avant-baseline", "avant-young", "avant-young-v3", "avant-hints", "avant-hints-v3",
            "valk-noregions", "go-plain-sink", "go-free-outside", "go-gogc-off",
            "d-minimize-outside", "csharp-notiered", "java-parallel"]
LABEL = {
    "avant": "Avant `--release` (x86-64-v2, the compiler's default)",
    "avant-v3": "Avant `--release --cpu=v3`",
    "valk": "Valk 0.8.5 `--release`",
    "go": "Go",
    "d": "D (LDC `-O3 -release -boundscheck=off`)",
    "csharp": "C# (.NET, tiered JIT)",
    "crystal": "Crystal `--release` (new port)",
    "java": "Java (G1, new port)",
    "avant-baseline": "Avant `--release --cpu=baseline`",
    "avant-young": "Avant, young collections (not complete collections)",
    "avant-young-v3": "Avant `--cpu=v3`, young collections (not complete)",
    "avant-hints": "Avant with hints (`gc.hold`, `gc.collect()`)",
    "avant-hints-v3": "Avant `--cpu=v3` with hints",
    "valk-noregions": "Valk `--release --no-regions`",
    "go-plain-sink": "Go, plain sink (no atomic store)",
    "go-free-outside": "Go, `FreeOSMemory` outside scenario 4's timer",
    "go-gogc-off": "Go, `GOGC=off` (upstream's README variant)",
    "d-minimize-outside": "D, `GC.minimize` outside scenario 4's timer",
    "csharp-notiered": "C#, `DOTNET_TieredCompilation=0`",
    "java-parallel": "Java, `-XX:+UseParallelGC`",
}
# The row a variant is read against.
BASE = {"avant-baseline": "avant", "avant-young": "avant", "avant-young-v3": "avant-v3",
        "avant-hints": "avant", "avant-hints-v3": "avant-v3", "valk-noregions": "valk",
        "go-plain-sink": "go", "go-free-outside": "go", "go-gogc-off": "go",
        "d-minimize-outside": "d", "csharp-notiered": "csharp", "java-parallel": "java"}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_times(path):
    """{scenario number: time_us} of one program output."""
    out, cur = {}, None
    for line in read(path).splitlines():
        m = re.match(r"^=== (.*) ===$", line)
        if m:
            cur = NUMBER.get(m.group(1))
            continue
        if cur and line.startswith("time_us:"):
            out[cur] = int(line.split()[1])
    return out


def rounds(d):
    """The measured rounds of a row directory, in order."""
    if not os.path.isdir(d):
        return []
    names = [f[:-4] for f in os.listdir(d) if re.match(r"^r[1-9][0-9]*\.out$", f)]
    return sorted(names, key=lambda n: int(n[1:]))


def load_row(d):
    """A row's runs: per-scenario times, wall (us), GNU time elapsed, RSS."""
    row = {"times": {}, "wall_us": [], "elapsed_s": [], "rss_kb": [], "n": 0}
    for r in rounds(d):
        if read(os.path.join(d, r + ".rc")).strip() != "0":
            continue
        for k, v in parse_times(os.path.join(d, r + ".out")).items():
            row["times"].setdefault(k, []).append(v)
        row["wall_us"].append(int(read(os.path.join(d, r + ".wall_us")).strip()))
        e, m = read(os.path.join(d, r + ".time")).split()[-2:]
        row["elapsed_s"].append(float(e))
        row["rss_kb"].append(int(m))
        row["n"] += 1
    return row


def ms(us):
    v = us / 1000.0
    if v < 1:
        return "%.3f" % v
    if v < 10:
        return "%.2f" % v
    if v < 100:
        return "%.1f" % v
    return "%.0f" % v


def cell(values, fmt=ms):
    if not values:
        return "—"
    return "%s (%s–%s)" % (fmt(statistics.median(values)), fmt(min(values)), fmt(max(values)))


def mb(kb):
    return "%.1f" % (kb / 1024.0)


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "| " + " | ".join(["---"] * len(header)) + " |"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out) + "\n"


def scenario_table(data, order, numbers):
    rows = []
    for name in order:
        if name not in data or not data[name]["n"]:
            continue
        rows.append([LABEL.get(name, name)] + [cell(data[name]["times"].get(k, [])) for k in numbers])
    return table(["Row"] + [SHORT[k] for k in numbers], rows)


def verdict(avant, valk):
    """Avant against Valk on one scenario, from all runs of both."""
    if not avant or not valk:
        return "—"
    if max(avant) < min(valk):
        return "leads"
    if min(avant) > max(valk):
        return "trails"
    return "ranges overlap"


def emit_raw(out_dir):
    raw = os.path.join(out_dir, "raw")
    if not os.path.isdir(raw):
        print("No timed session in %s (run.sh time).\n" % raw)
        return
    print("## The timed session\n")
    print("```")
    print(read(os.path.join(raw, "session.txt")).rstrip())
    print("```\n")
    head = {r: load_row(os.path.join(raw, "headline", r)) for r in HEADLINE}
    var = {r: load_row(os.path.join(raw, "variants", r)) for r in VARIANTS}
    n = max([d["n"] for d in head.values()] + [0])
    seven = [1, 2, 3, 4, 5, 6, 7]
    print("## Table 1. The seven scenarios in one process: `time_us` of each scenario, in ms, median (minimum–maximum) of %d runs\n" % n)
    print(scenario_table(head, HEADLINE, seven))
    print("## Table 2. Avant against Valk, scenario by scenario (Table 1's runs)\n")
    rows = []
    for k in seven:
        v = head["valk"]["times"].get(k, [])
        for a in ("avant", "avant-v3"):
            t = head[a]["times"].get(k, [])
            if not t or not v:
                continue
            rows.append([SHORT[k], LABEL[a].split(" (")[0], cell(t), cell(v),
                         "%.2f" % (statistics.median(t) / statistics.median(v)), verdict(t, v)])
    print(table(["Scenario", "Avant row", "Avant, ms", "Valk, ms", "Avant ÷ Valk (medians)", "Verdict (all runs)"], rows))
    print("A verdict is `leads` when Avant's slowest run is faster than Valk's fastest, `trails` when Avant's fastest is slower than Valk's slowest, `ranges overlap` otherwise.\n")
    print("## Table 3. The labelled variants: `time_us` of each scenario, in ms, median (minimum–maximum); each variant under the row it is read against\n")
    order, seen = [], set()
    for v in VARIANTS:
        b = BASE[v]
        if b not in seen:
            order.append(b)
            seen.add(b)
        order.append(v)
    both = dict(head)
    both.update(var)
    print(scenario_table(both, order, seven))
    print("## Table 4. Whole process (the seven scenarios): wall time and peak RSS, median (minimum–maximum)\n")
    rows = []
    for name in HEADLINE + VARIANTS:
        d = both[name]
        if not d["n"]:
            continue
        rows.append([LABEL[name], d["n"], cell(d["wall_us"], lambda us: "%.3f" % (us / 1e6)),
                     cell(d["elapsed_s"], lambda s: "%.2f" % s), cell(d["rss_kb"], mb)])
    print(table(["Row", "Runs", "Wall, s (shell clock around the launch)", "Elapsed, s (GNU time `%e`)", "Peak RSS, MB (GNU time `%M`)"], rows))
    iso = {}
    for r in HEADLINE:
        d = {"times": {}, "rss": {}, "wall": {}, "n": 0}
        for k in range(1, 9):
            row = load_row(os.path.join(raw, "iso", r, "s%d" % k))
            if row["n"]:
                d["times"][k] = row["times"].get(k, [])
                d["rss"][k] = row["rss_kb"]
                d["wall"][k] = row["wall_us"]
                d["n"] = max(d["n"], row["n"])
        iso[r] = d
    eight = [1, 2, 3, 4, 5, 6, 7, 8]
    if any(d["n"] for d in iso.values()):
        print("## Table 5. Scenario-isolated builds, one scenario per process: `time_us`, in ms, median (minimum–maximum). Scenario 8 is added by Avant and is not part of Valk's suite\n")
        print(scenario_table(iso, HEADLINE, eight))
        print("## Table 6. Scenario-isolated builds: peak RSS of the process that ran the scenario, MB, median (minimum–maximum). Scenario 4's process also ran scenario 3\n")
        rows = [[LABEL[r]] + [cell(iso[r]["rss"].get(k, []), mb) for k in eight] for r in HEADLINE if iso[r]["n"]]
        print(table(["Row"] + [SHORT[k] for k in eight], rows))


def stats_line(path):
    for line in read(path).splitlines():
        if line.startswith("avant gc:"):
            return {k: v for k, v in (kv.split("=", 1) for kv in line.split()[2:])}
    return {}


def ins(path):
    try:
        return int(read(path).strip())
    except (OSError, ValueError):
        return None


def per_scenario(get):
    """Values for scenarios 1..8 from per-process values: the start-up
    process (unit 0) is subtracted, and scenario 4 is (3 4) minus 3."""
    base = get(0)
    out = {}
    for k in range(1, 9):
        v = get(k)
        if v is None or base is None:
            out[k] = None
        elif k == 4:
            out[k] = v - get(3) if get(3) is not None else None
        else:
            out[k] = v - base
    return out


def num(v):
    return "—" if v is None else "{:,}".format(v)


def emit_counts(out_dir):
    c = os.path.join(out_dir, "counts")
    if not os.path.isdir(c):
        print("No counts in %s (run.sh counts).\n" % c)
        return
    eight = [1, 2, 3, 4, 5, 6, 7, 8]
    print("## Counts (no clock): one scenario per process, scenario-isolated builds\n")
    print("```")
    print(read(os.path.join(c, "session.txt")).rstrip())
    print("```\n")
    print("Each figure is the process that ran the scenario minus the process that ran nothing; scenario 4's is the process that ran 3 and 4 minus the one that ran 3. A scenario's untimed set-up (the live list of 2, 5, 6 and 8 and the collection that settles it) is inside its figure.\n")
    print("### Table 7. User-mode instructions per scenario (`perf stat -e instructions:u`)\n")
    rows = []
    names = [("avant-full", "Avant `--release`"), ("avant-young", "Avant, young collections"),
             ("avant-hints", "Avant with hints"), ("avant-v3", "Avant `--release --cpu=v3`"),
             ("valk", "Valk"), ("go", "Go"), ("d", "D"), ("crystal", "Crystal")]
    for d, label in names:
        p = per_scenario(lambda k, d=d: ins(os.path.join(c, d, "s%d.ins" % k)))
        rows.append([label] + [num(p[k]) for k in eight])
    print(table(["Row"] + [SHORT[k] for k in eight], rows))
    fields = [("collections", "collections"), ("young", "young"), ("full", "full"),
              ("alloc", "allocated, bytes"), ("copied", "copied, bytes"),
              ("prom_bytes", "promoted, bytes"), ("rem_adds", "remembered-set adds"),
              ("inline_allocs", "inline allocations"), ("pause_young_ns", "young pauses, ns"),
              ("pause_full_ns", "full pauses, ns")]
    for mode, title in (("full", "the headline program's collections (full where upstream forces one)"),
                        ("young", "young collections in place of the forced full ones"),
                        ("hints", "with hints")):
        print("### Table 8 (%s). Avant's `AVANT_GC_STATS=1` per scenario: %s\n" % (mode, title))
        st = {k: stats_line(os.path.join(c, "avant-" + mode, "s%d.stats" % k)) for k in range(0, 9)}
        rows = []
        for key, label in fields:
            p = per_scenario(lambda k, key=key: int(st[k][key]) if key in st[k] else None)
            rows.append([label] + [num(p[k]) for k in eight])
        rows.append(["peak heap of the process, bytes"] + [num(int(st[k]["peak_heap"])) if "peak_heap" in st[k] else "—" for k in eight])
        rows.append(["young budget at exit, MB"] + [st[k].get("young_mb", "—") for k in eight])
        print(table(["Counter"] + [SHORT[k] for k in eight], rows))
    print("The two pause rows are clock readings: they are evidence only when the counts were taken on a quiet machine.\n")
    print("### Table 9. Collections the other runtimes report, per scenario process\n")
    rows = []

    def count(d, ext, pattern):
        out = []
        for k in eight:
            p = os.path.join(c, d, "s%d.%s" % (k, ext))
            out.append(str(len(re.findall(pattern, read(p), re.M))) if os.path.exists(p) else "—")
        return out

    rows.append(["Go: collections (`GODEBUG=gctrace=1` lines)"] + count("go-gctrace", "err", r"^gc \d+ @"))
    rows.append(["Go: of them forced"] + count("go-gctrace", "err", r"^gc \d+ @.*\(forced\)"))

    def d_count(k):
        p = os.path.join(c, "d-profile", "s%d.out" % k)
        if not os.path.exists(p):
            return "—"
        m = re.search(r"Number of collections:\s*(\d+)", read(p) + read(p[:-3] + "err"))
        return m.group(1) if m else "—"

    rows.append(["D: collections (`--DRT-gcopt=profile:1`)"] + [d_count(k) for k in eight])
    rows.append(["Crystal (Boehm): collections (`GC_PRINT_STATS=1`)"] + count("crystal-gcstats", "err", r"^GC #\d+ "))
    rows.append(["Java G1: young pauses (`-Xlog:gc`)"] + count("java-xlog", "out", r"Pause Young"))
    rows.append(["Java G1: full pauses"] + count("java-xlog", "out", r"Pause Full"))
    rows.append(["Java Parallel: young pauses"] + count("java-parallel-xlog", "out", r"Pause Young"))
    rows.append(["Java Parallel: full pauses"] + count("java-parallel-xlog", "out", r"Pause Full"))
    print(table(["Runtime"] + [SHORT[k] for k in eight], rows))
    print("These are whole-process counts (nothing is subtracted): a scenario's set-up collections and the forced ones are in them.\n")


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    out_dir = sys.argv[1]
    print("# Valk's GC suite on this machine: tables regenerated by `bench/valk-gc/tables.py` from the raw per-run files\n")
    print("Scenarios, sizes and programs are Valk's (valk-lang/valk, `examples/bench/gc`, MIT, © 2024 Lorenz Vandevelde). Every number here was measured on this machine; Valk's published numbers come from another machine and are not in these tables.\n")
    emit_raw(out_dir)
    emit_counts(out_dir)


if __name__ == "__main__":
    main()
