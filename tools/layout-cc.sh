#!/bin/sh
# layout-cc.sh — a C compiler wrapper that links every program once more
# per code shift, so one compile gives the same program at several
# addresses. Set it as CC for `avant compile` (the compiler runs $CC for
# its compiles and its link):
#
#   CC=tools/layout-cc.sh LAYOUT_DIR=DIR LAYOUT_SHIFTS="0 64 192" avant compile ...
#
# A compile (`-c`) and `--version` pass through to LAYOUT_REAL_CC (default
# /usr/bin/cc). A link (an `-o OUT` without `-c`) is run as asked, then once
# per shift S in LAYOUT_SHIFTS into DIR/S/<basename of OUT>, with a block of
# S bytes of int3 placed before the link's first object, which is the
# program's own: the program's code and the runtime's after it start S
# bytes later and nothing else changes (every function is 64-byte aligned,
# so S is a multiple of 64; the data that follows .text moves by a page
# when the text crosses one). Shift 0 is a copy of OUT, byte for byte.
# The shifts are what an unrelated change before the program's code does
# (a PLT entry, a C start file, a larger runtime unit): a time measured
# across them is a time that one address cannot decide.
#
# A link that fails fails the wrapper; a shifted link that fails, too
# (with OUT already written). Without LAYOUT_DIR the wrapper is the real
# compiler. Exit status: the real compiler's, 2 on a bad shift.
set -u
real=${LAYOUT_REAL_CC:-/usr/bin/cc}
for a in "$@"; do
  case "$a" in -c|-E|-S|--version|-v|-dumpversion|-dumpmachine|-print-*) exec "$real" "$@" ;; esac
done
out=""; prev=""
for a in "$@"; do [ "$prev" = "-o" ] && out=$a; prev=$a; done
[ -n "${LAYOUT_DIR:-}" ] && [ -n "$out" ] || exec "$real" "$@"
"$real" "$@" || exit $?
dir=$LAYOUT_DIR
name=$(basename "$out")
for s in ${LAYOUT_SHIFTS:-0 64 192 448}; do
  case "$s" in *[!0-9]*|'') echo "layout-cc.sh: shift '$s' is not a number of bytes" >&2; exit 2 ;; esac
  [ $((s % 64)) -eq 0 ] || { echo "layout-cc.sh: shift $s is not a multiple of 64" >&2; exit 2; }
  mkdir -p "$dir/$s" "$dir/pads" || exit 2
  if [ "$s" -eq 0 ]; then
    cp "$out" "$dir/$s/$name" || exit 2
    continue
  fi
  pad=$dir/pads/shift$s.o
  if [ ! -f "$pad" ]; then
    # Written once, under a name of this process's own, then renamed into
    # place: links run in parallel.
    printf '.section .text\n.p2align 6\n.skip %d, 0xcc\n.section .note.GNU-stack,"",@progbits\n' "$s" > "$dir/pads/shift$s.$$.s" || exit 2
    as -o "$dir/pads/shift$s.$$.o" "$dir/pads/shift$s.$$.s" || exit 2
    mv -f "$dir/pads/shift$s.$$.o" "$pad"; rm -f "$dir/pads/shift$s.$$.s"
  fi
  # The arguments again, the pad before the first object and the output
  # renamed.
  first=1; prev=""
  for a in "$@"; do
    [ $first = 1 ] && set --
    first=0
    if [ "$prev" = "-o" ]; then set -- "$@" "$dir/$s/$name"; prev=$a; continue; fi
    set -- "$@" "$a"; prev=$a
  done
  "$real" "$pad" "$@" || { echo "layout-cc.sh: the link shifted by $s failed" >&2; exit 1; }
done
exit 0
