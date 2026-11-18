#!/bin/sh
# runtime/link-check.sh fails the runtime's build on a hidden function of a
# core unit that another object calls and the exports file does not list,
# and passes when it lists it. Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/runtime-link.sh WORK
#
# WORK is a new scratch directory. Two tiny core units and one other
# object are built with cc, the units merged as runtime/build.sh merges
# them (`ld -r`, then `objcopy --keep-global-symbols` with the exports
# file), and the check run on the result:
#   hidden-unlisted-fails: a hidden function one unit defines and the other
#     object calls, not in the exports file: status 1;
#   hidden-unlisted-named: the message names the symbol, the object that
#     calls it and the exports file as the reason;
#   hidden-listed-passes: the same with the function listed: status 0;
#   undefined-named: a call no object defines at all reads "no runtime
#     object defines it";
#   program-symbols-ignored: a non-avant_ undefined symbol (a C library
#     function) is not the check's.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/runtime-link.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }

# build NAME EXPORTS OTHER_SOURCE: DIR with avant_rt.o (two units merged)
# and avant_other.o.
build() {
  d=$w/$1; mkdir -p "$d/src" "$d/build"
  cat > "$d/src/unit_a.c" <<'EOF'
__attribute__((visibility("hidden"))) int avant_shared_helper(int x) { return x + 1; }
int avant_public_entry(int x) { return avant_shared_helper(x) * 2; }
EOF
  cat > "$d/src/unit_b.c" <<'EOF'
__attribute__((visibility("hidden"))) int avant_shared_helper(int x);
int avant_public_other(int x) { return avant_shared_helper(x) - 1; }
EOF
  printf '%s\n' "$3" > "$d/src/other.c"
  printf '%s\n' "$2" > "$d/src/avant_rt.exports"
  cc -O2 -c -o "$d/build/unit_a.o" "$d/src/unit_a.c" && cc -O2 -c -o "$d/build/unit_b.o" "$d/src/unit_b.c" \
    && ld -r -o "$d/build/avant_rt.o" "$d/build/unit_a.o" "$d/build/unit_b.o" \
    && objcopy --keep-global-symbols="$d/src/avant_rt.exports" "$d/build/avant_rt.o" \
    && rm -f "$d/build/unit_a.o" "$d/build/unit_b.o" \
    && cc -O2 -c -o "$d/build/avant_other.o" "$d/src/other.c" || { echo "FAIL: build $1: cc, ld or objcopy failed"; return 1; }
  sh "$root/runtime/link-check.sh" "$d/build" "$d/src/avant_rt.exports" < /dev/null > "$d.log" 2>&1
}
calls='__attribute__((visibility("hidden"))) int avant_shared_helper(int x);
#include <stdlib.h>
int avant_other_entry(int x) { return avant_shared_helper(x) + abs(x); }'

build unlisted "avant_public_entry
avant_public_other" "$calls"; r=$?
say hidden-unlisted-fails "$([ $r -eq 1 ] && echo 0 || echo 1)" "status $r, not 1: $(cat "$w/unlisted.log")"
grep -q 'avant_shared_helper, called by avant_other.o, .*avant_rt.exports does not list it' "$w/unlisted.log"
say hidden-unlisted-named $? "$(cat "$w/unlisted.log")"
if grep -q 'abs' "$w/unlisted.log"; then say program-symbols-ignored 1 "a C library call was read: $(cat "$w/unlisted.log")"; else say program-symbols-ignored 0; fi

build listed "avant_public_entry
avant_public_other
# runtime-internal
avant_shared_helper" "$calls"; r=$?
say hidden-listed-passes $r "status $r: $(cat "$w/listed.log")"

build undefined "avant_public_entry
avant_public_other
# runtime-internal
avant_shared_helper" "int avant_nowhere(int x);
int avant_other_entry(int x) { return avant_nowhere(x); }"; r=$?
grep -q 'avant_nowhere, called by avant_other.o, .*no runtime object defines it' "$w/undefined.log" && [ $r -eq 1 ]
say undefined-named $? "status $r: $(cat "$w/undefined.log")"
echo done
