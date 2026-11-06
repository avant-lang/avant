#!/bin/sh
# The standard gate (Stage 28, from the Wave T gate.sh/snap.sh; D127, D138).
#
#   sh tools/gate.sh        (any directory; stdin from /dev/null)
#
# Rebuilds the runtime, B (the seed builds it), C (B builds it); runs the
# suite on B and on C with JUnit and /usr/bin/time -p; snapshots /tmp and the
# repository before and after each suite (no litter); checks the fixed point
# (`dump compiler/main.av` from B and from C, byte for byte). Prints one
# summary line per suite, the fixed point, and a final `GATE PASS` or
# `GATE FAIL: ...` line; exits 0 only on PASS (1 on any failure, 2 on usage).
#
# Environment:
#   GATE_SEED       seed directory holding avant and myc-llvm (default bin/v0.34.0)
#   GATE_OUT        output directory (default results/gate-YYYYmmdd-HHMMSS);
#                   keep it under results/ or outside the repository, since the
#                   repository snapshot excludes only .git/ and results/
#   GATE_REUSE_C    unset: B builds C. "1": C is the C the B suite's
#                   test_fixed_point kept (AVANT_FIXED_POINT_KEEP, D138), else
#                   B builds it. "proof": keeps it, builds C too, and records
#                   whether the two are byte-identical (a difference fails).
#   GATE_SUITE_ENV  extra VAR=value words passed to both suites only (for a
#                   later per-backend run, e.g. AVANT_BACKEND=llvm, D176); the
#                   letters are always built with the seed's backend.
#
# Every child runs under `timeout -k 10` (runtime 300 s, compiler build and
# dump 900 s, suite 900 s) with stdin from /dev/null; a timeout is a failure.
# AVANT_MYC_LLVM is always passed explicitly (the seed's myc-llvm).
#
# Output files in GATE_OUT: build-{runtime,b,c}.log and time-build-{b,c}.txt;
# suite-{b,c}.log, junit-{b,c}.xml, time-{b,c}.txt; {before,after}-{b,c}.{tmp,repo}
# snapshots and the new-/gone- differences; fixed-point.txt; reuse.txt when
# GATE_REUSE_C is set; summary.txt (everything printed at the end).
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
[ -f compiler/main.av ] && [ -f runtime/build.sh ] || { echo "gate: $root is not an avant checkout" >&2; exit 2; }

seed=$(realpath -m "${GATE_SEED:-bin/v0.34.0}")
M=$seed/myc-llvm
[ -x "$seed/avant" ] && [ -x "$M" ] || { echo "gate: no avant and myc-llvm in $seed" >&2; exit 2; }
out=$(realpath -m "${GATE_OUT:-results/gate-$(date +%Y%m%d-%H%M%S)}")
mkdir -p "$out" || exit 2
REUSE=${GATE_REUSE_C:-}
case "$REUSE" in ''|1|proof) ;; *) echo "gate: GATE_REUSE_C must be unset, 1, or proof" >&2; exit 2 ;; esac
SUITE_ENV=${GATE_SUITE_ENV:-}

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

timeout -k 10 300 sh runtime/build.sh > "$out/build-runtime.log" 2>&1 < /dev/null || { fail "runtime build failed (rc=$?)"; finish; }
AVANT_MYC_LLVM=$M timeout -k 10 900 /usr/bin/time -p -o "$out/time-build-b.txt" "$seed/avant" compile compiler/main.av bin/avant-b --debug > "$out/build-b.log" 2>&1 < /dev/null || { fail "B build failed (rc=$?)"; finish; }

build_c() {
  AVANT_MYC_LLVM=$M timeout -k 10 900 /usr/bin/time -p -o "$out/time-build-c.txt" ./bin/avant-b compile compiler/main.av bin/avant-c --debug > "$out/build-c.log" 2>&1 < /dev/null || { fail "C build failed (rc=$?)"; finish; }
}
rm -f "$out/kept-c" "$out/reuse.txt"
[ -z "$REUSE" ] && build_c

for L in b c; do
  if [ "$L" = c ] && [ -n "$REUSE" ]; then
    if [ "$REUSE" = proof ]; then
      build_c
      if [ -f "$out/kept-c" ] && cmp -s "$out/kept-c" bin/avant-c; then
        echo "reuse proof: kept C is byte-identical to the gate-built C" > "$out/reuse.txt"
      else
        echo "reuse proof: kept C DIFFERS from the gate-built C (or none was kept)" > "$out/reuse.txt"
        fail "reuse proof differs"
      fi
      sha256sum "$out/kept-c" bin/avant-c 2>/dev/null | sed "s#$out/##" >> "$out/reuse.txt"
    elif [ -f "$out/kept-c" ]; then
      cp -f "$out/kept-c" bin/avant-c && echo "reuse: C is the fixed point's kept C" > "$out/reuse.txt"
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
  env AVANT_MYC_LLVM="$M" $keep $SUITE_ENV timeout -k 10 900 /usr/bin/time -p -o "$out/time-$L.txt" ./bin/avant-$L test --junit "$out/junit-$L.xml" > "$out/suite-$L.log" 2>&1 < /dev/null
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

AVANT_MYC_LLVM=$M timeout -k 10 900 ./bin/avant-b dump compiler/main.av > "$out/dump-b.myc" 2>&1 < /dev/null || fail "dump on B failed (rc=$?)"
AVANT_MYC_LLVM=$M timeout -k 10 900 ./bin/avant-c dump compiler/main.av > "$out/dump-c.myc" 2>&1 < /dev/null || fail "dump on C failed (rc=$?)"
if cmp -s "$out/dump-b.myc" "$out/dump-c.myc"; then
  echo "fixed point: identical" > "$out/fixed-point.txt"
else
  echo "fixed point: DIFFER" > "$out/fixed-point.txt"
  fail "fixed point differs"
fi
sha256sum "$out/dump-b.myc" "$out/dump-c.myc" | sed "s#$out/##" >> "$out/fixed-point.txt"
rm -f "$out/dump-b.myc" "$out/dump-c.myc"

for L in b c; do
  jf=$(grep -o 'failures="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
  [ "$jf" = 'failures="0"' ] || fail "junit on $L: ${jf:-no failures count}"
done

ptimes() { [ -f "$1" ] && grep -E '^(real|user|sys) ' "$1" | tr '\n' ' '; }
{
  echo "build B: $(ptimes "$out/time-build-b.txt")"
  [ -f "$out/time-build-c.txt" ] && echo "build C: $(ptimes "$out/time-build-c.txt")"
  for L in b c; do
    checks=$(grep -o 'tests="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    jf=$(grep -o 'failures="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    echo "suite $L: $(grep -E 'failed, .* passed' "$out/suite-$L.log" | tail -1) $checks $jf $(ptimes "$out/time-$L.txt")$(grep -E '^rc=' "$out/suite-$L.log") new-tmp=$(wc -l < "$out/new-$L.tmp") gone-tmp=$(wc -l < "$out/gone-$L.tmp") new-repo=$(wc -l < "$out/new-$L.repo") gone-repo=$(wc -l < "$out/gone-$L.repo")"
  done
  cat "$out/fixed-point.txt"
  [ -f "$out/reuse.txt" ] && cat "$out/reuse.txt"
  sha256sum bin/avant-b bin/avant-c
} | tee -a "$out/summary.txt"
finish
