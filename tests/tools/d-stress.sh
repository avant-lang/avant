#!/bin/sh
# tools/d-stress.sh and tools/letters.sh's d-stress step in a fake checkout:
# the real letters.sh and d-stress.sh, a seed and a C' that are shell
# scripts. C' writes the compiler it is asked for as a copy of itself, under
# the stress mode too (the compiler's output does not depend on it), and
# records the environment it was started in. Run by tests/cases/test_tools.av;
# by hand:
#
#   sh tests/tools/d-stress.sh WORK
#
#   stress-same: D' built under stress is C' byte for byte: status 0, said;
#   stress-env: that build ran with AVANT_GC_STRESS=1 and the verify mode;
#   stress-env-plain: the plain letters' D' did not set the stress mode;
#   stress-removed: bin/avant-d-stress is removed after the comparison;
#   stress-kept: and kept with D_STRESS_KEEP=1;
#   stress-differ: a D' that differs gives status 1, named;
#   stress-build-fails: a failed build gives status 2;
#   stress-timeout: a build past LETTERS_STRESS_TIMEOUT gives status 2, with
#     the limit's status (124) in the line;
#   stress-no-c: no C' gives status 2.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/d-stress.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w" || exit 2
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
g() { git -c user.name=t -c user.email=t@t "$@"; }
# The seed: it answers `version` and `compile SRC OUT ...` writes a copy of
# itself (B is not part of this test).
mkdir -p "$w/seed"
cat > "$w/seed/avant" <<'SEED'
#!/bin/sh
[ "$1" = version ] && exit 0
[ "$1" = compile ] || exit 2
cp "$0" "$3"; chmod +x "$3"
SEED
chmod +x "$w/seed/avant"
fake() { # NAME
  f=$w/$1
  mkdir -p "$f/tools" "$f/compiler" "$f/runtime/build" "$f/bin"
  echo 'fn main: Int { return 0 }' > "$f/compiler/main.av"
  : > "$f/runtime/build.sh"; : > "$f/runtime/build/avant_llvm.o"
  cp "$root/tools/letters.sh" "$root/tools/d-stress.sh" "$f/tools/"
  printf '[package]\nname = "fake"\nversion = "0.0.1"\n' > "$f/avant.toml"
  printf 'bin/\ncalls.txt\n' > "$f/.gitignore"
  # C': `compile SRC OUT FLAGS...` writes a copy of itself (and a line when
  # the checkout has a file named `drift` and the run is under stress), and
  # records the two modes it ran in.
  cat > "$f/bin/avant-c-llvm" <<'CSTUB'
#!/bin/sh
[ "$1" = version ] && exit 0
[ "$1" = compile ] || exit 2
[ -e fail-stress ] && [ "${AVANT_GC_STRESS:-}" = 1 ] && { echo "stub: failed under stress" >&2; exit 1; }
[ -e slow-stress ] && [ "${AVANT_GC_STRESS:-}" = 1 ] && sleep 30
out=$3
echo "stress=${AVANT_GC_STRESS:-unset} verify=${AVANT_VERIFY:-unset}" >> calls.txt
cp "$0" "$out"
[ -e drift ] && [ "${AVANT_GC_STRESS:-}" = 1 ] && echo drift >> "$out"
chmod +x "$out"
exit 0
CSTUB
  chmod +x "$f/bin/avant-c-llvm"
  (cd "$f" && git init -q -b master . && g add -A && g commit -q -m base) > /dev/null 2>&1
  echo "$f"
}
ds() { # DIR OUT [ENV...]: d-stress.sh in DIR, its status in $rc
  d=$1 o=$2; shift 2
  (cd "$d" && env LETTERS_SEED="$w/seed" LETTERS_MOLD=none "$@" sh tools/d-stress.sh "$o" < /dev/null > "$o.log" 2>&1); rc=$?
}

f=$(fake same)
ds "$f" "$w/same.out"
[ $rc = 0 ] && grep -q "^d-stress: D' built under AVANT_GC_STRESS=1 is C' byte for byte at " "$w/same.out/d-stress.txt" && [ "$(grep -c '^[0-9a-f]\{64\}  bin/avant-' "$w/same.out/d-stress.txt")" = 2 ]
say stress-same $? "status $rc; $(tr '\n' ';' < "$w/same.out/d-stress.txt" 2>/dev/null | cut -c1-300) $(head -n 3 "$w/same.out.log" | tr '\n' ';')"
[ "$(cat "$f/calls.txt" 2>/dev/null)" = "stress=1 verify=1" ]
say stress-env $? "$(tr '\n' ';' < "$f/calls.txt" 2>/dev/null)"
[ ! -e "$f/bin/avant-d-stress" ]
say stress-removed $? "$(ls "$f/bin" | tr '\n' ' ')"
ds "$f" "$w/keep.out" D_STRESS_KEEP=1
[ $rc = 0 ] && [ -x "$f/bin/avant-d-stress" ]
say stress-kept $? "status $rc; $(ls "$f/bin" | tr '\n' ' ')"
# The plain letters' D' does not run under stress.
rm -f "$f/calls.txt"
(cd "$f" && env LETTERS_SEED="$w/seed" LETTERS_MOLD=none sh tools/letters.sh d-llvm < /dev/null > /dev/null 2>&1)
[ "$(cat "$f/calls.txt" 2>/dev/null)" = "stress=unset verify=1" ]
say stress-env-plain $? "$(tr '\n' ';' < "$f/calls.txt" 2>/dev/null)"

f=$(fake differ); : > "$f/drift"
ds "$f" "$w/differ.out"
[ $rc = 1 ] && grep -q "^d-stress: red at .* (D' built under AVANT_GC_STRESS=1 differs from C'" "$w/differ.out/d-stress.txt"
say stress-differ $? "status $rc; $(head -n 1 "$w/differ.out.log")"

f=$(fake build); : > "$f/fail-stress"
ds "$f" "$w/build.out"
[ $rc = 2 ] && grep -q "^d-stress: red at .* (the build failed with status 1 after " "$w/build.out/d-stress.txt"
say stress-build-fails $? "status $rc; $(head -n 1 "$w/build.out.log")"

f=$(fake slow); : > "$f/slow-stress"
ds "$f" "$w/slow.out" LETTERS_STRESS_TIMEOUT=1
[ $rc = 2 ] && grep -q "^d-stress: red at .* (the build failed with status 124 after " "$w/slow.out/d-stress.txt"
say stress-timeout $? "status $rc; $(head -n 1 "$w/slow.out.log")"

f=$(fake noc); rm -f "$f/bin/avant-c-llvm"
ds "$f" "$w/noc.out"
[ $rc = 2 ] && grep -q "^d-stress: red at .* (no C': " "$w/noc.out/d-stress.txt"
say stress-no-c $? "status $rc; $(head -n 1 "$w/noc.out.log")"
echo done
