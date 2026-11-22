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
# bytes later for each S in LAYOUT_CHECK_SHIFTS (default "0 64 192 448"):
# whole lines, since every function starts on a 64-byte line, so these
# never move a loop within its line. Then once more for each S in
# LAYOUT_CHECK_SUBLINE (default "16l16 32l16 48l16", "" for none; each
# from 1 to 63, with its loops' alignment as below), compiled with
# AVANT_CODE_SHIFT=S: every function of the program
# starts S bytes into its line (S bytes of nop before its entry, never
# run), so each loop sits at another offset within its line, into
# DIR/layouts/S/SLUG; each such binary's `main` is checked to sit S bytes
# into its line. A loop's offset within its line can decide a task's time
# (a branch-dense loop at offset 0 instead of 32 cost a task 30% here).
# The runtime's C code moves with the program's: each sub-line shift's
# binary links this checkout's runtime built at that shift
# (tools/layout-runtime.sh, into DIR/runtime/S, through tools/layout-cc.sh's
# LAYOUT_RUNTIME), and its `avant_alloc` is checked to sit S bytes into its
# line too, so a task whose hot loop is runtime C (a parser, a codec, a
# copy) is sampled at as many offsets as one whose loop is its own.
# LAYOUT_CHECK_RUNTIME=fixed keeps the runtime at its one placement at
# every layout (the method before; shifts.txt names the method used).
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
subline=${LAYOUT_CHECK_SUBLINE-16l16 32l16 48l16}
rtmode=${LAYOUT_CHECK_RUNTIME:-moving}
case "$rtmode" in moving|fixed) ;; *) echo "layout-check.sh: LAYOUT_CHECK_RUNTIME is moving or fixed, not '$rtmode'" >&2; exit 2 ;; esac
# A sub-line entry KlN (16l16) is the shift K with loops aligned to N bytes
# (16 or 64) instead of 32, in the program (AVANT_LOOP_ALIGN=N) and in the
# runtime (tools/layout-runtime.sh's LOOPS): with loops on 32 bytes an
# aligned loop sits at offset 0 or 32 of its line at every shift, so the
# shifts 16 and 48 repeat 0 and 32 for it; on 16 it takes the other offsets.
# With 0 and 448 on 32 bytes, the default's five layouts put HuffEncode's
# and CSV::Parse's hot loops at three offsets of their lines (two with
# "16 32 48", the default before) and Brainfuck::Recursion's unaligned
# dispatch at all four, as before.
for e in $subline; do
  s=${e%%l*}; n=""; [ "$s" = "$e" ] || n=${e#*l}
  case "$n" in ''|16|64) ;; *) echo "layout-check.sh: sub-line entry '$e': the loops' alignment is 16 or 64" >&2; exit 2 ;; esac
  case "$s" in *[!0-9]*|0*|'') echo "layout-check.sh: sub-line shift '$s' is not a number of bytes from 1 to 63" >&2; exit 2 ;; esac
  [ "$s" -le 63 ] || { echo "layout-check.sh: sub-line shift $s is not below 64" >&2; exit 2; }
  case " $shifts " in *" $s "*) echo "layout-check.sh: shift $s is both a whole-line and a sub-line shift" >&2; exit 2 ;; esac
done
case "$1" in
  build)
    la=$(realpath "$2") || exit 2; d=$(realpath -m "$3"); shift 3
    [ $# -ge 1 ] || usage
    [ -x "$root/bin/avant-c-llvm" ] || { echo "layout-check.sh: $root/bin/avant-c-llvm: no such compiler" >&2; exit 2; }
    mkdir -p "$d/build" "$d/layouts" || exit 2
    src=$la/avant/langarena/tmp
    method=moving
    if [ "$rtmode" = fixed ]; then method=fixed
    else
      for e in $subline; do
        n=""; [ "${e%%l*}" = "$e" ] || n=${e#*l}
        sh "$root/tools/layout-runtime.sh" "$root/runtime" "${e%%l*}" "$d/runtime/$e" $n || { echo "layout-check.sh: the runtime did not build at $e" >&2; exit 1; }
      done
    fi
    for s in "$@"; do
      [ -f "$src/$s.av" ] && [ -f "$la/args/$s" ] || { echo "layout-check.sh: $s is not a stitched program of $la" >&2; exit 1; }
      set --; while IFS= read -r a; do set -- "$@" "$a"; done < "$la/args/$s"
      (cd "$src" && env -u AVANT_ROOT CC="$root/tools/layout-cc.sh" LAYOUT_DIR="$d/layouts" LAYOUT_SHIFTS="$shifts" \
        timeout -k 10 300 "$root/bin/avant-c-llvm" compile "$s.av" "$d/build/$s" "$@" < /dev/null > "$d/build/$s.log" 2>&1) \
        || { echo "layout-check.sh: $s did not compile ($d/build/$s.log)" >&2; exit 1; }
      # The task's configuration beside its layouts: tools/rounds.py reads
      # its threads there (cfg_threads), and scales its limit on
      # involuntary switches for a program with more threads than its CPUs.
      if [ -f "$src/$s.cfg.av" ]; then cp "$src/$s.cfg.av" "$d/layouts/$s.cfg.av" || exit 2; fi
      for e in $subline; do
        k=${e%%l*}; n=""; [ "$k" = "$e" ] || n=${e#*l}
        mkdir -p "$d/layouts/$e" || exit 2
        mv=""; [ $method = moving ] && mv="CC=$root/tools/layout-cc.sh LAYOUT_RUNTIME=$d/runtime/$e LAYOUT_C_SHIFT=$k${n:+ LAYOUT_C_LOOPS=$n}"
        # shellcheck disable=SC2086
        (cd "$src" && env -u AVANT_ROOT -u LAYOUT_DIR AVANT_CODE_SHIFT="$k" ${n:+AVANT_LOOP_ALIGN=$n} $mv \
          timeout -k 10 300 "$root/bin/avant-c-llvm" compile "$s.av" "$d/layouts/$e/$s" "$@" < /dev/null > "$d/build/$s@$e.log" 2>&1) \
          || { echo "layout-check.sh: $s did not compile at $e ($d/build/$s@$e.log)" >&2; exit 1; }
        a=$(nm "$d/layouts/$e/$s" 2>/dev/null | awk '$3 == "main" { print $1; exit }')
        [ -n "$a" ] && [ $((0x$a % 64)) -eq "$k" ] \
          || { echo "layout-check.sh: $s at sub-line shift $k: main is at ${a:-no address}, not $k bytes into its line" >&2; exit 1; }
        if [ $method = moving ]; then
          a=$(nm "$d/layouts/$e/$s" 2>/dev/null | awk '$3 == "avant_alloc" { print $1; exit }')
          [ -n "$a" ] && [ $((0x$a % 64)) -eq "$k" ] \
            || { echo "layout-check.sh: $s at sub-line shift $k: the runtime's avant_alloc is at ${a:-no address}, not $k bytes into its line" >&2; exit 1; }
        fi
      done
    done
    echo $shifts $subline > "$d/shifts.txt"
    echo "runtime: $method" > "$d/method.txt"
    ;;
  time)
    # The rounds argument is read before the task names reset the
    # positional parameters: with four or more tasks $4 is a task's name.
    d=$(realpath "$2") || exit 2; js=$3; nr=${4:-}; [ -f "$d/shifts.txt" ] || { echo "layout-check.sh: build first" >&2; exit 2; }
    sh=$(tr -s ' \n' ',,' < "$d/shifts.txt" | sed 's/,$//'); k=$(echo "$sh" | tr ',' '\n' | wc -l)
    names=$(python3 -I -B -c 'import sys
sys.path.insert(0, sys.argv[1]); import rounds
want = set(sys.argv[3:])
print("\n".join(n for n, _ in rounds.tasks_of(sys.argv[2]) if rounds.slug_of(n) in want))' "$root/tools" "$js" $(ls "$d/layouts/0"))
    set --; for n in $names; do set -- "$@" "$n"; done
    [ $# -ge 1 ] || { echo "layout-check.sh: no task of $js among $(ls "$d/layouts/0" | tr '\n' ' ')" >&2; exit 1; }
    rm -rf "$d/rounds"
    timeout -k 10 1800 python3 -I "$root/tools/rounds.py" langarena --rounds "${nr:-$k}" --run-js "$js" --out "$d/rounds" \
      --side head="$d/layouts" --layouts "$sh" "$@" < /dev/null > "$d/rounds.log" 2>&1 \
      || { echo "layout-check.sh: the rounds failed ($d/rounds.log)" >&2; exit 1; }
    ;;
  *) usage ;;
esac
exit 0
