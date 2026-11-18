#!/bin/sh
# layout-check.sh — a few LangArena tasks timed across code layouts, for the
# tasks whose time moved while their instruction count did not.
#
#   sh tools/layout-check.sh build LADIR DIR SLUG...   (shared: a build)
#   sh tools/layout-check.sh time DIR RUN_JS [ROUNDS]  (timed: the whole machine or a slot)
#
# build: each SLUG's stitched program of LADIR (a base cache's LangArena
# part, tools/base-cache.sh --la: avant/langarena/tmp/SLUG.av and its
# harness arguments args/SLUG) compiled by this checkout's C' through
# tools/layout-cc.sh, so DIR/layouts/S/SLUG is the program with its code S
# bytes later for each S in LAYOUT_CHECK_SHIFTS (default "0 64 192 448").
# time: tools/rounds.py's LangArena loop over DIR/layouts with --layouts,
# ROUNDS rounds (default: one per shift) after one warm-up, every run's
# checksum against RUN_JS, only undisturbed runs counted, into DIR/rounds/.
# tools/layout-flag.py --spread reads it. Status 0, 1 when a program did not
# compile or the rounds failed, 2 on usage.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ $# -ge 3 ] || usage
shifts=${LAYOUT_CHECK_SHIFTS:-0 64 192 448}
case "$1" in
  build)
    la=$(realpath "$2") || exit 2; d=$(realpath -m "$3"); shift 3
    [ $# -ge 1 ] || usage
    [ -x "$root/bin/avant-c-llvm" ] || { echo "layout-check.sh: $root/bin/avant-c-llvm: no such compiler" >&2; exit 2; }
    mkdir -p "$d/build" "$d/layouts" || exit 2
    src=$la/avant/langarena/tmp
    for s in "$@"; do
      [ -f "$src/$s.av" ] && [ -f "$la/args/$s" ] || { echo "layout-check.sh: $s is not a stitched program of $la" >&2; exit 1; }
      set --; while IFS= read -r a; do set -- "$@" "$a"; done < "$la/args/$s"
      (cd "$src" && env -u AVANT_ROOT CC="$root/tools/layout-cc.sh" LAYOUT_DIR="$d/layouts" LAYOUT_SHIFTS="$shifts" \
        timeout -k 10 300 "$root/bin/avant-c-llvm" compile "$s.av" "$d/build/$s" "$@" < /dev/null > "$d/build/$s.log" 2>&1) \
        || { echo "layout-check.sh: $s did not compile ($d/build/$s.log)" >&2; exit 1; }
    done
    echo "$shifts" > "$d/shifts.txt"
    ;;
  time)
    d=$(realpath "$2") || exit 2; js=$3; [ -f "$d/shifts.txt" ] || { echo "layout-check.sh: build first" >&2; exit 2; }
    sh=$(tr -s ' \n' ',,' < "$d/shifts.txt" | sed 's/,$//'); k=$(echo "$sh" | tr ',' '\n' | wc -l)
    names=$(python3 -I -B -c 'import sys
sys.path.insert(0, sys.argv[1]); import rounds
want = set(sys.argv[3:])
print("\n".join(n for n, _ in rounds.tasks_of(sys.argv[2]) if rounds.slug_of(n) in want))' "$root/tools" "$js" $(ls "$d/layouts/0"))
    set --; for n in $names; do set -- "$@" "$n"; done
    [ $# -ge 1 ] || { echo "layout-check.sh: no task of $js among $(ls "$d/layouts/0" | tr '\n' ' ')" >&2; exit 1; }
    rm -rf "$d/rounds"
    timeout -k 10 1800 python3 -I "$root/tools/rounds.py" langarena --rounds "${4:-$k}" --run-js "$js" --out "$d/rounds" \
      --side head="$d/layouts" --layouts "$sh" "$@" < /dev/null > "$d/rounds.log" 2>&1 \
      || { echo "layout-check.sh: the rounds failed ($d/rounds.log)" >&2; exit 1; }
    ;;
  *) usage ;;
esac
exit 0
