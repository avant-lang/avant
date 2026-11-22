#!/bin/sh
# The suite's start order from the last run's times: `avant test` starts
# the files `[test] first` names in avant.toml in that order, then the rest
# in walk order, and a free job slot takes a waiting run before a waiting
# compile. The longest files first keep the suite's wall near its longest
# file instead of a long file starting last (list scheduling).
#
#   sh tools/test-order.sh JUNIT [MIN_SECONDS]           prints the line
#   sh tools/test-order.sh --write JUNIT [MIN_SECONDS]   rewrites avant.toml's
#                                                        `first =` line
#
# JUNIT is a suite's JUnit file (a gate's OUT/junit-c-llvm.xml); the files
# whose time is at least MIN_SECONDS (default 10) are listed, longest first,
# each one only when it still exists. The order changes no check: the
# same files run, the same way.
set -u
write=0
[ "${1:-}" = --write ] && { write=1; shift; }
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: sh tools/test-order.sh [--write] JUNIT [MIN_SECONDS]" >&2; exit 2; }
junit=$1 min=${2:-10}
[ -s "$junit" ] || { echo "tools/test-order.sh: $junit is not a JUnit file" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
list=$(sed -n 's/.*<testsuite name="\([^"]*\)"[^>]* time="\([0-9.]*\)".*/\2 \1/p' "$junit" \
  | awk -v m="$min" '$1 + 0 >= m + 0' | LC_ALL=C sort -k1,1gr -k2,2 \
  | while read -r _ f; do [ -f "$root/$f" ] && printf '"%s", ' "$f"; done)
[ -n "$list" ] || { echo "tools/test-order.sh: no file of $junit took $min s or more" >&2; exit 1; }
line="first = [${list%, }]"
if [ $write = 0 ]; then echo "$line"; exit 0; fi
toml=$root/avant.toml
grep -q '^first = ' "$toml" || { echo "tools/test-order.sh: avant.toml has no first = line" >&2; exit 1; }
awk -v l="$line" '/^first = / { print l; next } { print }' "$toml" > "$toml.new" && mv "$toml.new" "$toml"
echo "$line"
