#!/bin/sh
# Build the linux-x86_64 release tarball (D148). One script serves the
# release workflow and local proofs, so the shipped layout can be tested
# before a tag.
#
#   sh tools/package.sh OUT_DIR COMPILER MYC_LLVM [VERSIONS_FILE]
#
# COMPILER is the avant binary to ship (the release job's B), MYC_LLVM the
# backend to bundle beside it. The bind helper and the runtime come from
# runtime/build/ (run `sh runtime/build.sh` first). VERSIONS_FILE, when
# given, becomes versions.txt; otherwise a short local one is written.
#
# Writes OUT_DIR/avant-linux-x86_64/, OUT_DIR/avant-linux-x86_64.tar.gz,
# and OUT_DIR/avant-linux-x86_64.tar.gz.sha256 (checkable from OUT_DIR).
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo "usage: sh tools/package.sh OUT_DIR COMPILER MYC_LLVM [VERSIONS_FILE]" >&2
  exit 2
fi
out=$1
compiler=$2
myc=$3
versions=${4:-}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
name=avant-linux-x86_64
dir="$out/$name"

fail() {
  echo "package: $*" >&2
  exit 1
}

[ -x "$compiler" ] || fail "compiler is not an executable file: $compiler"
[ -x "$myc" ] || fail "myc-llvm is not an executable file: $myc"
[ -x "$root/runtime/build/avant-bind" ] || fail "runtime/build/avant-bind is missing (run sh runtime/build.sh with llvm-config and libclang)"
[ -z "$versions" ] || [ -f "$versions" ] || fail "versions file is missing: $versions"

mkdir -p "$out"
rm -rf "$dir" "$out/$name.tar.gz" "$out/$name.tar.gz.sha256"
mkdir -p "$dir/lib/std"

install -m 755 "$compiler" "$dir/avant"
install -m 755 "$myc" "$dir/myc-llvm"
install -m 755 "$root/runtime/build/avant-bind" "$dir/avant-bind"
for f in "$root"/lib/std/*.av; do
  install -m 644 "$f" "$dir/lib/std/"
done

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
