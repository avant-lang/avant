#!/bin/sh
# Install the LLVM the compiler links: LLVM's official prebuilt release for
# linux-x86_64, pinned by version and sha256 and checked against its release
# manager's signature (the key's fingerprint pinned too), reduced to what
# tools/build-llvm.sh reads (llvm-config, the LLVM headers, the static
# libraries) and the clang runtime/build.sh compiles the runtime with
# (clang, its compiler headers). The release workflows and a local machine install the same
# bytes, so the compiler CI ships is the one measured locally.
#
#   sh tools/install-llvm.sh [DEST]    (default /opt/llvm-22.1.8; absent or empty)
#
# tools/build-llvm.sh uses /opt/llvm-22.1.8/bin/llvm-config when it is there;
# with another DEST, set LLVM_CONFIG=DEST/bin/llvm-config. The download (about 1.9 GB)
# goes to a new temporary directory and is removed; the unpacked subset is
# about 630 MB (clang 260 MB of it). Needs curl, xz, sha256sum and gpg.
set -eu
version=22.1.8
sha256=df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384
name=LLVM-$version-Linux-X64
url=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz
# The key that signed the 22.1.8 release (Douglas Yung), from LLVM's list.
key_fpr=FFB3368980F3E6BB5737145A316C56D064CACBA5
keys_url=https://releases.llvm.org/release-keys.asc

[ $# -le 1 ] || { echo "usage: sh tools/install-llvm.sh [DEST]" >&2; exit 2; }
dest=${1:-/opt/llvm-$version}
if [ -e "$dest" ] && [ -n "$(ls -A "$dest")" ]; then
  echo "tools/install-llvm.sh: $dest is not empty" >&2
  exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/install-llvm.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
curl -fsSL --retry 3 -o "$work/$name.tar.xz" "$url"
echo "$sha256  $work/$name.tar.xz" | sha256sum -c - > /dev/null || {
  echo "tools/install-llvm.sh: $name.tar.xz does not have the pinned sha256" >&2
  exit 1
}
curl -fsSL --retry 3 -o "$work/$name.tar.xz.sig" "$url.sig"
curl -fsSL --retry 3 -o "$work/keys.asc" "$keys_url"
mkdir -m 700 "$work/gnupg"
GNUPGHOME="$work/gnupg" gpg --batch --quiet --import "$work/keys.asc" 2> /dev/null
GNUPGHOME="$work/gnupg" gpg --batch --status-fd 1 --verify "$work/$name.tar.xz.sig" "$work/$name.tar.xz" 2> /dev/null |
  grep -q "^\[GNUPG:\] VALIDSIG $key_fpr " || {
  echo "tools/install-llvm.sh: $name.tar.xz is not signed by $key_fpr" >&2
  exit 1
}
GNUPGHOME="$work/gnupg" gpgconf --kill gpg-agent 2> /dev/null || true
mkdir -p "$work/x"
tar -C "$work/x" -xJf "$work/$name.tar.xz" --wildcards \
  "$name/bin/llvm-config" "$name/include/llvm/*" "$name/include/llvm-c/*" "$name/lib/libLLVM*.a" \
  "$name/bin/clang" "$name/bin/clang-${version%%.*}" "$name/lib/clang/${version%%.*}/include/*"
mkdir -p "$dest"
cp -a "$work/x/$name/." "$dest/"
got=$("$dest/bin/llvm-config" --version)
[ "$got" = "$version" ] || { echo "tools/install-llvm.sh: $dest/bin/llvm-config says $got, not $version" >&2; exit 1; }
"$dest/bin/clang" --version | grep -q "clang version $version " || { echo "tools/install-llvm.sh: $dest/bin/clang is not clang $version" >&2; exit 1; }
echo "tools/install-llvm.sh: LLVM $version in $dest"
