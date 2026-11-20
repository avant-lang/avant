#!/bin/sh
# layout-runtime.sh — a checkout's runtime objects built once more with
# every C function K bytes into its line, for the sub-line layouts.
#
#   sh tools/layout-runtime.sh RUNTIME K OUT [LOOPS]
#
# RUNTIME is a runtime source directory (a checkout's runtime/, any
# version with a build.sh); K a shift from 1 to 63; OUT the directory that
# receives the objects (replaced). The sources are copied (without build/)
# beside OUT and built by their own build.sh with CC=tools/layout-cc.sh and
# LAYOUT_C_SHIFT=K: every compile of the runtime, its vendored libraries
# included, gets K bytes of nop before each function's entry, never run
# (layout-cc.sh says how), and nothing else changes. A program compiled
# with AVANT_CODE_SHIFT=K links them through layout-cc.sh's LAYOUT_RUNTIME,
# so its code and the runtime's C move together within their lines; before,
# the runtime kept one placement at every sub-line layout (a task whose hot
# loop is runtime C was sampled at one offset). LOOPS (16, 32 or 64) also
# sets the C loops' alignment (layout-cc.sh's LAYOUT_C_LOOPS).
#
# K = 0 builds the objects through the same path (the assembly and the
# assembler, layout-cc.sh's LAYOUT_C_ASM) with no nops: they are the
# checkout's runtime/build objects byte for byte (`--check` compares them),
# the proof that the copy and the wrapper change nothing else. The
# compiler's LLVM object and avant-bind are not built (programs link
# neither). Status 0; 1 when the build failed; 2 on usage.
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
check=0
[ "${1:-}" = --check ] && { check=1; shift; }
[ $# -ge 3 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
src=$(realpath "$1") || exit 2
k=$2 out=$(realpath -m "$3") loops=${4:-}
case "$k" in *[!0-9]*|'') echo "layout-runtime.sh: shift '$k' is not a number of bytes" >&2; exit 2 ;; esac
[ "$k" -le 63 ] || { echo "layout-runtime.sh: shift $k is not below 64" >&2; exit 2; }
[ -f "$src/build.sh" ] || { echo "layout-runtime.sh: $src/build.sh: no runtime build script" >&2; exit 2; }
work=$out.src
rm -rf "$work" "$out"
mkdir -p "$work/runtime" || exit 2
(cd "$src" && tar --exclude=./build -cf - .) | (cd "$work/runtime" && tar -xf -) || exit 1
env CC="$root/tools/layout-cc.sh" LAYOUT_REAL_CC="${LAYOUT_REAL_CC:-${CC:-/usr/bin/cc}}" LAYOUT_C_SHIFT="$k" LAYOUT_C_LOOPS="$loops" LAYOUT_C_ASM=1 \
  LLVM_CONFIG=/nonexistent/llvm-config BIND_LLVM_CONFIG=/nonexistent/llvm-config AVANT_RUNTIME_BUILD_LOCK=1 \
  sh "$work/runtime/build.sh" < /dev/null > "$work/build.log" 2>&1 \
  || { echo "layout-runtime.sh: the runtime did not build at shift $k ($work/build.log)" >&2; exit 1; }
mkdir -p "$out" || exit 2
cp "$work/runtime/build/"*.o "$out/" || exit 1
rm -rf "$work"
if [ $check = 1 ]; then
  bad=0
  for f in "$out"/*.o; do
    cmp -s "$f" "$src/build/${f##*/}" || { echo "layout-runtime.sh: ${f##*/} differs from $src/build/${f##*/}" >&2; bad=1; }
  done
  [ $bad = 0 ] || exit 1
fi
exit 0
