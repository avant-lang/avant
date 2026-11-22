#!/bin/sh
# The standard gate.
#
#   sh tools/gate.sh        (any directory; stdin from /dev/null)
#
# One mode (the compiler has one back end): rebuilds the
# runtime, B (the seed builds it, on the seed's own back end), C' (B
# compiles the compiler, bin/avant-c-llvm); runs the suite on B and on C'
# with AVANT_BACKEND unset (so the suite proves what users get), with
# JUnit and /usr/bin/time -p; snapshots /tmp and the repository before and
# after each suite (no litter); builds D' (C' compiles the compiler,
# bin/avant-d-llvm); checks the fixed point (the pre-optimization .ll of
# compiler/main.av, `avant __emit-ll`, from C' and from D', byte for byte)
# and C' = D' byte for byte. Between the default suites and the GC passes
# it runs the README checks on C' (tools/readme-check.sh, every ```avant
# block compiled and run, and tools/readme-links.sh), on README.md,
# CONTRIBUTING.md and DESIGN.md, and `avant format
# --check` over every tracked source
# (tools/format-check.sh): a red one fails the
# gate. Before the builds it runs the runtime-C rooting lint
# (tools/gc-lint.sh) and the check that no tracked file names an
# identifier of the planning notes (tools/no-ledger-refs.sh). Prints one
# summary line per suite, one
# for the README checks, the fixed point, and a final `GATE PASS` or
# `GATE FAIL: ...` line; exits 0 only on PASS (1 on any failure, 2 on
# usage).
#
# Environment:
#   GATE_SEED       seed directory holding avant (default: tools/letters.sh's,
#                   bin/v0.48.0; `sh tools/letters.sh --print-seed`)
#   GATE_OUT        output directory (default results/gate-YYYYmmdd-HHMMSS);
#                   keep it under results/ or outside the repository, since the
#                   repository snapshot excludes only .git/ and results/
#   GATE_REUSE_C    unset: B builds C'. "1": C' is the compiler the B suite's
#                   test_fixed_point kept (AVANT_FIXED_POINT_KEEP), else
#                   B builds it. "proof": keeps it, builds C' too, and records
#                   whether the two are byte-identical (a difference fails).
#   GATE_SUITE_ENV  extra VAR=value words passed to both suites only (e.g.
#                   a GC environment).
#   GATE_GC_ENV     unset (default): nothing more. VAR=value words of one GC
#                   environment (e.g. AVANT_GC_VERIFY=1, AVANT_YOUNG_MB=1,
#                   AVANT_NURSERY=0): one extra suite pass on C' under it,
#                   after the two default passes, with its own logs
#                   (L = c-llvm-gc) and the same litter checks; red fails
#                   the gate. A list of environments
#                   separated by `;` (e.g. 'AVANT_GC_VERIFY=1;
#                   AVANT_YOUNG_MB=1;AVANT_NURSERY=0;AVANT_GC_STRESS=1')
#                   builds the letters and runs the two default passes
#                   once, then one pass on C' per environment, in order,
#                   each with its own logs (L = c-llvm-gc-SLUG, SLUG the
#                   environment in lower case with every other character
#                   than a-z and 0-9 made `_`) and its own summary line; empty
#                   entries are ignored, and
#                   a list of one environment is the single form. Checks that assert the
#                   default environment's GC numbers skip themselves
#                   under it, and checks whose program needs the nursery
#                   state AVANT_NURSERY=1 (tests/cases/harness.av,
#                   gc_env_outer). AVANT_GC_STATS is not a suite
#                   environment: it writes to every program's stderr.
#   GATE_GC_OVERLAP how many GC passes run at once (default 2). The timed
#                   passes (the builds, the B and C'
#                   suites, D' and __emit-ll) always run alone; the GC
#                   passes are proofs, not measurements, and with a list of
#                   environments up to GATE_GC_OVERLAP of them run
#                   concurrently, each with its own logs, counts and
#                   verdict, reported in the list's order. 1 runs them one
#                   after another. Measured when the overlap was built, on
#                   the four standard passes (16 threads): 4 at once with
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
#   GATE_PRIVATE_TMP 1 (default): the whole gate runs in a private /tmp,
#                   a tmpfs mounted in a mount namespace of its own
#                   (unshare --user --mount; capabilities dropped after
#                   the mount, TMPDIR unset), so the no-litter proof covers
#                   everything the gate writes to /tmp and nothing another
#                   lane does (with lanes
#                   in parallel under the shared lock, another lane's own
#                   /tmp entry, gone during a GC pass, failed the gate).
#                   The checkout, GATE_OUT and the seed, when under /tmp,
#                   are bound into the private one at the same paths. A
#                   tmpfs, as the real /tmp is on this machine, so the
#                   suites' files stay in memory as before and nothing is
#                   left to remove. A machine that cannot make the
#                   namespace (Ubuntu 24.04 restricts unprivileged user
#                   namespaces through AppArmor by default; GitHub's
#                   ubuntu-24.04 runners refuse the gate's) runs
#                   the gate against the real /tmp, snapshotted, as 0
#                   does; the summary's "private /tmp" line says which
#                   mode ran and why, and private-tmp.log keeps the
#                   refusal. The snapshot proves as much as it can there:
#                   an entry the gate leaves in /tmp, or removes from it,
#                   is litter; it cannot tell another process's entry
#                   from the gate's, so on a machine where other work
#                   uses /tmp meanwhile a red litter line may not be the
#                   gate's (the namespace is what removes that doubt).
#                   require: the namespace or GATE FAIL (no fallback).
#   GATE_SNAP_TMP   the directory snapshotted in place of /tmp when the
#                   real /tmp is (0, or the fallback); default /tmp. For
#                   tests/tools/gate-private-tmp.sh, whose stub gate must
#                   not see the suite's other files come and go in /tmp.
#   GATE_EMIT_BOUND seconds (default 20): the summary reports the C' (c-llvm)
#                   __emit-ll time of compiler/main.av within or OVER it;
#                   report only, never a failure.
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
# readme-check.log, readme-links.log, contributing-check.log,
# contributing-links.log and their litter-readme.{tmp,repo};
# format-check.log, time-format-check.txt;
# build-d-llvm.log, time-build-d-llvm.txt, emit-{c,d}-llvm.log,
# time-emit-{c,d}-llvm.txt;
# fixed-point.txt; identity.txt (C' against D'); reuse.txt when
# GATE_REUSE_C is set; summary.txt (everything printed at the end).
set -u
# Every compile the gate makes runs in the verify mode (the MIR
# verifier at stage final and LLVM's verifier: compiler/llvm/driver.av).
AVANT_VERIFY=1
export AVANT_VERIFY

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
[ -f compiler/main.av ] && [ -f runtime/build.sh ] || { echo "gate: $root is not an avant checkout" >&2; exit 2; }

# The seed: GATE_SEED, else tools/letters.sh's default (computed only there).
seed=$(LETTERS_SEED=${GATE_SEED:-} sh tools/letters.sh --print-seed) || exit 2
[ -x "$seed/avant" ] || { echo "gate: no avant in $seed" >&2; exit 2; }
# The package checks of the suite need the bundled re2c beside the compiler;
# a worktree that never had one fails nine of them with unrelated messages.
[ -x bin/re2c ] || { echo "gate: no bin/re2c in $root (the suite's package checks need it): copy one from another checkout, or run: sh tools/build-re2c.sh bin/re2c" >&2; exit 2; }
out=$(realpath -m "${GATE_OUT:-results/gate-$(date +%Y%m%d-%H%M%S)}")
mkdir -p "$out" || exit 2

# The private /tmp (GATE_PRIVATE_TMP): the gate runs itself again
# inside the namespace. GATE_IN_PRIVATE_TMP marks that run and is
# removed at once, so nothing the gate starts inherits it (a gate a test
# runs makes its own).
snap_tmp=/tmp
case "${GATE_PRIVATE_TMP:-1}" in
  0) private_tmp="off (GATE_PRIVATE_TMP=0): the real /tmp is snapshotted"; snap_tmp=${GATE_SNAP_TMP:-/tmp} ;;
  1|require)
    if [ "${GATE_IN_PRIVATE_TMP:-}" = 1 ]; then
      private_tmp="a tmpfs in a mount namespace of its own"
    elif unshare --user --map-current-user --mount --keep-caps sh -c 'mount -t tmpfs tmpfs /tmp' < /dev/null > "$out/private-tmp.log" 2>&1; then
      # Fd 9 keeps the real /tmp reachable (/proc/self/fd/9) after the
      # tmpfs covers it, to bind the checkout and GATE_OUT back when they
      # lie under it.
      # shellcheck disable=SC2016 # expanded by the inner shell
      exec unshare --user --map-current-user --mount --keep-caps sh -c '
        root=$1 out=$2 seed=$3; shift 3
        exec 9< /tmp
        mount -t tmpfs tmpfs /tmp || exit 2
        for p in "$root" "$out" "$seed"; do
          case "$p" in
            /tmp/*) mkdir -p "$p" && mount --no-canonicalize --rbind "/proc/self/fd/9${p#/tmp}" "$p" || exit 2 ;;
          esac
        done
        exec 9<&-
        exec setpriv --inh-caps=-all --ambient-caps=-all --bounding-set=-all env -u TMPDIR -u GATE_SNAP_TMP GATE_IN_PRIVATE_TMP=1 GATE_OUT="$out" GATE_SEED="$seed" sh "$root/tools/gate.sh" "$@"
      ' gate "$root" "$out" "$seed" "$@" < /dev/null
    else
      why=$(tail -1 "$out/private-tmp.log")
      if [ "${GATE_PRIVATE_TMP:-1}" = require ]; then
        echo "gate: cannot make a private /tmp ($why); GATE_PRIVATE_TMP=require stops here" | tee "$out/summary.txt"
        echo "GATE FAIL: no private /tmp" | tee -a "$out/summary.txt"
        exit 1
      fi
      private_tmp="none, this machine refuses the namespace ($why): the real /tmp is snapshotted (fallback)"
      snap_tmp=${GATE_SNAP_TMP:-/tmp}
    fi
    ;;
  *) echo "gate: GATE_PRIVATE_TMP must be 0, 1 or require" >&2; exit 2 ;;
esac
unset GATE_IN_PRIVATE_TMP
REUSE=${GATE_REUSE_C:-}
case "$REUSE" in ''|1|proof) ;; *) echo "gate: GATE_REUSE_C must be unset, 1, or proof" >&2; exit 2 ;; esac
SUITE_ENV=${GATE_SUITE_ENV:-}
GC_ENV=${GATE_GC_ENV:-}
# The bound the emit-ll report line compares the C' __emit-ll time of
# compiler/main.av with (seconds; report only; 10.6 s when it was set).
EMIT_BOUND=${GATE_EMIT_BOUND:-20}
case "${GATE_BACKEND:-llvm}" in
  llvm) ;;
  *) echo "gate: GATE_BACKEND=${GATE_BACKEND} is gone (one back end, one gate)" >&2; exit 2 ;;
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
  find "$snap_tmp" -mindepth 1 -maxdepth 1 ! -name 'claude-*' | LC_ALL=C sort > "$1.tmp"
  find . -path ./.git -prune -o -path ./results -prune -o -print | LC_ALL=C sort > "$1.repo"
}

echo "gate: root=$root seed=$seed out=$out reuse=${REUSE:-no} suite-env=${SUITE_ENV:-none} gc-env=${GC_ENV:-none}" | tee "$out/summary.txt"
echo "gate: private /tmp: $private_tmp" | tee -a "$out/summary.txt"

# The runtime-C rooting lint (runtime/avant_gc.h's RC1-RC4: a runtime C
# function that allocates opens a GC scope or a no-GC region).
if sh tools/gc-lint.sh > "$out/gc-lint.txt" 2>&1 < /dev/null; then
  echo "gate: $(tail -1 "$out/gc-lint.txt")" | tee -a "$out/summary.txt"
else
  fail "gc-lint: $(tail -1 "$out/gc-lint.txt") (gc-lint.txt)"
fi

# No tracked file names an identifier of the planning notes, which live
# outside this repository (tools/no-ledger-refs.sh, its allowlist
# tools/no-ledger-refs.allow); a second of reading, no build needed.
if sh tools/no-ledger-refs.sh > "$out/no-ledger-refs.txt" 2>&1 < /dev/null; then
  echo "gate: $(tail -1 "$out/no-ledger-refs.txt")" | tee -a "$out/summary.txt"
else
  fail "no-ledger-refs: $(tail -1 "$out/no-ledger-refs.txt") (no-ledger-refs.txt)"
fi

# The letters come from tools/letters.sh (its own timeouts: runtime 300 s,
# each compiler build 900 s; stdin /dev/null).
letters() { LETTERS_SEED=$seed LETTERS_TIME_DIR=$out sh tools/letters.sh "$@" < /dev/null; }
letters runtime > "$out/build-runtime.log" 2>&1 || { fail "runtime build failed (rc=$?)"; finish; }
# The bind helper is part of the runtime's build, and the suite tests
# `avant bind`: runtime/build.sh skips it with a notice when llvm-config or
# libclang is missing, so the gate stops here with that reason rather
# than in the suite.
[ -x runtime/build/avant-bind ] || { fail "avant-bind was not built: $(grep 'skipped avant-bind' "$out/build-runtime.log" | tail -n 1 | sed 's/^runtime\/build.sh: //') (build-runtime.log)"; finish; }
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
  env -u AVANT_BACKEND -u AVANT_COMPILER $3 timeout -k 10 900 /usr/bin/time -p -o "$out/time-$1.txt" ./bin/avant-$2 test --junit "$out/junit-$1.xml" > "$out/suite-$1.log" 2>&1 < /dev/null
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

# The programs and links of README.md, CONTRIBUTING.md and DESIGN.md, on
# C' and alone (a few seconds): every ```avant block compiled and run
# against what the file says it prints (tools/readme-check.sh; DESIGN.md
# only once it holds a block, the check refusing a file without one), and
# every heading, anchor and relative link (tools/readme-links.sh: a
# README anchor that DESIGN.md links and a rename removed is caught). A
# red check fails the gate; the repository and /tmp are snapshotted
# around them.
readme_checks() {
  snap "$out/before-readme"
  env -u AVANT_BACKEND AVANT_COMPILER="$root/bin/avant-$C" timeout -k 10 900 sh tools/readme-check.sh > "$out/readme-check.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "README check: $(tail -1 "$out/readme-check.log") (rc=$rc, readme-check.log)"
  timeout -k 10 300 sh tools/readme-links.sh > "$out/readme-links.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "README links: $(tail -1 "$out/readme-links.log") (rc=$rc, readme-links.log)"
  env -u AVANT_BACKEND AVANT_COMPILER="$root/bin/avant-$C" timeout -k 10 900 sh tools/readme-check.sh CONTRIBUTING.md > "$out/contributing-check.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "CONTRIBUTING check: $(tail -1 "$out/contributing-check.log") (rc=$rc, contributing-check.log)"
  timeout -k 10 300 sh tools/readme-links.sh CONTRIBUTING.md > "$out/contributing-links.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "CONTRIBUTING links: $(tail -1 "$out/contributing-links.log") (rc=$rc, contributing-links.log)"
  if grep -q '^```' DESIGN.md; then
    env -u AVANT_BACKEND AVANT_COMPILER="$root/bin/avant-$C" timeout -k 10 900 sh tools/readme-check.sh DESIGN.md > "$out/design-check.log" 2>&1 < /dev/null
    rc=$?
    [ $rc -eq 0 ] || fail "DESIGN check: $(tail -1 "$out/design-check.log") (rc=$rc, design-check.log)"
  fi
  timeout -k 10 300 sh tools/readme-links.sh DESIGN.md > "$out/design-links.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "DESIGN links: $(tail -1 "$out/design-links.log") (rc=$rc, design-links.log)"
  snap "$out/after-readme"
  LC_ALL=C comm -3 "$out/before-readme.tmp" "$out/after-readme.tmp" > "$out/litter-readme.tmp"
  LC_ALL=C comm -3 "$out/before-readme.repo" "$out/after-readme.repo" > "$out/litter-readme.repo"
  for k in litter-readme.tmp litter-readme.repo; do
    [ -s "$out/$k" ] && fail "litter: $k has $(wc -l < "$out/$k") entries"
  done
}
readme_checks

# Every source canonical for `avant format`,
# on C' and alone (about 3 s): tools/format-check.sh runs `format
# --check`, each file through the formatter's self-check, over the
# tracked .av files but tests/fixtures/format/surface.av (messy on
# purpose) and FORMAT_SKIP. FORMAT_SKIP is the dated list of sources left
# non-canonical because another lane held them when the check landed; the
# merge after each such lane formats its files and removes them from the
# list. A stale entry (a path that is no longer tracked) fails the step.
# 2026-11-14: empty, no running lane held a non-canonical source.
FORMAT_SKIP=""
format_check() {
  env -u AVANT_BACKEND AVANT_COMPILER="$root/bin/avant-$C" /usr/bin/time -p -o "$out/time-format-check.txt" timeout -k 10 300 sh tools/format-check.sh $FORMAT_SKIP > "$out/format-check.log" 2>&1 < /dev/null
  rc=$?
  [ $rc -eq 0 ] || fail "format check: $(tail -1 "$out/format-check.log") (rc=$rc, format-check.log)"
}
format_check
# One extra pass on C' per GC environment, when asked: GATE_GC_ENV
# is one environment or a `;`-separated list of them. GC_LIST is
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
# passes run: its own TMPDIR root, which must be empty
# afterwards (new-L.tmp lists what is left), the repository snapshot
# around it, and its failures written to fails-L.txt for the parent.
gc_pass() {
  t=$(cat "$out/tmpdir-$1")
  rm -f "$out/junit-$1.xml" "$out/time-$1.txt"
  : > "$out/fails-$1.txt"
  find . -path ./.git -prune -o -path ./results -prune -o -print | LC_ALL=C sort > "$out/before-$1.repo"
  # shellcheck disable=SC2086 # $2 holds VAR=value words
  env -u AVANT_BACKEND -u AVANT_COMPILER TMPDIR="$t" $2 timeout -k 10 900 /usr/bin/time -p -o "$out/time-$1.txt" ./bin/avant-$C test --jobs "$3" --junit "$out/junit-$1.xml" > "$out/suite-$1.log" 2>&1 < /dev/null
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

# D' by C' (never C' by itself), then the .ll fixed point and
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
  # Report only, never a failure: the compile time of the
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
  # The longest suite pass by its own wall time: with the GC passes
  # overlapped, the one that bounds their wall (a pass's CPU time can be
  # the largest while another pass is the longest).
  for L in $SUITES; do
    r=$(awk '$1 == "real" { print $2 }' "$out/time-$L.txt" 2>/dev/null)
    [ -n "$r" ] && printf '%s %s\n' "$r" "$L"
  done | sort -k1,1 -g -r | head -n 1 | while read -r r L; do
    tag=$L
    case "$L" in "$C"-gc*) tag="$L ($(gc_env_of "$L"))" ;; esac
    echo "longest pass: suite $tag, real $r s"
  done
  echo "format: $(tail -n 1 "$out/format-check.log" 2>/dev/null) $(ptimes "$out/time-format-check.txt")"
  echo "readme: $(tail -n 2 "$out/readme-check.log" 2>/dev/null | tr '\n' ' ')| $(tail -n 2 "$out/readme-links.log" 2>/dev/null | tr '\n' ' ')"
  echo "contributing: $(tail -n 2 "$out/contributing-check.log" 2>/dev/null | tr '\n' ' ')| $(tail -n 2 "$out/contributing-links.log" 2>/dev/null | tr '\n' ' ')"
  cat "$out/fixed-point.txt"
  [ -f "$out/identity.txt" ] && cat "$out/identity.txt"
  [ -f "$out/reuse.txt" ] && cat "$out/reuse.txt"
  sha256sum bin/avant-b bin/avant-c-llvm bin/avant-d-llvm
} | tee -a "$out/summary.txt"
finish
