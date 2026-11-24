#!/bin/sh
# tools/ci-steps.py on two made-up runs of one job, the second on a runner
# half as fast: every step takes twice as long, so the units columns agree.
#   ci-units-equal: the same step reads the same units in both runs;
#   ci-ref-given: a reference given on the command line is used and named;
#   ci-ref-step: without one, the whole re2c step is the reference, named.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/ci-steps.sh WORK
#
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/ci-steps.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1"); mkdir -p "$w"
job() { # FILE SCALE
  python3 -I - "$1" "$2" <<'PY'
import json, sys
k = int(sys.argv[2])
t = lambda s: "2026-01-01T00:%02d:%02dZ" % divmod(s, 60)
steps, at = [], 0
for name, s in (("Build the bundled re2c", 20), ("Native suite on C'", 150)):
    steps.append({"name": name, "started_at": t(at), "completed_at": t(at + s * k)})
    at += s * k
json.dump({"jobs": [{"name": "seed", "steps": steps}]}, open(sys.argv[1], "w"))
PY
}
job "$w/fast.json" 1; job "$w/slow.json" 2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
o=$(python3 -I "$root/tools/ci-steps.py" "$w/fast.json=15" "$w/slow.json=30" < /dev/null 2>&1)
echo "$o" | grep -qF "| Native suite on C' | 150 | 10.00 | 300 | 10.00 |"; say ci-units-equal $? "$o"
echo "$o" | grep -qF "fast.json: reference 15.00 s (re2c make)"; say ci-ref-given $? "$o"
o=$(python3 -I "$root/tools/ci-steps.py" "$w/fast.json" < /dev/null 2>&1)
echo "$o" | grep -qF "reference 20.00 s (the whole re2c step)" && echo "$o" | grep -qF "| Native suite on C' | 150 | 7.50 |"; say ci-ref-step $? "$o"
echo done
