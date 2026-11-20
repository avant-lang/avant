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
# Two more settings move the C code within its lines, for the sub-line
# layouts (a program compiled with AVANT_CODE_SHIFT=K, whose own functions
# start K bytes into their lines):
#
#   LAYOUT_C_SHIFT=K (1 to 63)  a compile (`-c`) is run as asked but to
#       assembly (`-S`), K bytes of nop are put before every function's
#       label (after its alignment; never run), and the result is
#       assembled: each function starts K bytes after its alignment and its
#       loops sit at other offsets of their lines, with the compiler's code
#       itself unchanged. (gcc's -fpatchable-function-entry=K,K, which
#       places the same nops, also changes the code: its register
#       allocation and frames differ in 11 of the runtime's 26 objects.)
#       LAYOUT_C_LOOPS=N adds -falign-loops=N after the compile's own
#       flags (a second layout axis: the loops' alignment).
#       LAYOUT_C_ASM=1 takes the same path with no nops: the object is
#       then the plain compile's byte for byte (tools/layout-runtime.sh
#       --check proves it). tools/layout-runtime.sh builds the runtime's
#       objects through this.
#   LAYOUT_RUNTIME=RTDIR  a link takes each runtime object it names
#       (an argument .../runtime/build/NAME.o) from RTDIR/NAME.o instead:
#       the runtime built at the program's own sub-line shift
#       (tools/layout-runtime.sh), so the runtime's C code moves with the
#       program's. A runtime object missing from RTDIR fails the link.
#
# A link that fails fails the wrapper; a shifted link that fails, too
# (with OUT already written). Without LAYOUT_DIR, LAYOUT_RUNTIME and
# LAYOUT_C_SHIFT the wrapper is the real compiler. Exit status: the real
# compiler's, 2 on a bad shift or a missing runtime object.
set -u
real=${LAYOUT_REAL_CC:-/usr/bin/cc}
cshift=${LAYOUT_C_SHIFT:-}
case "$cshift" in
  ''|0) cshift="" ;;
  *[!0-9]*|0*) echo "layout-cc.sh: LAYOUT_C_SHIFT '$cshift' is not a number of bytes from 1 to 63" >&2; exit 2 ;;
  *) [ "$cshift" -le 63 ] || { echo "layout-cc.sh: LAYOUT_C_SHIFT $cshift is not below 64" >&2; exit 2; } ;;
esac
loops=${LAYOUT_C_LOOPS:-}
case "$loops" in ''|16|32|64) ;; *) echo "layout-cc.sh: LAYOUT_C_LOOPS '$loops' is not 16, 32 or 64" >&2; exit 2 ;; esac
for a in "$@"; do
  case "$a" in
    -E|-S|--version|-v|-dumpversion|-dumpmachine|-print-*) exec "$real" "$@" ;;
  esac
done
for a in "$@"; do
  if [ "$a" = -c ]; then
    [ -n "$cshift" ] || [ -n "$loops" ] || [ -n "${LAYOUT_C_ASM:-}" ] || exec "$real" "$@"
    obj=""; prev=""
    for b in "$@"; do [ "$prev" = "-o" ] && obj=$b; prev=$b; done
    [ -n "$obj" ] || { echo "layout-cc.sh: a compile with LAYOUT_C_SHIFT needs -o" >&2; exit 2; }
    asm=$obj.layout$$.s
    # The arguments again, -S for -c and the assembly as the output.
    first=1; prev=""
    for b in "$@"; do
      [ $first = 1 ] && set --
      first=0
      if [ "$prev" = "-o" ]; then set -- "$@" "$asm"; prev=$b; continue; fi
      if [ "$b" = -c ]; then set -- "$@" -S; else set -- "$@" "$b"; fi
      prev=$b
    done
    "$real" "$@" ${loops:+-falign-loops=$loops} || { rm -f "$asm"; exit 1; }
    if [ -n "$cshift" ]; then
      awk -v k="$cshift" '
        /^[ \t]*\.type[ \t]/ && /@function/ { n = $2; sub(/,.*/, "", n); fn[n] = 1 }
        { l = $0; sub(/[ \t]*$/, "", l) }
        l ~ /:$/ { n = substr(l, 1, length(l) - 1); if (n in fn) { printf "\t.skip %d, 0x90\n", k; delete fn[n] } }
        { print }' "$asm" > "$asm.k" && mv -f "$asm.k" "$asm" || { rm -f "$asm" "$asm.k"; exit 2; }
    fi
    "$real" -c -o "$obj" "$asm" || { rm -f "$asm"; exit 1; }
    rm -f "$asm"
    exit 0
  fi
done
out=""; prev=""
for a in "$@"; do [ "$prev" = "-o" ] && out=$a; prev=$a; done
rt=${LAYOUT_RUNTIME:-}
if [ -n "$rt" ] && [ -n "$out" ]; then
  # The arguments again, each runtime object from RTDIR.
  first=1
  for a in "$@"; do
    [ $first = 1 ] && set --
    first=0
    case "$a" in
      */runtime/build/*.o)
        [ -f "$rt/${a##*/}" ] || { echo "layout-cc.sh: $rt/${a##*/}: no such runtime object (tools/layout-runtime.sh builds them)" >&2; exit 2; }
        set -- "$@" "$rt/${a##*/}" ;;
      *) set -- "$@" "$a" ;;
    esac
  done
fi
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
    # The block has a section of its own, so a link that orders sections
    # (ld's --section-ordering-file) can keep it first.
    printf '.section .text.layout_pad,"ax",@progbits\n.p2align 6\n.skip %d, 0xcc\n.section .note.GNU-stack,"",@progbits\n' "$s" > "$dir/pads/shift$s.$$.s" || exit 2
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
