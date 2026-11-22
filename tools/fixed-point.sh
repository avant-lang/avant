#!/bin/sh
# The fixed point alone: C' = D' for the checkout this is run in, without
# the rest of the gate.
#
#   sh tools/fixed-point.sh [OUT]     (from anywhere inside the checkout)
#
# Builds the runtime, B, C' and D' with that checkout's own tools/letters.sh
# (B from the seed, C' by B, D' by C'), writes the pre-optimization .ll of
# compiler/main.av with C' and with D' (`__emit-ll`), and compares both the
# .ll files and the executables byte for byte, as the gate's fixed point does.
# OUT (default: a new directory under TMPDIR) keeps the build logs and
# fixed-point.txt; the .ll files are removed once compared.
#
# Exit status: 0 when both are identical; 1 when either differs; 2 when a
# build or an emit fails. Every status but 0 marks the commit bad, so the
# script can drive `git bisect run` over an integration's merges when the
# fixed point fails at its head (run a copy kept outside the checkout: an
# older commit may not have this file):
#
#   cp tools/fixed-point.sh "$SCRATCH/fixed-point.sh"
#   git bisect start --first-parent HEAD BASE
#   git bisect run sh "$SCRATCH/fixed-point.sh"
#   git bisect reset
set -u
top=$(git rev-parse --show-toplevel 2>/dev/null) || top=$(pwd)
cd "$top" || exit 2
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/fixed-point.XXXXXX")}
mkdir -p "$out" || exit 2
out=$(realpath -m "$out")
rev=$(git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
say() { echo "$1" | tee -a "$out/fixed-point.txt"; }
: > "$out/fixed-point.txt"
if ! sh tools/letters.sh runtime b c-llvm d-llvm < /dev/null > "$out/letters.log" 2>&1; then
  say "fixed point: red at $rev (the build failed: $out/letters.log)"; exit 2
fi
for x in c-llvm d-llvm; do
  if ! env -u AVANT_BACKEND timeout -k 10 900 ./bin/avant-$x __emit-ll compiler/main.av "$out/main-$x.ll" < /dev/null > "$out/emit-$x.log" 2>&1; then
    say "fixed point: red at $rev (__emit-ll with $x failed: $out/emit-$x.log)"; rm -f "$out"/main-*.ll; exit 2
  fi
done
ll=same; cmp -s "$out/main-c-llvm.ll" "$out/main-d-llvm.ll" || ll=DIFFER
ex=same; cmp -s bin/avant-c-llvm bin/avant-d-llvm || ex=DIFFER
sha256sum "$out/main-c-llvm.ll" "$out/main-d-llvm.ll" bin/avant-c-llvm bin/avant-d-llvm | sed "s#$out/##" >> "$out/fixed-point.txt"
rm -f "$out"/main-*.ll
if [ $ll = same ] && [ $ex = same ]; then
  say "fixed point: C' = D' at $rev (.ll identical, executables identical)"; exit 0
fi
say "fixed point: red at $rev (.ll $ll, executables $ex)"; exit 1
