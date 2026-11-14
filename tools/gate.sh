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
# and C' = D' byte for byte. Between the default suites and the GC passes
# it runs the README checks on C' (tools/readme-check.sh, every ```avant
# block compiled and run, and tools/readme-links.sh; Stage 29 W-FIX1f2,
# O218): a red one fails the gate. Prints one summary line per suite, one
# for the README checks, the fixed point, and a final `GATE PASS` or
# `GATE FAIL: ...` line; exits 0 only on PASS (1 on any failure, 2 on
# usage).
#
# Environment:
#   GATE_SEED       seed directory holding avant (default: tools/letters.sh's,
#                   bin/v0.42.0; `sh tools/letters.sh --print-seed`)
#   GATE_OUT        output directory (default results/gate-YYYYmmdd-HHMMSS);
#                   keep it under results/ or outside the repository, since the
#                   repository snapshot excludes only .git/ and results/
#   GATE_REUSE_C    unset: B builds C'. "1": C' is the compiler the B suite's
#                   test_fixed_point kept (AVANT_FIXED_POINT_KEEP, D138), else
#                   B builds it. "proof": keeps it, builds C' too, and records
#                   whether the two are byte-identical (a difference fails).
#   GATE_SUITE_ENV  extra VAR=value words passed to both suites only (e.g.
#                   a GC environment).
#   GATE_GC_ENV     unset (default): nothing more. VAR=value words of one GC
#                   environment (e.g. AVANT_GC_VERIFY=1, AVANT_YOUNG_MB=1,
#                   AVANT_NURSERY=0): one extra suite pass on C' under it,
#                   after the two default passes, with its own logs
#                   (L = c-llvm-gc) and the same litter checks; red fails
#                   the gate (Stage 28 W12, O26). A list of environments
#                   separated by `;` (e.g. 'AVANT_GC_VERIFY=1;
#                   AVANT_YOUNG_MB=1;AVANT_NURSERY=0;AVANT_GC_STRESS=1')
#                   builds the letters and runs the two default passes
#                   once, then one pass on C' per environment, in order,
#                   each with its own logs (L = c-llvm-gc-SLUG, SLUG the
#                   environment in lower case with every other character
#                   than a-z and 0-9 made `_`) and its own summary line
#                   (Stage 28 W14b2, O68); empty entries are ignored, and
#                   a list of one environment is the single form. Checks that assert the
#                   default environment's GC numbers skip themselves
#                   under it, and checks whose program needs the nursery
#                   state AVANT_NURSERY=1 (tests/cases/harness.av,
#                   gc_env_outer). AVANT_GC_STATS is not a suite
#                   environment: it writes to every program's stderr.
#   GATE_GC_OVERLAP how many GC passes run at once (default 2; Stage 28
#                   W16t). The timed passes (the builds, the B and C'
#                   suites, D' and __emit-ll) always run alone; the GC
#                   passes are proofs, not measurements, and with a list of
#                   environments up to GATE_GC_OVERLAP of them run
#                   concurrently, each with its own logs, counts and
#                   verdict, reported in the list's order. 1 runs them one
#                   after another, as before W16t. Measured at W16t on
#                   D235's four passes (16 threads): 4 at once with
#                   --jobs 4 378 s, with --jobs 8 396 s, 2 with --jobs 8
#                   379 s, one at a time 562 s (the gate's wall); 2 keeps
#                   each pass nearer its time alone.
#   GATE_GC_JOBS    each concurrent GC pass's `avant test --jobs` (default
#                   the online processors divided by GATE_GC_OVERLAP, at
#                   least 1): the passes' processes together stay within
#                   the machine (the suite runner's own parallelism counts).
#                   Unused when the passes run one at a time (the runner's
#                   default, as before).
#                   A concurrent pass runs with its own TMPDIR root
#                   (a fresh avant-gate-XXXXXX in TMPDIR, else /tmp,
#                   named in GATE_OUT/tmpdir-L), which must be empty
#                   afterwards (else it is kept and listed); /tmp
#                   is snapshotted around the whole group, and an entry it
#                   gains or loses is counted against every pass of the
#                   group (new-L.tmp, gone-L.tmp), as is the repository's
#                   snapshot around each pass.
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
# Output files in GATE_OUT (L is b and c-llvm, and c-llvm-gc, or one
# c-llvm-gc-SLUG per environment of a list, under GATE_GC_ENV):
# build-runtime.log,
# build-L.log and time-build-L.txt; suite-L.log, junit-L.xml, time-L.txt;
# {before,after}-L.{tmp,repo} snapshots and the new-/gone- differences;
# readme-check.log, readme-links.log and their litter-readme.{tmp,repo};
# build-d-llvm.log, time-build-d-llvm.txt, emit-{c,d}-llvm.log,
# time-emit-{c,d}-llvm.txt;
# fixed-point.txt; identity.txt (C' against D'); reuse.txt when
# GATE_REUSE_C is set; summary.txt (everything printed at the end).
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
[ -f compiler/main.av ] && [ -f runtime/build.sh ] || { echo "gate: $root is not an avant checkout" >&2; exit 2; }

# The seed: GATE_SEED, else tools/letters.sh's default (computed only there, O31).
seed=$(LETTERS_SEED=${GATE_SEED:-} sh tools/letters.sh --print-seed) || exit 2
[ -x "$seed/avant" ] || { echo "gate: no avant in $seed" >&2; exit 2; }
out=$(realpath -m "${GATE_OUT:-results/gate-$(date +%Y%m%d-%H%M%S)}")
mkdir -p "$out" || exit 2
REUSE=${GATE_REUSE_C:-}
case "$REUSE" in ''|1|proof) ;; *) echo "gate: GATE_REUSE_C must be unset, 1, or proof" >&2; exit 2 ;; esac
SUITE_ENV=${GATE_SUITE_ENV:-}
GC_ENV=${GATE_GC_ENV:-}
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

echo "gate: root=$root seed=$seed out=$out reuse=${REUSE:-no} suite-env=${SUITE_ENV:-none} gc-env=${GC_ENV:-none}" | tee "$out/summary.txt"

# The runtime-C rooting lint (Stage 28 W16a, gc-design.md section 4.2
# check 3; runtime/avant_gc.h's RC1-RC4).
if sh tools/gc-lint.sh > "$out/gc-lint.txt" 2>&1 < /dev/null; then
  echo "gate: $(tail -1 "$out/gc-lint.txt")" | tee -a "$out/summary.txt"
else
  fail "gc-lint: $(tail -1 "$out/gc-lint.txt") (gc-lint.txt)"
fi

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

# suite_pass L COMPILER ENV: the suite on bin/avant-COMPILER with the
# VAR=value words ENV, logged as L (suite-L.log, junit-L.xml, time-L.txt),
# with the /tmp and repository snapshots around it (no litter).
suite_pass() {
  rm -f "$out/junit-$1.xml" "$out/time-$1.txt"
  snap "$out/before-$1"
  # shellcheck disable=SC2086 # $3 holds VAR=value words
  env -u AVANT_BACKEND $3 timeout -k 10 900 /usr/bin/time -p -o "$out/time-$1.txt" ./bin/avant-$2 test --junit "$out/junit-$1.xml" > "$out/suite-$1.log" 2>&1 < /dev/null
  rc=$?
  echo "rc=$rc" >> "$out/suite-$1.log"
  [ $rc -eq 0 ] || fail "suite on $1 failed (rc=$rc)"
  snap "$out/after-$1"
  LC_ALL=C comm -13 "$out/before-$1.tmp" "$out/after-$1.tmp" > "$out/new-$1.tmp"
  LC_ALL=C comm -23 "$out/before-$1.tmp" "$out/after-$1.tmp" > "$out/gone-$1.tmp"
  LC_ALL=C comm -13 "$out/before-$1.repo" "$out/after-$1.repo" > "$out/new-$1.repo"
  LC_ALL=C comm -23 "$out/before-$1.repo" "$out/after-$1.repo" > "$out/gone-$1.repo"
  for k in new-$1.tmp gone-$1.tmp new-$1.repo gone-$1.repo; do
    [ -s "$out/$k" ] && fail "litter: $k has $(wc -l < "$out/$k") entries"
  done
}

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
  suite_pass "$L" "$L" "$keep $SUITE_ENV"
done

# The README's programs and links (Stage 29 W-FIX1f2, O218), on C' and
# alone (a few seconds): every ```avant block compiled and run against
# what the README says it prints (tools/readme-check.sh), and every
# heading, anchor and relative link (tools/readme-links.sh). A red check
# fails the gate; the repository and /tmp are snapshotted around them.
readme_checks() {
  snap "$out/before-readme"
  env -u AVANT_BACKEND AVANT_COMPILER="$root/bin/avant-$C" timeout -k 10 900 sh tools/readme-check.sh > "$out/readme-check.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "README check: $(tail -1 "$out/readme-check.log") (rc=$rc, readme-check.log)"
  timeout -k 10 300 sh tools/readme-links.sh > "$out/readme-links.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "README links: $(tail -1 "$out/readme-links.log") (rc=$rc, readme-links.log)"
  snap "$out/after-readme"
  LC_ALL=C comm -3 "$out/before-readme.tmp" "$out/after-readme.tmp" > "$out/litter-readme.tmp"
  LC_ALL=C comm -3 "$out/before-readme.repo" "$out/after-readme.repo" > "$out/litter-readme.repo"
  for k in litter-readme.tmp litter-readme.repo; do
    [ -s "$out/$k" ] && fail "litter: $k has $(wc -l < "$out/$k") entries"
  done
}
readme_checks
# One extra pass on C' per GC environment (O26), when asked: GATE_GC_ENV
# is one environment or a `;`-separated list of them (O68). GC_LIST is
# the list's non-empty entries, one per line; with one entry the pass is
# logged as c-llvm-gc, as before the list form, else as c-llvm-gc-SLUG.
SUITES="b $C"
GC_LIST=$(printf '%s\n' "$GC_ENV" | tr ';' '\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d')
GC_N=$(printf '%s' "$GC_LIST" | grep -c .)
gc_label() {
  if [ "$GC_N" -eq 1 ]; then
    echo "$C-gc"
  else
    echo "$C-gc-$(printf '%s' "$1" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9' '_')"
  fi
}
gc_env_of() {
  printf '%s\n' "$GC_LIST" | while IFS= read -r e; do
    [ "$(gc_label "$e")" = "$1" ] && { printf '%s\n' "$e"; break; }
  done
}
# gc_pass L ENV JOBS: one GC environment's suite on C' while other GC
# passes run (Stage 28 W16t): its own TMPDIR root, which must be empty
# afterwards (new-L.tmp lists what is left), the repository snapshot
# around it, and its failures written to fails-L.txt for the parent.
gc_pass() {
  t=$(cat "$out/tmpdir-$1")
  rm -f "$out/junit-$1.xml" "$out/time-$1.txt"
  : > "$out/fails-$1.txt"
  find . -path ./.git -prune -o -path ./results -prune -o -print | LC_ALL=C sort > "$out/before-$1.repo"
  # shellcheck disable=SC2086 # $2 holds VAR=value words
  env -u AVANT_BACKEND TMPDIR="$t" $2 timeout -k 10 900 /usr/bin/time -p -o "$out/time-$1.txt" ./bin/avant-$C test --jobs "$3" --junit "$out/junit-$1.xml" > "$out/suite-$1.log" 2>&1 < /dev/null
  rc=$?
  echo "rc=$rc" >> "$out/suite-$1.log"
  [ $rc -eq 0 ] || echo "suite on $1 failed (rc=$rc)" >> "$out/fails-$1.txt"
  find "$t" -mindepth 1 | LC_ALL=C sort > "$out/new-$1.tmp"
  : > "$out/gone-$1.tmp"
  find . -path ./.git -prune -o -path ./results -prune -o -print | LC_ALL=C sort > "$out/after-$1.repo"
  LC_ALL=C comm -13 "$out/before-$1.repo" "$out/after-$1.repo" > "$out/new-$1.repo"
  LC_ALL=C comm -23 "$out/before-$1.repo" "$out/after-$1.repo" > "$out/gone-$1.repo"
}
if [ "$GC_N" -gt 0 ]; then
  # One label per environment; two entries with the same label would
  # share logs, so they are refused.
  labels=$(printf '%s\n' "$GC_LIST" | while IFS= read -r e; do gc_label "$e"; done)
  [ "$(printf '%s\n' "$labels" | sort | uniq -d)" = "" ] || { fail "GATE_GC_ENV names one environment twice"; finish; }
  OVERLAP=${GATE_GC_OVERLAP:-2}
  case "$OVERLAP" in ''|*[!0-9]*|0) fail "GATE_GC_OVERLAP must be a positive integer"; finish ;; esac
  [ "$OVERLAP" -gt "$GC_N" ] && OVERLAP=$GC_N
  if [ "$OVERLAP" -le 1 ]; then
    for L in $labels; do
      suite_pass "$L" "$C" "$(gc_env_of "$L")"
      SUITES="$SUITES $L"
    done
  else
    GJOBS=${GATE_GC_JOBS:-$(( $(getconf _NPROCESSORS_ONLN) / OVERLAP ))}
    case "$GJOBS" in ''|*[!0-9]*) fail "GATE_GC_JOBS must be a positive integer"; finish ;; esac
    [ "$GJOBS" -ge 1 ] || GJOBS=1
    echo "gate: GC passes: $GC_N, $OVERLAP at once, --jobs $GJOBS each" > "$out/gc-overlap.txt"
    # Each pass's TMPDIR root, outside the repository (a test names a
    # file under the checkout by its relative path), made before /tmp's
    # snapshot and removed after it.
    for L in $labels; do
      mktemp -d "${TMPDIR:-/tmp}/avant-gate-XXXXXX" > "$out/tmpdir-$L" || { fail "cannot make a TMPDIR root"; finish; }
    done
    snap "$out/before-gc"
    # A pool of OVERLAP tokens in a FIFO: a pass takes one, gives it back.
    mkfifo "$out/gc-pool" || { fail "cannot make the GC pass pool"; finish; }
    exec 4<> "$out/gc-pool"
    rm -f "$out/gc-pool"
    i=0
    while [ $i -lt "$OVERLAP" ]; do echo >&4; i=$((i + 1)); done
    for L in $labels; do
      read -r _ <&4
      ( (gc_pass "$L" "$(gc_env_of "$L")" "$GJOBS"); echo >&4 ) < /dev/null &
      SUITES="$SUITES $L"
    done
    wait
    exec 4>&-
    snap "$out/after-gc"
    for L in $labels; do
      rmdir "$(cat "$out/tmpdir-$L")" 2>/dev/null
    done
    LC_ALL=C comm -13 "$out/before-gc.tmp" "$out/after-gc.tmp" > "$out/new-gc.tmp"
    LC_ALL=C comm -23 "$out/before-gc.tmp" "$out/after-gc.tmp" > "$out/gone-gc.tmp"
    for L in $labels; do
      cat "$out/new-gc.tmp" >> "$out/new-$L.tmp"
      cat "$out/gone-gc.tmp" >> "$out/gone-$L.tmp"
      if [ -f "$out/fails-$L.txt" ]; then
        while IFS= read -r m; do fail "$m"; done < "$out/fails-$L.txt"
      else
        fail "suite on $L did not run"
      fi
      for k in new-$L.tmp gone-$L.tmp new-$L.repo gone-$L.repo; do
        [ -s "$out/$k" ] && fail "litter: $k has $(wc -l < "$out/$k") entries"
      done
    done
  fi
fi

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

for L in $SUITES; do
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
  for L in $SUITES; do
    checks=$(grep -o 'tests="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    jf=$(grep -o 'failures="[0-9]*"' "$out/junit-$L.xml" 2>/dev/null | head -1)
    tag=$L
    case "$L" in "$C"-gc*) tag="$L ($(gc_env_of "$L"))" ;; esac
    echo "suite $tag: $(grep -E 'failed, .* passed' "$out/suite-$L.log" | tail -1) $checks $jf $(ptimes "$out/time-$L.txt")$(grep -E '^rc=' "$out/suite-$L.log") new-tmp=$(wc -l < "$out/new-$L.tmp") gone-tmp=$(wc -l < "$out/gone-$L.tmp") new-repo=$(wc -l < "$out/new-$L.repo") gone-repo=$(wc -l < "$out/gone-$L.repo")"
  done
  echo "readme: $(tail -n 2 "$out/readme-check.log" 2>/dev/null | tr '\n' ' ')| $(tail -n 2 "$out/readme-links.log" 2>/dev/null | tr '\n' ' ')"
  cat "$out/fixed-point.txt"
  [ -f "$out/identity.txt" ] && cat "$out/identity.txt"
  [ -f "$out/reuse.txt" ] && cat "$out/reuse.txt"
  sha256sum bin/avant-b bin/avant-c-llvm bin/avant-d-llvm
} | tee -a "$out/summary.txt"
finish
