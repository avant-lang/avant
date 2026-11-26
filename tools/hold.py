#!/usr/bin/env python3
"""One whole-machine hold: chunks pinned to NUMA nodes, longest first, then
the steps that need the whole machine, in one run.

  python3 tools/hold.py PLAN [--nodes 0,1,2,3] [--walls PREVIOUS]... [--log WALLS]
                             [--pin numactl|none] [--dry-run]

Run inside one `ledger run --timed` (the machine held alone):
everything a wave times then goes through one turnstile, so no shared run
slips in between the chains, Matmul::T16's whole-machine run and the
suite's walls, and the hold ends when its last step ends.

PLAN, one step a line (# comments and blank lines skipped):

  chunk NAME [SECONDS] -- COMMAND    a pinned chunk (sh -c COMMAND)
  after NAME -- COMMAND              a whole-machine step, after every chunk

Chunks: each node runs one chunk at a time, pinned as `ledger run --timed
--slot` pins (numactl --cpunodebind=N --membind=N, LEDGER_NODE=N in its
environment); a free node takes the longest chunk still waiting, so the
placement is longest-processing-time first, made as the hold goes. A
chunk's length is its wall in the newest --walls file that names it (a
WALLS file an earlier hold, or an integration's chains, wrote), else the
SECONDS the plan gives, else 0 (such chunks go last, in plan order). The
after steps run in plan order, unpinned, once every chunk has ended.

--log WALLS appends one line per step as it ends, `NAME node=N wall=Ss
rc=R` (an after step: node=all), the form --walls reads. --pin none runs
the chunks unpinned (a machine without numactl, the tests). --dry-run
prints the order the chunks are taken in, with each one's length and
where it came from, and the longest-first estimate of the hold's length.
Exit status 0 when every step ended 0, 1 when one did not (the others
still run), 2 on a usage error; a TERM stops every running step's process
group and exits 143.
"""
import os
import re
import signal
import subprocess
import sys
import threading
import time


def die(msg):
    sys.stderr.write("hold.py: %s\n" % msg)
    sys.exit(2)


def read_plan(path):
    chunks, after = [], []
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        head, sep, cmd = line.partition(" -- ")
        w = head.split()
        if not sep or not cmd.strip() or len(w) < 2 or w[0] not in ("chunk", "after"):
            die("%s:%d: not `chunk NAME [SECONDS] -- COMMAND` or `after NAME -- COMMAND`" % (path, n))
        if w[0] == "chunk":
            if len(w) > 3 or (len(w) == 3 and not re.fullmatch(r"[0-9]+(\.[0-9]+)?", w[2])):
                die("%s:%d: a chunk's length is a number of seconds" % (path, n))
            chunks.append({"name": w[1], "plan": float(w[2]) if len(w) == 3 else None, "cmd": cmd, "order": len(chunks)})
        else:
            if len(w) != 2:
                die("%s:%d: an after step takes a name only" % (path, n))
            after.append({"name": w[1], "cmd": cmd})
    names = [c["name"] for c in chunks] + [a["name"] for a in after]
    dup = sorted({x for x in names if names.count(x) > 1})
    if dup:
        die("%s: a name twice: %s" % (path, " ".join(dup)))
    return chunks, after


def read_walls(paths):
    """{name: seconds}: the newest file that names a step wins."""
    est = {}
    for p in paths:
        for line in open(p):
            m = re.match(r"(.*?) node=\S+ wall=([0-9.]+)s", line.strip())
            if m:
                name = m.group(1).split()[-1]
                est[name] = float(m.group(2))
    return est


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if argv else 2
    plan, nodes, walls, log, pin, dry = None, [0, 1, 2, 3], [], None, "numactl", False
    a = list(argv)
    while a:
        x = a.pop(0)
        if x in ("--nodes", "--walls", "--log", "--pin"):
            if not a:
                die("%s needs a value" % x)
            v = a.pop(0)
            if x == "--nodes":
                try:
                    nodes = [int(n) for n in v.split(",") if n.strip()]
                except ValueError:
                    die("--nodes takes node numbers, comma-separated")
            elif x == "--walls":
                walls.append(v)
            elif x == "--log":
                log = v
            else:
                if v not in ("numactl", "none"):
                    die("--pin is numactl or none")
                pin = v
        elif x == "--dry-run":
            dry = True
        elif x.startswith("-"):
            die("unknown option %s" % x)
        elif plan is None:
            plan = x
        else:
            die("one PLAN")
    if not plan or not nodes:
        die("usage: hold.py PLAN [--nodes 0,1,2,3] [--walls PREVIOUS]... [--log WALLS] [--pin numactl|none] [--dry-run]")
    chunks, after = read_plan(plan)
    est = read_walls(walls)
    for c in chunks:
        if c["name"] in est:
            c["len"], c["from"] = est[c["name"]], "walls"
        elif c["plan"] is not None:
            c["len"], c["from"] = c["plan"], "plan"
        else:
            c["len"], c["from"] = 0.0, "unknown"
    queue = sorted(chunks, key=lambda c: (-c["len"], c["order"]))
    load = {n: 0.0 for n in nodes}
    for c in queue:
        n = min(load, key=lambda k: (load[k], nodes.index(k)))
        load[n] += c["len"]
    if dry:
        for c in queue:
            print("%s\t%.0f\t%s" % (c["name"], c["len"], c["from"]))
        print("estimate: chunks %.0f s on %d nodes (longest first), then %d after step%s" % (
            max(load.values()) if load else 0, len(nodes), len(after), "" if len(after) == 1 else "s"))
        return 0

    lock = threading.Lock()
    running, failed = {}, []
    stopping = threading.Event()

    def note(name, where, wall, rc):
        if log:
            with lock, open(log, "a") as f:
                f.write("%s node=%s wall=%ds rc=%d\n" % (name, where, round(wall), rc))
        if rc:
            failed.append(name)

    def start(cmd, node):
        env = dict(os.environ)
        argv = ["sh", "-c", cmd]
        if node is not None:
            env["LEDGER_NODE"] = str(node)
            if pin == "numactl":
                argv = ["numactl", "--cpunodebind=%d" % node, "--membind=%d" % node] + argv
        return subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, start_new_session=True)

    def worker(node):
        while not stopping.is_set():
            with lock:
                if not queue:
                    return
                c = queue.pop(0)
            t0 = time.monotonic()
            p = start(c["cmd"], node)
            with lock:
                running[p.pid] = p
            rc = p.wait()
            with lock:
                running.pop(p.pid, None)
            note(c["name"], node, time.monotonic() - t0, rc)

    def on_term(sig, frm):
        stopping.set()
        with lock:
            ps = list(running.values())
        for p in ps:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
        sys.exit(143)

    signal.signal(signal.SIGTERM, on_term)
    ts = [threading.Thread(target=worker, args=(n,), daemon=True) for n in nodes]
    for t in ts:
        t.start()
    for t in ts:
        while t.is_alive():
            t.join(0.5)
    for s in after:
        t0 = time.monotonic()
        p = start(s["cmd"], None)
        with lock:
            running[p.pid] = p
        rc = p.wait()
        with lock:
            running.pop(p.pid, None)
        note(s["name"], "all", time.monotonic() - t0, rc)
    if failed:
        sys.stderr.write("hold.py: not 0: %s\n" % " ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
