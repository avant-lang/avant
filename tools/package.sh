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
# Absolute paths: the glibc-floor step below runs the packaged compiler
# from another directory, so a relative OUT_DIR (the release job passes
# `dist`) would not resolve there.
out=$(realpath -m "$1")
compiler=$(realpath -m "$2")
myc=$(realpath -m "$3")
re2c=$(realpath -m "$4")
versions=${5:-}
[ -z "$versions" ] || versions=$(realpath -m "$versions")
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

# The glibc floor (D152): the highest GLIBC_ symbol version the shipped
# binaries need, and that a program linked from the shipped runtime needs
# (an object records no symbol versions; its references bind where the
# user links). The program uses every runtime object: the core and each
# std module. It is compiled by the packaged compiler outside the checkout,
# so this is also a first self-test of the archive.
floor_dir=$(mktemp -d "${TMPDIR:-/tmp}/avant-floor-XXXXXX")
trap 'rm -rf "$floor_dir"' EXIT
cat > "$floor_dir/all.av" <<'EOF_AV'
import std.json
import std.regex
import std.zlib
import std.base64
import std.http

fn main {
  if doc = Json.parse("{\"n\": 1}") {
    puts(doc.int("n") ?? 0)
  }
  if re = Regex.compile("a+") {
    puts(re.count("aa"))
  }
  p = "b"
  if re2 = Regex.compile(p + "+") {
    puts(re2.count("bb"))
  }
  if back = "z".bytes.compress.uncompress {
    puts(back.size)
  }
  puts("a".bytes.to_base64)
  if server = Http.listen(0) {
    server.close
  }
  h = spawn {
    1
  }
  puts(h.join)
}
EOF_AV
( cd "$floor_dir" && env -i HOME="$floor_dir" PATH=/usr/bin:/bin TMPDIR="$floor_dir" \
    "$dir/avant" compile all.av all --release < /dev/null > compile.log 2>&1 ) || {
  cat "$floor_dir/compile.log" >&2
  fail "the packaged compiler could not compile the glibc-floor program"
}
floor=$(for f in "$dir/avant" "$dir/myc-llvm" "$dir/avant-bind" "$floor_dir/all"; do
  objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' || true
done | sed 's/^GLIBC_//' | sort -uV | tail -n 1)
[ -n "$floor" ] || fail "could not measure the glibc floor"
echo "glibc-floor: $floor" >> "$dir/versions.txt"
echo "package: glibc floor $floor"

# Reproducible archive metadata: sorted names, fixed owner and mode bits.
tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
echo "package: $out/$name.tar.gz"
