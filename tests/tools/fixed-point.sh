#!/bin/sh
# tools/fixed-point.sh in fake checkouts whose tools/letters.sh is a stub:
# the compilers it "builds" are one shell script under both names, which
# writes compiler/main.av as its .ll, D' adding a line when the checkout has
# a file named `drift`. Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/fixed-point.sh WORK
#
#   fp-same: C' = D' gives status 0 and says so;
#   fp-differ: a .ll difference gives status 1, named;
#   fp-build-fails: a failed build gives status 2;
#   fp-bisect: `git bisect run` with a copy of the script, over four merges
#     of which the third brings the difference, names that merge.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/fixed-point.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
cp "$root/tools/fixed-point.sh" "$w/fixed-point.sh"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
g() { git -c user.name=t -c user.email=t@t "$@"; }
fake() { # NAME
  f=$w/$1
  mkdir -p "$f/tools" "$f/compiler"
  echo 'fn main: Int { return 0 }' > "$f/compiler/main.av"
  cat > "$f/tools/letters.sh" <<'EOF'
#!/bin/sh
[ -e fail-build ] && { echo "stub: build failed" >&2; exit 1; }
mkdir -p bin
cat > bin/avant-c-llvm <<'C'
#!/bin/sh
cat "$2" > "$3"
case "$0" in *d-llvm) [ -e drift ] && echo drift >> "$3" ;; esac
exit 0
C
cp bin/avant-c-llvm bin/avant-d-llvm; chmod +x bin/avant-c-llvm bin/avant-d-llvm
EOF
  printf 'bin/\n' > "$f/.gitignore"
  (cd "$f" && git init -q -b master . && g add -A && g commit -q -m base) > /dev/null 2>&1
  echo "$f"
}
fp() { # DIR OUT
  (cd "$1" && env TMPDIR="$w" sh "$w/fixed-point.sh" "$2" < /dev/null > "$2.log" 2>&1)
}

f=$(fake same)
fp "$f" "$w/same.out"; rc=$?
[ $rc = 0 ] && grep -q "^fixed point: C' = D' at " "$w/same.out/fixed-point.txt"
say fp-same $? "status $rc; $(head -n 1 "$w/same.out.log")"

f=$(fake differ); : > "$f/drift"
fp "$f" "$w/differ.out"; rc=$?
[ $rc = 1 ] && grep -q '^fixed point: red at .* (.ll DIFFER, executables same)$' "$w/differ.out/fixed-point.txt"
say fp-differ $? "status $rc; $(head -n 1 "$w/differ.out.log")"

f=$(fake build); : > "$f/fail-build"
fp "$f" "$w/build.out"; rc=$?
[ $rc = 2 ] && grep -q '^fixed point: red at .* (the build failed' "$w/build.out/fixed-point.txt"
say fp-build-fails $? "status $rc; $(head -n 1 "$w/build.out.log")"

# Four merges onto the base; the third adds the drift.
f=$(fake bisect)
(
  cd "$f" || exit 1
  for k in 1 2 3 4; do
    g checkout -q -b lane$k master
    echo "lane $k" > "lane$k.txt"
    [ $k = 3 ] && : > drift
    g add -A && g commit -q -m "lane $k"
    g checkout -q master
    g merge -q --no-ff -m "merge lane $k" lane$k
  done
) > /dev/null 2>&1
bad=$(git -C "$f" log --first-parent --format=%H --grep='^merge lane 3$' master)
(
  cd "$f" && g bisect start --first-parent master master~4 > /dev/null 2>&1 \
    && TMPDIR="$w" g bisect run sh "$w/fixed-point.sh" > "$w/bisect.log" 2>&1
  g bisect reset > /dev/null 2>&1
) < /dev/null
# git 2.56 quotes the term: "is the first 'bad' commit".
grep -Eq "^$bad is the first '?bad'? commit" "$w/bisect.log"
say fp-bisect $? "expected $bad; $(grep -E "first '?bad'?" "$w/bisect.log" | head -n 1)"
echo done
