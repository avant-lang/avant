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
# Prints every installed version at the end.
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

install_pins() {
  sudo apt-get install -y --no-install-recommends --allow-downgrades "${PACKAGES[@]}"
}

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

echo "toolchain:"
dpkg-query -W gcc-15 binutils libclang-20-dev libzstd-dev libc6-dev linux-libc-dev zlib1g-dev | sed 's/^/  /'
echo "  cc: $(cc --version | head -n 1)"
echo "  ld: $(ld --version | head -n 1)"
echo "  glibc: $(getconf GNU_LIBC_VERSION)"
