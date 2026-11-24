#!/bin/sh
# tools/gate.sh's sharded GC passes: an entry of GATE_GC_ENV with SHARDS=N
# stands for N passes, each a slice of the suite. Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/gate-shards.sh WORK
#
# WORK is a new scratch directory. The gate runs in a fake checkout whose
# letters, lint, README and format scripts are stubs and whose compilers are
# one stub script: `test` records its arguments and the environment words
# it was given in WORK/calls.txt and writes a JUnit file with one passing
# check. The checks (GATE_PRIVATE_TMP=0: the real /tmp is not the point):
#   shards-passes: 'STUB_MODE=a SHARDS=3;STUB_MODE=b' runs a's three slices
#     (--shard 1/3, 2/3, 3/3, each once, each with --skip-exclusive), its one
#     pass of the files that run alone (--only-exclusive) and b's one pass
#     without a slice, every pass with its mode's environment and none with a
#     variable named SHARD or SHARDS; the gate passes;
#   shards-default-passes: the two default passes (B and C') take no slice;
#   shards-labels: the summary names each slice's pass and its words
#     (`c-llvm-gc-stub_mode_a_shard_1_3 (STUB_MODE=a SHARD=1/3)`);
#   shards-limit: tests/limits.txt's limit for the mode holds for each slice;
#   shards-one: SHARDS=1 is the plain pass, without a slice;
#   shards-sequential: with GATE_GC_OVERLAP=1 the slices run one after
#     another and the gate passes the same way.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -ge 1 ] || { echo "usage: sh tests/tools/gate-shards.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
fake=$w/fake
mkdir -p "$fake/tools" "$fake/compiler" "$fake/runtime" "$fake/bin" "$fake/tests/cases" "$w/seed" "$w/snap"
: > "$fake/bin/re2c"; chmod +x "$fake/bin/re2c"
: > "$fake/compiler/main.av"
: > "$fake/runtime/build.sh"
: > "$fake/tests/cases/test_a.av"
cp "$root/tools/gate.sh" "$fake/tools/gate.sh"
cp "$root/tools/suite-limit.sh" "$fake/tools/suite-limit.sh"
printf 'tests/cases/test_a.av STUB_MODE=a 1500 a long file\n' > "$fake/tests/limits.txt"
printf '#!/bin/sh\necho stub > /dev/null\n' > "$w/seed/avant"
chmod +x "$w/seed/avant"
cat > "$fake/tools/letters.sh" <<'STUB'
#!/bin/sh
case "$1" in --print-seed) echo "$STUB_SEED"; exit 0 ;; esac
for l in "$@"; do
  case "$l" in b|c-llvm|d-llvm) cp tools/stub-avant "bin/avant-$l" && chmod +x "bin/avant-$l" ;; esac
  if [ "$l" = runtime ]; then
    mkdir -p runtime/build
    printf '#!/bin/sh\n' > runtime/build/avant-bind && chmod +x runtime/build/avant-bind
  fi
done
STUB
printf '#!/bin/sh\necho "gc-lint: clean (stub)"\n' > "$fake/tools/gc-lint.sh"
printf '#!/bin/sh\necho "README CHECK PASS (stub)"\n' > "$fake/tools/readme-check.sh"
printf '#!/bin/sh\necho "README LINKS PASS (stub)"\n' > "$fake/tools/readme-links.sh"
printf '#!/bin/sh\necho "FORMAT CHECK PASS (stub)"\n' > "$fake/tools/format-check.sh"
printf '#!/bin/sh\necho "NO LEDGER REFS PASS (stub)"\n' > "$fake/tools/no-ledger-refs.sh"
cat > "$fake/tools/stub-avant" <<'STUB'
#!/bin/sh
# The stub compiler: a test pass records "which: arguments | STUB_MODE | SHARD
# variables" in $STUB_CALLS; the letters' compilers all run through it.
case "$1" in
  __emit-ll) echo "the same text" > "$3"; exit 0 ;;
  test) ;;
  *) exit 0 ;;
esac
shift
junit=""
a=$*
while [ $# -gt 0 ]; do [ "$1" = --junit ] && junit=$2; shift; done
printf '%s | mode=%s | shardvars=%s%s\n' "$a" "${STUB_MODE:-none}" "${SHARD:-}" "${SHARDS:-}" >> "$STUB_CALLS"
[ -n "$junit" ] && printf '<testsuites tests="1" failures="0">\n<testsuite name="stub" tests="1" failures="0" time="0.0"/>\n</testsuites>\n' > "$junit"
echo "0 failed, 1 passed"
exit 0
STUB
chmod +x "$fake/tools/stub-avant"

say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
gate() { # NAME GC_ENV [VAR=value...]: the stub gate, calls in $w/NAME.calls, status in $rc
  n=$1 e=$2; shift 2
  : > "$w/$n.calls"
  env -i PATH="$PATH" HOME="${HOME:-/}" STUB_SEED="$w/seed" STUB_CALLS="$w/$n.calls" GATE_PRIVATE_TMP=0 GATE_SNAP_TMP="$w/snap" GATE_OUT="$w/$n.out" GATE_GC_ENV="$e" "$@" \
    timeout -k 10 300 sh "$fake/tools/gate.sh" < /dev/null > "$w/$n.log" 2>&1
  rc=$?
}
sorted() { sort "$1" | tr '\n' ';'; }

gate sh 'STUB_MODE=a SHARDS=3;STUB_MODE=b'
c=$w/sh.calls
shards=$(grep 'mode=a' "$c" | sed -n 's/.*--shard \([0-9]*\/[0-9]*\) --skip-exclusive.*/\1/p' | sort | tr '\n' ' ')
[ $rc = 0 ] && [ "$shards" = "1/3 2/3 3/3 " ] && [ "$(grep -c 'mode=a' "$c")" = 4 ] && [ "$(grep 'mode=a' "$c" | grep -c -e '--only-exclusive')" = 1 ] \
  && [ "$(grep -c 'mode=b' "$c")" = 1 ] && ! grep 'mode=b' "$c" | grep -q -e '--shard\|--only-exclusive\|--skip-exclusive' && ! grep -q 'shardvars=[^ ]' "$c"
say shards-passes $? "status $rc; $(sorted "$c" | cut -c1-500); $(tail -n 2 "$w/sh.log" | tr '\n' ';')"
[ "$(grep -c 'mode=none' "$c")" = 2 ] && ! grep 'mode=none' "$c" | grep -q -e '--shard\|--only-exclusive\|--skip-exclusive'
say shards-default-passes $? "$(grep 'mode=none' "$c" | tr '\n' ';')"
bad=""
for i in 1 2 3; do
  grep -q "^suite c-llvm-gc-stub_mode_a_shard_${i}_3 (STUB_MODE=a SHARD=$i/3): " "$w/sh.out/summary.txt" || bad=1
done
grep -q '^suite c-llvm-gc-stub_mode_a_shard_exclusive (STUB_MODE=a SHARD=exclusive): ' "$w/sh.out/summary.txt" || bad=1
grep -q '^suite c-llvm-gc-stub_mode_b (STUB_MODE=b): ' "$w/sh.out/summary.txt" && [ -z "$bad" ]
say shards-labels $? "$(grep '^suite ' "$w/sh.out/summary.txt" | cut -c1-90 | tr '\n' ';')"
[ "$(grep '^suite c-llvm-gc-stub_mode_a_shard' "$w/sh.out/summary.txt" | grep -c 'limit=1500')" = 4 ] && grep '^suite c-llvm-gc-stub_mode_b ' "$w/sh.out/summary.txt" | grep -q 'limit=900'
say shards-limit $? "$(grep '^suite c-llvm-gc' "$w/sh.out/summary.txt" | sed 's/.* limit=/limit=/' | tr '\n' ' ')"

gate one 'STUB_MODE=a SHARDS=1'
[ $rc = 0 ] && [ "$(grep -c 'mode=a' "$w/one.calls")" = 1 ] && ! grep 'mode=a' "$w/one.calls" | grep -q -e '--shard' && grep -q '^suite c-llvm-gc (STUB_MODE=a): ' "$w/one.out/summary.txt"
say shards-one $? "status $rc; $(grep 'mode=a' "$w/one.calls" | tr '\n' ';'); $(grep '^suite ' "$w/one.out/summary.txt" | cut -c1-60 | tr '\n' ';')"

gate seq 'STUB_MODE=a SHARDS=2' GATE_GC_OVERLAP=1
shards=$(grep 'mode=a' "$w/seq.calls" | sed -n 's/.*--shard \([0-9]*\/[0-9]*\) --skip-exclusive.*/\1/p' | tr '\n' ' ')
[ $rc = 0 ] && [ "$shards" = "1/2 2/2 " ] && [ "$(grep -c -e '--only-exclusive' "$w/seq.calls")" = 1 ]
say shards-sequential $? "status $rc; shards: $shards"
echo done
