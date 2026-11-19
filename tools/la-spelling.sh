#!/bin/sh
# la-spelling.sh — do two LangArena harnesses' programs compile alike?
#
#   sh tools/la-spelling.sh OLD NEW OUT      (from the checkout root)
#
# OLD and NEW are langarena/ directories (the harness and its tasks, such
# as the main checkout's copy and a lane's, or two scratch checkouts of
# the harness's repository); OUT is a new directory. Each side's programs
# are stitched by that side's own harness, exactly as its prod pass
# stitches them (helper.av, the task's extra files, its configuration from
# LangArena/run.js beside this checkout, the task; the twins too): the
# side is copied to one work place, OUT/work/avant/langarena, the same
# path for both, its harness built there (tools/la-harness.sh) and its
# prod pass run with a compiler that records each program's compile
# arguments and refuses, so no task runs. Every stitched program is then
# compiled by one compiler, this checkout's C' (bin/avant-c-llvm; a
# checkout that has one: LA_SPELLING_ROOT), through `__emit-ll`
# (tools/ll-digests.sh, from the work place: the two sides' paths are the
# same, so their .ll compare byte for byte).
#
#   OUT/old.tsv, OUT/new.tsv   tools/ll-digests.sh's lines of each side
#   OUT/old/, OUT/new/         each side's .ll and compiler output
#                              (LL_KEEP), and its args/SLUG
#   OUT/result.tsv             PROGRAM <TAB> SAME | LL-DIFF | ARGS-DIFF |
#                              REFUSED (old status, new status) | ONLY-OLD |
#                              ONLY-NEW
#   OUT/diff/PROGRAM.diff      the first 400 lines of `diff -u` of each
#                              LL-DIFF's two .ll
#
# The last line on stdout says how many programs compile alike; status 0
# when every program of either side is SAME, 1 otherwise, 2 on a usage
# error or when a side could not be stitched. It compiles but runs no
# task: the machine lock's shared side. Stdin from /dev/null.
set -u
[ $# -eq 3 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
cc_root=$(realpath "${LA_SPELLING_ROOT:-$root}") || exit 2
old=$(realpath "$1") || exit 2; new=$(realpath "$2") || exit 2; out=$(realpath -m "$3")
[ -e "$out" ] && { echo "la-spelling.sh: $out exists" >&2; exit 2; }
for s in "$old" "$new"; do
  [ -f "$s/harness/measure.cr" ] || { echo "la-spelling.sh: $s is not a langarena/ directory (no harness/measure.cr)" >&2; exit 2; }
done
[ -x "$cc_root/bin/avant-c-llvm" ] || { echo "la-spelling.sh: no compiler $cc_root/bin/avant-c-llvm" >&2; exit 2; }
[ -f "$root/../LangArena/run.js" ] || { echo "la-spelling.sh: no LangArena/run.js beside $root" >&2; exit 2; }
mkdir -p "$out/work/avant/tools" "$out/stitch" || exit 2
w=$out/work/avant
cp "$here/la-harness.sh" "$w/tools/la-harness.sh"
ln -s "$(realpath "$root/../LangArena")" "$out/work/LangArena"
cat > "$out/stitch/record-args" <<'EOF'
#!/bin/sh
# Records the compile arguments the harness gives (after `compile ROOT
# BIN`), one per line, in $STITCH_ARGS/SLUG; then refuses, so no task runs.
[ "$1" = compile ] || exit 1
slug=$(basename "$3"); shift 3
: > "$STITCH_ARGS/$slug"
for a in "$@"; do printf '%s\n' "$a" >> "$STITCH_ARGS/$slug"; done
exit 1
EOF
chmod +x "$out/stitch/record-args"

# side NAME DIR: DIR stitched at the work place and compiled.
side() {
  n=$1 src=$2
  rm -rf "$w/langarena"; mkdir -p "$w/langarena" "$out/$n/args" || exit 2
  (cd "$src" && tar --exclude=./tmp --exclude=./.git -cf - .) | tar -xf - -C "$w/langarena" || { echo "la-spelling.sh: copying $src failed" >&2; exit 2; }
  h=$(cd "$w" && timeout -k 10 900 sh tools/la-harness.sh 2> "$out/$n/harness.log" < /dev/null) \
    || { echo "la-spelling.sh: the $n side's harness did not build ($out/$n/harness.log)" >&2; exit 2; }
  (cd "$w" && env STITCH_ARGS="$out/$n/args" AVANT_COMPILER="$out/stitch/record-args" AVANT_CODEGEN=release AVANT_RESULTS_DIR="$out/stitch/$n" \
    timeout -k 10 900 "$h" prod < /dev/null > "$out/$n/stitch.log" 2>&1)
  ls "$out/$n/args" | LC_ALL=C sort | sed 's/$/.av/' > "$out/$n/list.txt"
  [ -s "$out/$n/list.txt" ] || { echo "la-spelling.sh: the $n side's harness stitched no program ($out/$n/stitch.log)" >&2; exit 2; }
  LL_KEEP=$out/$n sh "$here/ll-digests.sh" "$cc_root" "$out/$n/list.txt" "$out/$n.tsv" "$w/langarena/tmp" < /dev/null 2>> "$out/$n/stitch.log"
}
side old "$old"
side new "$new"
rm -rf "$out/work" "$out/stitch"

# The two sides, program by program.
mkdir -p "$out/diff"
LC_ALL=C join -t "$(printf '\t')" -a 1 -a 2 -e - -o 0,1.2,1.3,2.2,2.3 "$out/old.tsv" "$out/new.tsv" | while IFS="$(printf '\t')" read -r p ors oll nrs nll; do
  s=${p%.av}; k=$(echo "$p" | tr '/.' '__')
  if [ "$ors" = - ]; then v=ONLY-NEW
  elif [ "$nrs" = - ]; then v=ONLY-OLD
  elif [ "$ors" != 0 ] || [ "$nrs" != 0 ]; then v="REFUSED ($ors, $nrs)"
  elif [ "$oll" != "$nll" ]; then
    v=LL-DIFF
    diff -u "$out/old/$k.ll" "$out/new/$k.ll" 2>/dev/null | head -n 400 > "$out/diff/$s.diff"
  elif ! cmp -s "$out/old/args/$s" "$out/new/args/$s"; then v=ARGS-DIFF
  else v=SAME; fi
  printf '%s\t%s\n' "$p" "$v"
done > "$out/result.tsv"
rmdir "$out/diff" 2>/dev/null
n=$(wc -l < "$out/result.tsv")
same=$(grep -c '	SAME$' "$out/result.tsv")
if [ "$same" -eq "$n" ]; then
  echo "la-spelling: $n programs, every one compiles alike (the same .ll through $cc_root/bin/avant-c-llvm, the same compile arguments)"
  exit 0
fi
echo "la-spelling: $n programs, $same compile alike; $((n - same)) do not: $(grep -v '	SAME$' "$out/result.tsv" | sed 's/\.av	/ /' | head -n 8 | tr '\n' ',' | sed 's/,$//; s/,/, /g')$([ $((n - same)) -gt 8 ] && echo ', ...') (result.tsv, diff/)"
exit 1
