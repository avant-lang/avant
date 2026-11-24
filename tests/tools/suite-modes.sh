#!/bin/sh
# tools/suite-modes.sh in a fake checkout whose C' is a stub: `test` lists
# the files it would run (the same partition as avant test's --shard,
# --skip-exclusive and --only-exclusive over tests/cases/test_*.av and
# avant.toml's [test] exclusive) in a JUnit file and prints a total; a file
# whose text holds FAIL-IN-MODE fails under AVANT_GC_VERIFY, one holding
# LEAVE-TMP leaves an entry in TMPDIR. Run by tests/cases/test_tools.av; by
# hand:
#
#   sh tests/tools/suite-modes.sh WORK
#
#   modes-green: seven files (two exclusive), the default three slices: eight
#     steps (plain, young, verify, nursery, stress1 to stress3, stressx), all
#     green, each with its own environment, the depth line saying the stress
#     passes ran the plain pass's seven files each once, status 0;
#   modes-slices: the slices hold the files of round-robin slices of the
#     non-exclusive ones, stressx only the two exclusive;
#   modes-one-slice: SUITE_MODES_SLICES=1 runs one whole stress pass;
#   modes-red: a file that fails under the verify mode makes that step and
#     the run red, named, the other steps green, status 1;
#   modes-litter: a step that leaves an entry in its TMPDIR is red;
#   modes-depth: a stub that loses a file from one slice is caught by the
#     depth line (DIFFERENT), status 1;
#   modes-runner: with SUITE_MODES_RUNNER each step is one call of the
#     runner, in order;
#   modes-limit: a step's limit is tools/suite-limit.sh's for its mode
#     (tests/limits.txt) and reaches avant test as --file-limit;
#   modes-usage: no argument, an existing OUT, SUITE_MODES_SLICES=0: status 2.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/suite-modes.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
unset SUITE_MODES_RUNNER SUITE_MODES_JOBS SUITE_MODES_SLICES SUITE_MODES_STEPS SUITE_LIMIT
r=$w/fake
mkdir -p "$r/tools" "$r/bin" "$r/tests/cases" || exit 2
cp "$root/tools/suite-modes.sh" "$root/tools/suite-limit.sh" "$r/tools/"
for n in a1 a2 a3 a4 a5 x1 x2; do echo "test $n" > "$r/tests/cases/test_$n.av"; done
printf '[package]\nname = "fake"\n\n[test]\nexclusive = ["tests/cases/test_x1.av", "tests/cases/test_x2.av"]\n' > "$r/avant.toml"
printf 'tests/cases/test_a1.av AVANT_GC_STRESS=1 1500 a long file\n' > "$r/tests/limits.txt"
cat > "$r/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
[ "$1" = test ] || exit 2
shift
shard="" skip=0 only=0 junit="" limit=""
while [ $# -gt 0 ]; do
  case "$1" in
    --shard) shard=$2; shift ;;
    --skip-exclusive) skip=1 ;;
    --only-exclusive) only=1 ;;
    --junit) junit=$2; shift ;;
    --file-limit) limit=$2; shift ;;
    --jobs) shift ;;
  esac
  shift
done
mode=plain
[ -n "${AVANT_YOUNG_MB:-}" ] && mode=young
[ -n "${AVANT_GC_VERIFY:-}" ] && mode=verify
[ -n "${AVANT_NURSERY:-}" ] && mode=nursery
[ -n "${AVANT_GC_STRESS:-}" ] && mode=stress
echo "$mode|$shard|$skip$only|limit=$limit" >> calls.txt
files=""; pool=""
for f in tests/cases/test_*.av; do
  case $f in *_x[0-9].av) ex=1 ;; *) ex=0 ;; esac
  if [ $only = 1 ]; then [ $ex = 1 ] && files="$files $f"; continue; fi
  [ $skip = 1 ] && [ $ex = 1 ] && continue
  pool="$pool $f"
done
if [ $only = 0 ]; then
  i=0
  for f in $pool; do
    if [ -z "$shard" ] || [ $((i % ${shard#*/})) = $((${shard%/*} - 1)) ]; then files="$files $f"; fi
    i=$((i + 1))
  done
fi
# a stub that loses the last file of the second slice
if [ -e lose-file ] && [ "${shard%/*}" = 2 ]; then files=${files% *}; fi
f=0; p=0
: > "$junit.part"
for t in $files; do
  fail=0
  if [ -n "${AVANT_GC_VERIFY:-}" ] && grep -q FAIL-IN-MODE "$t"; then fail=1; f=$((f + 1)); echo "FAIL $t"; else p=$((p + 1)); echo "ok $t"; fi
  echo "<testsuite name=\"$t\" tests=\"1\" failures=\"$fail\" time=\"0.1\"/>" >> "$junit.part"
  if grep -q LEAVE-TMP "$t"; then : > "$TMPDIR/left-by-$mode"; fi
done
{ echo "<testsuites tests=\"$((f + p))\" failures=\"$f\">"; cat "$junit.part"; echo "</testsuites>"; } > "$junit"
rm -f "$junit.part"
echo "$f failed, $p passed"
[ $f -eq 0 ]
STUB
chmod +x "$r/bin/avant-c-llvm"
sm() { # NAME [VAR=value...]: suite-modes.sh in the fake into $w/NAME, its status in $rc
  n=$1; shift
  (cd "$r" && rm -f calls.txt && env "$@" sh tools/suite-modes.sh "$w/$n" < /dev/null > "$w/$n.log" 2>&1); rc=$?
}
files_of() { sed -n 's/.*name="tests\/cases\/test_\([a-z0-9]*\)\.av".*/\1/p' "$1" | tr '\n' ','; }

sm green X=1
steps=$(ls "$w/green"/step-*.txt 2>/dev/null | sed 's#.*/step-##; s#\.txt##' | LC_ALL=C sort | tr '\n' ' ')
modes=$(cut -d'|' -f1 "$r/calls.txt" | LC_ALL=C sort | uniq -c | tr -s ' ' | tr '\n' ';')
[ $rc = 0 ] && [ "$steps" = "nursery plain stress1 stress2 stress3 stressx verify young " ] \
  && grep -q '^depth: the stress passes ran the 7 files of the plain pass, each once$' "$w/green/summary.txt" \
  && [ "$(tail -n 1 "$w/green.log")" = "suite-modes: every pass is green" ] \
  && [ "$modes" = " 1 nursery; 1 plain; 4 stress; 1 verify; 1 young;" ]
say modes-green $? "rc=$rc steps: $steps; modes: $modes; $(tail -n 3 "$w/green.log" | tr '\n' ';')"
[ "$(files_of "$w/green/junit-plain.xml")" = "a1,a2,a3,a4,a5,x1,x2," ] && [ "$(files_of "$w/green/junit-stress1.xml")" = "a1,a4," ] \
  && [ "$(files_of "$w/green/junit-stress2.xml")" = "a2,a5," ] && [ "$(files_of "$w/green/junit-stress3.xml")" = "a3," ] && [ "$(files_of "$w/green/junit-stressx.xml")" = "x1,x2," ]
say modes-slices $? "$(for s in plain stress1 stress2 stress3 stressx; do echo "$s=$(files_of "$w/green/junit-$s.xml")"; done | tr '\n' ' ')"

sm one SUITE_MODES_SLICES=1
steps=$(ls "$w/one"/step-*.txt 2>/dev/null | sed 's#.*/step-##; s#\.txt##' | LC_ALL=C sort | tr '\n' ' ')
[ $rc = 0 ] && [ "$steps" = "nursery plain stress1 verify young " ] && [ "$(files_of "$w/one/junit-stress1.xml")" = "a1,a2,a3,a4,a5,x1,x2," ] && grep -q '^depth: the stress passes ran the 7 files' "$w/one/summary.txt"
say modes-one-slice $? "rc=$rc steps: $steps; stress1: $(files_of "$w/one/junit-stress1.xml")"

echo 'FAIL-IN-MODE' >> "$r/tests/cases/test_a2.av"
sm red X=1
[ $rc = 1 ] && grep -q '^verify: rc=1 1 failed, 6 passed' "$w/red/summary.txt" && grep -q '^plain: rc=0 0 failed, 7 passed' "$w/red/summary.txt" && [ "$(grep -c ': rc=1 ' "$w/red/summary.txt")" = 1 ] \
  && [ "$(tail -n 1 "$w/red.log")" = "suite-modes: red" ]
say modes-red $? "rc=$rc $(cat "$w/red/summary.txt" 2>/dev/null | cut -c1-80 | tr '\n' ';')"
echo "test a2" > "$r/tests/cases/test_a2.av"

echo 'LEAVE-TMP' >> "$r/tests/cases/test_a3.av"
sm litter X=1
[ $rc = 1 ] && [ "$(grep -c 'left-in-tmp=1 ' "$w/litter/summary.txt")" = 5 ] && [ "$(tail -n 1 "$w/litter.log")" = "suite-modes: red" ]
say modes-litter $? "rc=$rc $(grep -c 'left-in-tmp=1' "$w/litter/summary.txt" 2>/dev/null) passes with litter; $(tail -n 1 "$w/litter.log")"
echo "test a3" > "$r/tests/cases/test_a3.av"

: > "$r/lose-file"
sm depth X=1
[ $rc = 1 ] && grep -q '^depth: DIFFERENT' "$w/depth/summary.txt"
say modes-depth $? "rc=$rc $(grep '^depth' "$w/depth/summary.txt" 2>/dev/null | cut -c1-200)"
rm -f "$r/lose-file"

printf '#!/bin/sh\necho "call: $*" >> "%s/runner-calls.log"\nexec "$@"\n' "$w" > "$w/call-runner"
sm runner SUITE_MODES_RUNNER="sh $w/call-runner"
[ $rc = 0 ] && [ "$(sed 's/^call: sh [^ ]*suite-modes.sh --step \([^ ]*\) .*/\1/' "$w/runner-calls.log" | tr '\n' ' ')" = "stressx stress1 stress2 stress3 plain young verify nursery " ]
say modes-runner $? "rc=$rc $(sed 's/^call: sh [^ ]*suite-modes.sh --step \([^ ]*\) .*/\1/' "$w/runner-calls.log" 2>/dev/null | tr '\n' ' ')"

sm limit X=1
[ "$(grep '^stress' "$r/calls.txt" | grep -c 'limit=1500')" = 4 ] && [ "$(grep -v '^stress' "$r/calls.txt" | grep -c 'limit=900')" = 4 ]
say modes-limit $? "$(cut -d'|' -f1,4 "$r/calls.txt" | tr '\n' ';')"

(cd "$r" && sh tools/suite-modes.sh < /dev/null > /dev/null 2>&1); u1=$?
(cd "$r" && sh tools/suite-modes.sh "$w/green" < /dev/null > /dev/null 2>&1); u2=$?
(cd "$r" && SUITE_MODES_SLICES=0 sh tools/suite-modes.sh "$w/u3" < /dev/null > /dev/null 2>&1); u3=$?
[ "$u1$u2$u3" = 222 ]
say modes-usage $? "$u1 $u2 $u3"
echo done
