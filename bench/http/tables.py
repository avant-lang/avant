#!/usr/bin/env python3
"""The HTTP harness's tables, regenerated from the raw per-run files.

Usage: tables.py OUT_DIR [--label TEXT]

OUT_DIR is run.sh's HTTP_BENCH_OUT: <METHOD>/r<round>.<row>.{meta,wrk,time}
written by `METHOD=... run.sh`, check/<row>.json and .stop by `run.sh
check`, syscalls/<METHOD>.<row>.strace* by `run.sh syscalls`, and
spread/<row>.txt (spread.py's output), each optional. Markdown goes to
stdout: one table per method with the method printed beside it (median,
minimum and maximum over the valid runs; a run with a socket error, a
timeout, a non-2xx answer or a failed check is not a result and is listed
with its reason), Avant's collector figures per method, the correctness
table, the system calls per request and the connection spread. --label
adds a line under every method's title (for example "disturbed, not
evidence" for a dry run).
"""
import glob
import json
import os
import re
import statistics
import sys

ROW_ORDER = ["avant", "avant-nosigpipe", "h2o", "rust", "hyper", "hyper-nodate", "go", "fasthttp", "fasthttp-nodate", "crystal", "valk"]
ROW_NAME = {
    "avant": "Avant `std.http`",
    "h2o": "C (h2o)",
    "rust": "Rust std, thread per connection",
    "hyper": "Rust hyper (tokio)",
    "go": "Go `net/http`",
    "fasthttp": "Go fasthttp",
    "crystal": "Crystal `HTTP::Server`",
    "valk": "Valk `http.serve_fast`",
    "avant-nosigpipe": "Avant, variant: started with SIGPIPE ignored",
    "hyper-nodate": "Rust hyper, variant: no `Date`",
    "fasthttp-nodate": "Go fasthttp, variant: no `Date`, no `Server`",
}
METHOD_ORDER = ["A", "A8", "B", "C", "Q9"]
METHOD_TITLE = {
    "A": "Method A: 64 connections, no pipelining, 4 load threads",
    "A8": "Method A8: 256 connections, no pipelining, 8 load threads",
    "B": "Method B: 900 connections, 16 pipelined requests, 8 load threads",
    "C": "Method C: B with the server limited to 4 pinned threads",
    "Q9": "Method Q9: 900 connections, no pipelining, 8 load threads",
}
UNIT_US = {"us": 1.0, "ms": 1e3, "s": 1e6, "m": 6e7, "h": 3.6e9}


def read_kv(path):
    out = {}
    try:
        with open(path, errors="replace") as f:
            for line in f:
                key, sep, value = line.rstrip("\n").partition("=")
                if sep:
                    out[key] = value
    except OSError:
        pass
    return out


def to_us(text):
    m = re.match(r"([0-9.]+)([a-z]+)$", text)
    if not m or m.group(2) not in UNIT_US:
        return None
    return float(m.group(1)) * UNIT_US[m.group(2)]


def parse_wrk(path):
    """wrk's printed result: requests, seconds, req/s, percentiles (in
    microseconds), socket errors, non-2xx answers."""
    out = {"errors": []}
    try:
        with open(path, errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(r"^\s*(\d+) requests in ([0-9.]+)([a-z]+),", text, re.M)
    if m:
        out["requests"] = int(m.group(1))
        out["seconds"] = to_us(m.group(2) + m.group(3)) / 1e6
    m = re.search(r"^Requests/sec:\s*([0-9.]+)", text, re.M)
    if m:
        out["rps"] = float(m.group(1))
    for pct in ("50", "75", "90", "99"):
        m = re.search(r"^\s*%s%%\s+(\S+)" % pct, text, re.M)
        if m:
            out["p" + pct] = to_us(m.group(1))
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
    if m:
        names = ("connect", "read", "write", "timeout")
        found = ["%s %s" % (n, v) for n, v in zip(names, m.groups()) if int(v)]
        if found:
            out["errors"].append("socket errors: " + ", ".join(found))
    m = re.search(r"Non-2xx or 3xx responses: (\d+)", text)
    if m and int(m.group(1)):
        out["errors"].append("non-2xx answers: " + m.group(1))
    return out


def fmt_us(us):
    if us is None:
        return "n/a"
    if us == 0:
        return "0 (wrk printed 0.00us)"
    if us < 1000:
        return "%.0f µs" % us
    if us < 1e6:
        return "%.2f ms" % (us / 1e3)
    return "%.2f s" % (us / 1e6)


def med_range(values, fmt):
    if not values:
        return "no valid run"
    return "%s (%s–%s)" % (fmt(statistics.median(values)), fmt(min(values)), fmt(max(values)))


def gc_fields(line):
    return {k: v for k, v in (w.split("=", 1) for w in line.split() if "=" in w)}


def load_method(out_dir, method):
    runs = {}
    for meta_path in sorted(glob.glob(os.path.join(out_dir, method, "r*.meta"))):
        meta = read_kv(meta_path)
        row = meta.get("row")
        if not row:
            continue
        base = meta_path[:-len(".meta")]
        wrk = parse_wrk(base + ".wrk")
        warm = parse_wrk(base + ".warm")
        reasons = []
        if meta.get("status") != "ok":
            reasons.append("status " + meta.get("status", "?"))
        if meta.get("check") != "ok":
            reasons.append("check " + meta.get("check", "?"))
        if wrk is None or "rps" not in wrk:
            reasons.append("no wrk result")
        else:
            reasons.extend(wrk["errors"])
        runs.setdefault(row, []).append({"meta": meta, "wrk": wrk, "warm": warm, "bad": reasons, "file": os.path.basename(base)})
    return runs


def load_checks(out_dir):
    checks = {}
    for path in glob.glob(os.path.join(out_dir, "check", "*.json")):
        with open(path) as f:
            data = json.load(f)
        stop_path = path[:-len(".json")] + ".stop"
        try:
            with open(stop_path) as f:
                data["stop"] = f.read().strip()
        except OSError:
            data["stop"] = "not run"
        checks[data["row"]] = data
    return checks


def ordered(rows):
    return [r for r in ROW_ORDER if r in rows] + sorted(r for r in rows if r not in ROW_ORDER)


def cores(meta, key_ticks=True):
    try:
        elapsed = float(meta["wrk_elapsed"])
        if key_ticks:
            return float(meta["server_cpu_ticks"]) / float(meta["clk_tck"]) / elapsed
        return (float(meta["wrk_user"]) + float(meta["wrk_sys"])) / elapsed
    except (KeyError, ValueError, ZeroDivisionError):
        return None


def method_table(out_dir, method, checks, label):
    runs = load_method(out_dir, method)
    if not runs:
        return
    session = read_kv(os.path.join(out_dir, method, "session.meta"))
    print("## %s" % METHOD_TITLE.get(method, "Method " + method))
    print()
    if label:
        print("**%s**" % label)
        print()
    any_meta = next(iter(runs.values()))[0]["meta"]
    print("Load: `%s`. Server threads: %s; server CPUs: %s. Per row and round: a fresh server process, one request checked, a %s s warm-up with the same line, then the measured load. Session: %s, kernel %s, %s, governor `%s`, %s, %s rounds, row order rotating; load average at the start %s." % (
        re.sub(r"/\S*/(pipe\.lua)", r"\1", any_meta.get("wrk_cmd", "?")), any_meta.get("server_threads_arg", "?"), any_meta.get("server_cpus", "?"),
        any_meta.get("warmup_s", "?"), session.get("date", "?"), session.get("kernel", "?"), session.get("cpu", "?"),
        session.get("governor", "?"), session.get("wrk", "?"), session.get("runs", "?"), session.get("loadavg", "?")))
    print()
    print("| Server | Requests/s, median (min–max) | p50 | p99 | Peak RSS | Server CPU, cores (system share) | `wrk` CPU, cores | OS threads after the load | Answer bytes | `Date` | Valid runs |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |")
    notes = []
    best = 0.0
    medians = {}
    for row in ordered(runs):
        good = [r for r in runs[row] if not r["bad"]]
        for r in runs[row]:
            if r["bad"]:
                notes.append("`%s`: not a result (%s)" % (r["file"], "; ".join(r["bad"])))
            if r["meta"].get("alive_after_load") == "no":
                notes.append("`%s`: the server was no longer running after the load ended" % r["file"])
        rps = [r["wrk"]["rps"] for r in good]
        if rps:
            medians[row] = statistics.median(rps)
            best = max(best, medians[row])
        check = checks.get(row, {})
        srv = [c for c in (cores(r["meta"]) for r in good) if c is not None]
        sysshare = [float(r["meta"]["server_sys_ticks"]) / float(r["meta"]["server_cpu_ticks"]) for r in good
                    if r["meta"].get("server_sys_ticks") and float(r["meta"].get("server_cpu_ticks", 0)) > 0]
        gen = [c for c in (cores(r["meta"], False) for r in good) if c is not None]
        rss = [int(r["meta"].get("peak_kb", 0)) / 1024 for r in good]
        thr = sorted({r["meta"].get("server_os_threads", "?") for r in good})
        p50 = [r["wrk"].get("p50") for r in good if r["wrk"].get("p50") is not None]
        p99 = [r["wrk"].get("p99") for r in good if r["wrk"].get("p99") is not None]
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %d of %d |" % (
            ROW_NAME.get(row, row),
            med_range(rps, lambda v: "{:,.0f}".format(v)),
            fmt_us(statistics.median(p50)) if p50 else "n/a",
            fmt_us(statistics.median(p99)) if p99 else "n/a",
            ("%.1f MB" % statistics.median(rss)) if rss else "n/a",
            ("%.2f%s" % (statistics.median(srv), (" (%.0f%%)" % (100 * statistics.median(sysshare))) if sysshare else "")) if srv else "n/a",
            ("%.2f" % statistics.median(gen)) if gen else "n/a",
            "/".join(thr) if thr else "n/a",
            check.get("response_bytes", "?"),
            ("yes" if check.get("date") else "no") if check else "?",
            len(good), len(runs[row])))
    print()
    if medians:
        print("Relative to the fastest row's median (%s): %s." % (
            "{:,.0f}".format(best),
            "; ".join("%s %.3f" % (row, medians[row] / best) for row in ordered(medians))))
        print()
    for note in notes:
        print("- " + note)
    if notes:
        print()
    avant = [r for row in ordered(runs) if row.startswith("avant") for r in runs[row] if r["meta"].get("gc_stats")]
    if avant:
        print("Avant's collector during these runs (`AVANT_GC_STATS`, whole server life: the checked request, the warm-up and the measured load):")
        print()
        print("| Run | Requests answered (warm-up + measured) | Bytes allocated per request | Collections (young/full) | Requests per collection | Collections per second of load | Collection pauses, total (max) | Handshakes, total (max) |")
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for r in avant:
            g = gc_fields(r["meta"]["gc_stats"])
            reqs = sum(w["requests"] for w in (r["wrk"], r["warm"]) if w and "requests" in w)
            secs = sum(w["seconds"] for w in (r["wrk"], r["warm"]) if w and "seconds" in w)
            coll = int(g.get("collections", 0))
            pause = int(g.get("pause_young_ns", 0)) + int(g.get("pause_full_ns", 0))
            print("| `%s` | %s | %s | %s (%s/%s) | %s | %s | %.2f ms (%.1f µs) | %.2f ms (%.1f µs) |" % (
                r["file"], "{:,}".format(reqs),
                ("%.1f" % (int(g.get("alloc", 0)) / reqs)) if reqs else "n/a",
                "{:,}".format(coll), g.get("young", "?"), g.get("full", "?"),
                "{:,.0f}".format(reqs / coll) if coll else "n/a",
                ("%.1f" % (coll / secs)) if secs else "n/a",
                pause / 1e6, int(g.get("pause_max_ns", 0)) / 1e3,
                int(g.get("pause_hs_ns", 0)) / 1e6, int(g.get("pause_hs_max_ns", 0)) / 1e3))
        print()


def check_table(checks):
    if not checks:
        return
    print("## Correctness (`run.sh check`, untimed)")
    print()
    names = []
    for row in ordered(checks):
        for name in checks[row]["checks"]:
            if name not in names:
                names.append(name)
    order = ["single", "pipeline16", "pipeline16x200", "keepalive1000", "close_unread"]
    names.sort(key=lambda n: (order.index(n) if n in order else len(order), int(n[6:]) if n.startswith("split@") else 0))
    splits = [n for n in names if n.startswith("split@")]
    cols = [n for n in names if not n.startswith("split@")]
    if splits:
        cols.insert(cols.index("pipeline16") + 1 if "pipeline16" in cols else len(cols), "split")
    print("| Server | Answer bytes | `Date` | `Server` | Header names | %s | Stop |" % " | ".join("`%s`" % c for c in cols))
    print("| --- | ---: | --- | --- | --- | %s | --- |" % " | ".join("---" for _ in cols))
    failures = []
    for row in ordered(checks):
        c = checks[row]
        cells = []
        for col in cols:
            group = splits if col == "split" else [col]
            oks = [c["checks"].get(n, {}).get("ok") for n in group]
            if all(oks):
                cells.append("ok" if col != "split" else "ok (%d cuts)" % len(group))
            else:
                cells.append("**FAIL**")
                for n in group:
                    if not c["checks"].get(n, {}).get("ok"):
                        failures.append("%s `%s`: %s" % (ROW_NAME.get(row, row), n, c["checks"].get(n, {}).get("detail", "not run")))
        print("| %s | %s | %s | %s | %s | %s | %s |" % (
            ROW_NAME.get(row, row), c.get("response_bytes", "?"), "yes" if c.get("date") else "no",
            "yes" if c.get("server_header") else "no", ", ".join(c.get("headers", [])), " | ".join(cells), c.get("stop", "?")))
    print()
    if splits:
        print("`split`: the 16 pipelined requests sent in two writes cut at byte %s of %d." % (", ".join(s[len("split@"):] for s in splits), 16 * next(iter(checks.values())).get("request_bytes", 0)))
        print()
    for f in failures:
        print("- " + f)
    if failures:
        print()


RECV = ("recvfrom", "read", "recvmsg", "readv", "recvmmsg")
SEND = ("writev", "write", "sendto", "sendmsg", "sendmmsg")
WAIT = ("epoll_wait", "epoll_pwait", "epoll_pwait2")


def parse_strace(path):
    calls = {}
    try:
        with open(path, errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 5 and parts[0].replace(".", "").isdigit() and parts[-1] != "total":
                    try:
                        calls[parts[-1]] = int(parts[3])
                    except ValueError:
                        pass
    except OSError:
        return None
    return calls


def syscall_table(out_dir):
    files = sorted(glob.glob(os.path.join(out_dir, "syscalls", "*.strace")))
    if not files:
        return
    print("## System calls per request (`run.sh syscalls`: `strace -f -c` around the server, untimed)")
    print()
    print("The server runs many times slower under `strace`, so each wake-up finds more work than at full speed: the receive and send columns are what the server does per request or per batch; the waiting calls are a lower bound. Requests are the ones `wrk` completed (answers in flight when it stopped are not counted, at most connections × depth).")
    print()
    by_method = {}
    for path in files:
        method, row = os.path.basename(path)[:-len(".strace")].split(".", 1)
        by_method.setdefault(method, {})[row] = path
    for method in [m for m in METHOD_ORDER if m in by_method] + sorted(m for m in by_method if m not in METHOD_ORDER):
        rows = by_method[method]
        first = read_kv(next(iter(rows.values())) + ".meta")
        print("Load: `%s`." % re.sub(r"/\S*/(pipe\.lua)", r"\1", first.get("wrk_cmd", "?")))
        print()
        print("| Server | Requests | All calls | Calls per request | Receive per request | Send per request | `epoll_wait` per request | `io_uring_enter` per request | `futex` per request | Other per request | Largest counts |")
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
        for row in ordered(rows):
            calls = parse_strace(rows[row])
            wrk = parse_wrk(rows[row] + ".wrk")
            if not calls or not wrk or not wrk.get("requests"):
                print("| %s | no result | | | | | | | | | |" % ROW_NAME.get(row, row))
                continue
            n = wrk["requests"]
            total = sum(calls.values())
            recv = sum(calls.get(c, 0) for c in RECV)
            send = sum(calls.get(c, 0) for c in SEND)
            wait = sum(calls.get(c, 0) for c in WAIT)
            uring = calls.get("io_uring_enter", 0)
            futex = calls.get("futex", 0)
            other = total - recv - send - wait - uring - futex
            top = sorted(calls.items(), key=lambda kv: -kv[1])[:4]
            print("| %s | %s | %s | %.4f | %.4f | %.4f | %.4f | %.4f | %.4f | %.4f | %s |" % (
                ROW_NAME.get(row, row), "{:,}".format(n), "{:,}".format(total), total / n, recv / n, send / n,
                wait / n, uring / n, futex / n, other / n, ", ".join("`%s` %s" % (k, "{:,}".format(v)) for k, v in top)))
        print()


def spread_table(out_dir):
    files = sorted(glob.glob(os.path.join(out_dir, "spread", "*.txt")))
    if not files:
        return
    print("## Connections per event loop (`spread.py`: the members of each loop's epoll set, read from `/proc`)")
    print()
    print("| Server | Connections | Trial | Loops | Largest | Smallest | Empty loops | Largest / even share | Per loop |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")
    for path in files:
        row = os.path.basename(path)[:-len(".txt")]
        with open(path) as f:
            for line in f:
                kv = dict(w.split("=", 1) for w in line.split() if "=" in w)
                if "per_loop" not in kv:
                    continue
                print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                    ROW_NAME.get(row, row), kv["connections"], kv["trial"], kv["loops"], kv["max"], kv["min"],
                    kv["empty"], kv["max/ideal"], kv["per_loop"].replace(",", " ")))
    print()


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    out_dir = args[0]
    label = ""
    if len(args) >= 3 and args[1] == "--label":
        label = args[2]
    checks = load_checks(out_dir)
    methods = [m for m in METHOD_ORDER if os.path.isdir(os.path.join(out_dir, m))]
    for method in methods:
        method_table(out_dir, method, checks, label)
    check_table(checks)
    syscall_table(out_dir)
    spread_table(out_dir)


if __name__ == "__main__":
    main()
