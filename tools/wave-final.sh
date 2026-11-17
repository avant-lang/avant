#!/bin/sh
# A wave's final evidence in one run (owner, 2026-11-09: fewer
# hand-written end-of-wave scripts, the same checks). Run it last, on the
# final committed head with a clean tree, in the background:
#
#   sh tools/wave-final.sh OUT [BASE_STATS]      (from the checkout root)
#
# OUT is a new directory under results/ or outside the repository. Steps, in
# order, each logged under OUT; a red step is reported and the later steps
# still run, so one run gives the whole picture:
#
#   1. head.txt (HEAD, its tree and `git status --short`, which must be
#      empty; red outside a git checkout of its own), governor.txt,
#      tmp-before.txt
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
#   5. the GC-statistics set (the three churn fixtures in three modes, under
#      the default environment, AVANT_NURSERY=0, AVANT_GC_VERIFY=1 and
#      AVANT_YOUNG_MB=1, AVANT_GC_STATS=1: 36 .out and 36 .err files):
#      OUT/gcstats; with BASE_STATS (an earlier run's OUT/gcstats), stats.txt
#      compares every file with the pause_*_ns fields removed
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
#   7. tmp-after.txt (red on an entry this run left in /tmp, tmp-new.txt;
#      an entry another program removed meanwhile is listed in
#      tmp-gone.txt, not red), and SUMMARY.txt: one line per step
#
# Timed steps (the gate's suites, LangArena) assume nothing else runs and the
# governor is `performance` (governor.txt records it; the script does not
# stop on it). Every child runs under timeout with stdin from /dev/null.
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
# whatever is left, within the outer timeout's 10 s).
# Exit status: 0 when every step is green, 1 otherwise, 2 on a usage error.
set -u

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "wave-final: usage: sh tools/wave-final.sh OUT [BASE_STATS]" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
out=$(realpath -m "$1")
base=${2:+$(realpath -m "$2")}
[ -e "$out" ] && { echo "wave-final: $out exists" >&2; exit 2; }
case "$out/" in "$root"/results/*) ;; "$root"/*) echo "wave-final: OUT must be under results/ or outside the repository" >&2; exit 2 ;; esac
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
# PID and every descendant, parents first.
tree() {
  echo "$1"
  for t_child in $(ps -o pid= --ppid "$1" 2>/dev/null); do tree "$t_child"; done
}
# True while PID exists and is not a zombie (a dead child not yet reaped).
alive() { t_st=$(ps -o stat= -p "$1" 2>/dev/null) || return 1; case "$t_st" in Z*|"") return 1 ;; esac; return 0; }
stop_step() {
  [ -n "$cur" ] || return 0
  # Freeze the tree until a listing finds nothing new: a stopped process
  # neither forks nor dies, so none of the tree is reparented away from it.
  t_all=""
  while :; do
    t_now=$(tree "$cur" | LC_ALL=C sort -n | tr '\n' ' ')
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

# 1. provenance
# The /tmp the litter check reads (WAVE_TMP, for the tool's own tests).
tmpd=${WAVE_TMP:-/tmp}
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c > "$out/governor.txt"
find "$tmpd" -mindepth 1 -maxdepth 1 | sort > "$out/tmp-before.txt"
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

# 2. the gate
t0=$(now); curname=gate
run env GATE_OUT="$out/gate" GATE_GC_ENV='AVANT_GC_STRESS=1;AVANT_YOUNG_MB=1;AVANT_GC_VERIFY=1;AVANT_NURSERY=0' \
  timeout -k 10 3600 sh tools/gate.sh < /dev/null > "$out/gate.out" 2>&1
rc=$?; t1=$(now); echo "wall $(wall "$t0" "$t1") s" > "$out/gate-wall.txt"
if [ $rc -eq 0 ] && grep -q '^GATE PASS' "$out/gate.out"; then step "gate: pass, $(cat "$out/gate-wall.txt")"; else step "gate: red (rc=$rc; $(tail -1 "$out/gate.out"))"; fi

# 3. letters and exports
sha256sum bin/avant-b bin/avant-c-llvm bin/avant-d-llvm runtime/build/avant_rt.o > "$out/letters.txt" 2>&1
nm -g --defined-only runtime/build/avant_rt.o 2>/dev/null | awk '{print $3}' | LC_ALL=C sort > "$out/nm.txt"
grep -v '^#' runtime/avant_rt.exports | sed '/^$/d' | LC_ALL=C sort > "$out/exports.txt"
if cmp -s "$out/nm.txt" "$out/exports.txt"; then step "nm: equals runtime/avant_rt.exports ($(wc -l < "$out/nm.txt") symbols)"; else step "nm: red (differs from runtime/avant_rt.exports; diff in nm.diff)"; diff "$out/exports.txt" "$out/nm.txt" > "$out/nm.diff"; fi

# 4. the run goldens
golden() { # NAME MODE ENV
  t0=$(now); curname="goldens $1"
  run env GOLDEN_COMPILER=bin/avant-c-llvm GOLDEN_ENV="$3" GOLDEN_MODE="$2" GOLDEN_OUT="$out/goldens-$1" \
    timeout -k 10 3600 sh tools/run-goldens.sh < /dev/null > "$out/goldens-$1.log" 2>&1
  rc=$?; t1=$(now)
  line=$(grep -iE 'same|different|skipped' "$out/goldens-$1.log" | tail -1)
  echo "$1: rc=$rc $line wall=$(wall "$t0" "$t1") s" >> "$out/goldens.txt"
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
g="$out/gcstats"; mkdir -p "$g/bin"; cfail=0; curname=gcstats
for f in heap/churn closures/churn objects/churn; do
  n=$(echo "$f" | tr / _)
  for m in debug bare release; do
    fl=--$m; [ $m = bare ] && fl=
    run timeout -k 10 120 bin/avant-c-llvm compile "tests/fixtures/$f.av" "$g/bin/$n.$m" $fl < /dev/null > "$g/bin/$n.$m.compile.log" 2>&1 || cfail=1
  done
done
for b in "$g"/bin/*.debug "$g"/bin/*.bare "$g"/bin/*.release; do
  [ -x "$b" ] || continue
  n=$(basename "$b")
  for e in default AVANT_NURSERY=0 AVANT_GC_VERIFY=1 AVANT_YOUNG_MB=1; do
    ee=$e; [ $e = default ] && ee=
    run env $ee AVANT_GC_STATS=1 timeout -k 10 120 "$b" < /dev/null > "$g/$n.$e.out" 2> "$g/$n.$e.err"
    echo "rc=$?" >> "$g/$n.$e.out"
  done
done
rm -rf "$g/bin"/*.debug "$g/bin"/*.bare "$g/bin"/*.release
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
  here_cpu=$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)
  here_n=$(getconf _NPROCESSORS_ONLN)
  ref_cpu=$(sed -n 's/^cpu: *//p' "$ref/machine.txt" | sed 's/ *(.*$//' | head -n 1)
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
    t0=$(now); curname=langarena
    # The harness binary of that checkout, built once (tools/la-harness.sh).
    # shellcheck disable=SC2016
    run sh -c 'cd "$1" && h=$(sh tools/la-harness.sh 2> "$2/harness-build.log" < /dev/null) && echo "harness: $h" > "$2/la-prod.log" \
      && AVANT_COMPILER="$3/bin/avant-c-llvm" AVANT_CODEGEN=release AVANT_RESULTS_DIR="$2" \
        timeout -k 10 2700 "$h" prod < /dev/null >> "$2/la-prod.log" 2>&1' la-prod "$side/avant" "$la" "$root"
    rc=$?; t1=$(now)
    # shellcheck disable=SC2016
    run sh -c 'cd "$1" && sh tools/lr-table.sh avant "$4/langarena/tmp" "$5/LangArena/run.js" "$2/lr-avant.tsv" < /dev/null > "$2/lr-avant.log" 2>&1 \
      && sh tools/lr-table.sh table "$3" "$2" "$5/LangArena/results/2026-10-21-kostya-ms7c37.js" < /dev/null > "$2/table.md" 2> "$2/table.log"' lr-table "$side/avant" "$la" "$ref" "$root" "$side"
    lrc=$?; curname=floor
    if [ $rc -eq 0 ] && [ $lrc -eq 0 ]; then step "langarena: prod rc=0 wall=$(wall "$t0" "$t1") s; LR table la/table.md against $ref$ref_from$(la_disturbed)"; else step "langarena: red (prod rc=$rc, LR table rc=$lrc)"; fi
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
  fi
else step "langarena: skipped (WAVE_LA unset)"; fi

# 7. litter and summary
# Litter is an entry this run's steps left in /tmp: one there after and
# not before. An entry gone during the run is another program removing
# its own (a lane's temporary directory, a daemon's socket): listed, not
# red. (The gate checks its own litter in a private /tmp.)
curname=""
find "$tmpd" -mindepth 1 -maxdepth 1 | sort > "$out/tmp-after.txt"
LC_ALL=C comm -13 "$out/tmp-before.txt" "$out/tmp-after.txt" > "$out/tmp-new.txt"
LC_ALL=C comm -23 "$out/tmp-before.txt" "$out/tmp-after.txt" > "$out/tmp-gone.txt"
gone=$(wc -l < "$out/tmp-gone.txt")
if [ -s "$out/tmp-new.txt" ]; then step "tmp: red ($(wc -l < "$out/tmp-new.txt") new in $tmpd, tmp-new.txt: $(head -3 "$out/tmp-new.txt" | tr '\n' ' '))"
elif [ "$gone" -gt 0 ]; then step "tmp: nothing new ($gone gone during the run, removed by their owners: tmp-gone.txt)"
else step "tmp: unchanged"; fi
if [ -z "$red" ]; then step "WAVE FINAL GREEN"; cat "$out/SUMMARY.txt"; exit 0; fi
step "WAVE FINAL RED:$red"; cat "$out/SUMMARY.txt"; exit 1
