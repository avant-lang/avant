#!/bin/sh
# tools/no-ledger-refs.sh reports the labels of the notes' other numbered
# lists in the forms they were written in, and leaves the same letters
# and digits alone where they are names. Run by tests/cases/test_tools.av;
# by hand:
#
#   sh tests/tools/ledger-labels.sh WORK
#
# WORK is a new scratch directory; the lint runs in a git checkout made
# there with one file of labelled lines and one of names. Prints one line
# per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/ledger-labels.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
mkdir -p "$w/repo/tools"
cp "$root/tools/no-ledger-refs.sh" "$w/repo/tools/"
: > "$w/repo/tools/no-ledger-refs.allow"
cat > "$w/repo/labels.av" <<'X'
// C14: generic instances are typed again
// passes by the C ABI (C3), a bare local
// the lowering, C3, is for calls into C
// corrected, C20).
// links nothing since M4, and LLVM inlines
// done at M7: the plan
// as audit N2b found
// see audit 12 shape 2
// review 4 section 11 says
X
cat > "$w/repo/names.av" <<'X'
// Junk is id 1 and C1 .. C398 are ids 2 .. 399
// a C11 atomic, the C99 standard, M1 total
// M2 = 3 and C2 = 4 in a program
X
(cd "$w/repo" && git init -q . && git add -A) > /dev/null 2>&1
(cd "$w/repo" && sh tools/no-ledger-refs.sh < /dev/null) > "$w/out" 2>&1; rc=$?
[ $rc -eq 1 ]; say labels-fail $? "status $rc"
n=$(grep -c '^labels\.av:' "$w/out"); [ "$n" -eq 9 ]; say labels-each-reported $? "$n of 9: $(grep '^labels' "$w/out" | tr '\n' ';')"
! grep -q '^names\.av:' "$w/out"; say names-left $? "$(grep '^names' "$w/out" | tr '\n' ';')"
echo done
