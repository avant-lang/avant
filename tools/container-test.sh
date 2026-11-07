#!/bin/sh
# Install a release into a clean container and smoke-test it (D147).
#
#   sh tools/container-test.sh IMAGE DIST_DIR [EXPECT]
#
# DIST_DIR holds avant-linux-x86_64.tar.gz and its .sha256 (tools/package.sh
# output). The container gets only what a user needs (a C compiler and the
# C library's development files, plus curl and tar for the installer) and
# no LLVM, PCRE2, zlib development files, re2c, or myc-llvm. It runs install.sh
# against a file:// release, then tools/smoke.sh on the installed ~/.avant.
#
# EXPECT selects a negative case instead:
#   ok        (default) install and smoke pass
#   no-cc     no C compiler: the install completes, the self-test fails
#             with the distribution's install hint
#   no-binary the installer refuses the platform (an Alpine/musl image)
#   old-glibc the installer refuses a glibc older than the release's floor
#
# Needs docker. The repository and DIST_DIR are mounted read-only.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "usage: sh tools/container-test.sh IMAGE DIST_DIR [ok|no-cc|no-binary|old-glibc]" >&2
  exit 2
fi
image=$1
dist=$(CDPATH= cd -- "$2" && pwd)
expect=${3:-ok}
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

case $expect in
ok | no-cc | no-binary | old-glibc) ;;
*)
  echo "container-test: unknown expectation $expect" >&2
  exit 2
  ;;
esac

# Runs inside the container as root, from /src (the repository).
inner=$(cat <<'EOF_INNER'
set -u
expect=$1
cc_pkgs=yes
[ "$expect" = no-cc ] && cc_pkgs=no
if command -v apt-get > /dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq > /dev/null
  pkgs="curl ca-certificates"
  [ $cc_pkgs = yes ] && pkgs="$pkgs gcc libc6-dev"
  apt-get install -y -qq --no-install-recommends $pkgs > /dev/null
elif command -v dnf > /dev/null; then
  pkgs="curl tar gzip findutils"
  [ $cc_pkgs = yes ] && pkgs="$pkgs gcc glibc-devel"
  dnf install -y -q $pkgs > /dev/null
elif command -v apk > /dev/null; then
  apk add -q bash curl tar
fi
echo "container: $(. /etc/os-release && echo "$PRETTY_NAME"), $(getconf GNU_LIBC_VERSION 2>/dev/null || echo 'no glibc')"
for tool in llvm-config-20 llvm-config re2c pcre2-config myc-llvm; do
  if command -v $tool > /dev/null; then
    echo "container: $tool is present, the image is not clean" >&2
    exit 1
  fi
done
export HOME=/root
touch /root/.bashrc
log=/tmp/install.log
SHELL=/bin/bash AVANT_INTERNAL_DOWNLOAD_BASE=file:///dist bash /src/install.sh < /dev/null > $log 2>&1
rc=$?
cat $log
case $expect in
ok)
  [ $rc -eq 0 ] || { echo "container: install failed ($rc)" >&2; exit 1; }
  grep -q '^# avant$' /root/.bashrc || { echo "container: PATH not added to ~/.bashrc" >&2; exit 1; }
  sh /src/tools/smoke.sh /root/.avant
  ;;
no-cc)
  [ $rc -ne 0 ] || { echo "container: install without cc should fail its self-test" >&2; exit 1; }
  grep -q 'no C compiler (cc) found' $log && grep -Eq 'apt install gcc libc6-dev|dnf install gcc glibc-devel' $log ||
    { echo "container: missing the cc hint" >&2; exit 1; }
  [ -x /root/.avant/avant ] || { echo "container: the install itself should complete" >&2; exit 1; }
  echo "container: no-cc behaves as expected"
  ;;
no-binary)
  [ $rc -ne 0 ] && grep -q 'Avant has no binary' $log || { echo "container: expected the no-binary error" >&2; exit 1; }
  [ ! -e /root/.avant ] || { echo "container: nothing should be installed" >&2; exit 1; }
  echo "container: no-binary behaves as expected"
  ;;
old-glibc)
  [ $rc -ne 0 ] && grep -q 'needs glibc' $log || { echo "container: expected the glibc floor error" >&2; exit 1; }
  [ ! -e /root/.avant ] || { echo "container: nothing should be installed" >&2; exit 1; }
  echo "container: old-glibc behaves as expected"
  ;;
esac
EOF_INNER
)

docker run --rm --network host \
  -v "$root:/src:ro" -v "$dist:/dist:ro" \
  "$image" sh -c "$inner" container "$expect"
