#!/bin/sh
# neutral.sh — compiler neutrality: every program of a corpus through
# `__emit-ll` with two compilers, the .ll compared byte for byte.
#
#   sh tools/neutral.sh BASE NEW OUT [LA_TMP]
#
# BASE and NEW are checkouts, each with its C' built (bin/avant-c-llvm, a
# compiler that reads the lib/ of the checkout it was built in). OUT is a
# new directory. Two corpora:
#
#   1. the sources of NEW: examples/, langarena/tasks/, tests/fixtures/,
#      tests/cases/, tests/probe_matrix.av and compiler/main.av, each run
#      from its own checkout (BASE's copy of the file when it has one, else
#      the file is `new`), the BASE checkout's path written as NEW's in the
#      .ll and the messages. A LangArena task file alone is not a program
#      (the harness stitches it with its helper and configuration), so
#      langarena/tasks/ mostly compares refusals;
#   2. the LangArena programs as the harness stitches them: every *.av in
#      LA_TMP (default NEW/langarena/tmp, filled by any run of the harness)
#      whose first line is the harness's `// file: ` marker, the one
#      stitched copy given to both compilers from LA_TMP's directory (its
#      `// file:` paths, which the compiler reads as the source locations,
#      are the same on both sides).
#
# Each program reads: ll-same (both compile, the same .ll), LL-DIFF,
# refused-same (both refuse with the same message), refused-msg (both
# refuse, the message differs), STATUS a->b (one compiles, the other not),
# or new. OUT/result.txt (corpus 1), OUT/la-result.txt (corpus 2),
# OUT/ll-digests.txt and OUT/la-digests.txt (the sha256 of each identical
# .ll), and a summary line per corpus on stdout and in OUT/summary.txt.
# Status 0 when nothing reads LL-DIFF or STATUS, 1 otherwise, 2 on usage.
# NEUTRAL_JOBS programs at once (default 8). Every compile runs under
# `timeout -k 5 300`, AVANT_ROOT unset, stdin from /dev/null.
set -u

if [ "${1:-}" = --one ]; then
  # --one KIND FILE: one program of a corpus (the script calling itself
  # through xargs). KIND is src or la.
  kind=$2 f=$3
  k=$(echo "$f" | tr '/.' '__')
  if [ "$kind" = la ]; then
    wa=$LA wb=$LA src=$f
  else
    wa=$BW wb=$NW src=$f
  fi
  if [ "$kind" = la ] || [ -e "$BW/$f" ]; then
    (cd "$wa" && env -u AVANT_ROOT timeout -k 5 300 "$BW/bin/avant-c-llvm" __emit-ll "$src" "$OUT/a/$k.ll" > "$OUT/a/$k.log" 2>&1 < /dev/null; echo $? > "$OUT/a/$k.rc")
    if [ "$kind" = src ]; then
      [ -e "$OUT/a/$k.ll" ] && sed -i "s#$BW#$NW#g" "$OUT/a/$k.ll"
      sed -i "s#$BW#$NW#g" "$OUT/a/$k.log"
    fi
  else
    echo new > "$OUT/a/$k.rc"; : > "$OUT/a/$k.log"
  fi
  (cd "$wb" && env -u AVANT_ROOT timeout -k 5 300 "$NW/bin/avant-c-llvm" __emit-ll "$src" "$OUT/b/$k.ll" > "$OUT/b/$k.log" 2>&1 < /dev/null; echo $? > "$OUT/b/$k.rc")
  ra=$(cat "$OUT/a/$k.rc"); rb=$(cat "$OUT/b/$k.rc")
  if [ "$ra" = new ]; then
    echo "new $f"; rm -f "$OUT/b/$k.ll"
  elif [ "$ra" = 0 ] && [ "$rb" = 0 ]; then
    if cmp -s "$OUT/a/$k.ll" "$OUT/b/$k.ll"; then
      echo "ll-same $(sha256sum < "$OUT/b/$k.ll" | cut -c1-64) $f"; rm -f "$OUT/a/$k.ll" "$OUT/b/$k.ll" "$OUT/a/$k.log" "$OUT/b/$k.log"
    else
      echo "LL-DIFF $f"
    fi
  elif [ "$ra" = "$rb" ]; then
    if cmp -s "$OUT/a/$k.log" "$OUT/b/$k.log"; then echo "refused-same $f"; rm -f "$OUT/a/$k.log" "$OUT/b/$k.log"; else echo "refused-msg $f"; fi
    rm -f "$OUT/a/$k.ll" "$OUT/b/$k.ll"
  else
    echo "STATUS $ra->$rb $f"
  fi
  rm -f "$OUT/a/$k.rc" "$OUT/b/$k.rc"
  exit 0
fi

[ $# -ge 3 ] && [ $# -le 4 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
BW=$(realpath "$1") NW=$(realpath "$2") OUT=$(realpath -m "$3")
LA=$(realpath -m "${4:-$NW/langarena/tmp}")
for c in "$BW" "$NW"; do
  [ -x "$c/bin/avant-c-llvm" ] || { echo "neutral.sh: $c/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
done
[ -e "$OUT" ] && { echo "neutral.sh: $OUT exists" >&2; exit 2; }
mkdir -p "$OUT/a" "$OUT/b"
export BW NW OUT LA
self=$(realpath "$0")
jobs=${NEUTRAL_JOBS:-8}

(cd "$NW" && find examples langarena/tasks tests/fixtures tests/cases tests/probe_matrix.av compiler/main.av -name '*.av' 2>/dev/null | LC_ALL=C sort) > "$OUT/list.txt"
xargs -P "$jobs" -I{} sh "$self" --one src {} < "$OUT/list.txt" > "$OUT/result.txt"
: > "$OUT/la-list.txt"
if [ -d "$LA" ]; then
  for f in "$LA"/*.av; do
    [ -f "$f" ] && head -n 1 "$f" | grep -q '^// file: ' && basename "$f"
  done | LC_ALL=C sort > "$OUT/la-list.txt"
fi
xargs -P "$jobs" -I{} sh "$self" --one la {} < "$OUT/la-list.txt" > "$OUT/la-result.txt"

count() { grep -c "^$1" "$2"; }
line() { # NAME RESULT LIST DIGESTS
  grep '^ll-same' "$2" | LC_ALL=C sort -k3 | awk '{ print $2 "  " $3 }' > "$4"
  echo "$1: files $(wc -l < "$3") ll-same $(count ll-same "$2") LL-DIFF $(count LL-DIFF "$2") refused-same $(count refused-same "$2") refused-msg $(count refused-msg "$2") STATUS $(count STATUS "$2") new $(count 'new ' "$2") digest-of-digests $(sha256sum < "$4" | cut -c1-64)"
}
{
  line sources "$OUT/result.txt" "$OUT/list.txt" "$OUT/ll-digests.txt"
  line langarena "$OUT/la-result.txt" "$OUT/la-list.txt" "$OUT/la-digests.txt"
  [ -s "$OUT/la-list.txt" ] || echo "langarena: no stitched program in $LA (run the harness once: its prod or test run stitches every task)"
} | tee "$OUT/summary.txt"
rmdir "$OUT/a" "$OUT/b" 2>/dev/null
if grep -q '^\(LL-DIFF\|STATUS\)' "$OUT/result.txt" "$OUT/la-result.txt"; then exit 1; fi
[ -s "$OUT/la-list.txt" ] || exit 1
exit 0
