#!/bin/sh
# The coverage report's lcov writer on a hand-made slot map. Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/cov-lcov.sh COMPILER WORK
#
# WORK is a new scratch directory. A hello program is built with coverage,
# its slot map and hit counts are replaced by a made-up pair (16 padding
# statement slots that absorb the program's own hits, then function, branch
# and line slots of two files whose slots interleave, with a line that has
# several slots, a line number below zero, a function with no name and
# counts of zero), and the program's lcov must read exactly as the
# reference below: files in the order of their first slot, each file's slots
# in map order, a line's count the sum over its slots written at the first
# of them.
#   lcov-built: the program builds with coverage and runs;
#   lcov-exact: the report is byte for byte the reference.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 2 ] || { echo "usage: sh tests/tools/cov-lcov.sh COMPILER WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
c=$1
w=$(realpath -m "$2")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
fail() { say "$1" 1 "$2"; echo done; exit 0; }

AVANT_COVERAGE=1 AVANT_COVERAGE_MAP="$w/m.map" "$c" compile "$root/examples/hello.av" "$w/hello" > "$w/build.log" 2>&1 \
  || fail lcov-built "the coverage build failed: $(tail -n 3 "$w/build.log")"

pad=16
{
  printf 'avant-cov 1\n'
  i=0
  while [ $i -lt $pad ]; do printf 'S\tpad.av\t1\t\n'; i=$((i + 1)); done
  printf 'F\ta.av\t1\tf\n'
  printf 'L\ta.av\t1\t\n'
  printf 'L\tb.av\t5\t\n'
  printf 'F\tb.av\t5\tg\n'
  printf 'L\ta.av\t1\t\n'
  printf 'L\ta.av\t7\t\n'
  printf 'B\ta.av\t7\t\n'
  printf 'B\tb.av\t6\t\n'
  printf 'L\tb.av\t2\t\n'
  printf 'L\ta.av\t-1\t\n'
  printf 'L\tb.av\t5\t\n'
  printf 'L\ta.av\t2\t\n'
  printf 'L\ta.av\t-1\t\n'
  printf 'F\ta.av\t9\t\n'
} > "$w/m.map"
u64() { printf "\\$(printf '%03o' "$1")\\0\\0\\0\\0\\0\\0\\0"; }
{
  i=0
  while [ $i -lt $pad ]; do u64 0; i=$((i + 1)); done
  for h in 2 1 0 0 3 0 5 0 1 2 0 9 1 0; do u64 $h; done
} > "$w/m.map.hits"

AVANT_COVERAGE_LCOV="$w/got.lcov" AVANT_COVERAGE_REPORT=0 "$w/hello" > "$w/run.out" 2>&1 \
  || fail lcov-built "the program failed: $(tail -n 3 "$w/run.out")"
say lcov-built 0

cat > "$w/want.lcov" <<'WANT'
TN:
SF:pad.av
FNF:0
FNH:0
BRF:0
BRH:0
LF:0
LH:0
end_of_record
TN:
SF:a.av
FN:1,f
FN:9,fn
FNDA:2,f
FNDA:0,fn
FNF:2
FNH:1
BRDA:7,0,0,5
BRF:1
BRH:1
DA:1,4
DA:7,0
DA:-1,3
DA:2,9
LF:4
LH:3
end_of_record
TN:
SF:b.av
FN:5,g
FNDA:0,g
FNF:1
FNH:0
BRDA:6,0,0,-
BRF:1
BRH:0
DA:5,0
DA:2,1
LF:2
LH:1
end_of_record
WANT
if cmp -s "$w/got.lcov" "$w/want.lcov"; then
  say lcov-exact 0
else
  say lcov-exact 1 "the report differs: $(diff "$w/want.lcov" "$w/got.lcov" | head -n 6 | tr '\n' ' ')"
fi
echo done
