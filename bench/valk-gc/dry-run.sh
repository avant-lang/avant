#!/bin/bash
# A dry run of run.sh's modes that VALK-B1 added or changed (the tests of
# the tool, not a measurement): build with rows (O618), check of a row
# subset and of the tree ports (O386), time with the governor check
# (O740), rounds with two sides (O652), trees-counts (O386), and every
# table regenerated (O380). The timed modes run with
# VALK_GC_ALLOW_GOVERNOR=1, one round, so their numbers are not evidence.
#
#   bench/valk-gc/dry-run.sh DIR     (DIR: a new directory; about 3 minutes)
#
# Needs bin/avant-c-llvm (or AVANT_BENCH_COMPILER), valk, go and perf.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
[ $# = 1 ] || { echo "usage: dry-run.sh DIR" >&2; exit 2; }
[ -e "$1" ] && { echo "dry-run.sh: $1 exists" >&2; exit 2; }
D=$1
mkdir -p "$D"
D=$(CDPATH='' cd -- "$D" && pwd)
RUN="bash $HERE/run.sh"
fail() { echo "dry-run.sh: FAIL: $*" >&2; exit 1; }
step() { echo "== $*"; }
export VALK_GC_BUILD=$D/build VALK_GC_OUT=$D/out

step "build with rows (O618)"
$RUN build avant valk go trees </dev/null || fail "build avant valk go trees"
[ "$(wc -l <"$D/build/binaries.sha256")" = 10 ] || fail "binaries.sha256 has $(wc -l <"$D/build/binaries.sha256") lines, not 10"
grep -q '^sha256: ' "$D/build/avant-compiler.txt" || fail "no avant-compiler.txt"
$RUN build no-such-row </dev/null 2>/dev/null && fail "an unknown row built"
$RUN build go </dev/null || fail "build go again"
[ "$(wc -l <"$D/build/binaries.sha256")" = 10 ] || fail "a rebuilt row added lines"

step "check: rows avant valk go, then the tree ports"
VALK_GC_ROWS="avant valk go" $RUN check </dev/null || fail "check"
[ "$(find "$D/out/check" -name once.verify | wc -l)" = 27 ] || fail "check: not 27 verified runs (3 headline, 24 iso)"
VALK_GC_SETS=trees $RUN check </dev/null || fail "check trees"
[ "$(find "$D/out/check/trees" -name once.verify | wc -l)" = 4 ] || fail "check trees: not 4 runs"

step "time 1 (O740: the governor files, not nproc; dry run)"
VALK_GC_ALLOW_GOVERNOR=1 VALK_GC_SETS="headline iso" VALK_GC_ROWS="avant valk" $RUN time 1 </dev/null >"$D/time.log" || fail "time"
[ "$(find "$D/out/raw" -path '*warmup*' -prune -o -name r1.out -print | wc -l)" = 18 ] || fail "time: not 18 measured runs"

step "rounds 1, two sides (O652; dry run)"
VALK_GC_ALLOW_GOVERNOR=1 VALK_GC_SIDES="base=$D/build final=$D/build" VALK_GC_SETS="headline iso trees" \
  VALK_GC_ROWS="avant valk go binarytrees-obj calc-ast" VALK_GC_UNITS="5 8" $RUN rounds 1 </dev/null >"$D/rounds.log" || fail "rounds"
n=$(find "$D/out/rounds/raw/round-1" -name '*.run' | wc -l)
[ "$n" = 22 ] || fail "rounds: $n runs in round 1, not 22 (2 sides x (3 headline + 6 iso + 2 trees))"
grep -q 'DISTURBED, NOT EVIDENCE' "$D/out/rounds/session.txt" || fail "rounds: a dry run not marked"
[ -f "$D/out/rounds/sides/final.compiler.txt" ] || fail "rounds: no side's compiler record"
VALK_GC_ALLOW_GOVERNOR=1 VALK_GC_SIDES="bad" $RUN rounds 1 </dev/null >/dev/null 2>&1 && fail "rounds: a side without a directory ran"
[ -f "$D/out/rounds/session.txt" ] || fail "rounds: a refused side removed the last session"

step "trees-counts (O386)"
VALK_GC_ROWS="binarytrees-obj calc-ast" $RUN trees-counts </dev/null || fail "trees-counts"
for r in binarytrees-obj calc-ast; do
  [ -s "$D/out/trees-counts/$r.ins" ] && grep -q '^avant gc:' "$D/out/trees-counts/$r.stats" || fail "trees-counts: $r"
done

step "tables (O380)"
python3 -I "$HERE/tables.py" "$D/out" >"$D/tables.md" || fail "tables.py"
for t in "Table 1\\." "Table 2\\." "Table 4\\." "Table 5\\." "Table 6\\." "Table 10\\." "Table 12\\." "Table R1\\." "Table R2\\." "Table R3\\." "Table R4\\." \
  "Peak RSS, MB (the process)" " MB |"; do
  grep -q "$t" "$D/tables.md" || fail "tables: no '$t'"
done
echo "dry run green: $D (tables in $D/tables.md)"
