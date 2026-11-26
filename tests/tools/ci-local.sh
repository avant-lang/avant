#!/bin/sh
# tools/ci-local.sh and tools/ci-local.py without a container: the plan and
# the job scripts the rehearsal writes, on the real workflows and on a small
# fake one. Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/ci-local.sh WORK
#
#   real-plan: the real seed, release and native workflows convert with
#     nothing the rehearsal cannot do, every job of each but release's
#     publish, each after the jobs it needs;
#   expr-env: ${{ env.X }}, ${{ github.workspace }} and A || B are replaced,
#     in the job's environment and in a step's script;
#   expr-hash: ${{ hashFiles('PATH') }} is the file's sha256;
#   step-order: the steps run in their order, each named, with the step's
#     own env;
#   if-output: an `if:` on a step's output is a shell test of that output;
#   if-always: `if: always()` runs after a failed step, a plain step not;
#   refuse-action: an action the rehearsal cannot do is refused, status 2,
#     named;
#   refuse-if: so is a condition it cannot read;
#   job-runs: the job script, run on this machine with /ci/ paths moved
#     under WORK, runs the steps, says each one's status and exits 1 when
#     one failed.
# Prints one line per check, `ok: NAME`, `skip: NAME: why` (no PyYAML) or
# `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/ci-local.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
rm -rf "$w"; mkdir -p "$w" || exit 2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
py=$root/tools/ci-local.py
# PyYAML reads the workflows; a machine without it (python3-yaml) skips every check, said
if ! python3 -I -c 'import yaml' 2> /dev/null; then
  for n in real-plan expr-env expr-hash step-order if-output if-always refuse-action refuse-if job-runs; do
    echo "skip: $n: python3 has no PyYAML here (python3-yaml)"
  done
  echo done; exit 0
fi

# real-plan
CI_LOCAL_DRY=1 sh "$root/tools/ci-local.sh" "$w/dry" seed release native > "$w/dry.out" 2>&1
rc=$?
want="seed:seed seed:install-check release:binary release:install-check native:native"
got=$(sed -n 's/^\([a-z-]*:[a-z-]*\): green (dry.*/\1/p' "$w/dry/summary.txt" | tr '\n' ' ' | sed 's/ $//')
[ $rc -eq 0 ] && [ "$got" = "$want" ]; say real-plan $? "status $rc, jobs '$got' ($(tail -3 "$w/dry.out" | tr '\n' ' '))"

# a fake checkout with one workflow
f=$w/repo; mkdir -p "$f/.github/workflows" "$f/tools"
printf 'x\n' > "$f/tools/pin.sh"
cat > "$f/.github/workflows/t.yml" << 'EOF'
name: t
env:
  TOP: top-value
jobs:
  one:
    runs-on: ubuntu-26.04
    env:
      ROOTED: ${{ github.workspace }}/sub
      PICK: ${{ github.event.inputs.tag || github.ref_name }}
      KEY: k-${{ hashFiles('tools/pin.sh') }}
    steps:
      - uses: actions/checkout@v7
      - name: First ${{ env.TOP }}
        env:
          OWN: own-value
        run: |
          echo "first $TOP $ROOTED $PICK $OWN ${{ env.TOP }}" >> /ci/out/trace
      - name: Cache it
        id: c
        uses: actions/cache@v4
        with:
          path: /ci/out/cached
          key: ${{ env.KEY }}
      - name: Only on a miss
        if: steps.c.outputs.cache-hit != 'true'
        run: echo "miss" >> /ci/out/trace
      - name: Fails
        run: exit 3
      - name: Not after a failure
        run: echo "not" >> /ci/out/trace
      - name: Always
        if: always()
        run: echo "always $KEY" >> /ci/out/trace
  bad-action:
    runs-on: ubuntu-26.04
    steps:
      - uses: someone/else@v1
  bad-if:
    runs-on: ubuntu-26.04
    steps:
      - name: x
        if: github.ref == 'refs/heads/master' && contains(x, y)
        run: true
EOF
mkdir -p "$w/one"
python3 -I "$py" "$f/.github/workflows/t.yml" one "$w/one" github.ref_name=v9.9.9 > "$w/one.meta" 2> "$w/one.err"
rc=$?
grep -q '^export ROOTED=/home/runner/work/avant/avant/sub$' "$w/one/job.sh" && grep -q '^export PICK=v9.9.9$' "$w/one/job.sh" \
  && grep -q 'echo "first $TOP $ROOTED $PICK $OWN top-value"' "$w/one/step-02.sh" && grep -q "First top-value" "$w/one/job.sh"
say expr-env $? "status $rc: $(cat "$w/one.err")"
sum=$(sha256sum "$f/tools/pin.sh" | cut -d' ' -f1)
grep -q "^export KEY=k-$sum\$" "$w/one/job.sh"; say expr-hash $? "$(grep KEY "$w/one/job.sh")"
grep -q '"${ci_out_c_cache_hit:-}" != true' "$w/one/job.sh"; say if-output $? "$(grep 'Only on a miss' "$w/one/job.sh")"
python3 -I "$py" "$f/.github/workflows/t.yml" bad-action "$w/ba" > /dev/null 2> "$w/ba.err"
rc=$?
[ $rc -eq 2 ] && grep -q "cannot rehearse the action 'someone/else@v1'" "$w/ba.err"; say refuse-action $? "status $rc: $(cat "$w/ba.err")"
mkdir -p "$w/bi"
python3 -I "$py" "$f/.github/workflows/t.yml" bad-if "$w/bi" > /dev/null 2> "$w/bi.err"
rc=$?
[ $rc -eq 2 ] && grep -q "cannot rehearse the condition" "$w/bi.err"; say refuse-if $? "status $rc: $(cat "$w/bi.err")"

# job-runs: the script with its container paths moved under WORK
run=$w/run; mkdir -p "$run/ws" "$run/out" "$run/cache" "$run/artifacts"
sed -e "s#/home/runner/work/avant/avant#$run/ws#g" -e "s#/home/runner/work/_temp#$run/temp#g" -e "s#/ci/job#$w/one#g" \
    -e "s#/ci/out#$run/out#g" -e "s#/ci/cache#$run/cache#g" -e "s#/ci/artifacts#$run/artifacts#g" "$w/one/job.sh" > "$run/job.sh"
for s in "$w/one"/step-*.sh; do sed -i "s#/ci/out#$run/out#g" "$s"; done
bash "$run/job.sh" > "$run/log" 2>&1 < /dev/null
rc=$?
order=$(sed -n 's/^ci-local: step \([0-9]*\) \([a-zA-Z]*\) .*/\1 \2/p' "$run/log" | tr '\n' ',')
[ $rc -eq 1 ] && [ "$order" = "1 ok,2 ok,3 ok,4 ok,5 FAILED,6 skipped,7 ok," ]; say job-runs $? "status $rc, steps '$order'"
trace=$(tr '\n' '|' < "$run/out/trace")
[ "$trace" = "first top-value $run/ws/sub v9.9.9 own-value top-value|miss|always k-$sum|" ]; say step-order $? "trace '$trace'"
grep -q 'ci-local: step 7 ok' "$run/log" && ! grep -q '^not$' "$run/out/trace"; say if-always $? "$(grep 'step [67]' "$run/log")"
echo done
