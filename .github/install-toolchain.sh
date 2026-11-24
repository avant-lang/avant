#!/usr/bin/env bash
# The build jobs' toolchain on an ubuntu-26.04 runner: the measuring
# server's compilers, at the server's exact versions.
#
#   bash .github/install-toolchain.sh
#
# gcc 15.2 (it builds the runtime), binutils, mold (tools/letters.sh links
# the compilers with it), libclang 20 (the bind helper)
# and the static libraries tools/build-llvm.sh merges are pinned to the
# server's versions. They come from the runner's archive while it carries
# them, else from Ubuntu's archive as it was at SNAPSHOT
# (snapshot.ubuntu.com, whose 2026-11-16 state has exactly these versions),
# so a pinned version cannot vanish when the archive moves on; apt checks
# the archive's signatures either way. The C library headers, zlib and the kernel headers follow the
# runner's own archive (through .github/setup-apt-mirrors.sh), as the
# server follows its updates. LLVM 22.1.8 itself is not in Ubuntu's archive
# (26.04 has 22.1.2): the jobs install LLVM's official release with
# tools/install-llvm.sh (sha256 and signature checked) and cache it.
# python3 (re2c's configure), xz-utils and gnupg (tools/install-llvm.sh)
# and GNU time (the suite) are installed too, though runners have them.
#
# The plain amd64 packages, never an architecture variant: GitHub's
# ubuntu-26.04 image sets apt to Ubuntu's amd64v3 variant, whose gcc emits
# x86-64-v3 code (AVX2, BMI) by default and whose static libraries
# (libstdc++.a, libc's start files, libzstd.a) hold such code, so the
# archive's runtime objects and compiler would not run on an older x86-64.
# The variant setting is cleared, and every package the build uses (the
# owners of the files the installed toolchain runs and links, build_files
# below) that came from a variant is installed again from amd64; the
# script fails if one is left or if gcc's default is not the baseline
# x86-64. Other preinstalled variant packages (libmysqlclient-dev and the
# like) are not used by the build and are listed as ignored
# (tools/isa-check.sh checks the archive itself).
# Prints every installed version, with its variant, at the end.
set -Eeuo pipefail

readonly SNAPSHOT=20261116T000000Z
readonly SOURCE=/etc/apt/sources.list.d/avant-snapshot.sources

readonly PACKAGES=(
  gcc=4:15.2.0-5ubuntu1
  g++=4:15.2.0-5ubuntu1
  cpp=4:15.2.0-5ubuntu1
  gcc-15=15.2.0-16ubuntu1
  g++-15=15.2.0-16ubuntu1
  cpp-15=15.2.0-16ubuntu1
  gcc-15-x86-64-linux-gnu=15.2.0-16ubuntu1
  g++-15-x86-64-linux-gnu=15.2.0-16ubuntu1
  cpp-15-x86-64-linux-gnu=15.2.0-16ubuntu1
  libgcc-15-dev=15.2.0-16ubuntu1
  libstdc++-15-dev=15.2.0-16ubuntu1
  binutils=2.46-3ubuntu2
  binutils-x86-64-linux-gnu=2.46-3ubuntu2
  mold=2.40.4+dfsg-2.1ubuntu1
  llvm-20=1:20.1.8-2ubuntu8
  libllvm20=1:20.1.8-2ubuntu8
  libclang-20-dev=1:20.1.8-2ubuntu8
  libzstd-dev=1.5.7+dfsg-3
  make=4.4.1-3
  libc6-dev
  zlib1g-dev
  python3
  xz-utils
  gnupg
  time
)

readonly NO_VARIANT=/etc/apt/apt.conf.d/99zz-avant-no-variant

# The files Avant's build runs or takes into its output, asked of the
# installed toolchain itself: the compiler driver and its passes, the
# assembler and linker (runtime/build.sh, tools/build-re2c.sh, every
# program's link), the start files and static libraries a link takes in
# (re2c links statically; tools/build-llvm.sh merges libstdc++.a, libz.a
# and libzstd.a into the compiler), and the libclang the bind helper links.
# Shared libraries the tools merely load (the runner's libc6 among them)
# add no code to what the build writes, so they are not listed.
build_files() {
  local t f p
  for t in cc gcc g++ c++ cpp as ld ld.bfd mold ar ranlib nm objcopy strip llvm-config-20; do
    p=$(command -v "$t" 2> /dev/null) && readlink -f "$p"
  done
  for t in cc1 cc1plus collect2 lto-wrapper lto1; do
    p=$(g++ -print-prog-name="$t" 2> /dev/null) && [ -f "$p" ] && readlink -f "$p"
  done
  for f in liblto_plugin.so crt1.o Scrt1.o rcrt1.o crti.o crtn.o crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o \
      libgcc.a libgcc_eh.a libc.a libc_nonshared.a libm.a libmvec.a libpthread.a libdl.a librt.a \
      libstdc++.a libsupc++.a libz.a libzstd.a; do
    p=$(g++ -print-file-name="$f" 2> /dev/null) && [ "$p" != "$f" ] && [ -f "$p" ] && readlink -f "$p"
  done
  if p=$(llvm-config-20 --libdir 2> /dev/null); then
    for f in "$p"/libclang-20.so* "$p"/libclang.so*; do [ -f "$f" ] && readlink -f "$f"; done
  fi
  return 0
}

# The installed packages that own those files (dpkg -S), as NAME=VERSION.
build_packages() {
  build_files | sort -u | while read -r f; do
    dpkg -S "$f" 2> /dev/null | sed 's/: .*//; s/, /\n/g'
  done | sed 's/:amd64$//' | sort -u | while read -r pkg; do
    dpkg-query -W -f '${Package}=${Version}\n' "$pkg"
  done
}

# Of the given NAME=VERSION, those from an architecture variant (dpkg
# records the variant a package came from).
from_variant() {
  local pv
  for pv in "$@"; do
    [ -n "$(dpkg-query -W -f '${Architecture-Variant}' "${pv%%=*}")" ] && echo "$pv"
  done
  return 0
}

install_pins() {
  sudo apt-get install -y --no-install-recommends --allow-downgrades "${PACKAGES[@]}" || return 1
  local used again
  mapfile -t used < <(build_packages)
  mapfile -t again < <(from_variant "${used[@]}")
  if ((${#again[@]})); then
    echo "install-toolchain: installing again from amd64 (the build uses them; they came from an architecture variant): ${again[*]}"
    # Not fatal here: the check after the install decides, naming what is
    # left.
    sudo apt-get install -y --no-install-recommends --allow-downgrades --reinstall "${again[@]}" ||
      echo "install-toolchain: installing them again failed; the check below names what is left"
  fi
}

echo "install-toolchain: apt's architecture variants before: $(apt-config dump | sed -n 's/^APT::Architecture-Variants[: ]*//p' | tr '\n' ' ')"
echo '#clear APT::Architecture-Variants;' | sudo tee "$NO_VARIANT" > /dev/null
if apt-config dump | grep -E '^APT::Architecture-Variants' | grep -vqE '^APT::Architecture-Variants "";$'; then
  echo "install-toolchain: apt still selects an architecture variant after $NO_VARIANT: $(apt-config dump | grep -E '^APT::Architecture-Variants')" >&2
  exit 1
fi

# The runner's own archive first (through the mirrors), which carries these
# versions while they are current; the snapshot only when one is gone
# (snapshot.ubuntu.com is slower and has answered 503 at times).
sudo apt-get update
if ! install_pins; then
  echo "install-toolchain: a pinned version is not in the archive any more; adding the snapshot $SNAPSHOT"
  sudo tee "$SOURCE" > /dev/null <<SOURCES
Types: deb
URIs: https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT/
Suites: resolute resolute-updates resolute-security
Components: main universe
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
SOURCES
  sudo apt-get update
  install_pins
fi

mapfile -t used < <(build_packages)
echo "install-toolchain: the packages the build uses (owners of $(build_files | sort -u | wc -l) files): ${used[*]}"
left=$(from_variant "${used[@]}")
if [[ -n "$left" ]]; then
  echo "install-toolchain: still from an architecture variant, and the build uses them: $left" >&2
  exit 1
fi
others=$(dpkg-query -W -f '${db:Status-Abbrev}\t${Package}\t${Architecture-Variant}\n' |
  awk -F'\t' '$1 ~ /^ii/ && $3 != "" { print $2 }' | grep -vxF -f <(printf '%s\n' "${used[@]%%=*}") | tr '\n' ' ' || true)
echo "install-toolchain: from an architecture variant, ignored, not used by the build (none of their files is a compiler pass, the assembler or the linker, or linked into what the build writes): ${others:-none}"
march=$(cc -Q --help=target | awk '$1 == "-march=" { print $2 }')
if [[ "$march" != "x86-64" ]]; then
  echo "install-toolchain: cc targets $march by default, not the baseline x86-64" >&2
  exit 1
fi

echo "toolchain:"
dpkg-query -W -f '${Package}\t${Version}\t${Architecture}\t${Architecture-Variant}\n' \
  gcc-15 gcc-15-x86-64-linux-gnu libgcc-15-dev libstdc++-15-dev binutils libclang-20-dev libzstd-dev libc6-dev linux-libc-dev zlib1g-dev |
  awk -F'\t' '{ printf "  %s\t%s\t%s, variant %s\n", $1, $2, $3, ($4 == "" ? "none" : $4) }'
echo "  variant: none (plain amd64; apt: $NO_VARIANT); cc's default -march=$march"
echo "  cc: $(cc --version | head -n 1)"
echo "  ld: $(ld --version | head -n 1)"
echo "  mold: $(mold --version)"
echo "  glibc: $(getconf GNU_LIBC_VERSION)"
