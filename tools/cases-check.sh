#!/bin/sh
# cases-check.sh — every test file compiles with this checkout's C': a
# check of a minute, before the gate, for a test file the compiler now
# refuses (a rule that tightened, a rebase that brought in a file written
# under the old rule). The builds and the goldens compile no file of
# tests/cases/, so otherwise the gate's suites are the first to say so.
#
#   sh tools/cases-check.sh OUT        (from the checkout root)
#
# Each tests/cases/test_*.av goes through `bin/avant-c-llvm __emit-ll`
# (the whole front end and the lowering: the parse, the checks, the loop
# rules), CASES_JOBS at a time (default 16), each under `timeout -k 5 300`,
# AVANT_ROOT unset, stdin from /dev/null; the helpers beside them
# (harness.av, ...) are compiled through the test files that import them.
# OUT (a new directory) gets cases.tsv (FILE <TAB> STATUS <TAB> the first
# `error` line of the output, or its first line), the output of each
# refused file as OUT/refused/NAME.log, and summary.txt: one line,
#
#   cases: 101 files compile (12.3 s)
#   cases: red (1 of 101 refused, 12.3 s: tests/cases/test_x.av: error: ...)
#
# also printed on stdout. Status 0 when every file compiles, 1 when one is
# refused, 2 on a usage error or without bin/avant-c-llvm.
set -u

if [ "${1:-}" = --one ]; then
  f=$2; k=$(basename "$f" .av)
  env -u AVANT_ROOT timeout -k 5 300 ./bin/avant-c-llvm __emit-ll "$f" "$CASES_TMP/$k.ll" > "$CASES_TMP/$k.log" 2>&1 < /dev/null
  rc=$?
  rm -f "$CASES_TMP/$k.ll"
  msg=""
  if [ $rc -ne 0 ]; then
    msg=$(grep -m 1 -i 'error' "$CASES_TMP/$k.log" || head -n 1 "$CASES_TMP/$k.log")
    [ -n "$msg" ] || msg="status $rc, no output"
    mv "$CASES_TMP/$k.log" "$CASES_OUT/refused/$k.log"
  fi
  rm -f "$CASES_TMP/$k.log"
  printf '%s\t%s\t%s\n' "$f" "$rc" "$(printf '%s' "$msg" | tr '\t' ' ')"
  exit 0
fi

[ $# -eq 1 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
out=$(realpath -m "$1")
[ -e "$out" ] && { echo "cases-check.sh: $out exists" >&2; exit 2; }
[ -x bin/avant-c-llvm ] || { echo "cases-check.sh: bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
mkdir -p "$out/refused" || exit 2
CASES_TMP=$(mktemp -d "$out/.tmp.XXXXXX") || exit 2
CASES_OUT=$out
export CASES_TMP CASES_OUT
t0=$(date +%s.%N)
ls tests/cases/test_*.av 2>/dev/null | LC_ALL=C sort > "$out/list.txt"
xargs -P "${CASES_JOBS:-16}" -I{} sh "$root/tools/cases-check.sh" --one {} < "$out/list.txt" | LC_ALL=C sort > "$out/cases.tsv"
rm -rf "$CASES_TMP"
rmdir "$out/refused" 2>/dev/null
t=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')
n=$(wc -l < "$out/list.txt")
bad=$(awk -F'\t' '$2 != 0' "$out/cases.tsv" | wc -l)
if [ "$n" -eq 0 ]; then line="cases: red (no tests/cases/test_*.av file)"
elif [ "$bad" -eq 0 ]; then line="cases: $n files compile ($t s)"
else
  first=$(awk -F'\t' '$2 != 0 { print $1 ": " $3; exit }' "$out/cases.tsv")
  line="cases: red ($bad of $n refused, $t s: $first$([ "$bad" -gt 1 ] && echo "; the others in cases.tsv"))"
fi
echo "$line" | tee "$out/summary.txt"
[ "$n" -gt 0 ] && [ "$bad" -eq 0 ]
