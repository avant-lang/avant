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
#   sh tools/test-order.sh --check JUNIT [MIN_SECONDS]   says whether the
#                                                        list is still the
#                                                        order the times ask
#                                                        for (default 30 s)
#
# JUNIT is a suite's JUnit file (a gate's OUT/junit-c-llvm.xml); the files
# whose time is at least MIN_SECONDS (default 10) are listed, longest first,
# each one only when it still exists. The order changes no check: the
# same files run, the same way.
#
# --check changes nothing: it names the files of MIN_SECONDS (default 30)
# or more that the list does not hold (a file that grew since the list was
# written starts at its alphabetical place, and the suite's wall is the
# time the last long file starts plus its own), the [test] exclusive files
# aside (they run after the others, listed or not). Status 0 when every such
# file is listed, 1 when some are not (tools/wave-final.sh prints the line
# after the gate; the list is rewritten by hand with --write).
set -u
write=0 check=0
[ "${1:-}" = --write ] && { write=1; shift; }
[ "${1:-}" = --check ] && { check=1; shift; }
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: sh tools/test-order.sh [--write|--check] JUNIT [MIN_SECONDS]" >&2; exit 2; }
junit=$1 min=${2:-10}
[ $check = 1 ] && min=${2:-30}
[ -s "$junit" ] || { echo "tools/test-order.sh: $junit is not a JUnit file" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if [ $check = 1 ]; then
  toml=$root/avant.toml
  # The paths of one `key = [...]` line of avant.toml's [test] table.
  paths_of() { sed -n "s/^$1 = \\[\\(.*\\)\\].*/\\1/p" "$toml" | tr ',' '\n' | sed -n 's/^[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p'; }
  listed=$( { paths_of first; paths_of exclusive; } | LC_ALL=C sort -u)
  late=$(sed -n 's/.*<testsuite name="\([^"]*\)"[^>]* time="\([0-9.]*\)".*/\2 \1/p' "$junit" \
    | awk -v m="$min" '$1 + 0 >= m + 0' | LC_ALL=C sort -k1,1gr -k2,2 \
    | while read -r t f; do [ -f "$root/$f" ] && ! printf '%s\n' "$listed" | grep -qx -F "$f" && printf '%s (%.0f s), ' "$f" "$t"; done)
  if [ -z "$late" ]; then
    echo "test order: current (every file of $min s or more in $(basename "$junit") is in [test] first or exclusive)"; exit 0
  fi
  echo "test order: stale, files of $min s or more outside [test] first: ${late%, }; sh tools/test-order.sh --write $junit rewrites the list"; exit 1
fi
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
