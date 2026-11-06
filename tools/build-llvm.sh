#!/bin/sh
# Build runtime/build/avant_llvm.o: the compiler's in-process LLVM (D162,
# D172, D173). One relocatable object, merged with `ld -r`, so the v0.34.0
# seed (whose link arguments take .o, .c, and -l..., not -L or .a) can link
# it into B unchanged:
#
#   runtime/avant_llvm.c (the glue over the LLVM C API)
#   + the LLVM 20 static members it needs (llvm-config-20 --libfiles)
#   + libstdc++.a, libz.a, libzstd.a (so the compiler loads only glibc and
#     libgcc_s, D146)
#
# Every symbol except the avant_llvm_* entry points is made local, so the
# object cannot clash with the runtime or a library. It is linked into the
# compiler only (tools/letters.sh and test_fixed_point pass it to
# `avant compile compiler/main.av`), never into a user program.
#
#   sh tools/build-llvm.sh        (any directory)
#
# Rebuilds only when the stamp beside the object (runtime/build/
# avant_llvm.stamp: the LLVM version, and the sha256 of the glue, of
# runtime/avant_rt.h, and of this script) differs; a cached run is a few
# sha256 sums. LLVM_CONFIG names llvm-config (default llvm-config-20).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
llvm_config=${LLVM_CONFIG:-llvm-config-20}
src="$root/runtime/avant_llvm.c"
out_dir="$root/runtime/build"
obj="$out_dir/avant_llvm.o"
stamp="$out_dir/avant_llvm.stamp"

command -v "$llvm_config" > /dev/null 2>&1 || {
  echo "tools/build-llvm.sh: $llvm_config not found (install llvm-20-dev, or set LLVM_CONFIG)" >&2
  exit 1
}
version=$("$llvm_config" --version)
case "$version" in
  20.*) ;;
  *) echo "tools/build-llvm.sh: LLVM 20 is required (D173); $llvm_config is $version" >&2; exit 1 ;;
esac
want="llvm $version
glue $(sha256sum "$src" | cut -d' ' -f1)
header $(sha256sum "$root/runtime/avant_rt.h" | cut -d' ' -f1)
script $(sha256sum "$0" | cut -d' ' -f1)"
if [ -f "$obj" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
  exit 0
fi

mkdir -p "$out_dir"
work=$(mktemp -d "$out_dir/avant_llvm.work.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
rm -f "$stamp"

# The components the glue calls, and what they pull in.
components="core irreader passes analysis target x86codegen x86asmparser x86desc x86info"
libfiles=$("$llvm_config" --link-static --libfiles $components)

static_lib() {
  p=$("$cc" -print-file-name="$1")
  [ -f "$p" ] || { echo "tools/build-llvm.sh: $1 not found (libstdc++ static, zlib1g-dev, libzstd-dev)" >&2; exit 1; }
  echo "$p"
}
cxx=$(static_lib libstdc++.a)
zlib=$(static_lib libz.a)
zstd=$(static_lib libzstd.a)

# shellcheck disable=SC2046 # llvm-config's flags are words
"$cc" -c -O2 -std=c11 -fno-omit-frame-pointer -I"$root/runtime" $("$llvm_config" --cflags) -o "$work/glue.o" "$src"
# shellcheck disable=SC2086 # libfiles is a list of paths
ld -r -o "$work/merged.o" "$work/glue.o" --start-group $libfiles "$cxx" "$zlib" "$zstd" --end-group
objcopy -w --keep-global-symbol='avant_llvm_*' "$work/merged.o"
mv "$work/merged.o" "$obj"
printf '%s\n' "$want" > "$stamp"
