#!/bin/sh
# A wave's final evidence in one run (Stage 28; owner, 2026-11-09: fewer
# hand-written end-of-wave scripts, the same checks). Run it last, on the
# final committed head with a clean tree, in the background:
#
#   sh tools/wave-final.sh OUT [BASE_STATS]      (from the checkout root)
#
# OUT is a new directory under results/ or outside the repository. Steps, in
# order, each logged under OUT; a red step is reported and the later steps
# still run, so one run gives the whole picture:
#
#   1. head.txt (HEAD and `git status --short`, which must be empty),
#      governor.txt, tmp-before.txt
#   2. the gate (tools/gate.sh) with the standard GC list (D235, overlapping
#      as D248): OUT/gate, gate.out, gate-wall.txt
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
#   6. only with WAVE_LA=1: LangArena prod once with C', then the LR table
#      (tools/lr-table.sh), OUT/la; needs WAVE_LA_SIDE (a directory holding
#      this checkout's copy as avant/ and LangArena/ side by side, the layout
#      W12r used) and WAVE_LR_REFERENCE (the reference results directory of
#      records/2026-11-07-s28-lr)
#   7. tmp-after.txt, and SUMMARY.txt: one line per step
#
# Timed steps (the gate's suites, LangArena) assume nothing else runs and the
# governor is `performance` (governor.txt records it; the script does not
# stop on it). Every child runs under timeout with stdin from /dev/null.
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

# 1. provenance
{ git rev-parse HEAD; git status --short; } > "$out/head.txt"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c > "$out/governor.txt"
find /tmp -mindepth 1 -maxdepth 1 | sort > "$out/tmp-before.txt"
if [ "$(sed -n '2,$p' "$out/head.txt" | wc -l)" -eq 0 ]; then step "head: $(head -1 "$out/head.txt") clean"; else step "head: red (the tree is not clean)"; fi

# 2. the gate
t0=$(now)
GATE_OUT="$out/gate" GATE_GC_ENV='AVANT_GC_VERIFY=1;AVANT_YOUNG_MB=1;AVANT_NURSERY=0;AVANT_GC_STRESS=1' \
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
  t0=$(now)
  GOLDEN_COMPILER=bin/avant-c-llvm GOLDEN_ENV="$3" GOLDEN_MODE="$2" GOLDEN_OUT="$out/goldens-$1" \
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
g="$out/gcstats"; mkdir -p "$g/bin"; cfail=0
for f in heap/churn closures/churn objects/churn; do
  n=$(echo "$f" | tr / _)
  for m in debug bare release; do
    fl=--$m; [ $m = bare ] && fl=
    timeout -k 10 120 bin/avant-c-llvm compile "tests/fixtures/$f.av" "$g/bin/$n.$m" $fl < /dev/null > "$g/bin/$n.$m.compile.log" 2>&1 || cfail=1
  done
done
for b in "$g"/bin/*.debug "$g"/bin/*.bare "$g"/bin/*.release; do
  [ -x "$b" ] || continue
  n=$(basename "$b")
  for e in default AVANT_NURSERY=0 AVANT_GC_VERIFY=1 AVANT_YOUNG_MB=1; do
    ee=$e; [ $e = default ] && ee=
    env $ee AVANT_GC_STATS=1 timeout -k 10 120 "$b" < /dev/null > "$g/$n.$e.out" 2> "$g/$n.$e.err"
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
if [ "${WAVE_LA:-0}" = 1 ]; then
  side=${WAVE_LA_SIDE:-}; ref=${WAVE_LR_REFERENCE:-}
  if [ -z "$side" ] || [ ! -d "$side/avant" ] || [ ! -d "$side/LangArena" ] || [ -z "$ref" ] || [ ! -d "$ref" ]; then
    step "langarena: red (set WAVE_LA_SIDE with avant/ and LangArena/, and WAVE_LR_REFERENCE)"
  else
    la="$out/la"; mkdir -p "$la"
    t0=$(now)
    ( cd "$side/avant" && AVANT_COMPILER="$root/bin/avant-c-llvm" AVANT_CODEGEN=release AVANT_RESULTS_DIR="$la" \
        timeout -k 10 2700 crystal run langarena/harness/measure.cr -- prod < /dev/null > "$la/la-prod.log" 2>&1 )
    rc=$?; t1=$(now)
    ( cd "$side/avant" && sh tools/lr-table.sh avant "$root/langarena/tmp" "$side/LangArena/run.js" "$la/lr-avant.tsv" < /dev/null > "$la/lr-avant.log" 2>&1 \
      && sh tools/lr-table.sh table "$ref" "$la" "$side/LangArena/results/2026-10-21-kostya-ms7c37.js" < /dev/null > "$la/table.md" 2> "$la/table.log" )
    lrc=$?
    if [ $rc -eq 0 ] && [ $lrc -eq 0 ]; then step "langarena: prod rc=0 wall=$(wall "$t0" "$t1") s; LR table la/table.md"; else step "langarena: red (prod rc=$rc, LR table rc=$lrc)"; fi
  fi
else step "langarena: skipped (WAVE_LA unset)"; fi

# 7. litter and summary
find /tmp -mindepth 1 -maxdepth 1 | sort > "$out/tmp-after.txt"
if cmp -s "$out/tmp-before.txt" "$out/tmp-after.txt"; then step "tmp: unchanged"; else step "tmp: red (/tmp changed: compare tmp-before.txt and tmp-after.txt)"; fi
if [ -z "$red" ]; then step "WAVE FINAL GREEN"; cat "$out/SUMMARY.txt"; exit 0; fi
step "WAVE FINAL RED:$red"; cat "$out/SUMMARY.txt"; exit 1
