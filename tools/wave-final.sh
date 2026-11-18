#!/bin/sh
# A wave's final evidence in one run (owner, 2026-11-09: fewer
# hand-written end-of-wave scripts, the same checks). Run it last, on the
# final committed head with a clean tree, in the background:
#
#   sh tools/wave-final.sh OUT [BASE_STATS]      (from the checkout root)
#
# The machine lock: with WAVE_RUNNER (the runner's command words, such as
# `python3 -I PATH/ledger run`), each step that builds or runs a program
# goes through it, every one on the lock's shared side but the LangArena
# timing (step 6's prod pass and LR re-measure), the only one that holds
# the whole machine (`--timed`): the gate, the goldens, the statistics,
# the smoke test and the neutrality are verdicts that do not depend on
# time, so they run beside other work. Each run is named `final: STEP`
# (the runner's --step) with WAVE_RUN_KIND as its --kind (default new);
# the runner's log and wave come from its own environment. Started this
# way the script itself is not run under the runner. Without WAVE_RUNNER
# every step runs as it is: the caller holds the lock (the whole run under
# one `--timed` run, as before). SUMMARY.txt's `lock:` line says which,
# and the LangArena line how long the whole machine was held. A run started
# inside an exclusive `ledger run` (the old form: LEDGER_LOCK_HELD, which
# the runner exports to its command, names an exclusive lock) holds the
# whole machine for every step whatever WAVE_RUNNER says: the `lock:` line
# and the script's first line on stdout say so.
#
# WAVE_GROUP=1 (with WAVE_RUNNER): the shared steps from 1b to 5c run in one
# runner call, `final: shared steps` (the script calling itself with
# WAVE_PART=shared), instead of one call each: the lock is taken once, so
# another lane's whole-machine run that arrives meanwhile waits for the
# whole shared part rather than going first at the next step (the lock's
# turnstile). One lane's wall shrinks by those waits; the other lane's
# grows by as much. SUMMARY.txt gains a `shared steps:` line with that
# call's wall and wait; the steps' own walls then have no wait beside them.
#
# OUT is a new directory under results/ or outside the repository. Steps, in
# order, each logged under OUT; a red step is reported and the later steps
# still run, so one run gives the whole picture:
#
#   1. head.txt (HEAD, its tree and `git status --short`, which must be
#      empty; red outside a git checkout of its own), governor.txt,
#      tmp-before.txt
#   1b. cases-check: every tests/cases/test_*.av through C''s __emit-ll
#      (tools/cases-check.sh, under a minute): OUT/cases, cases.log; red
#      with the first refused file and its message, the line printed at
#      once on stdout too; with WAVE_CASES_STOP=1 a red one ends the run
#      there (the gate's suites would only say it again, later)
#   2. the gate (tools/gate.sh) with the standard GC list (overlapping
#      two at a time, the gate's default): OUT/gate, gate.out, gate-wall.txt
#   3. letters.txt: sha256 of B, C', D' and avant_rt.o; nm.txt: the global
#      symbols of avant_rt.o against runtime/avant_rt.exports
#   4. the run goldens with C' in --debug, bare and --release, and once
#      under the full AVANT_GC_STRESS=1 (--debug): OUT/goldens-*, goldens.txt;
#      red unless the checkout is laid out for the harness: its directory is
#      named avant, LangArena/ is its sibling and the untracked langarena/
#      harness is copied in (create a worktree as SCRATCH/side/avant beside
#      SCRATCH/side/LangArena), or the 50 LangArena goldens are skipped
#   5. the GC-statistics set (tools/gcstats.sh: the three churn fixtures
#      in three modes, under the default environment, AVANT_NURSERY=0,
#      AVANT_GC_VERIFY=1 and AVANT_YOUNG_MB=1, AVANT_GC_STATS=1: 36 .out
#      and 36 .err files): OUT/gcstats; with BASE_STATS (an earlier run's
#      OUT/gcstats), else the base cache's set when WAVE_BASE is given
#      (of the merge base, as 5c),
#      stats.txt compares every file with the pause_*_ns fields removed
#   5b. the smoke test: tools/package.sh packs C' (bin/avant-c-llvm, with
#      bin/re2c) into OUT/smoke, tools/smoke.sh runs the tarball's cases:
#      smoke.log; red on a failed case
#   5c. only with WAVE_BASE (a revision, such as the branch the wave starts
#      from): the neutrality, tools/neutral.sh with the base cache's entry
#      of the base (tools/base-cache.sh, built when missing) as the base,
#      the files changed since the base (`git diff --name-only`) as
#      NEUTRAL_CHANGED: OUT/neutral; a program outside them that moved is
#      named in the line (a difference is explained in the record, not
#      red). The base is where HEAD left WAVE_BASE (`git merge-base`): a
#      branch that moved on since is not this head's base.
#   6. only with WAVE_LA=1: LangArena prod once with C' (the harness binary
#      of tools/la-harness.sh, built once per checkout), then the LR table
#      (tools/lr-table.sh), OUT/la; needs WAVE_LA_SIDE (a directory holding
#      this checkout's copy as avant/ and LangArena/ side by side, the layout
#      the harness expects) and this machine's LR reference (the results
#      directory the other languages were measured into): WAVE_LR_REFERENCE,
#      else the directory named on the first line of WAVE_LR_DEFAULT
#      (default ~/.config/avant/lr-reference); red when its machine.txt is
#      missing or names another CPU or thread count than this machine's.
#      The LR re-measure runs every task under the undisturbed rule
#      (tools/lr-table.sh); a row still disturbed after its retries is
#      named in the step's line
#      and the floor (tools/floor.py): floor.md beside SUMMARY.txt,
#      with the base's floor beside it when WAVE_FLOOR_BASE names a base
#      la/table.md, else the la/table.md beside BASE_STATS when there is one
#      (its column headed by WAVE_FLOOR_BASE_LABEL, else by its path)
#   6b. with WAVE_LA=1 and WAVE_BASE, when the floor's base table is of the
#      base cache entry's tree: the head's instruction counts of the base
#      cache's stitched programs (tools/la-count.sh, shared), and the tasks
#      whose time moved beyond 3% and 0.02 s at counts within 0.1% named
#      "layout?" (tools/layout-flag.py): a `layout:` line, la/layout.tsv and
#      a "Layout?" section in floor.md (tools/layout-retime.sh is the
#      experiment that tells)
#   7. tmp-after.txt (red on an entry this run left in /tmp, tmp-new.txt;
#      an entry another program removed meanwhile is listed in
#      tmp-gone.txt, not red), and SUMMARY.txt: one line per step
#
# The timed step (LangArena) assumes nothing else runs and the governor is
# `performance` (governor.txt records it; the script does not stop on it).
# The gate's wall time is recorded, and compared only when it ran alone.
# Every child runs under timeout with stdin from /dev/null.
#
# Stopping it: a TERM, INT or HUP (the outer
# `timeout -k 10 N` firing, or a signal by hand) stops the running step and
# every process under it, its children's children included, before the
# script exits (status 128 + the signal's number), so that `ledger run`
# releases the machine lock only when nothing of the run is left.
# SUMMARY.txt then ends with a `stopped:` line naming the signal and the
# step. Each step's own `timeout` puts it in a process group of its own,
# which the outer timeout's signal does not reach; and a shell runs a trap
# only after its foreground command ends: so each step runs in the
# background, the script waits for it, and the trap stops its whole tree
# (frozen with SIGSTOP while it is listed, then TERM; KILL after 5 s for
# whatever is left, within the outer timeout's 10 s). A step started
# through WAVE_RUNNER is first sent the signal alone, for the runner to
# stop its own tree and free the lock (up to 5 s), then treated the same.
# Exit status: 0 when every step is green, 1 otherwise, 2 on a usage error.
set -u

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "wave-final: usage: sh tools/wave-final.sh OUT [BASE_STATS]" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
out=$(realpath -m "$1")
base=${2:+$(realpath -m "$2")}
# (OUT is made before the private /tmp, to be bound into it: the run inside
# finds it made.)
[ "${WAVE_IN_PRIVATE_TMP:-}" != 1 ] && [ -e "$out" ] && { echo "wave-final: $out exists" >&2; exit 2; }
case "$out/" in "$root"/results/*) ;; "$root"/*) echo "wave-final: OUT must be under results/ or outside the repository" >&2; exit 2 ;; esac
# The private /tmp (WAVE_PRIVATE_TMP, default 1; not with WAVE_TMP): the
# script runs itself again in a mount namespace of its own, on a tmpfs
# /tmp, as tools/gate.sh does, so the litter check sees what this run
# leaves and nothing another lane does beside its shared steps. The
# checkout, OUT, BASE_STATS, WAVE_LA_SIDE, WAVE_LR_REFERENCE and
# WAVE_FLOOR_BASE's directory, when under /tmp, are bound into it at the
# same paths; any other path under /tmp the steps are given is not seen
# from outside (WAVE_PRIVATE_TMP=0 for that). A machine that refuses the namespace snapshots the real
# /tmp, said in the `tmp:` line (another lane's entry can then read red);
# `require` stops there instead. 0 snapshots the real /tmp.
pt_mode=${WAVE_PRIVATE_TMP_MODE:-}
if [ -z "${WAVE_TMP:-}" ] && [ "${WAVE_IN_PRIVATE_TMP:-}" != 1 ]; then
  case "${WAVE_PRIVATE_TMP:-1}" in
    0) pt_mode="the real /tmp, snapshotted (WAVE_PRIVATE_TMP=0)" ;;
    1|require)
      if pt_why=$(unshare --user --map-current-user --mount --keep-caps sh -c 'mount -t tmpfs tmpfs /tmp' < /dev/null 2>&1); then
        # shellcheck disable=SC2016 # expanded by the inner shell
        mkdir -p "$out" || exit 2
        export WF_ROOT="$root" WF_OUT="$out" WF_BASE="$base" WF_FLOOR="${WAVE_FLOOR_BASE:+$(dirname "$WAVE_FLOOR_BASE")}"
        exec unshare --user --map-current-user --mount --keep-caps sh -c '
          self=$1; shift
          exec 9< /tmp
          mount -t tmpfs tmpfs /tmp || exit 2
          for p in "$WF_ROOT" "$WF_OUT" "$WF_BASE" "${WAVE_LA_SIDE:-}" "${WAVE_LR_REFERENCE:-}" "$WF_FLOOR"; do
            case "$p" in
              /tmp/*) [ -d "/proc/self/fd/9${p#/tmp}" ] || continue
                mkdir -p "$p" && mount --no-canonicalize --rbind "/proc/self/fd/9${p#/tmp}" "$p" || exit 2 ;;
            esac
          done
          exec 9<&-
          exec setpriv --inh-caps=-all --ambient-caps=-all --bounding-set=-all env -u WF_ROOT -u WF_OUT -u WF_BASE -u WF_FLOOR WAVE_IN_PRIVATE_TMP=1 \
            WAVE_PRIVATE_TMP_MODE="a tmpfs in a mount namespace of its own" sh "$self" "$@"
        ' wave-final "$root/tools/wave-final.sh" "$@" < /dev/null
      fi
      pt_why=$(printf '%s' "$pt_why" | tail -n 1)
      if [ "${WAVE_PRIVATE_TMP:-1}" = require ]; then echo "wave-final: cannot make a private /tmp ($pt_why); WAVE_PRIVATE_TMP=require stops here" >&2; exit 2; fi
      pt_mode="the real /tmp, snapshotted: this machine refuses the namespace ($pt_why)" ;;
    *) echo "wave-final: WAVE_PRIVATE_TMP must be 0, 1 or require" >&2; exit 2 ;;
  esac
fi
unset WAVE_IN_PRIVATE_TMP WAVE_PRIVATE_TMP_MODE
[ -n "${WAVE_TMP:-}" ] && pt_mode="WAVE_TMP, snapshotted"
mkdir -p "$out"
red=""
step() { echo "$1" >> "$out/SUMMARY.txt"; case "$1" in *": red"*) red="$red ${1%%:*}" ;; esac; }
now() { date +%s.%N; }
wall() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", b-a}'; }

# The running step: run CMD... starts it in the background, records
# its PID in $cur and waits for it; its status is the command's.
cur=""; curname=""
# A command a non-interactive shell starts in the background ignores SIGINT
# and SIGQUIT; env --default-signal gives the step the dispositions it had
# in the foreground (a test that sends SIGINT to a child expects it to act).
run() { env --default-signal=INT,QUIT "$@" & cur=$!; wait "$cur"; rc_run=$?; cur=""; return $rc_run; }
# lk shared|timed STEP CMD...: run CMD through WAVE_RUNNER on that side of
# the machine lock, as `final: STEP`; without WAVE_RUNNER, run it as it is.
runner=${WAVE_RUNNER:-}
via=""
lk() {
  l_side=$1 l_step=$2; shift 2
  lk_start=""; rm -f "$out/.lk-start.$$"
  if [ -n "$runner" ]; then
    via=1
    # The command's own start, the lock granted, in OUT/.lk-start: a step's
    # wall leaves the lock's wait out (lk_wait).
    # shellcheck disable=SC2086,SC2016
    run $runner --"$l_side" --step "final: $l_step" --kind "${WAVE_RUN_KIND:-new}" -- sh -c 'date +%s.%N > "$0"; exec "$@"' "$out/.lk-start.$$" "$@"
  else run "$@"; fi
  l_rc=$?; via=""
  [ -s "$out/.lk-start.$$" ] && lk_start=$(cat "$out/.lk-start.$$"); rm -f "$out/.lk-start.$$"
  return $l_rc
}
# own T0 T1: the step's wall from T0 (its start, or the lock's grant
# through the runner) to T1, and the lock's wait when there was one.
own() {
  if [ -n "$lk_start" ]; then echo "$(wall "$lk_start" "$2") s (waited $(wall "$1" "$lk_start") s for the lock)"
  else echo "$(wall "$1" "$2") s"; fi
}
# PID and every descendant, parents first.
tree() {
  echo "$1"
  for t_child in $(ps -o pid= --ppid "$1" 2>/dev/null); do tree "$t_child"; done
}
# True while PID exists and is not a zombie (a dead child not yet reaped).
alive() { t_st=$(ps -o stat= -p "$1" 2>/dev/null) || return 1; case "$t_st" in Z*|"") return 1 ;; esac; return 0; }
stop_step() {
  [ -n "$cur" ] || return 0
  t_roots=$cur
  if [ -n "$via" ]; then
    # The runner stops its own tree and frees the lock: give it 5 s, then
    # stop whatever of the tree it had then is left.
    t_list=$(tree "$cur" | tr '\n' ' ')
    kill -TERM "$cur" 2>/dev/null
    t_i=0
    while alive "$cur" && [ $t_i -lt 50 ]; do sleep 0.1; t_i=$((t_i + 1)); done
    t_roots=""
    for t_p in $t_list; do alive "$t_p" && t_roots="$t_roots $t_p"; done
    if [ -z "$t_roots" ]; then wait "$cur" 2>/dev/null; cur=""; return 0; fi
  fi
  # Freeze the tree until a listing finds nothing new: a stopped process
  # neither forks nor dies, so none of the tree is reparented away from it.
  t_all=""
  while :; do
    t_now=$(for t_r in $t_roots; do tree "$t_r"; done | LC_ALL=C sort -nu | tr '\n' ' ')
    # shellcheck disable=SC2086
    kill -STOP $t_now 2>/dev/null
    [ "$t_now" = "$t_all" ] && break
    t_all=$t_now
  done
  # shellcheck disable=SC2086
  kill -TERM $t_all 2>/dev/null; kill -CONT $t_all 2>/dev/null
  t_i=0
  while :; do
    t_left=""
    for t_p in $t_all; do alive "$t_p" && t_left="$t_left $t_p"; done
    [ -z "$t_left" ] && break
    if [ $t_i -ge 50 ]; then
      # What is left, and anything it started meanwhile.
      for t_p in $t_left; do
        # shellcheck disable=SC2086
        kill -KILL $(tree "$t_p") 2>/dev/null
      done
      sleep 0.2
      break
    fi
    sleep 0.1; t_i=$((t_i + 1))
  done
  wait "$cur" 2>/dev/null
  cur=""
}
on_signal() { # NAME NUMBER
  trap '' TERM INT HUP
  stop_step
  echo "stopped: SIG$1 during ${curname:-no step}; every process of the step stopped" >> "$out/SUMMARY.txt"
  exit $((128 + $2))
}
trap 'on_signal TERM 15' TERM
trap 'on_signal INT 2' INT
trap 'on_signal HUP 1' HUP

# The lock this run is under. `ledger run` exports LEDGER_LOCK_HELD
# (MODE:LOCKFILE) to its command: a final started inside an exclusive one
# (the old form, the whole run under `ledger run --timed`) holds the whole
# machine for every step, gate and goldens included, which the steps' own
# runner calls cannot undo. It still runs, and says so first.
# (A run under another lock file, LEDGER_LOCK set to another path than the
# one held, is not inside it: the runner takes that other lock.)
held_mode=""
case "${LEDGER_LOCK_HELD:-}" in exclusive:*|shared:*|slot:*) held_mode=${LEDGER_LOCK_HELD%%:*} ;; esac
[ -n "${LEDGER_LOCK:-}" ] && [ "$(realpath -m "$LEDGER_LOCK")" != "$(realpath -m "${LEDGER_LOCK_HELD#*:}")" ] && held_mode=""
if [ -n "$runner" ]; then
  lock_line="lock: through WAVE_RUNNER ($runner): shared for every step but the LangArena timing, the whole machine for that"
  [ "${WAVE_GROUP:-0}" = 1 ] && lock_line="$lock_line; the shared steps from the cases to the neutrality in one runner call (WAVE_GROUP=1)"
else lock_line="lock: the caller's (WAVE_RUNNER unset: every step runs under the lock the run was started with)"; fi
case "$held_mode" in
  exclusive) lock_line="$lock_line; but the whole run is inside an exclusive \`ledger run\` (LEDGER_LOCK_HELD), which holds the whole machine for every step: the old form; start wave-final.sh outside ledger run, with WAVE_RUNNER, to hold it only for the LangArena timing" ;;
  shared|slot) [ -n "$runner" ] && lock_line="$lock_line; but the whole run is inside a $held_mode \`ledger run\` (LEDGER_LOCK_HELD): its LangArena timing cannot take the whole machine there (start wave-final.sh outside ledger run)" ;;
esac
[ "${WAVE_PART:-}" = shared ] || echo "wave-final: $lock_line"
# The steps on the shared side, 1b to 5c: run step by step, each through
# the runner, or (WAVE_GROUP=1, with WAVE_RUNNER) by the script itself
# called again (WAVE_PART=shared) under one runner call.
shared_steps() {
# 1b. the test files compile: a minute's check before the gate's suites.
curname=cases
lk shared cases timeout -k 10 900 sh tools/cases-check.sh "$out/cases" < /dev/null > "$out/cases.log" 2>&1
crc=$?
if [ -f "$out/cases/summary.txt" ]; then cases_line=$(head -n 1 "$out/cases/summary.txt")
else cases_line="cases: red (rc=$crc, no summary: cases.log)"; fi
step "$cases_line"
echo "wave-final: $cases_line"
case "$cases_line" in
  *": red"*) if [ "${WAVE_CASES_STOP:-0}" = 1 ]; then : > "$out/.cases-stop"; return 0; fi ;;
esac

# 2. the gate
t0=$(now); curname=gate
lk shared gate env GATE_OUT="$out/gate" GATE_GC_ENV='AVANT_GC_STRESS=1;AVANT_YOUNG_MB=1;AVANT_GC_VERIFY=1;AVANT_NURSERY=0' \
  timeout -k 10 3600 sh tools/gate.sh < /dev/null > "$out/gate.out" 2>&1
rc=$?; t1=$(now); echo "wall $(own "$t0" "$t1")" > "$out/gate-wall.txt"
if [ $rc -eq 0 ] && grep -q '^GATE PASS' "$out/gate.out"; then step "gate: pass, $(cat "$out/gate-wall.txt")"; else step "gate: red (rc=$rc; $(tail -1 "$out/gate.out"))"; fi

# 3. letters and exports
sha256sum bin/avant-b bin/avant-c-llvm bin/avant-d-llvm runtime/build/avant_rt.o > "$out/letters.txt" 2>&1
nm -g --defined-only runtime/build/avant_rt.o 2>/dev/null | awk '{print $3}' | LC_ALL=C sort > "$out/nm.txt"
grep -v '^#' runtime/avant_rt.exports | sed '/^$/d' | LC_ALL=C sort > "$out/exports.txt"
if cmp -s "$out/nm.txt" "$out/exports.txt"; then step "nm: equals runtime/avant_rt.exports ($(wc -l < "$out/nm.txt") symbols)"; else step "nm: red (differs from runtime/avant_rt.exports; diff in nm.diff)"; diff "$out/exports.txt" "$out/nm.txt" > "$out/nm.diff"; fi

# 4. the run goldens
golden() { # NAME MODE ENV
  t0=$(now); curname="goldens $1"
  lk shared "goldens $1" env GOLDEN_COMPILER=bin/avant-c-llvm GOLDEN_ENV="$3" GOLDEN_MODE="$2" GOLDEN_OUT="$out/goldens-$1" \
    timeout -k 10 3600 sh tools/run-goldens.sh < /dev/null > "$out/goldens-$1.log" 2>&1
  rc=$?; t1=$(now)
  line=$(grep -iE 'same|different|skipped' "$out/goldens-$1.log" | tail -1)
  echo "$1: rc=$rc $line wall=$(own "$t0" "$t1")" >> "$out/goldens.txt"
  [ $rc -eq 0 ] || red_goldens=1
}
red_goldens=0
# The goldens' LangArena set needs the harness layout (langarena/harness/measure.cr
# finds avant/ and LangArena/ as siblings of its checkout): the checkout's
# directory is named avant, LangArena/ sits beside it, and the untracked
# langarena/ harness is copied in. Without it the runner skips 50 tasks.
if [ "$(basename "$root")" != avant ] || [ ! -d "$root/../LangArena" ] || [ ! -f "$root/langarena/harness/measure.cr" ]; then
  red_goldens=1
  echo "layout: the checkout must be .../avant beside .../LangArena with langarena/ copied in (the LangArena goldens need it)" >> "$out/goldens.txt"
fi
golden debug --debug ""
golden bare "" ""
golden release --release ""
golden stress --debug "AVANT_GC_STRESS=1"
if [ $red_goldens -eq 0 ]; then step "goldens: green ($(tr '\n' ';' < "$out/goldens.txt"))"; else step "goldens: red ($(tr '\n' ';' < "$out/goldens.txt"))"; fi

# 5. the GC-statistics set
g="$out/gcstats"; curname=gcstats
# The base cache's entry of WAVE_BASE's tree (built when missing), for the
# statistics' base and the neutrality.
entry=""; entry_why=""
if [ -n "${WAVE_BASE:-}" ]; then
  wbase=$(git merge-base "$WAVE_BASE" HEAD 2>/dev/null) || wbase=$WAVE_BASE
  curname="base cache"
  if lk shared "base cache" sh -c 'sh tools/base-cache.sh "$1" > "$2/base-cache.txt" 2> "$2/base-cache.log" < /dev/null' base-cache "$wbase" "$out"; then
    entry=$(cat "$out/base-cache.txt")
    # Held shared while the steps read it: tools/base-cache.sh --prune
    # removes no entry a run holds.
    exec 7>> "$entry.lock" && flock -s 7
  else entry_why="tools/base-cache.sh $wbase failed (base-cache.log)"; fi
  curname=gcstats
fi
[ -z "$base" ] && [ -n "$entry" ] && base=$entry/gcstats
lk shared gcstats timeout -k 10 1800 sh tools/gcstats.sh . "$g" < /dev/null > "$out/gcstats.log" 2>&1
cfail=$?
files=$(ls "$g"/*.out "$g"/*.err 2>/dev/null | wc -l)
if [ $cfail -ne 0 ]; then step "gcstats: red (a fixture did not compile; logs in gcstats/bin)"
elif [ -n "$base" ]; then
  same=0; differ=0
  strip() { sed -E 's/ pause_[a-z_]+_ns=[0-9]+//g' "$1"; }
  for f in "$g"/*.out "$g"/*.err; do
    n=$(basename "$f")
    if [ -f "$base/$n" ] && [ "$(strip "$f")" = "$(strip "$base/$n")" ]; then same=$((same+1)); else differ=$((differ+1)); echo "DIFF $n" >> "$out/stats.txt"; fi
  done
  echo "same=$same differ=$differ" >> "$out/stats.txt"
  step "gcstats: $files files; against $base: same=$same differ=$differ (stats.txt; a difference is explained in the record, not red)"
else step "gcstats: $files files (no base given)"; fi

# 5b. the smoke test: the head's C' packed, the tarball's cases run.
curname=smoke
# shellcheck disable=SC2016
lk shared smoke sh -c 'sh tools/package.sh "$1/smoke" bin/avant-c-llvm bin/re2c < /dev/null > "$1/package.log" 2>&1 || exit 3
  t=$(sed -n "s/^package: \(.*\.tar\.gz\)\$/\1/p" "$1/package.log" | tail -n 1)
  [ -n "$t" ] || exit 4
  timeout -k 10 1200 sh tools/smoke.sh "$t" < /dev/null > "$1/smoke.log" 2>&1' smoke "$out"
src=$?
rm -rf "$out/smoke"
sp=$(grep -c '^ok ' "$out/smoke.log" 2>/dev/null); sf=$(grep -c '^FAIL' "$out/smoke.log" 2>/dev/null)
case $src in
  0) step "smoke: ${sp:-0} passed, ${sf:-0} failed (smoke.log)" ;;
  3|4) step "smoke: red (tools/package.sh failed: package.log)" ;;
  *) step "smoke: red (rc=$src; ${sp:-0} passed, ${sf:-0} failed: smoke.log)" ;;
esac

# 5c. the neutrality against the base cache (WAVE_BASE).
if [ -n "${WAVE_BASE:-}" ]; then
  if [ -z "$entry" ]; then step "neutral: red ($entry_why)"
  else
    curname=neutral
    git diff --name-only "$wbase" HEAD > "$out/changed.txt" 2>/dev/null
    lk shared neutral env NEUTRAL_CHANGED="$out/changed.txt" timeout -k 10 1800 sh tools/neutral.sh "$entry" . "$out/neutral" < /dev/null > "$out/neutral.log" 2>&1
    nrc=$?
    if [ ! -f "$out/neutral/summary.txt" ]; then step "neutral: red (rc=$nrc, no summary: neutral.log)"
    else
      moved=$(grep -h '^\(LL-DIFF\|STATUS\|refused-msg\)' "$out/neutral/result.txt" "$out/neutral/la-result.txt" 2>/dev/null | awk '{ print $NF }' | head -n 5 | tr '\n' ' ')
      step "neutral: against $WAVE_BASE's $(git rev-parse --short=8 "$wbase") ($entry): $(tr '\n' ';' < "$out/neutral/summary.txt")${moved:+ outside the change: $moved}(neutral/; a difference is explained in the record, not red)"
    fi
  fi
else step "neutral: skipped (WAVE_BASE unset)"; fi
}

if [ "${WAVE_PART:-}" = shared ]; then shared_steps; exit 0; fi

# 1. provenance
# The /tmp the litter check reads (WAVE_TMP, for the tool's own tests).
tmpd=${WAVE_TMP:-/tmp}
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c > "$out/governor.txt"
find "$tmpd" -mindepth 1 -maxdepth 1 | LC_ALL=C sort > "$out/tmp-before.txt"
if ! head_rev=$(git rev-parse --verify -q HEAD 2>/dev/null) || [ "$(git rev-parse --show-toplevel 2>/dev/null)" != "$(pwd -P)" ]; then
  echo "not a git checkout of its own: $root" > "$out/head.txt"
  step "head: red (not a git checkout: no commit to name)"
elif ! git status --short > "$out/status.txt" 2>&1; then
  { echo "$head_rev"; cat "$out/status.txt"; } > "$out/head.txt"
  step "head: red (git status failed: $(tail -1 "$out/status.txt"))"
else
  { echo "$head_rev"; git rev-parse 'HEAD^{tree}'; cat "$out/status.txt"; } > "$out/head.txt"
  if [ -s "$out/status.txt" ]; then step "head: red (the tree is not clean)"; else step "head: $head_rev clean (tree $(git rev-parse 'HEAD^{tree}'))"; fi
fi
rm -f "$out/status.txt"
step "$lock_line"

rm -f "$out/.cases-stop"
if [ -n "$runner" ] && [ "${WAVE_GROUP:-0}" = 1 ]; then
  # One runner call for the shared steps: the lock is taken once, so a
  # whole-machine run of another lane that arrives meanwhile waits for the
  # whole shared part instead of going first at the next step. The script
  # itself runs them (WAVE_PART=shared), in this private /tmp; its lines go
  # to SUMMARY.txt as the steps end.
  t0=$(now); curname="shared steps"
  lk shared "shared steps" env WAVE_PART=shared WAVE_RUNNER= WAVE_IN_PRIVATE_TMP=1 WAVE_PRIVATE_TMP_MODE="$pt_mode" \
    sh "$root/tools/wave-final.sh" "$out" ${base:+"$base"} < /dev/null
  grc=$?; t1=$(now)
  # What the steps left for the later ones: the verdicts, the base cache's entry.
  red=""
  while IFS= read -r sl; do case "$sl" in *": red"*) red="$red ${sl%%:*}" ;; esac; done < "$out/SUMMARY.txt"
  entry=""; [ -s "$out/base-cache.txt" ] && entry=$(cat "$out/base-cache.txt")
  [ -z "$base" ] && [ -n "$entry" ] && base=$entry/gcstats
  if [ $grc -eq 0 ]; then step "shared steps: one runner call, wall $(own "$t0" "$t1")"
  else step "shared steps: red (the runner call's rc=$grc, wall $(own "$t0" "$t1"))"; fi
else shared_steps; fi
if [ -f "$out/.cases-stop" ]; then
  rm -f "$out/.cases-stop"
  step "WAVE FINAL RED:$red (stopped after the cases: WAVE_CASES_STOP=1)"; cat "$out/SUMMARY.txt"; exit 1
fi

# 6. LangArena (optional)
# This machine's LR reference: WAVE_LR_REFERENCE, else the directory named
# in WAVE_LR_DEFAULT (default ~/.config/avant/lr-reference, one line, kept
# on the machine itself). Its machine.txt must name this machine's CPU and
# thread count: a reference measured elsewhere makes every ratio wrong.
la_ref() {
  ref=${WAVE_LR_REFERENCE:-}
  cfg=${WAVE_LR_DEFAULT:-${HOME:-/nonexistent}/.config/avant/lr-reference}
  if [ -z "$ref" ] && [ -f "$cfg" ]; then ref=$(sed -n '1p' "$cfg"); ref_from=" (from $cfg)"; fi
  [ -n "$ref" ] || { ref_why="no LR reference: set WAVE_LR_REFERENCE, or write this machine's reference directory into $cfg"; return 1; }
  [ -d "$ref" ] || { ref_why="the LR reference $ref is not a directory"; return 1; }
  [ -f "$ref/machine.txt" ] || { ref_why="the LR reference $ref has no machine.txt: which machine measured it cannot be checked"; return 1; }
  here_cpu=$(sed -n 's/^model name[[:space:]]*: *//p' "${WAVE_CPUINFO:-/proc/cpuinfo}" | head -n 1)
  here_n=$(getconf _NPROCESSORS_ONLN)
  # machine.txt may add a note in parentheses after the CPU's name; the
  # name's own parentheses (Intel(R) Xeon(R) ...) stay.
  ref_cpu=$(sed -n 's/^cpu: *//p' "$ref/machine.txt" | sed 's/ ([^()]*)$//' | head -n 1)
  ref_n=$(sed -n 's/^threads: *//p' "$ref/machine.txt" | head -n 1)
  if [ "$ref_cpu" != "$here_cpu" ] || [ "$ref_n" != "$here_n" ]; then
    ref_why="the LR reference $ref was measured on another machine ($ref_cpu, $ref_n threads; this one: $here_cpu, $here_n threads)"
    return 1
  fi
}
# The LR re-measure's rows the undisturbed rule still called disturbed after
# their retries (lr-table.sh's last column), named in the step's line.
la_disturbed() {
  awk -F'\t' '!/^#/ && $1 != "task" && $13 ~ /^no/ { n++; t = t (t == "" ? "" : ", ") $1 }
    END { if (n) printf "; %d disturbed after retries, not evidence: %s", n, t }' "$la/lr-avant.tsv" 2>/dev/null
}
if [ "${WAVE_LA:-0}" = 1 ]; then
  side=${WAVE_LA_SIDE:-}; ref_from=""; ref_why=""
  if [ -z "$side" ] || [ ! -d "$side/avant" ] || [ ! -d "$side/LangArena" ]; then
    step "langarena: red (set WAVE_LA_SIDE with avant/ and LangArena/)"
  elif ! la_ref; then
    step "langarena: red ($ref_why)"
  else
    echo "$ref$ref_from" > "$out/lr-reference.txt"
    la="$out/la"; mkdir -p "$la"
    curname=langarena
    # The harness binary of that checkout, built once (tools/la-harness.sh):
    # a build, on the shared side.
    # shellcheck disable=SC2016
    lk shared "langarena harness" sh -c 'cd "$1" && h=$(sh tools/la-harness.sh 2> "$2/harness-build.log" < /dev/null) && echo "$h" > "$2/harness.txt"' la-harness "$side/avant" "$la"
    # The timing alone holds the whole machine: the prod pass, then the LR
    # re-measure of its binaries. Its own start and end (timed.txt) give the
    # time it held the machine, the lock's wait left out.
    # shellcheck disable=SC2016
    lk timed langarena sh -c 'date +%s.%N > "$2/timed.txt"; h=$(cat "$2/harness.txt" 2>/dev/null) || exit 3; echo "harness: $h" > "$2/la-prod.log"
      t0=$(date +%s.%N)
      cd "$1" && AVANT_COMPILER="$3/bin/avant-c-llvm" AVANT_CODEGEN=release AVANT_RESULTS_DIR="$2" \
        timeout -k 10 2700 "$h" prod < /dev/null >> "$2/la-prod.log" 2>&1
      echo "prod $? $t0 $(date +%s.%N)" > "$2/prod.txt"
      sh tools/lr-table.sh avant "$4/langarena/tmp" "$5/LangArena/run.js" "$2/lr-avant.tsv" < /dev/null > "$2/lr-avant.log" 2>&1 \
        && sh tools/lr-table.sh table "$6" "$2" "$5/LangArena/results/2026-10-21-kostya-ms7c37.js" < /dev/null > "$2/table.md" 2> "$2/table.log"
      lrc=$?; date +%s.%N >> "$2/timed.txt"; exit $lrc' la-timed "$side/avant" "$la" "$root" "$root" "$side" "$ref"
    lrc=$?; curname=floor
    rc=$(awk '{ print $2 }' "$la/prod.txt" 2>/dev/null); [ -n "$rc" ] || rc=none
    pw=$(awk '{ printf "%.1f", $4 - $3 }' "$la/prod.txt" 2>/dev/null)
    if [ -n "$runner" ]; then held_what="the whole machine held"; else held_what="the timed part took"; fi
    held=$(awk 'NR == 1 { a = $1 } NR == 2 { printf "%.1f", $1 - a }' "$la/timed.txt" 2>/dev/null)
    if [ "$rc" = 0 ] && [ $lrc -eq 0 ]; then step "langarena: prod rc=0 wall=$pw s; LR table la/table.md against $ref$ref_from$(la_disturbed); $held_what ${held:-?} s"; else step "langarena: red (prod rc=$rc, LR table rc=$lrc)"; fi
    # The floor: tools/floor.py on la/table.md, beside the base's.
    fbase=${WAVE_FLOOR_BASE:-}
    [ -z "$fbase" ] && [ -n "$base" ] && [ -f "$(dirname "$base")/la/table.md" ] && fbase=$(dirname "$base")/la/table.md
    if [ $lrc -eq 0 ]; then
      {
        echo "# The floor: tasks where Avant is behind Crystal or Go, in time or memory"
        echo
        echo "Re-derived by \`tools/floor.py\` from \`la/table.md\` (the combined table's \"vs Crystal\" and \"vs Go\" columns, a ratio above 1.00)${fbase:+, beside the base's \`$fbase\`}; the totals from \`la/runtime.tsv\`."
        echo
        python3 -I tools/floor.py "$la/table.md" --runtime "$la/runtime.tsv" ${fbase:+--base "$fbase"} ${WAVE_FLOOR_BASE_LABEL:+--base-label "$WAVE_FLOOR_BASE_LABEL"} 2> "$out/floor.err"
      } > "$out/floor.md"
      frc=$?
      if [ $frc -eq 0 ]; then step "floor: $(grep -E '^[0-9]+ tasks' "$out/floor.md" | head -1) (floor.md)"; else step "floor: red (tools/floor.py rc=$frc; floor.err)"; fi
    else step "floor: red (no LR table)"; fi
    # Layout?: a task whose time moved against the base's LR re-measure
    # while its instruction count (the head's C' and the base cache's, on
    # the same stitched programs) did not is named, here and in floor.md:
    # worth one layout experiment (tools/layout-retime.sh) before the time
    # is believed. Only when the base's table and the base cache are of one
    # tree, so the times and the counts are of one base.
    curname=layout
    blr=${fbase:+$(dirname "$fbase")/lr-avant.tsv}
    btree=""; [ -n "$fbase" ] && btree=$(sed -n '2p' "$(dirname "$(dirname "$fbase")")/head.txt" 2>/dev/null)
    if [ $lrc -ne 0 ]; then :
    elif [ -z "$entry" ]; then step "layout: skipped (WAVE_BASE unset: no base instruction counts)"
    elif [ -z "$blr" ] || [ ! -f "$blr" ]; then step "layout: skipped (no base LR re-measure: lr-avant.tsv beside the floor's base table)"
    elif [ "$btree" != "$(basename "$entry")" ]; then step "layout: skipped (the floor's base table is of tree ${btree:-unknown}, the base cache's entry of $(basename "$entry"): times and counts of two bases)"
    else
      # shellcheck disable=SC2016
      lk shared "langarena counts" sh -c 'lap=$(BASE_CACHE=$(dirname "$1") sh tools/base-cache.sh --la "$(basename "$1")" 2> "$2/ins-head.log" < /dev/null) || exit 3
        echo "$lap" > "$2/la-part.txt"
        sh tools/la-count.sh . "$lap" "$2/ins-head.tsv" < /dev/null >> "$2/ins-head.log" 2>&1; exit 0' la-counts "$entry" "$la"
      if [ -s "$la/ins-head.tsv" ] && [ -s "$la/la-part.txt" ]; then
        ll=$(python3 -I tools/layout-flag.py "$la/lr-avant.tsv" "$blr" "$la/ins-head.tsv" "$(cat "$la/la-part.txt")/ins.tsv" "$la/layout.tsv" 2> "$la/layout.err")
        if [ -n "$ll" ]; then
          step "$ll (la/layout.tsv)"
          if [ -f "$out/floor.md" ]; then
            {
              echo
              echo "## Layout?"
              echo
              echo "Tasks whose time moved against the base's (\`$blr\`) while their instruction count did not (the head's C' and the base cache's, \`la/ins-head.tsv\` and the part's \`ins.tsv\`): where the code sits is the first suspect, one layout experiment each (\`tools/layout-retime.sh\`) before the time is believed."
              echo
              if [ "$(wc -l < "$la/layout.tsv")" -gt 1 ]; then
                echo "| Task | Base s | Head s | Time | Instructions | On the floor |"
                echo "| --- | ---: | ---: | ---: | ---: | --- |"
                awk -F'\t' 'FILENAME == ARGV[1] { if ($0 ~ /^\| [A-Z]/) { split($0, c, " \\| "); sub(/^\| /, "", c[1]); fl[c[1]] = 1 }; next }
                  FNR > 1 { printf "| %s | %.3f | %.3f | %s%% | %s%% | %s |\n", $1, $3, $4, $5, $6, ($1 in fl) ? "yes" : "no" }' "$out/floor.md" "$la/layout.tsv"
              else echo "None: $ll."; fi
            } >> "$out/floor.md"
          fi
        else step "layout: red (tools/layout-flag.py failed: la/layout.err)"; fi
      else step "layout: red (no head instruction counts: la/ins-head.log)"; fi
    fi
  fi
else step "langarena: skipped (WAVE_LA unset)"; fi

# 7. litter and summary
# Litter is an entry this run's steps left in /tmp: one there after and
# not before. An entry gone during the run is another program removing
# its own (a lane's temporary directory, a daemon's socket): listed, not
# red. (The gate checks its own litter in a private /tmp.)
curname=""
find "$tmpd" -mindepth 1 -maxdepth 1 | LC_ALL=C sort > "$out/tmp-after.txt"
LC_ALL=C comm -13 "$out/tmp-before.txt" "$out/tmp-after.txt" > "$out/tmp-new.txt"
LC_ALL=C comm -23 "$out/tmp-before.txt" "$out/tmp-after.txt" > "$out/tmp-gone.txt"
gone=$(wc -l < "$out/tmp-gone.txt")
if [ -s "$out/tmp-new.txt" ]; then step "tmp: red ($(wc -l < "$out/tmp-new.txt") new in $tmpd, tmp-new.txt: $(head -3 "$out/tmp-new.txt" | tr '\n' ' '); $pt_mode)"
elif [ "$gone" -gt 0 ]; then step "tmp: nothing new ($gone gone during the run, removed by their owners: tmp-gone.txt; $pt_mode)"
else step "tmp: unchanged ($pt_mode)"; fi
if [ -z "$red" ]; then step "WAVE FINAL GREEN"; cat "$out/SUMMARY.txt"; exit 0; fi
step "WAVE FINAL RED:$red"; cat "$out/SUMMARY.txt"; exit 1
