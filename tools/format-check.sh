#!/bin/sh
# Every Avant source of the repository is canonical for `avant format`
# (Stage 29 W5d1, O170, O200): `format --check` over the tracked .av
# files (git ls-files; without git, every .av under compiler, lib,
# examples, bench, tests and tools), each through the formatter's
# self-check (O464).
#
#   AVANT_COMPILER=bin/avant-c-llvm sh tools/format-check.sh [SKIP...]
#
# Never checked: tests/fixtures/format/surface.av, messy on purpose (the
# formatter's own test input). SKIP names further paths, relative to the
# checkout, left out of this run (tools/gate.sh passes its dated list).
# The last line is `FORMAT CHECK PASS: N files canonical, S skipped` or
# `FORMAT CHECK FAIL: ...`; the exit status is 0 only on PASS (2 on
# usage).
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
compiler=${AVANT_COMPILER:-}
[ -n "$compiler" ] && [ -x "$compiler" ] || { echo "format-check: set AVANT_COMPILER to the compiler to check with" >&2; exit 2; }
list=$(mktemp "${TMPDIR:-/tmp}/format-check-XXXXXX") || exit 2
trap 'rm -f "$list" "$list.all"' EXIT

if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
  git ls-files -- '*.av' > "$list.all"
else
  find compiler lib examples bench tests tools -name '*.av' 2> /dev/null | LC_ALL=C sort > "$list.all"
fi
skipped=0
cp "$list.all" "$list"
for s in tests/fixtures/format/surface.av "$@"; do
  if grep -qxF "$s" "$list"; then
    grep -vxF "$s" "$list" > "$list.all" && cp "$list.all" "$list"
    skipped=$((skipped + 1))
  else
    echo "format-check: skipped path $s is not a tracked source (remove it from the list)"
    echo "FORMAT CHECK FAIL: a stale skip"
    exit 1
  fi
done
n=$(wc -l < "$list")
# One process for every file; xargs keeps the list within the argument limit.
out=$(xargs "$compiler" format --check < "$list" 2>&1)
rc=$?
echo "$out"
if [ $rc -eq 0 ]; then
  echo "FORMAT CHECK PASS: $n files canonical, $skipped skipped"
  exit 0
fi
echo "FORMAT CHECK FAIL: $(echo "$out" | grep -c '^would change') of $n files would change (rc=$rc); run \`avant format --write\` on them"
exit 1
