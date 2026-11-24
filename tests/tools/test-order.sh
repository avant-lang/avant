#!/bin/sh
# tools/test-order.sh on a made-up JUnit file in a fake checkout:
#   order-longest-first: the files at or over the minimum, longest first,
#     a file that no longer exists left out;
#   order-write: --write replaces avant.toml's `first =` line and nothing else.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/test-order.sh WORK
#
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/test-order.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1"); f=$w/avant
mkdir -p "$f/tools" "$f/tests/cases"
cp "$root/tools/test-order.sh" "$f/tools/"
for n in a b c d; do : > "$f/tests/cases/test_$n.av"; done
printf '[package]\nname = "x"\n\n[test]\nfirst = ["tests/cases/test_a.av"]\nexclusive = ["tests/cases/test_d.av"]\n' > "$f/avant.toml"
cat > "$w/junit.xml" <<'EOF2'
<testsuites tests="4" failures="0" time="1">
<testsuite name="tests/cases/test_a.av" tests="1" failures="0" time="12.5">
<testsuite name="tests/cases/test_b.av" tests="1" failures="0" time="40.0">
<testsuite name="tests/cases/test_c.av" tests="1" failures="0" time="3.0">
<testsuite name="tests/cases/test_gone.av" tests="1" failures="0" time="90.0">
</testsuites>
EOF2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
o=$(sh "$f/tools/test-order.sh" "$w/junit.xml" 10 < /dev/null 2>&1)
[ "$o" = 'first = ["tests/cases/test_b.av", "tests/cases/test_a.av"]' ]; say order-longest-first $? "$o"
sh "$f/tools/test-order.sh" --write "$w/junit.xml" 2 < /dev/null > /dev/null 2>&1
grep -qx 'first = \["tests/cases/test_b.av", "tests/cases/test_a.av", "tests/cases/test_c.av"\]' "$f/avant.toml" \
  && grep -qx 'exclusive = \["tests/cases/test_d.av"\]' "$f/avant.toml" && [ "$(wc -l < "$f/avant.toml")" = 6 ]
say order-write $? "$(tr '\n' ';' < "$f/avant.toml")"
echo done
