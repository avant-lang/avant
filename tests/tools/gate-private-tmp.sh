#!/bin/sh
# tools/gate.sh runs in a private /tmp.
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
# On a machine that refuses the namespace (Ubuntu 24.04's AppArmor
# rule for unprivileged user namespaces, GitHub's runners), these two
# print `skip: NAME: why` for each of their checks. Then, on every
# machine, the fallback, with an `unshare` first on PATH that refuses as
# Ubuntu's does: the gate snapshots the real /tmp (GATE_SNAP_TMP, a
# directory of WORK, stands for it, so the suite's other files coming
# and going in /tmp cannot disturb the checks):
#   3. fallback-green: a GC pass that leaves nothing: the gate passes and
#      its summary says the fallback ran and why;
#   4. fallback-litter: a GC pass leaves an entry: the gate fails on that
#      pass's litter and lists the entry;
#   5. fallback-gone: an entry is removed from the snapshotted directory
#      during a pass: the gate fails (the snapshot cannot tell whose it
#      was), naming the entry;
#   6. require: GATE_PRIVATE_TMP=require refuses to fall back;
#   7. bind-missing: when the runtime's build skipped avant-bind (its
#      notice), the gate stops after the runtime with that reason.
# Prints one line per check, `ok: NAME`, `skip: NAME: why` or
# `FAIL: NAME: why`, then `done`.
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
  # the runtime: the bind helper, or the build's notice without it
  if [ "$l" = runtime ]; then
    mkdir -p runtime/build; rm -f runtime/build/avant-bind
    if [ "${STUB_NO_BIND:-0}" = 1 ]; then echo "runtime/build.sh: skipped avant-bind: llvm-config-22 not found (avant bind needs llvm-config and libclang)" >&2
    else printf '#!/bin/sh\n' > runtime/build/avant-bind && chmod +x runtime/build/avant-bind; fi
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
[ "${STUB_LITTER:-0}" = 1 ] && : > "${STUB_LITTER_DIR:-/tmp}/gate-stub-litter-$STUB_TAG"
[ -n "$junit" ] && printf '<testsuites tests="1" failures="0">\n<testsuite name="stub" tests="1" failures="0" time="0.0"/>\n</testsuites>\n' > "$junit"
echo "0 failed, 1 passed"
exit 0
STUB
chmod +x "$fake/tools/stub-avant"

say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
tag=$$-$(date +%s%N)

# Whether this machine makes the namespace: the gate's own probe.
if unshare --user --map-current-user --mount --keep-caps sh -c 'mount -t tmpfs tmpfs /tmp' < /dev/null > "$w/probe.log" 2>&1; then
  ns=1
else
  ns=0
  why="this machine refuses the namespace ($(tail -1 "$w/probe.log")); the fallback's checks ran"
  for n in gone-pass-started gone-green gone-summary gone-private-line litter-red litter-named litter-entry litter-stays-private; do
    echo "skip: $n: $why"
  done
fi

if [ $ns = 1 ]; then
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
fi

# The fallback: an unshare that refuses as Ubuntu 24.04's does.
mkdir -p "$w/refuse" "$w/snap"
cat > "$w/refuse/unshare" <<'STUB'
#!/bin/sh
echo "unshare: write failed /proc/self/uid_map: Operation not permitted" >&2
exit 1
STUB
chmod +x "$w/refuse/unshare"
fb() { # NAME OUT EXTRA-ENV...: the stub gate, the namespace refused
  n=$1 o=$2; shift 2
  env -i PATH="$w/refuse:$PATH" HOME="$HOME" STUB_SEED="$w/seed" STUB_TAG="$tag" STUB_LITTER_DIR="$w/snap" GATE_SNAP_TMP="$w/snap" GATE_OUT="$o" "$@" \
    timeout -k 10 300 sh "$fake/tools/gate.sh" < /dev/null > "$o.log" 2>&1
}

# 3. fallback-green.
fb fallback-green "$w/fb-green.out" GATE_GC_ENV="STUB_PASS=1;STUB_PASS=2"
rc=$?
say fallback-green $rc "status $rc: $(tail -1 "$w/fb-green.out.log")"
grep -q '^GATE PASS$' "$w/fb-green.out/summary.txt" 2>/dev/null; say fallback-summary $? "no GATE PASS in summary.txt"
grep -q '^gate: private /tmp: none, this machine refuses the namespace (unshare: write failed /proc/self/uid_map: Operation not permitted): the real /tmp is snapshotted (fallback)$' "$w/fb-green.out/summary.txt" 2>/dev/null; say fallback-mode-line $? "the summary does not say the fallback ran and why: $(grep 'private /tmp' "$w/fb-green.out/summary.txt" 2>/dev/null)"

# 4. fallback-litter.
fb fallback-litter "$w/fb-litter.out" GATE_GC_ENV="STUB_LITTER=1;STUB_PASS=2"
rc=$?
[ $rc -eq 1 ]; say fallback-litter-red $? "status $rc, want 1"
grep -q 'litter: new-c-llvm-gc-stub_litter_1.tmp has 1 entries' "$w/fb-litter.out/summary.txt" 2>/dev/null; say fallback-litter-named $? "the summary does not name the pass's litter: $(tail -1 "$w/fb-litter.out/summary.txt" 2>/dev/null)"
grep -q "$w/snap/gate-stub-litter-$tag" "$w/fb-litter.out/new-gc.tmp" 2>/dev/null; say fallback-litter-entry $? "new-gc.tmp does not list the entry"
rm -f "$w/snap/gate-stub-litter-$tag"

# 5. fallback-gone: an entry of the snapshotted directory removed while a
# GC pass waits.
d5=$w/fb-gone.out/stub; mkdir -p "$d5"
mkdir "$w/snap/someone-else"
fb fallback-gone "$w/fb-gone.out" STUB_DIR="$d5" GATE_GC_ENV="STUB_WAIT=1;STUB_PASS=2" &
gp=$!
n=0
while [ ! -e "$d5/started" ] && [ $n -lt 600 ]; do
  kill -0 $gp 2>/dev/null || break
  sleep 0.1; n=$((n + 1))
done
[ -e "$d5/started" ]; say fallback-gone-started $? "the GC pass never started ($w/fb-gone.out.log)"
rmdir "$w/snap/someone-else"
: > "$d5/go"
wait $gp; rc=$?
[ $rc -eq 1 ]; say fallback-gone-red $? "status $rc, want 1"
grep -q 'litter: gone-c-llvm-gc-stub_wait_1.tmp has 1 entries' "$w/fb-gone.out/summary.txt" 2>/dev/null; say fallback-gone-named $? "the summary does not name the gone entry: $(tail -1 "$w/fb-gone.out/summary.txt" 2>/dev/null)"

# 6. require: no fallback.
fb require "$w/fb-require.out" GATE_PRIVATE_TMP=require
rc=$?
[ $rc -eq 1 ] && grep -q '^GATE FAIL: no private /tmp$' "$w/fb-require.out/summary.txt" 2>/dev/null; say require-refuses $? "status $rc: $(tail -1 "$w/fb-require.out/summary.txt" 2>/dev/null)"

# 7. bind-missing.
fb bind-missing "$w/fb-bind.out" STUB_NO_BIND=1
rc=$?
[ $rc -eq 1 ] && grep -q '^GATE FAIL: avant-bind was not built: skipped avant-bind: llvm-config-22 not found (avant bind needs llvm-config and libclang) (build-runtime.log)$' "$w/fb-bind.out/summary.txt" 2>/dev/null && [ ! -e "$w/fb-bind.out/suite-b.log" ]
say bind-missing $? "status $rc: $(tail -1 "$w/fb-bind.out/summary.txt" 2>/dev/null)"
echo done
