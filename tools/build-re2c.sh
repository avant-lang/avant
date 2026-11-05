#!/bin/sh
# Build the re2c the release ships beside avant (D151): the pinned upstream
# release, checked by sha256, linked fully static (re2c is a standalone
# generator: no NSS, no dlopen), so it loads nothing and adds no glibc
# floor. The release workflow runs this; run it locally the same way.
#
#   sh tools/build-re2c.sh OUT_FILE
#
# Needs curl, xz, a C++ compiler, make, and the static libstdc++ and libc
# archives (Ubuntu: build-essential). Downloads and builds in a temporary
# directory, removed at the end.
set -eu

RE2C_VERSION=4.6
RE2C_SHA256=75bf2696445e831d0d44e0d9f2909eeffc18c09757f222b2fb025f7e59fe130b
RE2C_URL="https://github.com/skvadrik/re2c/releases/download/${RE2C_VERSION}/re2c-${RE2C_VERSION}.tar.xz"

if [ "$#" -ne 1 ]; then
  echo "usage: sh tools/build-re2c.sh OUT_FILE" >&2
  exit 2
fi
out=$(realpath -m "$1")
work=$(mktemp -d "${TMPDIR:-/tmp}/avant-re2c-XXXXXX")
trap 'rm -rf "$work"' EXIT
trap 'exit 130' INT TERM

curl -fsSL -o "$work/re2c.tar.xz" "$RE2C_URL"
echo "$RE2C_SHA256  $work/re2c.tar.xz" | sha256sum -c - > /dev/null || {
  echo "build-re2c: sha256 mismatch for $RE2C_URL" >&2
  exit 1
}
mkdir "$work/src" "$work/build"
tar -C "$work/src" -xJf "$work/re2c.tar.xz"
(
  cd "$work/build"
  # Only re2c (C) is built; the other language front ends are on by default.
  "$work/src/re2c-$RE2C_VERSION/configure" --quiet --disable-dependency-tracking \
    --disable-dlang --disable-golang --disable-haskell --disable-java --disable-js \
    --disable-ocaml --disable-python --disable-rust --disable-swift --disable-vlang \
    --disable-zig \
    CXXFLAGS="-O2" > "$work/configure.log" 2>&1 || {
    cat "$work/configure.log" >&2
    exit 1
  }
  # Plain `make`: automake builds BUILT_SOURCES (the bootstrap lexer and
  # parser) only for `all`, so `make re2c` alone fails on a fresh tree.
  # The link goes through libtool, where a fully static program is
  # -all-static (given to make, not configure: plain g++ rejects it).
  make -s -j"$(nproc)" LDFLAGS="-all-static" > "$work/make.log" 2>&1 || {
    tail -40 "$work/make.log" >&2
    exit 1
  }
)
mkdir -p "$(dirname "$out")"
install -m 755 "$work/build/re2c" "$out"
strip "$out"
if ldd "$out" > /dev/null 2>&1; then
  echo "build-re2c: $out is not static" >&2
  exit 1
fi
"$out" --version
