#!/bin/sh
# The LangArena harness built once per checkout (O115, Stage 29 W9Fb1).
#
#   sh tools/la-harness.sh          prints the path of the harness binary,
#                                   building it first when needed
#
# The binary is langarena/tmp/harness/measure (langarena/tmp/ is ignored by
# the harness's repository, and langarena/ by this one), built with
# `crystal build --no-debug` from langarena/harness/measure.cr. Beside it,
# measure.key holds what it was built from: the sha256 of measure.cr, the
# first line of `crystal --version`, and the harness's absolute directory
# (the binary finds avant/ and LangArena/ from the path it was compiled at,
# Crystal's __DIR__). When the key of the checkout differs, or the binary is
# missing, it is rebuilt; otherwise the same binary is reused. A build goes
# to a temporary name and is renamed into place, so two runs that build at
# once both end with a whole binary.
#
# Used by tools/run-goldens.sh (its langarena set) and tools/wave-final.sh
# (the LangArena prod run). Exit 0 and the path on stdout; on a failed build
# the compiler's output on stderr and exit 1.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
src=$root/langarena/harness/measure.cr
dir=$root/langarena/tmp/harness
[ -f "$src" ] || { echo "la-harness: no $src (copy the langarena/ harness into the checkout)" >&2; exit 1; }
cv=$(crystal --version 2>/dev/null | head -n 1)
[ -n "$cv" ] || { echo "la-harness: crystal not found" >&2; exit 1; }
key=$(printf '%s\n%s\n%s\n' "$(sha256sum < "$src" | cut -d' ' -f1)" "$cv" "$(dirname "$src")")
if [ -x "$dir/measure" ] && [ -f "$dir/measure.key" ] && [ "$(cat "$dir/measure.key")" = "$key" ]; then
  echo "$dir/measure"
  exit 0
fi
mkdir -p "$dir" || exit 1
tmp=$dir/measure.new.$$
log=$dir/build.log.$$
if timeout -k 10 900 crystal build --no-debug -o "$tmp" "$src" > "$log" 2>&1 < /dev/null; then
  mv -f "$tmp" "$dir/measure" && printf '%s\n' "$key" > "$dir/measure.key.$$" && mv -f "$dir/measure.key.$$" "$dir/measure.key"
  rm -f "$log" "$tmp.dwarf"
  echo "$dir/measure"
  exit 0
fi
cat "$log" >&2
rm -f "$log" "$tmp" "$tmp.dwarf"
exit 1
