#!/bin/sh
# la-half.sh — the second half's programs (the 52 tasks and the twin of
# langarena/second-half/) screened by instruction counts, the way
# tools/la-count.sh counts the original 50: each program as the half's own
# builder stitches it, compiled by a base's C' and by a head's C', counted
# once each, and a table of the two.
#
#   sh tools/la-half.sh stitch LANGARENA LADIR
#   sh tools/la-half.sh screen OUT LANGARENA BASE_ROOT HEAD_ROOT [SLUG...]
#   sh tools/la-half.sh table HEAD.tsv BASE.tsv NAMES.tsv
#
# stitch: LANGARENA is the langarena repository's checkout (its helper.av and
# second-half/ and the original suite's tasks/ are read, copied first: nothing
# is written there). The half's
# builder (second-half/refs/run.py, `avant prod`) is run with a compiler that
# only records what it was asked to compile, so the programs are stitched by
# the builder itself, at the prod sizes, and never by a copy of its logic
# here. LADIR gets what tools/la-count.sh reads of a base cache's LangArena
# part: avant/langarena/tmp/SLUG.av for each program and args/SLUG, the
# compile arguments (one per line), and names.tsv (SLUG, then the task's
# name). Exit 0, 1 when the builder did not name every program, 2 on usage.
#
# screen: stitch into OUT/ladir, then count every program (or the SLUGs
# given) with BASE_ROOT's C' and with HEAD_ROOT's (each a checkout with its
# C' built: bin/avant-c-llvm), tools/la-count.sh run once on each
# (OUT/base.tsv, OUT/head.tsv; its environment applies: LA_RUNNER, LA_JOBS,
# LA_SEEDS, LA_MEM_RUNS), and write OUT/table.md. The counts are not times:
# the machine's load does not change them, so this runs under the shared lock
# (LA_RUNNER splits each side into steps of their own). The half is held
# out of tuning: its table informs and never decides a change.
#
# table: the markdown table of two count files (la-count.sh's OUT.tsv: SLUG,
# instructions, the stdout's md5, status, first line) with the names of
# NAMES.tsv: per program the base's and the head's count and their ratio,
# the output (the checksum line) the same or not, then the geometric mean of
# the ratios, how many moved beyond 0.1% each way, and the programs that
# differ in output or did not run, which are the red part. Exit 0 whatever
# the counts say; 1 when a program is red (compile-failed, a status other
# than 0, or an output that differs between the sides); 2 on usage.
set -u
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }

stitch() { # LANGARENA LADIR
  src=$(realpath "$1") || return 2
  ladir=$(realpath -m "$2")
  [ -f "$src/helper.av" ] && [ -f "$src/second-half/refs/run.py" ] || { echo "la-half.sh: $src is not a langarena checkout with second-half/" >&2; return 2; }
  work=$(mktemp -d "${TMPDIR:-/tmp}/la-half.XXXXXX") || return 2
  mkdir -p "$ladir/avant/langarena/tmp" "$ladir/args" "$work/langarena/second-half/refs"
  # What the builder reads, copied: it writes under second-half/tmp beside itself.
  cp "$src/helper.av" "$work/langarena/"
  cp "$src/second-half/refs/run.py" "$work/langarena/second-half/refs/"
  cp -r "$src/second-half/refs/tasks" "$work/langarena/second-half/refs/"
  cp -r "$src/second-half/tasks" "$work/langarena/second-half/"
  # The twin of Json::Requests stitches in the original suite's reader, read only.
  cp -r "$src/tasks" "$work/langarena/"
  cat > "$work/record-compiler" <<'STUB'
#!/bin/sh
# compile SOURCE OUT ARGS...: keeps the stitched source and the arguments, and refuses.
[ "$1" = compile ] || exit 2
s=$(basename "$2" .av)
cp "$2" "$LH_LADIR/avant/langarena/tmp/$s.av" || exit 2
shift 3
printf '%s\n' "$@" > "$LH_LADIR/args/$s"
exit 1
STUB
  chmod +x "$work/record-compiler"
  env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_COMPILER="$work/record-compiler" LH_LADIR="$ladir" \
    python3 -I "$work/langarena/second-half/refs/run.py" avant prod > "$work/builder.log" 2>&1 < /dev/null
  # The builder exits 1 (the recorder refuses every compile); what it must have done is named each program.
  : > "$ladir/names.tsv"
  sed -n 's/^avant compile \(.*\) (prod): .*/\1/p' "$work/builder.log" | while IFS= read -r n; do
    printf '%s\t%s\n' "$(printf '%s' "$n" | sed 's/::/_/g; s/-/_/g' | tr 'A-Z' 'a-z')" "$n"
  done | LC_ALL=C sort > "$ladir/names.tsv"
  got=$(ls "$ladir/avant/langarena/tmp" | grep -c '\.av$')
  named=$(wc -l < "$ladir/names.tsv")
  rm -rf "$work"
  if [ "$got" -eq 0 ] || [ "$got" -ne "$named" ] || [ "$got" -ne "$(ls "$ladir/args" | wc -l)" ]; then
    echo "la-half.sh: the builder named $named programs and $got were stitched ($(ls "$ladir/args" | wc -l) with arguments)" >&2
    return 1
  fi
  echo "la-half.sh: $got programs stitched into $ladir" >&2
}

case "${1:-}" in
  stitch) [ $# -eq 3 ] || usage; stitch "$2" "$3"; exit $? ;;
  table) [ $# -eq 4 ] || usage; exec python3 -I "$here/la-half.py" table "$2" "$3" "$4" ;;
  screen)
    [ $# -ge 5 ] || usage
    out=$(realpath -m "$2"); la=$3; base=$(realpath "$4") || exit 2; head=$(realpath "$5") || exit 2
    shift 5
    for r in "$base" "$head"; do [ -x "$r/bin/avant-c-llvm" ] || { echo "la-half.sh: $r/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }; done
    mkdir -p "$out" || exit 2
    stitch "$la" "$out/ladir" || exit $?
    rc=0
    for side in base head; do
      eval r=\$$side
      sh "$here/la-count.sh" "$r" "$out/ladir" "$out/$side.tsv" "$@" > "$out/$side.log" 2>&1 || rc=1
    done
    sh "$here/la-half.sh" table "$out/head.tsv" "$out/base.tsv" "$out/ladir/names.tsv" > "$out/table.md" || rc=1
    tail -n 8 "$out/table.md"
    exit $rc ;;
  *) usage ;;
esac
