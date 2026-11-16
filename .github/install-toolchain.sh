#!/usr/bin/env bash
# The build jobs' toolchain on an ubuntu-26.04 runner: the measuring
# server's compilers, at the server's exact versions.
#
#   bash .github/install-toolchain.sh
#
# gcc 15.2 (it builds the runtime), binutils, libclang 20 (the bind helper)
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
# The variant setting is cleared, and every installed variant package that
# ships a compiler, a static library or an object file is installed again
# from amd64; the script fails if one is left or if gcc's default is not
# the baseline x86-64 (tools/isa-check.sh checks the archive itself).
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

# Installed packages of an architecture variant (dpkg records the variant
# a package came from) that ship a compiler or what a link takes in.
variant_build_packages() {
  dpkg-query -W -f '${db:Status-Abbrev}\t${Package}=${Version}\t${Architecture-Variant}\n' |
    awk -F'\t' '$1 ~ /^ii/ && $3 != "" { print $2 }' |
    while read -r pv; do
      if dpkg -L "${pv%%=*}" 2> /dev/null | grep -qE '\.(a|o)$|^/usr/(lib|libexec)/gcc/'; then
        echo "$pv"
      fi
    done
}

install_pins() {
  sudo apt-get install -y --no-install-recommends --allow-downgrades "${PACKAGES[@]}" || return 1
  local again
  mapfile -t again < <(variant_build_packages)
  if ((${#again[@]})); then
    echo "install-toolchain: installing again from amd64 (they came from an architecture variant): ${again[*]}"
    sudo apt-get install -y --no-install-recommends --allow-downgrades --reinstall "${again[@]}" || return 1
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

left=$(variant_build_packages)
if [[ -n "$left" ]]; then
  echo "install-toolchain: still from an architecture variant: $left" >&2
  exit 1
fi
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
echo "  glibc: $(getconf GNU_LIBC_VERSION)"
