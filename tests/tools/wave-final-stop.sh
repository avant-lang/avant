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
# `timeout`, it must leave nothing either; a fifth the same with
# WAVE_GROUP=1 (the shared steps one runner call); a sixth whose step has a
# process that left its tree (reparented away) before the TERM; a seventh
# through the runner with a `ps` that fails, so no listing of the tree finds
# the step's processes: both are found by the run's mark in their
# environment. An eighth run has a step that ignores TERM, under a `sleep`
# ten times slower than asked (as on a machine so loaded that each wait of
# the stop takes several times its nominal time): the stop's waits are
# measured in elapsed time, so it still ends within the bound. Prints one
# line per check,
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
# WF_STUB_IGNORE=1: the step and everything it starts ignore TERM, so only
# the stop's KILL ends them.
if [ "${WF_STUB_IGNORE:-0}" = 1 ]; then trap '' TERM; fi
# WF_STUB_ESCAPE=1: a process that leaves the step's tree (its parent exits,
# so it is reparented away), found only by the run's mark.
if [ "${WF_STUB_ESCAPE:-0}" = 1 ]; then sh -c 'setsid sleep 600 & echo $! > "$1/escaped.pid"' escape "$d"; fi
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
# The run snapshots the real /tmp (WAVE_PRIVATE_TMP=0): the stub step's
# PID files, under WORK, may be under /tmp (inside the gate's private
# /tmp), which a private /tmp of the run's own would hide from this script.
# Start wave-final.sh under an outer timeout of LIMIT seconds; $tp is that
# timeout's PID. In an emptied environment: run inside a real final
# evidence (the gate's suite under wave-final.sh), the caller's WAVE_LA,
# WAVE_LA_SIDE and GATE_* would otherwise reach the fake run, whose
# LangArena step would then run in the real checkout. The steps run in
# turn (WAVE_BESIDE=0): the fake checkout is no git checkout to copy.
start() { # NAME LIMIT [VAR=VALUE...]
  n=$1; l=$2; shift 2
  mkdir -p "$w/$n.stub"
  env -i PATH="$PATH" HOME="$HOME" WAVE_BESIDE=0 "$@" WF_STUB="$w/$n.stub" WAVE_PRIVATE_TMP=0 timeout -k 10 "$l" sh "$fake/tools/wave-final.sh" "$w/$n.out" < /dev/null > "$w/$n.log" 2>&1 &
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

# 3. The outer timeout fires during the goldens step: its limit (30 s) is far
# longer than the script needs to reach that step, also on a machine so loaded
# that a start of 4 s ended before the step began.
start expire 30
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

# 5. The same with WAVE_GROUP=1: the shared steps are one runner call
# whose command is the script itself; a TERM leaves nothing either, and the
# step stopped is named.
start group 300 WAVE_RUNNER="sh $w/runner" WAVE_GROUP=1
if started group; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp
  t1=$(date +%s)
  l=$(left "$w/group.stub" | tr '\n' ' ')
  [ -z "$l" ] && [ $((t1 - t0)) -lt 10 ]; say group-term-leaves-nothing $? "left: $l; $((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during shared steps; every process of the step stopped$' "$w/group.out/SUMMARY.txt"
  say group-term-summary $? "$(tr '\n' ';' < "$w/group.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say group-term-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/group.stub"

# 6. A process that left the step's tree before the TERM (reparented away
# from it, which no listing of the tree finds): the run's mark finds it.
start escape 300 WF_STUB_ESCAPE=1
if started escape; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp
  t1=$(date +%s)
  l=$(left "$w/escape.stub" | tr '\n' ' ')
  [ -s "$w/escape.stub/escaped.pid" ] && [ -z "$l" ] && [ $((t1 - t0)) -lt 10 ]; say escape-leaves-nothing $? "left: $l; $((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during goldens debug; every process of the step stopped$' "$w/escape.out/SUMMARY.txt"
  say escape-summary $? "$(tail -1 "$w/escape.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say escape-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/escape.stub"

# 7. Through the runner with a `ps` that fails (lists nothing, status 1,
# as an overloaded machine's may): the listing of the tree is empty, and
# the run's mark still finds every process of the step.
mkdir -p "$w/failing-ps"
printf '#!/bin/sh\nexit 1\n' > "$w/failing-ps/ps"; chmod +x "$w/failing-ps/ps"
start psfail 300 WAVE_RUNNER="sh $w/runner" PATH="$w/failing-ps:$PATH"
if started psfail; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp
  t1=$(date +%s)
  l=$(left "$w/psfail.stub" | tr '\n' ' ')
  [ -z "$l" ] && [ $((t1 - t0)) -lt 10 ]; say psfail-leaves-nothing $? "left: $l; $((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during goldens debug; every process of the step stopped$' "$w/psfail.out/SUMMARY.txt"
  say psfail-summary $? "$(tail -1 "$w/psfail.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say psfail-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/psfail.stub"

# 8. A step that ignores TERM, with a slow `sleep`: the stop's grace ends by
# the clock, not after a count of rounds (fifty slow rounds would take
# minutes), and the stop ends within the bound with nothing left.
mkdir -p "$w/slow-sleep"
real_sleep=$(command -v sleep)
printf '#!/bin/sh\ncase "$1" in 0.*) exec %s 1 ;; *) exec %s "$@" ;; esac\n' "$real_sleep" "$real_sleep" > "$w/slow-sleep/sleep"; chmod +x "$w/slow-sleep/sleep"
start slow 300 WF_STUB_IGNORE=1 PATH="$w/slow-sleep:$PATH"
if started slow; then
  t0=$(date +%s)
  kill -TERM $tp
  wait $tp
  t1=$(date +%s)
  l=$(left "$w/slow.stub" | tr '\n' ' ')
  [ -z "$l" ] && [ $((t1 - t0)) -lt 10 ]; say slow-leaves-nothing $? "left: $l; $((t1 - t0)) s"
  grep -q '^stopped: SIGTERM during goldens debug; every process of the step stopped$' "$w/slow.out/SUMMARY.txt"
  say slow-summary $? "$(tail -1 "$w/slow.out/SUMMARY.txt" 2>/dev/null)"
else
  kill -TERM $tp; wait $tp
  say slow-leaves-nothing 1 "the stub step did not start"
fi
reap "$w/slow.stub"
echo done
