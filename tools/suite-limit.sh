#!/bin/sh
# suite-limit.sh — the time limit of one suite pass, in seconds:
#
#   sh tools/suite-limit.sh [VAR=value...]     (from the checkout root)
#
# The words are the pass's environment, as tools/gate.sh passes it (none:
# the plain pass). A pass's limit is SUITE_LIMIT (default 900), or the
# larger own limit of a test file in that pass's environment:
# tests/limits.txt holds one line per such file, `PATH ENV SECONDS REASON`,
# ENV the environment's words joined by `,` (`plain` for none), and `#`
# starts a comment. The suite runs its files side by side, longest first,
# so a pass lasts about as long as its longest file: a file that needs
# more time than the pass gives it in one environment (the compiler's own
# dump under the verifier with a 1 MiB young generation) is given its
# own limit there, and every other pass keeps SUITE_LIMIT. The checks
# and the environment stay the same. A line whose SECONDS is not a
# number, or a PATH that is not a file of the suite, stops the gate
# (exit 2) rather than being passed over.
set -u
limit=${SUITE_LIMIT:-900}
case "$limit" in ''|*[!0-9]*) echo "suite-limit.sh: SUITE_LIMIT must be a number" >&2; exit 2 ;; esac
env=$(printf '%s' "$*" | tr -s ' ' ',')
[ -n "$env" ] || env=plain
f=tests/limits.txt
[ -f "$f" ] || { echo "$limit"; exit 0; }
awk -v e="$env" -v lim="$limit" '
  /^[ \t]*(#|$)/ { next }
  {
    if ($3 !~ /^[0-9]+$/) { print "suite-limit.sh: " FILENAME ":" NR ": SECONDS is not a number: " $3 > "/dev/stderr"; bad = 1; exit }
    if ($1 !~ /^tests\/cases\/test_[A-Za-z0-9_]+\.av$/ || system("test -f " $1) != 0) { print "suite-limit.sh: " FILENAME ":" NR ": not a test file of the suite: " $1 > "/dev/stderr"; bad = 1; exit }
    if ($2 == e && $3 + 0 > lim + 0) { lim = $3 }
  }
  END { if (bad) { exit 2 } print lim }
' "$f"
