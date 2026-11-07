#!/bin/sh
# The standard gate (Stage 28, from the Wave T gate.sh/snap.sh; D127, D138).
#
#   sh tools/gate.sh        (any directory; stdin from /dev/null)
#
# One mode (Stage 28 W11: the compiler has one back end): rebuilds the
# runtime, B (the seed builds it, on the seed's own back end), C' (B
# compiles the compiler, bin/avant-c-llvm); runs the suite on B and on C'
# with AVANT_BACKEND unset (so the suite proves what users get), with
# JUnit and /usr/bin/time -p; snapshots /tmp and the repository before and
# after each suite (no litter); builds D' (C' compiles the compiler,
# bin/avant-d-llvm); checks the fixed point (the pre-optimization .ll of
# compiler/main.av, `avant __emit-ll`, from C' and from D', byte for byte)
# and C' = D' byte for byte. Prints one summary line per suite, the fixed
# point, and a final `GATE PASS` or `GATE FAIL: ...` line; exits 0 only on
# PASS (1 on any failure, 2 on usage).
#
# Environment:
#   GATE_SEED       seed directory holding avant (default bin/seeds/s28-w2
#                   when present, D189, else bin/v0.34.0); a seed from before
#                   W10 also holds the myc-llvm it compiles through
#                   (tools/letters.sh passes it to step b only)
#   GATE_OUT        output directory (default results/gate-YYYYmmdd-HHMMSS);
#                   keep it under results/ or outside the repository, since the
#                   repository snapshot excludes only .git/ and results/
#   GATE_REUSE_C    unset: B builds C'. "1": C' is the compiler the B suite's
#                   test_fixed_point kept (AVANT_FIXED_POINT_KEEP, D138), else
#                   B builds it. "proof": keeps it, builds C' too, and records
#                   whether the two are byte-identical (a difference fails).
#   GATE_SUITE_ENV  extra VAR=value words passed to both suites only (e.g.
#                   a GC environment).
#   GATE_EMIT_BOUND seconds (default 20): the summary reports the C' (c-llvm)
#                   __emit-ll time of compiler/main.av within or OVER it;
#                   report only, never a failure (W11d).
#
# Every child runs under `timeout -k 10` (runtime 300 s, compiler build and
# dump 900 s, suite 900 s) with stdin from /dev/null; a timeout is a failure.
# The runtime, B, C', and D' are built by tools/letters.sh (the one copy
# of the letters' incantation, with those timeouts inside). The suites run
# with no AVANT_BACKEND.
#
# Output files in GATE_OUT (L is b and c-llvm): build-runtime.log,
# build-L.log and time-build-L.txt; suite-L.log, junit-L.xml, time-L.txt;
# {before,after}-L.{tmp,repo} snapshots and the new-/gone- differences;
# build-d-llvm.log, time-build-d-llvm.txt, emit-{c,d}-llvm.log,
# time-emit-{c,d}-llvm.txt;
# fixed-point.txt; identity.txt (C' against D'); reuse.txt when
# GATE_REUSE_C is set; summary.txt (everything printed at the end).
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
[ -f compiler/main.av ] && [ -f runtime/build.sh ] || { echo "gate: $root is not an avant checkout" >&2; exit 2; }

# Default seed (D189): the local seed when present, else the released one.
default_seed=bin/v0.34.0
[ -x bin/seeds/s28-w2/avant ] && default_seed=bin/seeds/s28-w2
seed=$(realpath -m "${GATE_SEED:-$default_seed}")
[ -x "$seed/avant" ] || { echo "gate: no avant in $seed" >&2; exit 2; }
out=$(realpath -m "${GATE_OUT:-results/gate-$(date +%Y%m%d-%H%M%S)}")
mkdir -p "$out" || exit 2
REUSE=${GATE_REUSE_C:-}
case "$REUSE" in ''|1|proof) ;; *) echo "gate: GATE_REUSE_C must be unset, 1, or proof" >&2; exit 2 ;; esac
SUITE_ENV=${GATE_SUITE_ENV:-}
# The bound the emit-ll report line compares the C' __emit-ll time of
# compiler/main.av with (seconds; report only, W11d: 10.6 s measured).
EMIT_BOUND=${GATE_EMIT_BOUND:-20}
case "${GATE_BACKEND:-llvm}" in
  llvm) ;;
  *) echo "gate: GATE_BACKEND=${GATE_BACKEND} is gone (Stage 28 W11: one back end, one gate)" >&2; exit 2 ;;
esac
C=c-llvm

fails=""
fail() { fails="$fails${fails:+; }$1"; echo "gate: $1"; }
finish() {
  if [ -z "$fails" ]; then echo "GATE PASS" | tee -a "$out/summary.txt"; exit 0; fi
  echo "GATE FAIL: $fails" | tee -a "$out/summary.txt"; exit 1
}

# snap PREFIX: PREFIX.tmp (top-level /tmp entries, excluding the agent
# harness's own claude-* directory) and PREFIX.repo (every repository path,
# ignored files included, except .git/ and results/).
snap() {
  find /tmp -mindepth 1 -maxdepth 1 ! -name 'claude-*' | LC_ALL=C sort > "$1.tmp"
  find . -path ./.git -prune -o -path ./results -prune -o -print | LC_ALL=C sort > "$1.repo"
}

echo "gate: root=$root seed=$seed out=$out reuse=${REUSE:-no} suite-env=${SUITE_ENV:-none}" | tee "$out/summary.txt"

# The letters come from tools/letters.sh (its own timeouts: runtime 300 s,
# each compiler build 900 s; stdin /dev/null).
letters() { LETTERS_SEED=$seed LETTERS_TIME_DIR=$out sh tools/letters.sh "$@" < /dev/null; }
letters runtime > "$out/build-runtime.log" 2>&1 || { fail "runtime build failed (rc=$?)"; finish; }
letters b > "$out/build-b.log" 2>&1 || { fail "B build failed (rc=$?)"; finish; }

build_c() {
  letters $C > "$out/build-$C.log" 2>&1 || { fail "$C build failed (rc=$?)"; finish; }
}
rm -f "$out/kept-c" "$out/reuse.txt"
[ -z "$REUSE" ] && build_c

for L in b $C; do
  if [ "$L" = "$C" ] && [ -n "$REUSE" ]; then
    if [ "$REUSE" = proof ]; then
      build_c
      if [ -f "$out/kept-c" ] && cmp -s "$out/kept-c" bin/avant-$C; then
        echo "reuse proof: kept C is byte-identical to the gate-built C" > "$out/reuse.txt"
      else
        echo "reuse proof: kept C DIFFERS from the gate-built C (or none was kept)" > "$out/reuse.txt"
        fail "reuse proof differs"
      fi
      sha256sum "$out/kept-c" bin/avant-$C 2>/dev/null | sed "s#$out/##" >> "$out/reuse.txt"
    elif [ -f "$out/kept-c" ]; then
      cp -f "$out/kept-c" bin/avant-$C && echo "reuse: C is the fixed point's kept C" > "$out/reuse.txt"
    else
      build_c
      echo "reuse: no kept C; C built by B" > "$out/reuse.txt"
    fi
    rm -f "$out/kept-c"
  fi
  keep=""
  [ "$L" = b ] && [ -n "$REUSE" ] && keep="AVANT_FIXED_POINT_KEEP=$out/kept-c"
  rm -f "$out/junit-$L.xml" "$out/time-$L.txt"
  snap "$out/before-$L"
  # shellcheck disable=SC2086 # SUITE_ENV and keep are VAR=value words
  env -u AVANT_BACKEND $keep $SUITE_ENV timeout -k 10 900 /usr/bin/time -p -o "$out/time-$L.txt" ./bin/avant-$L test --junit "$out/junit-$L.xml" > "$out/suite-$L.log" 2>&1 < /dev/null
  rc=$?
  echo "rc=$rc" >> "$out/suite-$L.log"
  [ $rc -eq 0 ] || fail "suite on $L failed (rc=$rc)"
  snap "$out/after-$L"
  LC_ALL=C comm -13 "$out/before-$L.tmp" "$out/after-$L.tmp" > "$out/new-$L.tmp"
  LC_ALL=C comm -23 "$out/before-$L.tmp" "$out/after-$L.tmp" > "$out/gone-$L.tmp"
  LC_ALL=C comm -13 "$out/before-$L.repo" "$out/after-$L.repo" > "$out/new-$L.repo"
  LC_ALL=C comm -23 "$out/before-$L.repo" "$out/after-$L.repo" > "$out/gone-$L.repo"
  for k in new-$L.tmp gone-$L.tmp new-$L.repo gone-$L.repo; do
    [ -s "$out/$k" ] && fail "litter: $k has $(wc -l < "$out/$k") entries"
  done
done

# D' by C' (never C' by itself), then the .ll fixed point (D171) and
# the executables' identity.
letters d-llvm > "$out/build-d-llvm.log" 2>&1 || { fail "d-llvm build failed (rc=$?)"; finish; }
for L in c-llvm d-llvm; do
  env -u AVANT_BACKEND timeout -k 10 900 /usr/bin/time -p -o "$out/time-emit-$L.txt" ./bin/avant-$L __emit-ll compiler/main.av "$out/main-$L.ll" > "$out/emit-$L.log" 2>&1 < /dev/null || fail "__emit-ll on $L failed (rc=$?)"
done
if cmp -s "$out/main-c-llvm.ll" "$out/main-d-llvm.ll"; then
  echo "fixed point (.ll): identical" > "$out/fixed-point.txt"
else
  echo "fixed point (.ll): DIFFER" > "$out/fixed-point.txt"
  fail "fixed point (.ll) differs"
fi
sha256sum "$out/main-c-llvm.ll" "$out/main-d-llvm.ll" | sed "s#$out/##" >> "$out/fixed-point.txt"
rm -f "$out/main-c-llvm.ll" "$out/main-d-llvm.ll"
if cmp -s bin/avant-c-llvm bin/avant-d-llvm; then
  echo "C' = D': byte-identical" > "$out/identity.txt"
else
  echo "C' = D': DIFFER" > "$out/identity.txt"
  fail "C' and D' differ"
fi

for L in b $C; do
  jf=$(grep -o 'failures="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
  [ "$jf" = 'failures="0"' ] || fail "junit on $L: ${jf:-no failures count}"
done

ptimes() { [ -f "$1" ] && grep -E '^(real|user|sys) ' "$1" | tr '\n' ' '; }
{
  echo "build B: $(ptimes "$out/time-build-b.txt")"
  [ -f "$out/time-build-$C.txt" ] && echo "build $C: $(ptimes "$out/time-build-$C.txt")"
  [ -f "$out/time-build-d-llvm.txt" ] && echo "build d-llvm: $(ptimes "$out/time-build-d-llvm.txt")"
  # Report only, never a failure (W11d): the compile time of the
  # compiler's own source, against a stated bound.
  if [ -f "$out/time-emit-c-llvm.txt" ]; then
    er=$(awk '$1 == "real" { print $2 }' "$out/time-emit-c-llvm.txt")
    eo=$(awk -v r="$er" -v b="$EMIT_BOUND" 'BEGIN { print (r + 0 <= b + 0) ? "within" : "OVER" }')
    echo "emit-ll c-llvm: real ${er} s, ${eo} the report bound of ${EMIT_BOUND} s (report only)"
  fi
  for L in b $C; do
    checks=$(grep -o 'tests="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    jf=$(grep -o 'failures="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    echo "suite $L: $(grep -E 'failed, .* passed' "$out/suite-$L.log" | tail -1) $checks $jf $(ptimes "$out/time-$L.txt")$(grep -E '^rc=' "$out/suite-$L.log") new-tmp=$(wc -l < "$out/new-$L.tmp") gone-tmp=$(wc -l < "$out/gone-$L.tmp") new-repo=$(wc -l < "$out/new-$L.repo") gone-repo=$(wc -l < "$out/gone-$L.repo")"
  done
  cat "$out/fixed-point.txt"
  [ -f "$out/identity.txt" ] && cat "$out/identity.txt"
  [ -f "$out/reuse.txt" ] && cat "$out/reuse.txt"
  sha256sum bin/avant-b bin/avant-c-llvm bin/avant-d-llvm
} | tee -a "$out/summary.txt"
finish
