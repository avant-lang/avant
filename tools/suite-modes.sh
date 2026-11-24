#!/bin/sh
# suite-modes.sh — the whole test suite under every collector mode, in steps
# that each fit one shared run.
#
#   sh tools/suite-modes.sh OUT     (from the checkout root)
#
# A change to the runtime, the collector or the code generator runs every
# collector mode over the whole suite, not only over the collector's own
# test files: two latent bugs of one round showed in a test no collector lane
# had run under the verifier. C' (bin/avant-c-llvm) runs `avant test` over
# every test file plain and under AVANT_YOUNG_MB=1, AVANT_GC_VERIFY=1,
# AVANT_NURSERY=0 and AVANT_GC_STRESS=1, one pass per mode. The stress pass is
# the long one (three to five times any other, 14 minutes in one run on a
# loaded machine, past the 10 minutes a shared run may hold the machine), so
# it is split into SUITE_MODES_SLICES slices (default 3) of the files that may
# run beside others (`--shard I/N --skip-exclusive`) and one pass of the files
# that must run alone (`--only-exclusive`): together every file once. Each
# pass is a step of its own, one shared run through SUITE_MODES_RUNNER, with
# its own TMPDIR, which must be empty afterwards.
#
#   OUT/suite-STEP.log, junit-STEP.xml, time-STEP.txt   each step's run
#   OUT/summary.txt                                     one line per step
#
# STEP is plain, young, verify, nursery, stress1 to stressN and stressx. The
# last lines say whether every pass is green and whether the stress slices and
# the exclusive pass together ran the files the plain pass ran, each once (the
# depth check: a slice list that lost a file would be a lighter mode, not a
# faster one). Status 0 when every step is green and the files agree, 1
# otherwise, 2 on a usage error.
#
# Environment:
#   SUITE_MODES_RUNNER  the runner's command words (such as `python3 -I
#                       PATH/ledger run --shared --wave W --`): each step runs
#                       through it
#   SUITE_MODES_JOBS    avant test --jobs of each step (default: the CPUs)
#   SUITE_MODES_SLICES  slices of the stress pass (default 3; 1: not split)
#   SUITE_MODES_STEPS   the steps to run, in order (default: all)
# Each pass's limit is tools/suite-limit.sh's for its mode (tests/limits.txt).
# Stdin from /dev/null.
set -u
step=""
if [ "${1:-}" = --step ]; then step=${2:-}; shift 2; fi
[ $# -eq 1 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
cd "$root" || exit 2
out=$(realpath -m "$1")
slices=${SUITE_MODES_SLICES:-3}
case "$slices" in ''|*[!0-9]*|0) echo "suite-modes.sh: SUITE_MODES_SLICES is a number from 1" >&2; exit 2 ;; esac
jobs=${SUITE_MODES_JOBS:-$(nproc 2> /dev/null || echo 2)}
runner=${SUITE_MODES_RUNNER:-}
stress_steps=stress1
if [ "$slices" -gt 1 ]; then
  stress_steps=stressx
  k=1; while [ "$k" -le "$slices" ]; do stress_steps="$stress_steps stress$k"; k=$((k + 1)); done
fi
all_steps="$stress_steps plain young verify nursery"
# The long steps first, as the gate's list does.
steps=${SUITE_MODES_STEPS:-$all_steps}

# step_env STEP: the environment words of a step, step_args its avant test arguments.
step_env() {
  case "$1" in
    plain) echo "" ;;
    young) echo "AVANT_YOUNG_MB=1" ;;
    verify) echo "AVANT_GC_VERIFY=1" ;;
    nursery) echo "AVANT_NURSERY=0" ;;
    stress*) echo "AVANT_GC_STRESS=1" ;;
    *) return 1 ;;
  esac
}
step_args() {
  case "$1" in
    stressx) echo "--only-exclusive" ;;
    stress[0-9]*) if [ "$slices" -gt 1 ]; then echo "--shard ${1#stress}/$slices --skip-exclusive"; else echo ""; fi ;;
    *) echo "" ;;
  esac
}

do_step() {
  n=$1
  e=$(step_env "$n") || { echo "suite-modes.sh: no step $n" >&2; exit 2; }
  a=$(step_args "$n")
  [ -x bin/avant-c-llvm ] || { echo "suite-modes.sh: no C' (bin/avant-c-llvm) in $root" >&2; exit 2; }
  # shellcheck disable=SC2086
  lim=$(sh tools/suite-limit.sh $e 2> "$out/limit-$n.txt") || { echo "suite-modes.sh: $n: $(cat "$out/limit-$n.txt")" >&2; exit 2; }
  t=$out/tmp-$n; rm -rf "$t"; mkdir -p "$t"
  # shellcheck disable=SC2086
  env -u AVANT_BACKEND -u AVANT_COMPILER TMPDIR="$t" $e timeout -k 10 "$((lim + 120))" /usr/bin/time -p -o "$out/time-$n.txt" ./bin/avant-c-llvm test --jobs "$jobs" $a --file-limit "$lim" --junit "$out/junit-$n.xml" > "$out/suite-$n.log" 2>&1 < /dev/null
  rc=$?
  echo "rc=$rc" >> "$out/suite-$n.log"
  left=$(find "$t" -mindepth 1 | wc -l)
  [ "$left" -eq 0 ] && rmdir "$t"
  echo "$n $rc $left $lim" > "$out/step-$n.txt"
  [ $rc -eq 0 ] && [ "$left" -eq 0 ]
}

case "$step" in
  "") ;;
  *) do_step "$step"; exit $? ;;
esac
[ -e "$out" ] && { echo "suite-modes.sh: $out exists" >&2; exit 2; }
mkdir -p "$out" || exit 2
bad=0
for n in $steps; do
  if [ -n "$runner" ]; then
    # shellcheck disable=SC2086
    $runner sh "$0" --step "$n" "$out" < /dev/null
  else sh "$0" --step "$n" "$out" < /dev/null; fi || bad=$((bad + 1))
done
: > "$out/summary.txt"
for n in $steps; do
  [ -f "$out/step-$n.txt" ] || { echo "$n: did not run" >> "$out/summary.txt"; bad=$((bad + 1)); continue; }
  read -r _ rc left lim < "$out/step-$n.txt"
  printf '%s: rc=%s %s %s tests=%s failures=%s left-in-tmp=%s limit=%s\n' "$n" "$rc" \
    "$(grep -E 'failed, .* passed' "$out/suite-$n.log" | tail -n 1)" "$(tr '\n' ' ' < "$out/time-$n.txt" 2> /dev/null | sed 's/ $//')" \
    "$(grep -o 'tests="[0-9]*"' "$out/junit-$n.xml" 2> /dev/null | head -n 1 | tr -dc '0-9')" "$(grep -o 'failures="[0-9]*"' "$out/junit-$n.xml" 2> /dev/null | head -n 1 | tr -dc '0-9')" "$left" "$lim" >> "$out/summary.txt"
done
# The depth check: the files the stress slices and the exclusive pass ran are
# the files the plain pass ran, each once.
# (from the log's `ok FILE` and `FAIL FILE` lines: a file that records no check under a mode, as the
# tools' shell tests do under a collector mode, has no suite in the JUnit file but still runs and reports)
names_of() { sed -n 's/^\(ok\|FAIL\) \(tests\/[^: ]*\.av\).*/\2/p' "$out/suite-$1.log" 2> /dev/null; }
case " $steps " in
  *" plain "*stress*|*stress*" plain "*)
    names_of plain | LC_ALL=C sort > "$out/files-plain.txt"
    : > "$out/files-stress.txt"
    for n in $steps; do case "$n" in stress*) names_of "$n" >> "$out/files-stress.txt" ;; esac; done
    LC_ALL=C sort -o "$out/files-stress.txt" "$out/files-stress.txt"
    if [ -s "$out/files-plain.txt" ] && cmp -s "$out/files-plain.txt" "$out/files-stress.txt"; then
      echo "depth: the stress passes ran the $(wc -l < "$out/files-plain.txt") files of the plain pass, each once" >> "$out/summary.txt"
    else
      echo "depth: DIFFERENT, the stress passes' files differ from the plain pass's ($(LC_ALL=C comm -3 "$out/files-plain.txt" "$out/files-stress.txt" | tr -d '\t' | tr '\n' ' ' | cut -c1-300))" >> "$out/summary.txt"
      bad=$((bad + 1))
    fi ;;
esac
cat "$out/summary.txt"
[ "$(grep -c -v ': rc=0 .* left-in-tmp=0 \|^depth: the stress' "$out/summary.txt")" -eq 0 ] || bad=$((bad + 1))
if [ "$bad" -eq 0 ]; then echo "suite-modes: every pass is green"; exit 0; fi
echo "suite-modes: red"; exit 1
