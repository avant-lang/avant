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
#      stitched copy given to both compilers. The programs are copied first
#      to a directory outside both checkouts (under TMPDIR, else /tmp;
#      removed at the end), their `// file:` paths inside either checkout
#      renamed into the copy, and compiled from there: a compiler writes a
#      source location under its own checkout relative to that checkout,
#      so the checkout's own stitched copy would read relative for one
#      compiler and absolute for the other, every program LL-DIFF.
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
#
# BASE may instead be a base cache's entry (tools/base-cache.sh, a
# directory holding COMPLETE and ll.tsv): its side is then read from the
# cache, not compiled, and only NEW's C' runs. Corpus 1 compares NEW's
# programs with the entry's ll.tsv and, for langarena/tasks/, with its
# LangArena part's tasks.tsv; corpus 2 is the LangArena part's stitched
# programs (LA_TMP is not read), compiled by NEW's C' from the part's
# directory. Each side's checkout path is written as @ROOT@ before the
# digests are taken (tools/ll-digests.sh), so the digests in OUT differ
# from a two-checkout run's, the readings do not; NEW's .ll of a
# difference is kept in OUT/b/, the base's is not (build the base's C' and
# run the two-checkout form to see both). Its LangArena part must be the
# one of NEW's harness (base-cache.sh --la from NEW).
#
# A base cache entry whose LangArena part records programs the base's C'
# refuses (its refused.txt: NEW's harness writes the tasks in a spelling
# the base does not know): those NEW compiles, and the task files alone
# (langarena/tasks/) that only NEW compiles, read `not-comparable`, do not
# make the status 1, and the summary gains a line `langarena: not
# comparable: the base refuses N of M (first message: ...)`; the sources
# corpus is judged as ever.
#
# NEUTRAL_CHANGED=FILE (both forms): FILE lists the paths the change being
# judged edited, one per line (`git diff --name-only BASE HEAD`). A program
# in it (compiler/main.av too when a compiler/ path is in it) that reads
# LL-DIFF, refused-msg or STATUS reads `changed (WAS) PATH` instead: the
# change's own files, reported apart, which do not make the status 1.
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
cached=0
[ -f "$BW/COMPLETE" ] && [ -f "$BW/ll.tsv" ] && cached=1
if [ $cached = 1 ]; then cs="$NW"; else cs="$BW $NW"; fi
for c in $cs; do
  [ -x "$c/bin/avant-c-llvm" ] || { echo "neutral.sh: $c/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
done
[ -e "$OUT" ] && { echo "neutral.sh: $OUT exists" >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ $cached = 1 ]; then
  # The LangArena part of NEW's harness (built when missing; the entry's
  # tree is its key, so any revision with that tree names it).
  lap=$(BASE_CACHE=$(dirname "$BW") sh "$here/base-cache.sh" --la "$(basename "$BW")" < /dev/null) \
    || { echo "neutral.sh: no LangArena part for $BW (tools/base-cache.sh --la failed)" >&2; exit 2; }
  # The entry's lock held shared while it is read: tools/base-cache.sh
  # --prune removes no entry a run holds.
  exec 7>> "$BW.lock" && flock -s 7
fi
mkdir -p "$OUT/a" "$OUT/b"
export BW NW OUT LA
self=$(realpath "$0")
jobs=${NEUTRAL_JOBS:-8}

(cd "$NW" && find examples langarena/tasks tests/fixtures tests/cases tests/probe_matrix.av compiler/main.av -name '*.av' 2>/dev/null | LC_ALL=C sort) > "$OUT/list.txt"
# compare BASE.tsv NEW.tsv: tools/ll-digests.sh's lines of the two sides, one
# reading per NEW program, as --one reads it.
compare() {
  awk -F'\t' 'FILENAME == ARGV[1] { rc[$1] = $2; ll[$1] = $3; lg[$1] = $4; next }
    !($1 in rc) { print "new " $1; next }
    rc[$1] == 0 && $2 == 0 { if (ll[$1] == $3) print "ll-same " $3 " " $1; else print "LL-DIFF " $1; next }
    rc[$1] == $2 { if (lg[$1] == $4) print "refused-same " $1; else print "refused-msg " $1; next }
    { print "STATUS " rc[$1] "->" $2 " " $1 }' "$1" "$2"
}
if [ $cached = 1 ]; then
  # NEW's side compiled; a difference's .ll kept in OUT/b.
  LL_JOBS=$jobs LL_KEEP="$OUT/b/keep" sh "$here/ll-digests.sh" "$NW" "$OUT/list.txt" "$OUT/b.tsv" < /dev/null
  cat "$BW/ll.tsv" "$lap/tasks.tsv" | LC_ALL=C sort > "$OUT/a.tsv"
  compare "$OUT/a.tsv" "$OUT/b.tsv" > "$OUT/result.txt"
  ls "$lap/avant/langarena/tmp" | grep -v '\.cfg\.av$' | grep '\.av$' | LC_ALL=C sort > "$OUT/la-list.txt"
  LL_JOBS=$jobs LL_KEEP="$OUT/b/keep-la" sh "$here/ll-digests.sh" "$NW" "$OUT/la-list.txt" "$OUT/la-b.tsv" "$lap/avant/langarena/tmp" < /dev/null
  compare "$lap/ll.tsv" "$OUT/la-b.tsv" > "$OUT/la-result.txt"
  # The programs the base's C' refuses and NEW's compiles (the part's
  # refused.txt: NEW's harness writes its tasks in a spelling the base does
  # not know) cannot be compared: read `not-comparable`, apart.
  if [ -s "$lap/refused.txt" ]; then
    awk '$1 == "STATUS" && $2 ~ /->0$/ { print "not-comparable " $NF; next } { print }' "$OUT/la-result.txt" > "$OUT/la-result.part" \
      && mv "$OUT/la-result.part" "$OUT/la-result.txt"
    # The task files alone, in the sources corpus, likewise.
    awk '$1 == "STATUS" && $2 ~ /->0$/ && $NF ~ /^langarena\/tasks\// { print "not-comparable " $NF; next } { print }' "$OUT/result.txt" > "$OUT/result.part" \
      && mv "$OUT/result.part" "$OUT/result.txt"
    cp "$lap/refused.txt" "$OUT/la-refused.txt"
  fi
  # Keep NEW's .ll and messages of the programs that differ only.
  for r in result la-result; do
    k=keep; [ $r = la-result ] && k=keep-la
    grep -v '^\(ll-same\|refused-same\|new\) ' "$OUT/$r.txt" | awk '{ print $NF }' | while read -r f; do
      x=$(echo "$f" | tr '/.' '__')
      for y in ll log; do [ -e "$OUT/b/$k/$x.$y" ] && mv "$OUT/b/$k/$x.$y" "$OUT/b/$x.$y"; done
    done
    rm -rf "${OUT:?}/b/$k"
  done
  echo "base: the cache entry $BW (its LangArena part $lap)" > "$OUT/base.txt"
else
  xargs -P "$jobs" -I{} sh "$self" --one src {} < "$OUT/list.txt" > "$OUT/result.txt"
  : > "$OUT/la-list.txt"
  if [ -d "$LA" ]; then
    for f in "$LA"/*.av; do
      [ -f "$f" ] && head -n 1 "$f" | grep -q '^// file: ' && basename "$f"
    done | LC_ALL=C sort > "$OUT/la-list.txt"
  fi
  # The stitched copy outside both checkouts, its paths into either renamed.
  t=${TMPDIR:-/tmp}
  case "$(realpath -m "$t")/" in "$BW"/*|"$NW"/*) t=/tmp ;; esac
  LAC=$(mktemp -d "$t/neutral-la.XXXXXX") || exit 2
  trap 'rm -rf "$LAC"; exit 130' INT TERM HUP
  while read -r f; do
    sed "s#^// file: $NW/#// file: $LAC/new/#; s#^// file: $BW/#// file: $LAC/base/#" "$LA/$f" > "$LAC/$f"
  done < "$OUT/la-list.txt"
  echo "stitched programs: $LA, compiled from a copy outside both checkouts" > "$OUT/la-source.txt"
  LA=$LAC xargs -P "$jobs" -I{} sh "$self" --one la {} < "$OUT/la-list.txt" > "$OUT/la-result.txt"
  rm -rf "$LAC"; trap - INT TERM HUP
fi
# The change's own files, read apart (NEUTRAL_CHANGED).
if [ -n "${NEUTRAL_CHANGED:-}" ]; then
  for r in "$OUT/result.txt" "$OUT/la-result.txt"; do
    awk 'FILENAME == ARGV[1] { ch[$0] = 1; if ($0 ~ /^compiler\//) ch["compiler/main.av"] = 1; next }
      ($1 == "LL-DIFF" || $1 == "refused-msg" || $1 == "STATUS") && ($NF in ch) { w = $1; if ($1 == "STATUS") w = w " " $2; print "changed (" w ") " $NF; next }
      { print }' "$NEUTRAL_CHANGED" "$r" > "$r.part" && mv "$r.part" "$r"
  done
fi

count() { grep -c "^$1" "$2"; }
line() { # NAME RESULT LIST DIGESTS
  grep '^ll-same' "$2" | LC_ALL=C sort -k3 | awk '{ print $2 "  " $3 }' > "$4"
  echo "$1: files $(wc -l < "$3") ll-same $(count ll-same "$2") LL-DIFF $(count LL-DIFF "$2") refused-same $(count refused-same "$2") refused-msg $(count refused-msg "$2") STATUS $(count STATUS "$2") new $(count 'new ' "$2")$([ -n "${NEUTRAL_CHANGED:-}" ] && echo " changed $(count changed "$2")")$([ -s "$OUT/la-refused.txt" ] && echo " not-comparable $(count not-comparable "$2")") digest-of-digests $(sha256sum < "$4" | cut -c1-64)"
}
{
  line sources "$OUT/result.txt" "$OUT/list.txt" "$OUT/ll-digests.txt"
  line langarena "$OUT/la-result.txt" "$OUT/la-list.txt" "$OUT/la-digests.txt"
  if [ -s "$OUT/la-refused.txt" ]; then
    awk -F'\t' '$1 == "refused" { n = $2; of = $3 } $1 == "first" { f = $2; m = $3 }
      END { printf "langarena: not comparable: the base refuses %d of %d (first message: %s: %s); the sources corpus is judged alone\n", n, of, f, m }' "$OUT/la-refused.txt"
  fi
  [ -s "$OUT/la-list.txt" ] || echo "langarena: no stitched program in $LA (run the harness once: its prod or test run stitches every task)"
} | tee "$OUT/summary.txt"
rmdir "$OUT/a" "$OUT/b" 2>/dev/null
if grep -q '^\(LL-DIFF\|STATUS\)' "$OUT/result.txt" "$OUT/la-result.txt"; then exit 1; fi
[ -s "$OUT/la-list.txt" ] || exit 1
exit 0
