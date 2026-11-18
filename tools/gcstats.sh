#!/bin/sh
# The GC-statistics set of a checkout, the collector's numbers that a
# change must explain when they move.
#
#   sh tools/gcstats.sh ROOT OUT
#
# ROOT is a checkout with its C' built (bin/avant-c-llvm). OUT is a
# directory (created). The three churn fixtures (tests/fixtures/heap,
# closures and objects churn.av) are compiled by ROOT's C' in --debug, bare
# and --release, from ROOT with the fixture's relative path, and each binary
# runs under AVANT_GC_STATS=1 in the default environment and under
# AVANT_NURSERY=0, AVANT_GC_VERIFY=1 and AVANT_YOUNG_MB=1: 36 runs, each
# writing OUT/NAME.MODE.ENV.out (its stdout and `rc=N`) and .err (the
# statistics). The compile logs stay in OUT/bin; the binaries are removed.
# Two sets are compared with the pause_*_ns fields removed (times, not
# counts): tools/wave-final.sh's statistics step and the base cache
# (tools/base-cache.sh) both write this set.
#
# Exit status 0, or 1 when a fixture did not compile (its log in OUT/bin);
# 2 on a usage error. Every compile and run is under `timeout -k 10 120`
# with stdin from /dev/null.
set -u
[ $# -eq 2 ] || { echo "gcstats.sh: usage: sh tools/gcstats.sh ROOT OUT" >&2; exit 2; }
root=$(realpath "$1") || exit 2
g=$(realpath -m "$2")
[ -x "$root/bin/avant-c-llvm" ] || { echo "gcstats.sh: $root/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
mkdir -p "$g/bin" || exit 2
cd "$root" || exit 2
cfail=0
for f in heap/churn closures/churn objects/churn; do
  n=$(echo "$f" | tr / _)
  for m in debug bare release; do
    fl=--$m; [ $m = bare ] && fl=
    timeout -k 10 120 bin/avant-c-llvm compile "tests/fixtures/$f.av" "$g/bin/$n.$m" $fl < /dev/null > "$g/bin/$n.$m.compile.log" 2>&1 || cfail=1
  done
done
for b in "$g"/bin/*.debug "$g"/bin/*.bare "$g"/bin/*.release; do
  [ -x "$b" ] || continue
  n=$(basename "$b")
  for e in default AVANT_NURSERY=0 AVANT_GC_VERIFY=1 AVANT_YOUNG_MB=1; do
    ee=$e; [ $e = default ] && ee=
    env $ee AVANT_GC_STATS=1 timeout -k 10 120 "$b" < /dev/null > "$g/$n.$e.out" 2> "$g/$n.$e.err"
    echo "rc=$?" >> "$g/$n.$e.out"
  done
done
rm -f "$g"/bin/*.debug "$g"/bin/*.bare "$g"/bin/*.release
exit $cfail
