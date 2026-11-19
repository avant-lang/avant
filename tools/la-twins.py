#!/usr/bin/env python3
"""la-twins.py — LangArena's pure-Avant twins timed on the NUMA nodes at once.

  python3 tools/la-twins.py LA_DIR HARNESS RUN_JS [--nodes 0,1,2,3] [--estimates TSV]

The twins (LA_DIR/library.tsv's rows with a twin, written there by the
harness's tasks pass) are timed as before, each once by the harness's prod
pass (its compile, its run, its checksum) and once by the LR re-measure
(tools/lr-table.sh avant, the undisturbed rule), but on the machine's NUMA
nodes side by side instead of one after another on the whole machine: every
run is pinned with its memory to one node (`numactl --cpunodebind=N
--membind=N`), one run a node at a time, so no two runs share a core, a
cache or a memory controller. Run it only where the whole machine is held
(the final's LangArena timing): the nodes are free because nothing else
runs. Every twin is single-threaded; the tasks themselves, compared with
the other languages' whole-machine references, stay on the whole machine.

Each twin is two jobs, its prod pass (P) and its LR re-measure (R); R runs
the binary P compiled and starts once P's compile is done (the harness's
`[i/n] NAME run` line), on another node while P runs, so a long twin's two
runs overlap. A free node takes the first ready job of the list, longest
first (the estimates: an earlier run's lr-avant-pure.tsv, --estimates; else
the library's order), P before R; a job is never run twice, and no job
waits for one behind it (R waits only for its own P's compile, which is
ahead of it in the list), so the list always drains.

Run from the checkout whose langarena/ holds the harness (its binaries go
to langarena/tmp). The caller's environment (AVANT_COMPILER, AVANT_CODEGEN,
...) reaches every job; a P job adds AVANT_TASK_LIST=NAME and
AVANT_RESULTS_DIR=LA_DIR/twins/SLUG/prod, an R job LR_TASKS=0,
LR_TWIN_LIST=NAME and LR_LIBRARY=LA_DIR/library.tsv. Afterwards the rows
are merged into LA_DIR as the sequential passes wrote them:
runtime-pure.json (the harness's report pass then writes runtime-pure.tsv
and SUMMARY.md from it), raw/SLUG.txt, lr-avant-pure.tsv (the twins in the
library's order, the first job's header) and lr-avant-pure.tsv.raw/; each
job's log stays in LA_DIR/twins/SLUG/, and LA_DIR/twins.tsv lists the jobs
(job, node, start, end, seconds, status). Status 0 when every job ended 0
and every twin has both rows, else 1; 2 on a usage error.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time


def die(msg, code=2):
    sys.stderr.write("la-twins.py: %s\n" % msg)
    sys.exit(code)


def nodes_of_machine():
    base = "/sys/devices/system/node"
    try:
        return sorted(int(n[4:]) for n in os.listdir(base) if n.startswith("node") and n[4:].isdigit())
    except OSError:
        return []


def twins_of(lib):
    """[(task, twin, slug)] in library.tsv's order."""
    out = []
    for line in open(lib):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 4 and f[0] != "task" and f[2] not in ("-", ""):
            out.append((f[0], f[2], f[3]))
    return out


def estimates_of(path):
    est = {}
    if path and os.path.exists(path):
        for line in open(path):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 3 and not line.startswith("#") and f[0] != "task":
                try:
                    est[f[0]] = float(f[2])
                except ValueError:
                    pass
    return est


def main(argv):
    args, nodes, est_path = [], None, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--nodes" and i + 1 < len(argv):
            try:
                nodes = [int(x) for x in argv[i + 1].split(",") if x != ""]
            except ValueError:
                die("--nodes takes node numbers, such as 0,1,2,3")
            i += 2
        elif a == "--estimates" and i + 1 < len(argv):
            est_path = argv[i + 1]
            i += 2
        elif a.startswith("-"):
            die("unknown option %s (--help)" % a)
        else:
            args.append(a)
            i += 1
    if len(args) != 3:
        die("usage: la-twins.py LA_DIR HARNESS RUN_JS [--nodes LIST] [--estimates TSV]")
    la, harness, run_js = (os.path.abspath(x) for x in args)
    lib = os.path.join(la, "library.tsv")
    if not os.path.exists(lib):
        print("la-twins: no library.tsv in %s (the harness lists no twin): none timed" % la)
        return 0
    if not os.access(harness, os.X_OK):
        die("%s: not an executable harness" % harness)
    if not shutil.which("numactl"):
        die("numactl is not installed")
    have = nodes_of_machine()
    nodes = nodes if nodes is not None else have
    if not nodes or any(n not in have for n in nodes):
        die("nodes %s: the machine's are %s" % (nodes, have))
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.getcwd()
    bindir = os.path.join(root, "langarena", "tmp")
    twins = twins_of(lib)
    if not twins:
        print("la-twins: no twin in %s" % lib)
        return 0
    est = estimates_of(est_path)
    order = sorted(twins, key=lambda t: -est.get(t[1], 0.0)) if est else list(twins)
    jobs = []  # each: kind, twin, slug, dir, deps, state
    for task, name, slug in order:
        d = os.path.join(la, "twins", slug)
        os.makedirs(d, exist_ok=False)
        p = {"kind": "P", "name": name, "slug": slug, "dir": d, "state": "todo", "compiled": threading.Event()}
        r = {"kind": "R", "name": name, "slug": slug, "dir": d, "state": "todo", "after": p}
        jobs += [p, r]
    lock = threading.Lock()
    rows = []
    t_origin = time.time()

    def cmd_of(job, node):
        pin = ["numactl", "--cpunodebind=%d" % node, "--membind=%d" % node]
        env = dict(os.environ)
        if job["kind"] == "P":
            env.update(AVANT_TASK_LIST=job["name"], AVANT_RESULTS_DIR=os.path.join(job["dir"], "prod"))
            return pin + ["timeout", "-k", "10", "2700", harness, "prod"], env, os.path.join(job["dir"], "prod.log")
        env.update(LR_TASKS="0", LR_TWIN_LIST=job["name"], LR_LIBRARY=lib)
        return (pin + ["timeout", "-k", "10", "2700", "sh", os.path.join(here, "lr-table.sh"), "avant", bindir, run_js,
                       os.path.join(job["dir"], "lr", "lr-avant.tsv")], env, os.path.join(job["dir"], "lr.log"))

    def run(job, node):
        os.makedirs(os.path.join(job["dir"], "lr"), exist_ok=True)
        cmd, env, log = cmd_of(job, node)
        t0 = time.time()
        with open(log, "wb") as lf:
            proc = subprocess.Popen(cmd, cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=lf, stderr=subprocess.STDOUT)
            if job["kind"] == "P":
                needle = ("] %s run (" % job["name"]).encode()
                while proc.poll() is None and not job["compiled"].is_set():
                    try:
                        if needle in open(log, "rb").read():
                            job["compiled"].set()
                    except OSError:
                        pass
                    time.sleep(0.2)
                job["compiled"].set()  # ended (a failed compile too): its R reads what is there
            rc = proc.wait()
        t1 = time.time()
        with lock:
            rows.append((job["kind"] + " " + job["name"], node, t0 - t_origin, t1 - t_origin, t1 - t0, rc))
        return rc

    def worker(node):
        while True:
            with lock:
                pick = None
                left = [j for j in jobs if j["state"] == "todo"]
                if not left:
                    return
                for j in left:
                    if j["kind"] == "P" or j["after"]["compiled"].is_set():
                        pick = j
                        break
                if pick:
                    pick["state"] = "running"
            if not pick:
                time.sleep(0.2)
                continue
            pick["rc"] = run(pick, node)
            with lock:
                pick["state"] = "done"

    threads = [threading.Thread(target=worker, args=(n,)) for n in nodes]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # The merge: the rows where the sequential passes put them.
    bad = [j for j in jobs if j.get("rc") != 0]
    pure = []
    os.makedirs(os.path.join(la, "raw"), exist_ok=True)
    praw = os.path.join(la, "lr-avant-pure.tsv.raw")
    os.makedirs(praw, exist_ok=True)
    header, lr_rows, missing = None, {}, []
    for task, name, slug in twins:
        d = os.path.join(la, "twins", slug)
        pj = os.path.join(d, "prod", "runtime-pure.json")
        got = [r for r in json.load(open(pj)) if r.get("name") == name] if os.path.exists(pj) else []
        if got:
            pure.append(got[0])
        else:
            missing.append("%s (prod)" % name)
        rf = os.path.join(d, "prod", "raw", slug + ".txt")
        if os.path.exists(rf):
            shutil.copy(rf, os.path.join(la, "raw", slug + ".txt"))
        lp = os.path.join(d, "lr", "lr-avant-pure.tsv")
        found = False
        if os.path.exists(lp):
            lines = open(lp).read().splitlines()
            if header is None:
                header = [l for l in lines if l.startswith("#") or l.startswith("task\t")]
            for l in lines:
                if l.split("\t")[0] == name:
                    lr_rows[name] = l
                    found = True
            rd = lp + ".raw"
            if os.path.isdir(rd):
                for f in os.listdir(rd):
                    if f.startswith(slug + ".") and not os.path.isdir(os.path.join(rd, f)):
                        shutil.copy(os.path.join(rd, f), os.path.join(praw, f))
        if not found:
            missing.append("%s (LR)" % name)
    with open(os.path.join(la, "runtime-pure.json"), "w") as f:
        json.dump(pure, f, indent=2)
        f.write("\n")
    with open(os.path.join(la, "lr-avant-pure.tsv"), "w") as f:
        for l in header or []:
            f.write(l + "\n")
        for task, name, slug in twins:
            if name in lr_rows:
                f.write(lr_rows[name] + "\n")
    rows.sort(key=lambda r: r[2])
    with open(os.path.join(la, "twins.tsv"), "w") as f:
        f.write("job\tnode\tstart_s\tend_s\tseconds\tstatus\n")
        for r in rows:
            f.write("%s\t%d\t%.1f\t%.1f\t%.1f\t%d\n" % r)
    wall = max(r[3] for r in rows) if rows else 0.0
    busy = sum(r[4] for r in rows)
    print("la-twins: %d twins, %d jobs on nodes %s in %.1f s (%.1f s of runs: %.1f s saved against one after another)%s"
          % (len(twins), len(rows), ",".join(map(str, nodes)), wall, busy, busy - wall,
             "; failed: " + ", ".join(j["kind"] + " " + j["name"] for j in bad) if bad else "")
          + ("; missing: " + ", ".join(missing) if missing else ""))
    return 1 if bad or missing else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(0)
    sys.exit(main(sys.argv[1:]))
