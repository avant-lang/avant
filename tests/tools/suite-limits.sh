#!/bin/sh
# tools/suite-limit.sh's pass limits (tests/limits.txt) in a fake checkout:
#   suite-limit-plain: a pass with no environment keeps the default;
#   suite-limit-own: a file's own limit in its environment is the pass's;
#   suite-limit-other-env: it does not raise another environment's pass;
#   suite-limit-words: an environment of two words matches its `,` form;
#   suite-limit-shard: a SHARD=I/N word is no part of the environment;
#   suite-limit-default: SUITE_LIMIT is the default, and an own limit below
#     it does not lower it;
#   suite-limit-bad-seconds, suite-limit-not-a-test: a malformed line stops
#     (exit 2) instead of being passed over;
#   suite-limit-repo: the checkout's own tests/limits.txt is well formed.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/suite-limits.sh WORK
#
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/suite-limits.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
f=$w/avant
rm -rf "$f"
mkdir -p "$f/tools" "$f/tests/cases"
cp "$root/tools/suite-limit.sh" "$f/tools/suite-limit.sh"
: > "$f/tests/cases/test_a.av"
: > "$f/tests/cases/test_b.av"
cat > "$f/tests/limits.txt" <<'LIM'
# PATH ENV SECONDS REASON
tests/cases/test_a.av AVANT_YOUNG_MB=1 1500 a long file

tests/cases/test_b.av AVANT_GC_VERIFY=1,AVANT_NURSERY=0 1200 another
tests/cases/test_b.av AVANT_YOUNG_MB=1 600 below the default
LIM
say() { if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1: got $2, want $3"; fi; }
lim() { (cd "$f" && sh tools/suite-limit.sh "$@" 2>&1 < /dev/null); }
say suite-limit-plain "$(lim)" 900
say suite-limit-own "$(lim AVANT_YOUNG_MB=1)" 1500
say suite-limit-other-env "$(lim AVANT_GC_VERIFY=1)" 900
say suite-limit-words "$(lim AVANT_GC_VERIFY=1 AVANT_NURSERY=0)" 1200
say suite-limit-shard "$(lim AVANT_YOUNG_MB=1 SHARD=2/3)" 1500
say suite-limit-shard-plain "$(lim SHARD=1/2)" 900
say suite-limit-default "$(cd "$f" && SUITE_LIMIT=1800 sh tools/suite-limit.sh AVANT_YOUNG_MB=1 < /dev/null)" 1800
cp "$f/tests/limits.txt" "$w/limits.good"
echo "tests/cases/test_a.av AVANT_GC_STRESS=1 soon a reason" >> "$f/tests/limits.txt"
(cd "$f" && sh tools/suite-limit.sh AVANT_GC_STRESS=1 > /dev/null 2>&1 < /dev/null); say suite-limit-bad-seconds $? 2
cp "$w/limits.good" "$f/tests/limits.txt"
echo "tests/cases/test_gone.av AVANT_GC_STRESS=1 1000 a reason" >> "$f/tests/limits.txt"
(cd "$f" && sh tools/suite-limit.sh AVANT_GC_STRESS=1 > /dev/null 2>&1 < /dev/null); say suite-limit-not-a-test $? 2
ok=0
for e in "" AVANT_GC_VERIFY=1 AVANT_YOUNG_MB=1 AVANT_NURSERY=0 AVANT_GC_STRESS=1; do
  # shellcheck disable=SC2086 # e holds VAR=value words
  (cd "$root" && sh tools/suite-limit.sh $e > /dev/null 2>&1 < /dev/null) || ok=1
done
say suite-limit-repo $ok 0
echo done
