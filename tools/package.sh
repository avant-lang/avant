#!/bin/sh
# Build the linux-x86_64 release tarball (D148). One script serves the
# release workflow and local proofs, so the shipped layout can be tested
# before a tag.
#
#   sh tools/package.sh OUT_DIR COMPILER RE2C [VERSIONS_FILE]
#
# COMPILER is the avant binary to ship (Stage 28 W10: C', the compiler B
# builds on the new back end, built --release by the release job), RE2C
# the static re2c to bundle (D151, tools/build-re2c.sh). There is no
# myc-llvm in the release: the compiler's one back end is the in-process
# LLVM linked into it (D172; the old back end retired at W11). The
# bind helper and the runtime objects come from runtime/build/ (run `sh
# runtime/build.sh` first), except avant_llvm.o, which only the
# compiler's own link uses (D190); runtime/avant_rt.h ships too, for the
# scanners re2c generates. The compiler finds this runtime beside its
# executable (D149). examples/, LICENSE and third_party/ (the notices)
# ship too (O294). VERSIONS_FILE, when given, becomes versions.txt;
# otherwise a short local one is written.
#
# Writes OUT_DIR/avant-linux-x86_64/, OUT_DIR/avant-linux-x86_64.tar.gz,
# and OUT_DIR/avant-linux-x86_64.tar.gz.sha256 (checkable from OUT_DIR).
set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  echo "usage: sh tools/package.sh OUT_DIR COMPILER RE2C [VERSIONS_FILE]" >&2
  exit 2
fi
# Absolute paths: the glibc-floor step below runs the packaged compiler
# from another directory, so a relative OUT_DIR (the release job passes
# `dist`) would not resolve there.
out=$(realpath -m "$1")
compiler=$(realpath -m "$2")
re2c=$(realpath -m "$3")
versions=${4:-}
[ -z "$versions" ] || versions=$(realpath -m "$versions")
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
name=avant-linux-x86_64
dir="$out/$name"

fail() {
  echo "package: $*" >&2
  exit 1
}

[ -x "$compiler" ] || fail "compiler is not an executable file: $compiler"
[ -x "$re2c" ] || fail "re2c is not an executable file: $re2c (build it with sh tools/build-re2c.sh)"
[ -f "$root/runtime/build/avant_rt.o" ] || fail "runtime/build/avant_rt.o is missing (run sh runtime/build.sh)"
[ -x "$root/runtime/build/avant-bind" ] || fail "runtime/build/avant-bind is missing (run sh runtime/build.sh with llvm-config and libclang)"
[ -z "$versions" ] || [ -f "$versions" ] || fail "versions file is missing: $versions"

mkdir -p "$out"
rm -rf "$dir" "$out/$name.tar.gz" "$out/$name.tar.gz.sha256"
mkdir -p "$dir/lib/std" "$dir/runtime/build"

install -m 755 "$compiler" "$dir/avant"
install -m 755 "$re2c" "$dir/re2c"
install -m 755 "$root/runtime/build/avant-bind" "$dir/avant-bind"
for f in "$root"/lib/std/*.av; do
  install -m 644 "$f" "$dir/lib/std/"
done
for f in "$root"/runtime/build/*.o; do
  case "$f" in
    */avant_llvm.o) continue ;;  # the compiler's own link only (D190)
  esac
  install -m 644 "$f" "$dir/runtime/build/"
done
install -m 644 "$root/runtime/avant_rt.h" "$dir/runtime/avant_rt.h"

# What the README points to that a user of the archive needs (O294,
# Stage 29 W-FIX1e): examples/ ("Runnable programs ... live in
# examples/"), with the C sources its interop examples link; Avant's
# LICENSE; and the third-party notices D148 lists, which the vendored
# libraries' terms require beside a binary that carries them (the
# runtime objects carry all six): runtime/third_party/NOTICE.md and each
# tree's license, the two that keep theirs in a source header (yyjson,
# picohttpparser) as that header's license comment.
(cd "$root" && find examples -type f | LC_ALL=C sort) | while IFS= read -r f; do
  install -D -m 644 "$root/$f" "$dir/$f"
done
install -m 644 "$root/LICENSE" "$dir/LICENSE"
tp="$root/runtime/third_party"
install -D -m 644 "$tp/NOTICE.md" "$dir/third_party/NOTICE.md"
for f in base64/LICENSE pcre2/LICENCE.md pcre2/AUTHORS.md pcre2/deps/sljit/LICENSE uSockets/LICENSE zlib/LICENSE; do
  [ -f "$tp/$f" ] || fail "third-party license is missing: runtime/third_party/$f"
  install -D -m 644 "$tp/$f" "$dir/third_party/$f"
done
for h in yyjson/yyjson.h picohttpparser/picohttpparser.h; do
  lic="$dir/third_party/$(dirname "$h")/LICENSE"
  mkdir -p "$(dirname "$lic")"
  sed -n '1,/\*\//p' "$tp/$h" > "$lic"
  grep -q 'Permission is hereby granted' "$lic" || fail "no license comment at the top of runtime/third_party/$h"
  chmod 644 "$lic"
done

# versions.txt is also what `avant version` reads (O534). Without
# VERSIONS_FILE: the compiler's own build record (COMPILER.versions.txt,
# written by tools/letters.sh) when it has one, else avant.toml's version
# and this checkout's commit; `dirty: yes` when compiler/, lib/ or
# runtime/ differ from that commit.
if [ -n "$versions" ]; then
  install -m 644 "$versions" "$dir/versions.txt"
else
  {
    if [ -f "$compiler.versions.txt" ]; then
      grep -E '^(version|avant|dirty): ' "$compiler.versions.txt" || true
    else
      v=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$root/avant.toml" | head -n 1)
      c=$(git -C "$root" rev-parse HEAD 2>/dev/null) || c=unknown
      [ -z "$v" ] || echo "version: $v"
      echo "avant: $c"
      if [ "$c" != unknown ] && [ -n "$(git -C "$root" status --porcelain -- compiler lib runtime 2>/dev/null)" ]; then
        echo "dirty: yes"
      fi
    fi
    echo "built: local (tools/package.sh)"
  } > "$dir/versions.txt"
fi

# The glibc floor (D152): the highest GLIBC_ symbol version the shipped
# binaries need, and that a program linked from the shipped runtime needs
# (an object records no symbol versions; its references bind where the
# user links). The program uses every runtime object: the core and each
# std module. It is compiled by the packaged compiler outside the checkout,
# with an empty environment (no AVANT_BACKEND or AVANT_ROOT;
# PATH=/usr/bin:/bin), so this is also a first self-test of the archive.
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
floor=$(for f in "$dir/avant" "$dir/avant-bind" "$floor_dir/all"; do
  objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' || true
done | sed 's/^GLIBC_//' | sort -uV | tail -n 1)
[ -n "$floor" ] || fail "could not measure the glibc floor"
echo "glibc-floor: $floor" >> "$dir/versions.txt"
echo "package: glibc floor $floor"

# Reproducible archive metadata: sorted names, fixed owner and mode bits.
tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
echo "package: $out/$name.tar.gz"
