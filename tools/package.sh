#!/bin/sh
# Build the linux-x86_64 release tarball (D148). One script serves the
# release workflow and local proofs, so the shipped layout can be tested
# before a tag.
#
#   sh tools/package.sh OUT_DIR COMPILER MYC_LLVM RE2C [VERSIONS_FILE]
#
# COMPILER is the avant binary to ship (the release job's B), MYC_LLVM the
# backend to bundle beside it, RE2C the static re2c to bundle (D151,
# tools/build-re2c.sh). The bind helper and the runtime objects come
# from runtime/build/ (run `sh runtime/build.sh` first); runtime/avant_rt.h
# ships too, for the scanners re2c generates. The compiler finds this
# runtime beside its executable (D149). VERSIONS_FILE, when
# given, becomes versions.txt; otherwise a short local one is written.
#
# Writes OUT_DIR/avant-linux-x86_64/, OUT_DIR/avant-linux-x86_64.tar.gz,
# and OUT_DIR/avant-linux-x86_64.tar.gz.sha256 (checkable from OUT_DIR).
set -eu

if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
  echo "usage: sh tools/package.sh OUT_DIR COMPILER MYC_LLVM RE2C [VERSIONS_FILE]" >&2
  exit 2
fi
out=$1
compiler=$2
myc=$3
re2c=$4
versions=${5:-}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
name=avant-linux-x86_64
dir="$out/$name"

fail() {
  echo "package: $*" >&2
  exit 1
}

[ -x "$compiler" ] || fail "compiler is not an executable file: $compiler"
[ -x "$myc" ] || fail "myc-llvm is not an executable file: $myc"
[ -x "$re2c" ] || fail "re2c is not an executable file: $re2c (build it with sh tools/build-re2c.sh)"
[ -f "$root/runtime/build/avant_rt.o" ] || fail "runtime/build/avant_rt.o is missing (run sh runtime/build.sh)"
[ -x "$root/runtime/build/avant-bind" ] || fail "runtime/build/avant-bind is missing (run sh runtime/build.sh with llvm-config and libclang)"
[ -z "$versions" ] || [ -f "$versions" ] || fail "versions file is missing: $versions"

mkdir -p "$out"
rm -rf "$dir" "$out/$name.tar.gz" "$out/$name.tar.gz.sha256"
mkdir -p "$dir/lib/std" "$dir/runtime/build"

install -m 755 "$compiler" "$dir/avant"
install -m 755 "$myc" "$dir/myc-llvm"
install -m 755 "$re2c" "$dir/re2c"
install -m 755 "$root/runtime/build/avant-bind" "$dir/avant-bind"
for f in "$root"/lib/std/*.av; do
  install -m 644 "$f" "$dir/lib/std/"
done
for f in "$root"/runtime/build/*.o; do
  install -m 644 "$f" "$dir/runtime/build/"
done
install -m 644 "$root/runtime/avant_rt.h" "$dir/runtime/avant_rt.h"

if [ -n "$versions" ]; then
  install -m 644 "$versions" "$dir/versions.txt"
else
  {
    echo "avant: $(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "built: local (tools/package.sh)"
  } > "$dir/versions.txt"
fi

# Reproducible archive metadata: sorted names, fixed owner and mode bits.
tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
echo "package: $out/$name.tar.gz"
