#!/bin/sh
# tools/gate.sh runs in a private /tmp (O693, the Valk stage, VALK-TM3).
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/gate-private-tmp.sh WORK [GATE]
#
# WORK is a new scratch directory; GATE is the gate script to test (the
# checkout's tools/gate.sh by default; give another one to show it on a
# base). The gate runs in a fake checkout (WORK/fake) whose letters,
# lint, README and format scripts are stubs and whose compilers are one stub
# script: `test` writes a JUnit file with one passing check, `__emit-ll`
# writes the same text for every letter. Two GC environments run as two
# concurrent passes. Two runs:
#   1. gone: an entry the caller made in /tmp before the gate started is
#      removed from outside while a GC pass runs (as another lane's
#      own entry is): the gate must pass;
#   2. litter: a GC pass leaves an entry in /tmp from inside: the gate
#      must fail on that pass's litter, and the entry must not reach the
#      caller's /tmp.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -ge 1 ] || { echo "usage: sh tests/tools/gate-private-tmp.sh WORK [GATE]" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
gate=${2:-$root/tools/gate.sh}
fake=$w/fake
mkdir -p "$fake/tools" "$fake/compiler" "$fake/runtime" "$fake/bin" "$w/seed"
: > "$fake/compiler/main.av"
: > "$fake/runtime/build.sh"
cp "$gate" "$fake/tools/gate.sh"
printf '#!/bin/sh\necho stub > /dev/null\n' > "$w/seed/avant"
chmod +x "$w/seed/avant"
cat > "$fake/tools/letters.sh" <<'STUB'
#!/bin/sh
# The stub letters: the seed, and each compiler a copy of the stub.
case "$1" in --print-seed) echo "$STUB_SEED"; exit 0 ;; esac
for l in "$@"; do
  case "$l" in b|c-llvm|d-llvm) cp tools/stub-avant "bin/avant-$l" && chmod +x "bin/avant-$l" ;; esac
done
STUB
printf '#!/bin/sh\necho "gc-lint: clean (stub)"\n' > "$fake/tools/gc-lint.sh"
printf '#!/bin/sh\necho "README CHECK PASS (stub)"\n' > "$fake/tools/readme-check.sh"
printf '#!/bin/sh\necho "README LINKS PASS (stub)"\n' > "$fake/tools/readme-links.sh"
printf '#!/bin/sh\necho "FORMAT CHECK PASS (stub)"\n' > "$fake/tools/format-check.sh"
cat > "$fake/tools/stub-avant" <<'STUB'
#!/bin/sh
# The stub compiler. STUB_WAIT=1: tell the caller this pass started
# (STUB_DIR/started) and wait for STUB_DIR/go, at most 60 s. STUB_LITTER=1: leave an
# entry in /tmp.
case "$1" in
  __emit-ll) echo "the same text" > "$3"; exit 0 ;;
  test) ;;
  *) exit 0 ;;
esac
junit=""
while [ $# -gt 0 ]; do [ "$1" = --junit ] && junit=$2; shift; done
if [ "${STUB_WAIT:-0}" = 1 ]; then
  : > "$STUB_DIR/started"
  n=0
  while [ ! -e "$STUB_DIR/go" ] && [ $n -lt 600 ]; do sleep 0.1; n=$((n + 1)); done
fi
[ "${STUB_LITTER:-0}" = 1 ] && : > "/tmp/gate-stub-litter-$STUB_TAG"
[ -n "$junit" ] && printf '<testsuites tests="1" failures="0">\n<testsuite name="stub" tests="1" failures="0" time="0.0"/>\n</testsuites>\n' > "$junit"
echo "0 failed, 1 passed"
exit 0
STUB
chmod +x "$fake/tools/stub-avant"

say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
tag=$$-$(date +%s%N)

# 1. gone: the caller's entry vanishes during a GC pass.
# The stub talks to this script through a directory in GATE_OUT, which
# the gate binds into its private /tmp when WORK lies under /tmp.
d1=$w/gone.out/stub; mkdir -p "$d1"
mine=$(mktemp -d /tmp/gate-test-mine-XXXXXX)
env -i PATH="$PATH" HOME="$HOME" STUB_SEED="$w/seed" STUB_DIR="$d1" STUB_TAG="$tag" GATE_OUT="$w/gone.out" GATE_GC_ENV="STUB_WAIT=1;STUB_PASS=2" \
  timeout -k 10 300 sh "$fake/tools/gate.sh" < /dev/null > "$w/gone.log" 2>&1 &
gp=$!
n=0
while [ ! -e "$d1/started" ] && [ $n -lt 600 ]; do
  kill -0 $gp 2>/dev/null || break
  sleep 0.1; n=$((n + 1))
done
[ -e "$d1/started" ]; say gone-pass-started $? "the GC pass never started ($w/gone.log)"
rmdir "$mine"
: > "$d1/go"
wait $gp; rc=$?
say gone-green $rc "status $rc: $(tail -1 "$w/gone.log")"
grep -q '^GATE PASS$' "$w/gone.out/summary.txt" 2>/dev/null; say gone-summary $? "no GATE PASS in summary.txt"
grep -q '^gate: private /tmp: a tmpfs in a mount namespace' "$w/gone.out/summary.txt" 2>/dev/null; say gone-private-line $? "the summary does not name the private /tmp"

# 2. litter: a GC pass leaves an entry in /tmp.
d2=$w/litter.out/stub; mkdir -p "$d2"
env -i PATH="$PATH" HOME="$HOME" STUB_SEED="$w/seed" STUB_DIR="$d2" STUB_TAG="$tag" GATE_OUT="$w/litter.out" GATE_GC_ENV="STUB_LITTER=1;STUB_PASS=2" \
  timeout -k 10 300 sh "$fake/tools/gate.sh" < /dev/null > "$w/litter.log" 2>&1
rc=$?
[ $rc -eq 1 ]; say litter-red $? "status $rc, want 1"
grep -q 'litter: new-c-llvm-gc-stub_litter_1.tmp has 1 entries' "$w/litter.out/summary.txt" 2>/dev/null; say litter-named $? "the summary does not name the pass's litter: $(tail -1 "$w/litter.out/summary.txt" 2>/dev/null)"
grep -q 'gate-stub-litter' "$w/litter.out/new-gc.tmp" 2>/dev/null; say litter-entry $? "new-gc.tmp does not list the entry"
[ ! -e "/tmp/gate-stub-litter-$tag" ]; say litter-stays-private $? "the entry reached the caller's /tmp"
rm -f "/tmp/gate-stub-litter-$tag"
echo done
