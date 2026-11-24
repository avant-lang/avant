#!/bin/sh
# D' under the full stress mode: the compiler built by itself with a
# collection at every allocation, compared with C'.
#
#   sh tools/d-stress.sh [OUT]     (from anywhere inside the checkout)
#
# The compiler is the largest program there is, and building it under
# AVANT_GC_STRESS=1 puts every allocation of the compiler's whole runtime
# traffic (strings, arrays, hash tables, the code generator's lists) behind
# a collection: a codegen or runtime change that leaves a reference
# unrooted across an allocation fails here, in the one build that exercises
# all of them, though no test file does. A merge that touches the runtime,
# the collector or the code generator runs it once, besides the gate (4 to
# 10 minutes here, a shared run of its own).
#
# C' (bin/avant-c-llvm, built by tools/letters.sh) compiles compiler/main.av
# with AVANT_GC_STRESS=1 into bin/avant-d-stress, as the letters build D'
# (tools/letters.sh d-stress: verify mode, --debug, --cpu=baseline, the
# same linker), and the two are compared byte for byte: a collection at
# every allocation changes no byte the compiler writes. OUT (default: a new
# directory under TMPDIR) keeps letters.log and d-stress.txt (the line
# below and the two sha256 sums); bin/avant-d-stress is removed once
# compared unless D_STRESS_KEEP=1.
#
# Exit status: 0 when D' is C' byte for byte; 1 when it differs; 2 when C'
# is missing, the build fails or runs past LETTERS_STRESS_TIMEOUT (default
# 1800 s).
set -u
top=$(git rev-parse --show-toplevel 2>/dev/null) || top=$(pwd)
cd "$top" || exit 2
out=${1:-$(mktemp -d "${TMPDIR:-/tmp}/d-stress.XXXXXX")}
mkdir -p "$out" || exit 2
out=$(realpath -m "$out")
rev=$(git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
say() { echo "$1" | tee -a "$out/d-stress.txt"; }
: > "$out/d-stress.txt"
if [ ! -x bin/avant-c-llvm ]; then
  say "d-stress: red at $rev (no C': sh tools/letters.sh runtime b c-llvm builds it)"; exit 2
fi
rm -f bin/avant-d-stress
t0=$(date +%s)
sh tools/letters.sh d-stress < /dev/null > "$out/letters.log" 2>&1; rc=$?
wall=$(( $(date +%s) - t0 ))
if [ $rc -ne 0 ]; then
  say "d-stress: red at $rev (the build failed with status $rc after $wall s: $out/letters.log)"; exit 2
fi
sha256sum bin/avant-c-llvm bin/avant-d-stress >> "$out/d-stress.txt"
same=0; cmp -s bin/avant-c-llvm bin/avant-d-stress && same=1
[ "${D_STRESS_KEEP:-0}" = 1 ] || rm -f bin/avant-d-stress bin/avant-d-stress.versions.txt
if [ $same = 1 ]; then
  say "d-stress: D' built under AVANT_GC_STRESS=1 is C' byte for byte at $rev ($wall s)"; exit 0
fi
say "d-stress: red at $rev (D' built under AVANT_GC_STRESS=1 differs from C', $wall s)"; exit 1
