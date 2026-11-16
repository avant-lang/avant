#!/usr/bin/env bash
# A directory holding the plain amd64 C library, for qemu-x86_64 to load in
# place of the runner's own:
#
#   bash .github/baseline-sysroot.sh DIR
#
# The ubuntu-26.04 image installs its libraries from apt's amd64v3 variant,
# so a program emulated on an older x86-64 (tools/isa-check.sh --run) would
# stop in the runner's libc, not in the archive. .github/install-toolchain.sh
# has cleared the variant, so `apt-get download` fetches the amd64 builds of
# libc6 (libc, libm, the dynamic loader) and libgcc-s1, which the compiler
# and its programs load; they are unpacked into DIR, given to qemu as
# QEMU_LD_PREFIX.
set -Eeuo pipefail
dir=$1
mkdir -p "$dir"
debs=$(mktemp -d)
(cd "$debs" && apt-get download libc6 libgcc-s1)
for d in "$debs"/*.deb; do
  dpkg-deb -x "$d" "$dir"
  echo "baseline-sysroot: $(dpkg-deb -f "$d" Package Version Architecture | tr '\n' ' ')"
done
rm -rf "$debs"
# The packages hold the merged /usr layout; the paths the loader and its
# programs name (/lib64/ld-linux-x86-64.so.2, /lib/x86_64-linux-gnu) are
# the base system's links, made here too, or qemu would take the runner's.
for l in lib lib64; do
  [ -e "$dir/$l" ] || ln -s "usr/$l" "$dir/$l"
done
