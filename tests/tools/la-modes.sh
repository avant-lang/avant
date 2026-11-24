#!/bin/sh
# tools/la-modes.sh on a fake checkout. Run by tests/cases/test_tools.av; by
# hand:
#
#   sh tests/tools/la-modes.sh WORK
#
# WORK is a new scratch directory. A fake checkout carries the real
# tools/la-modes.sh, a stub harness (tools/la-harness.sh: its probe and test
# passes stitch langarena/tasks/*.av and ask the compiler to build each, as
# the real one does, recording the pass it was asked for) and a stub C'
# (`compile SRC OUT ARGS...` writes a program that prints a line and acts on
# the task's text and on the environment).
#   modes-all-correct: three programs under the five modes: 15 runs, none
#     bad, status 0, the verdict line and summary.txt say so, every mode's
#     lines are there;
#   modes-stress-output: a program whose output differs under
#     AVANT_GC_STRESS=1 is named with its mode, status 1;
#   modes-verify-status: a program that fails under AVANT_GC_VERIFY=1 is
#     named with its status, status 1;
#   modes-hash-seed: every run has AVANT_HASH_SEED=0 and no other ambient
#     variable (the caller's AVANT_GC_STRESS never reaches the plain run);
#   modes-limit: a program over LA_MODES_LIMIT is a failed run (status
#     124), the others still run;
#   modes-compile-fails: a program the compiler refuses stops the run
#     before any run, status 2;
#   modes-slugs: named programs only;
#   modes-runner: with LA_MODES_RUNNER every piece of work (stitch, compile,
#     each mode) is one call of the runner, in order;
#   modes-size: LA_MODES_SIZE picks the harness's pass (probe, or test);
#   modes-usage: no argument, an existing OUT, a size or a mode list that
#     is not accepted: status 2.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/la-modes.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
# The caller's settings never reach the fake's runs.
unset LA_MODES LA_MODES_ROOT LA_MODES_SIZE LA_MODES_LIMIT LA_MODES_JOBS LA_MODES_RUNNER AVANT_GC_STRESS AVANT_GC_VERIFY AVANT_NURSERY AVANT_YOUNG_MB AVANT_HASH_SEED
r=$w/fake/avant
mkdir -p "$r/tools" "$r/bin" "$r/langarena/harness" "$r/langarena/tasks" "$w/fake/LangArena"
: > "$r/langarena/harness/measure.cr"
echo '[]' > "$w/fake/LangArena/run.js"
cp "$root/tools/la-modes.sh" "$r/tools/la-modes.sh"
printf 'task a\n' > "$r/langarena/tasks/a.av"
printf 'task b\n' > "$r/langarena/tasks/b.av"
printf 'task c\n' > "$r/langarena/tasks/c.av"
cat > "$r/tools/la-harness.sh" <<'STUB'
#!/bin/sh
# The stub harness: its pass (probe or test, the argument) stitches each task.
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
d=$root/langarena/tmp/harness; mkdir -p "$d"
cat > "$d/measure" <<'EOF'
#!/bin/sh
A=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
T=$A/langarena/tmp
[ -f "$A/../LangArena/run.js" ] || exit 2
echo "$1" >> "$STITCH_ARGS/../pass.txt"
for f in "$A"/langarena/tasks/*.av; do
  s=$(basename "$f" .av)
  cp "$f" "$T/$s.av"
  "$AVANT_COMPILER" compile "$T/$s.av" "$T/$s" --release --cpu=v3 > /dev/null 2>&1
done
exit 0
EOF
chmod +x "$d/measure"; echo "$d/measure"
STUB
cat > "$r/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
# compile SRC OUT ARGS...: a program that prints "ran SRC-NAME", and its environment's AVANT_ words and the
# caller's AVANT_GC_STRESS check, as the task's text asks.
[ "$1" = compile ] || exit 2
src=$2; out=$3
if grep -q REFUSE "$src"; then echo "error: refused"; exit 1; fi
{
  echo '#!/bin/sh'
  echo "name=$(basename "$src" .av)"
  echo 'echo "ran $name"'
  echo 'echo "seed=${AVANT_HASH_SEED:-none}" >&2; env | grep "^AVANT_" | sort | tr "\n" " " >&2; echo >&2'
  if grep -q STRESSOUT "$src"; then echo '[ "${AVANT_GC_STRESS:-}" = 1 ] && echo "different"'; fi
  if grep -q VERIFYFAIL "$src"; then echo '[ "${AVANT_GC_VERIFY:-}" = 1 ] && exit 3'; fi
  if grep -q SLOW "$src"; then echo 'sleep 30'; fi
  echo 'exit 0'
} > "$out"
chmod +x "$out"
STUB
chmod +x "$r/bin/avant-c-llvm"
# runm NAME ARGS...: la-modes.sh in the fake with ARGS, output in $w/NAME.{out,err}, its status in $rc. The
# environment it takes is set by the caller (and unset again by it).
runm() {
  n=$1; shift
  (cd "$r" && sh tools/la-modes.sh "$w/$n" "$@" > "$w/$n.out" 2> "$w/$n.err" < /dev/null); rc=$?
}

# 1. Every program under the five modes, correct.
runm all
runs=$w/all/runs.txt
[ $rc -eq 0 ] && [ "$(wc -l < "$runs")" = 15 ] && [ "$(tail -n 1 "$w/all.out")" = "la-modes: 15 runs, 0 bad" ] \
  && grep -qx 'bad 0' "$w/all/summary.txt" && grep -qx 'runs 15 over 3 programs' "$w/all/summary.txt" \
  && [ "$(for m in plain AVANT_GC_STRESS=1 AVANT_GC_VERIFY=1 AVANT_NURSERY=0 AVANT_YOUNG_MB=1; do grep -c " $m rc=0 " "$runs"; done | tr '\n' ' ')" = "3 3 3 3 3 " ] \
  && [ "$(cat "$w/all/p/pass.txt")" = probe ]
say modes-all-correct $? "rc=$rc $(tail -n 2 "$w/all.out" | tr '\n' ';') $(cat "$w/all.err" | head -c 300)"

# 2. An output that differs under stress.
printf 'task a STRESSOUT\n' > "$r/langarena/tasks/a.av"
runm stress
[ $rc -eq 1 ] && grep -q '^bad: a AVANT_GC_STRESS=1 (output [0-9a-f]* differs from the plain run, [0-9a-f]*)$' "$w/stress/summary.txt" \
  && [ "$(grep -c '^bad: ' "$w/stress/summary.txt")" = 1 ] && [ "$(tail -n 1 "$w/stress.out")" = "la-modes: 15 runs, 1 bad" ]
say modes-stress-output $? "rc=$rc $(cat "$w/stress/summary.txt" 2>/dev/null | tr '\n' ';')"
printf 'task a\n' > "$r/langarena/tasks/a.av"

# 3. A status that is not 0 under verify.
printf 'task b VERIFYFAIL\n' > "$r/langarena/tasks/b.av"
runm verify
[ $rc -eq 1 ] && grep -qx 'bad: b AVANT_GC_VERIFY=1 rc=3 (not rc=0)' "$w/verify/summary.txt" && [ "$(grep -c '^bad: ' "$w/verify/summary.txt")" = 1 ]
say modes-verify-status $? "rc=$rc $(cat "$w/verify/summary.txt" 2>/dev/null | tr '\n' ';')"
printf 'task b\n' > "$r/langarena/tasks/b.av"

# 4. The hash seed and an ambient mode.
AVANT_GC_STRESS=1; export AVANT_GC_STRESS
runm seed
unset AVANT_GC_STRESS
ok=1
for s in a b c; do
  [ "$(head -n 1 "$w/seed/run/$s.plain.err")" = "seed=0" ] && ! grep -q AVANT_GC_STRESS "$w/seed/run/$s.plain.err" || ok=0
  grep -q 'AVANT_GC_STRESS=1' "$w/seed/run/$s.AVANT_GC_STRESS=1.err" || ok=0
done
[ $ok = 1 ] && [ $rc -eq 0 ]
say modes-hash-seed $? "rc=$rc $(cat "$w/seed/run/a.plain.err" 2>/dev/null | tr '\n' ';')"

# 5. A run over the limit.
printf 'task c SLOW\n' > "$r/langarena/tasks/c.av"
LA_MODES_LIMIT=1 LA_MODES="plain AVANT_NURSERY=0"; export LA_MODES_LIMIT LA_MODES
runm limit a b c
unset LA_MODES_LIMIT LA_MODES
[ $rc -eq 1 ] && [ "$(grep -c '^bad: ' "$w/limit/summary.txt")" = 2 ] && grep -qx 'bad: c plain rc=124 (not rc=0)' "$w/limit/summary.txt" \
  && grep -q '^a plain rc=0 ' "$w/limit/runs.txt"
say modes-limit $? "rc=$rc $(cat "$w/limit/summary.txt" 2>/dev/null | tr '\n' ';')"
printf 'task c\n' > "$r/langarena/tasks/c.av"

# 6. A program the compiler refuses.
printf 'task b REFUSE\n' > "$r/langarena/tasks/b.av"
runm refused
[ $rc -eq 2 ] && [ ! -e "$w/refused/runs.txt" ] && grep -qx 'b rc=1' "$w/refused/compile.txt" && grep -q '1 failed' "$w/refused.out"
say modes-compile-fails $? "rc=$rc $(cat "$w/refused/compile.txt" 2>/dev/null | tr '\n' ';') $(cat "$w/refused.out" | tr '\n' ';')"
printf 'task b\n' > "$r/langarena/tasks/b.av"

# 7. Named programs only.
LA_MODES="plain AVANT_NURSERY=0"; export LA_MODES
runm slugs b
unset LA_MODES
[ $rc -eq 0 ] && [ "$(wc -l < "$w/slugs/runs.txt")" = 2 ] && ! grep -q '^a ' "$w/slugs/runs.txt" && [ "$(wc -l < "$w/slugs/compile.txt")" = 1 ]
say modes-slugs $? "rc=$rc $(cat "$w/slugs/runs.txt" 2>/dev/null | tr '\n' ';')"

# 8. Through a runner: one call for each piece of work.
printf '#!/bin/sh\necho "call: $*" >> "%s/runner.log"\nexec "$@"\n' "$w" > "$w/call-runner"
LA_MODES_RUNNER="sh $w/call-runner" LA_MODES="plain AVANT_NURSERY=0"; export LA_MODES_RUNNER LA_MODES
runm runner
unset LA_MODES_RUNNER LA_MODES
[ $rc -eq 0 ] && [ "$(sed 's/^call: sh [^ ]*la-modes.sh --step \([^ ]*\) .*/\1/' "$w/runner.log" | tr '\n' ' ')" = "stitch compile mode:plain mode:AVANT_NURSERY=0 " ] \
  && [ "$(wc -l < "$w/runner/runs.txt")" = 6 ]
say modes-runner $? "rc=$rc $(cat "$w/runner.log" 2>/dev/null | cut -c1-120 | tr '\n' ';')"

# 9. The harness's pass.
LA_MODES_SIZE=test LA_MODES=plain; export LA_MODES_SIZE LA_MODES
runm size a
unset LA_MODES_SIZE LA_MODES
[ $rc -eq 0 ] && [ "$(cat "$w/size/p/pass.txt")" = test ]
say modes-size $? "rc=$rc pass=$(cat "$w/size/p/pass.txt" 2>/dev/null)"

# 10. Usage.
(cd "$r" && sh tools/la-modes.sh < /dev/null > /dev/null 2>&1); u1=$?
(cd "$r" && sh tools/la-modes.sh "$w/all" < /dev/null > /dev/null 2>&1); u2=$?
(cd "$r" && LA_MODES_SIZE=huge sh tools/la-modes.sh "$w/u3" < /dev/null > /dev/null 2>&1); u3=$?
(cd "$r" && LA_MODES="AVANT_GC_STRESS=1" sh tools/la-modes.sh "$w/u4" < /dev/null > /dev/null 2>&1); u4=$?
[ "$u1$u2$u3$u4" = 2222 ]
say modes-usage $? "$u1 $u2 $u3 $u4"
echo done
