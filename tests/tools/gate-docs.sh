#!/bin/sh
# The gate's document checks cover DESIGN.md: its links into the README
# are checked, so a README heading renamed under them fails the gate.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/gate-docs.sh WORK
#
# Reads tools/gate.sh's readme_checks for each document's links check,
# runs tools/readme-links.sh on the tree's DESIGN.md, then on a copy whose
# README lost a heading DESIGN.md links. Prints one line per check,
# `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/gate-docs.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
rm -rf "$w"
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
sed -n '/^readme_checks() {/,/^}/p' "$root/tools/gate.sh" > "$w/readme_checks.sh"
for doc in README CONTRIBUTING DESIGN; do
  name=$(echo "$doc" | tr 'A-Z' 'a-z')
  if [ "$doc" = README ]; then
    grep -q 'tools/readme-links.sh >' "$w/readme_checks.sh"
  else
    grep -q "tools/readme-links.sh $doc.md >" "$w/readme_checks.sh"
  fi
  say "gate-links-$name" $? "tools/gate.sh's readme_checks runs no readme-links.sh on $doc.md"
done
TMPDIR=$w sh "$root/tools/readme-links.sh" "$root/DESIGN.md" > "$w/design.out" 2>&1
rc=$?
[ $rc -eq 0 ]; say design-links-pass $? "status $rc: $(tail -n 1 "$w/design.out")"
# A heading of the README that DESIGN.md links, renamed.
anchor=$(grep -o '](README.md#[a-z0-9_-]*)' "$root/DESIGN.md" | head -n 1 | sed 's/^](README.md#//; s/)$//')
mkdir -p "$w/tree"
cp "$root/DESIGN.md" "$w/tree/DESIGN.md"
awk -v A="$anchor" '
function slug(s) { s = tolower(s); gsub(/[^a-z0-9 _-]/, "", s); gsub(/ /, "-", s); return s }
/^#+ / { h = $0; sub(/^#+ /, "", h); if (slug(h) == A && !done) { print $0 " renamed"; done = 1; next } }
{ print }' "$root/README.md" > "$w/tree/README.md"
TMPDIR=$w sh "$root/tools/readme-links.sh" "$w/tree/DESIGN.md" > "$w/broken.out" 2>&1
rc=$?
[ -n "$anchor" ] && [ $rc -eq 1 ] && grep -q "$anchor" "$w/broken.out"
say design-renamed-heading-fails $? "anchor '$anchor', status $rc: $(tail -n 2 "$w/broken.out" | tr '\n' ';')"
echo done
