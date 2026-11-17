#!/bin/sh
# Stopping tools/wave-final.sh stops every process its steps started.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/wave-final-stop.sh WORK
#
# WORK is a new scratch directory. The real wave-final.sh runs in a fake
# checkout (WORK/side/avant) whose tools/gate.sh passes at once and whose
# tools/run-goldens.sh is a stub step shaped like the real one: a
# `timeout` (a process group of its own) whose shell starts a child in a
# session of its own and another child, each recording its PID. Three runs:
# one left to end (the statuses still reach SUMMARY.txt), one stopped by a
# TERM to its outer `timeout` during the goldens step (as by hand), one
# stopped by its outer `timeout` firing. A stopped run must leave no
# process: neither a recorded PID nor any process whose working directory
# is the fake checkout, at the moment the outer `timeout` returns (when
# `ledger run` would release the machine lock). A fourth run goes through
# WAVE_RUNNER, a stub runner that runs its command in its place (as a
# runner that does not stop its own tree): stopped by a TERM to its outer
# `timeout`, it must leave nothing either. Prints one line per check,
# `ok: NAME` or `FAIL: NAME: why`; test_tools.av expects each NAME.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/wave-final-stop.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
fake=$w/side/avant
mkdir -p "$fake/tools" "$fake/langarena/harness" "$w/side/LangArena"
: > "$fake/langarena/harness/measure.cr"
cp "$root/tools/wave-final.sh" "$fake/tools/wave-final.sh"
printf '#!/bin/sh\necho "GATE PASS (stub)"\n' > "$fake/tools/gate.sh"
cat > "$fake/tools/run-goldens.sh" <<'EOF'
#!/bin/sh
# The stub goldens step: WF_STUB names the directory for the PIDs.
d=$WF_STUB
if [ "${WF_STUB_QUICK:-0}" = 1 ]; then echo "1 same, 0 different, 0 skipped"; exit 0; fi
echo $$ > "$d/step.pid"
timeout -k 5 600 sh -c 'd=$1; echo $$ > "$d/inner.pid"; setsid sleep 600 & echo $! > "$d/session.pid"; sleep 600 & echo $! > "$d/child.pid"; wait' inner "$d" &
echo $! > "$d/timeout.pid"
while [ ! -s "$d/session.pid" ] || [ ! -s "$d/child.pid" ]; do sleep 0.05; done
echo started > "$d/started"
wait
EOF

say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
alive() { s=$(ps -o stat= -p "$1" 2>/dev/null) || return 1; case "$s" in Z*|"") return 1 ;; esac; return 0; }
# The processes whose working directory is the fake checkout (or below it).
in_fake() {
  for p in /proc/[0-9]*; do
    c=$(readlink "$p/cwd" 2>/dev/null) || continue
    case "$c/" in "$fake"/*) echo "${p#/proc/}" ;; esac
  done
}
# What a run left: the recorded PIDs still alive, then any process in the
# fake checkout.
left() { # STUBDIR
  for f in "$1"/*.pid; do
    [ -s "$f" ] || continue
    p=$(cat "$f"); alive "$p" && echo "$(basename "$f" .pid)=$p"
  done
  for p in $(in_fake); do alive "$p" && echo "cwd=$p"; done
}
# Cleanup after a failed check: only processes this script started, found
# by their PID files and by their working directory in the fake checkout.
reap() { # STUBDIR
  for f in "$1"/*.pid; do
    [ -s "$f" ] || continue
    p=$(cat "$f")
    case "$(readlink "/proc/$p/cwd" 2>/dev/null)/" in "$fake"/*) kill -KILL "$p" 2>/dev/null ;; esac
  done
  for p in $(in_fake); do [ "$p" != $$ ] && kill -KILL "$p" 2>/dev/null; done
}
# Start wave-final.sh under an outer timeout of LIMIT seconds; $tp is that
# timeout's PID. In an emptied environment: run inside a real final
# evidence (the gate's suite under wave-final.sh), the caller's WAVE_LA,
# WAVE_LA_SIDE and GATE_* would otherwise reach the fake run, whose
# LangArena step would then run in the real checkout.
start() { # NAME LIMIT [VAR=VALUE...]
  n=$1; l=$2; shift 2
  mkdir -p "$w/$n.stub"
  env -i PATH="$PATH" HOME="$HOME" "$@" WF_STUB="$w/$n.stub" timeout -k 10 "$l" sh "$fake/tools/wave-final.sh" "$w/$n.out" < /dev/null > "$w/$n.log" 2>&1 &
  tp=$!
}
started() { # NAME: wait up to 60 s for the stub step to start
  i=0
  while [ ! -e "$w/$1.stub/started" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
  [ -e "$w/$1.stub/started" ]
}

# 1. A run left to end: the steps' statuses reach SUMMARY.txt as before.
start ends 300 WF_STUB_QUICK=1
wait $tp; rc=$?
sm=$w/ends.out/SUMMARY.txt
[ $rc -eq 1 ] && grep -q '^gate: pass' "$sm" && grep -q '^WAVE FINAL RED' "$sm" && ! grep -q '^stopped:' "$sm"
say run-ends $? "status $rc; $(tr '\n' ';' < "$sm" 2>/dev/null | head -c 400)"
grep -q '^debug: rc=0 1 same' "$w/ends.out/goldens.txt" 2>/dev/null; say step-status $? "$(tr '\n' ';' < "$w/ends.out/goldens.txt" 2>/dev/null)"

# 2. A TERM to the outer timeout during the goldens step.
start term 300
if started term; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp; rc=$?
  t1=$(date +%s)
  l=$(left "$w/term.stub" | tr '\n' ' ')
  [ -z "$l" ]; say term-leaves-nothing $? "left: $l"
  [ $rc -eq 124 ] || [ $rc -eq 143 ]; say term-status $? "status $rc"
  [ $((t1 - t0)) -lt 10 ]; say term-within-kill-after $? "$((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during goldens debug; every process of the step stopped$' "$w/term.out/SUMMARY.txt"
  say term-summary $? "$(tail -1 "$w/term.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say term-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/term.stub"

# 3. The outer timeout fires during the goldens step.
start expire 4
started expire; st=$?
wait $tp; rc=$?
l=$(left "$w/expire.stub" | tr '\n' ' ')
[ $st -eq 0 ] && [ -z "$l" ]; say expire-leaves-nothing $? "started=$st; left: $l"
[ $rc -eq 124 ]; say expire-status $? "status $rc"
grep -q '^stopped: SIGTERM during goldens debug' "$w/expire.out/SUMMARY.txt"
say expire-summary $? "$(tail -1 "$w/expire.out/SUMMARY.txt" 2>/dev/null)"
reap "$w/expire.stub"

# 4. Through a runner, a TERM to the outer timeout during the goldens step.
printf '#!/bin/sh\nwhile [ $# -gt 0 ] && [ "$1" != -- ]; do shift; done\nshift\nexec "$@"\n' > "$w/runner"
start runner 300 WAVE_RUNNER="sh $w/runner"
if started runner; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp
  t1=$(date +%s)
  l=$(left "$w/runner.stub" | tr '\n' ' ')
  [ -z "$l" ] && [ $((t1 - t0)) -lt 10 ]; say runner-term-leaves-nothing $? "left: $l; $((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during goldens debug; every process of the step stopped$' "$w/runner.out/SUMMARY.txt"
  say runner-term-summary $? "$(tail -1 "$w/runner.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say runner-term-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/runner.stub"
echo done
