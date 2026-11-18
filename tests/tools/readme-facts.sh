#!/bin/sh
# tools/readme-facts.py lists the facts a prose edit changed.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/readme-facts.sh WORK
#
# Three versions of a small document: the same facts in other words
# (FACTS SAME, status 0), and one that loses a span, changes a number, adds
# a link and edits a code block (each named, status 1). Prints one line per
# check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/readme-facts.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
printf 'The suite has 81 files and `avant test` runs them ([Testing](#testing)).\n\n```avant\nputs(1)\n```\n' > "$w/old.md"
printf 'Running `avant test` runs all 81 files ([Testing](#testing)).\n\n```avant\nputs(1)\n```\n' > "$w/same.md"
printf 'The suite has 82 files; see [Testing](#testing) and [Tools](#tools).\n\n```avant\nputs(2)\n```\n' > "$w/new.md"
python3 -I "$root/tools/readme-facts.py" "$w/old.md" "$w/same.md" > "$w/same.out" 2>&1; rc=$?
[ $rc -eq 0 ] && tail -n 1 "$w/same.out" | grep -qx 'FACTS SAME'; say facts-same $? "status $rc: $(tail -n 1 "$w/same.out")"
python3 -I "$root/tools/readme-facts.py" "$w/old.md" "$w/new.md" > "$w/new.out" 2>&1; rc=$?
[ $rc -eq 1 ]; say facts-differ-status $? "status $rc"
o=$w/new.out
sed -n '/^spans lost/,/^spans new/p' "$o" | grep -qx '  avant test'; say facts-span-lost $? "$(tr '\n' ';' < "$o")"
sed -n '/^numbers lost/,/^numbers new/p' "$o" | grep -qx '  81' && sed -n '/^numbers new/,/^links lost/p' "$o" | grep -qx '  82'
say facts-number-changed $? "$(tr '\n' ';' < "$o")"
sed -n '/^links new/,/^code blocks/p' "$o" | grep -qx '  #tools'; say facts-link-new $? "$(tr '\n' ';' < "$o")"
grep -q '^code blocks: 1 and 1; differ (changed in order: 1)' "$o" && tail -n 1 "$o" | grep -q '^FACTS DIFFER: 2 lost, 2 new, code blocks differ$'
say facts-code-block $? "$(tail -n 2 "$o" | tr '\n' ';')"
echo done
