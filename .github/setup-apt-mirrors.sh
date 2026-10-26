#!/usr/bin/env bash
# GitHub-hosted Ubuntu 24.04 runners list azure.archive.ubuntu.com first.
# That host has been slow or unavailable from Actions. This script rewrites
# the local mirror list before any apt-get update, so a sick Azure mirror
# cannot block the rewrite itself.
#
# Primary is xTom (https://mirrors.xtom.com/ubuntu/), a Launchpad archive
# mirror in the United States, 10 Gbps, marked up to date, and serving
# noble-updates. kernel.org is the second full archive. archive.ubuntu.com
# is only the last resort: APT tries it when both of the others fail, so
# ordinary jobs do not add load there.
#
# Enzu is listed at 100 Gbps, but mirror.enzu.com does not currently
# publish noble-updates, so it is not a candidate.
set -Eeuo pipefail

readonly MIRROR_FILE="/etc/apt/apt-mirrors.txt"
readonly APT_CONFIG_FILE="/etc/apt/apt.conf.d/99-ci-apt-reliability"

readonly PRIMARY_URI="https://mirrors.xtom.com/ubuntu/"
readonly SECOND_URI="https://mirrors.kernel.org/ubuntu/"
readonly ARCHIVE_URI="https://archive.ubuntu.com/ubuntu/"

readonly APT_RETRIES="${CI_APT_RETRIES:-5}"
readonly APT_TIMEOUT="${CI_APT_TIMEOUT:-90}"

log() {
  printf '%s\n' "$*"
}

notice() {
  printf '::notice::%s\n' "$*"
}

error() {
  printf '::error::%s\n' "$*"
}

if [[ ! -r /etc/os-release ]]; then
  error "Cannot determine operating system: /etc/os-release is missing"
  exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release

if [[ "${ID:-}" != "ubuntu" ]]; then
  error "This script only supports Ubuntu; detected ID=${ID:-unknown}"
  exit 1
fi

if [[ "${VERSION_ID:-}" != "24.04" ]]; then
  error "This script currently supports Ubuntu 24.04 only; detected VERSION_ID=${VERSION_ID:-unknown}"
  exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
  error "apt-get is not available"
  exit 1
fi

if ! command -v apt-config >/dev/null 2>&1; then
  error "apt-config is not available"
  exit 1
fi

arch="$(dpkg --print-architecture)"

if [[ "$arch" != "amd64" ]]; then
  error "This script targets Ubuntu 24.04 amd64 runners; detected architecture=$arch"
  exit 1
fi

if [[ ! "$APT_RETRIES" =~ ^[0-9]+$ ]]; then
  error "CI_APT_RETRIES must be a non-negative integer"
  exit 1
fi

if [[ ! "$APT_TIMEOUT" =~ ^[0-9]+$ ]] || (( APT_TIMEOUT < 1 )); then
  error "CI_APT_TIMEOUT must be a positive integer"
  exit 1
fi

if (( EUID == 0 )); then
  SUDO=()
elif command -v sudo >/dev/null 2>&1; then
  SUDO=(sudo)
else
  error "Root privileges are required, but sudo is unavailable"
  exit 1
fi

# No apt-get update, curl, or DNS. The rewrite has to work while the
# current mirror is the thing that is broken.
tmp_mirrors="$(mktemp)"
tmp_config="$(mktemp)"

cleanup() {
  rm -f "$tmp_mirrors" "$tmp_config"
}
trap cleanup EXIT

cat >"$tmp_mirrors" <<EOF
${PRIMARY_URI} priority:1
${SECOND_URI} priority:2
${ARCHIVE_URI} priority:3
EOF

"${SUDO[@]}" install \
  -o root \
  -g root \
  -m 0644 \
  "$tmp_mirrors" \
  "$MIRROR_FILE"

source_files=()

if [[ -f /etc/apt/sources.list ]]; then
  source_files+=("/etc/apt/sources.list")
fi

while IFS= read -r -d '' file; do
  source_files+=("$file")
done < <(
  find /etc/apt/sources.list.d \
    -maxdepth 1 \
    -type f \
    \( -name '*.list' -o -name '*.sources' \) \
    -print0 2>/dev/null
)

for file in "${source_files[@]}"; do
  if grep -qE 'https?://azure\.archive\.ubuntu\.com/ubuntu/' "$file"; then
    log "Replacing direct Azure Ubuntu APT source in $file"

    "${SUDO[@]}" sed -i \
      -e "s#http://azure\.archive\.ubuntu\.com/ubuntu/#${PRIMARY_URI}#g" \
      -e "s#https://azure\.archive\.ubuntu\.com/ubuntu/#${PRIMARY_URI}#g" \
      "$file"
  fi
done

cat >"$tmp_config" <<EOF
// Managed by setup-apt-mirrors.sh.
Acquire::Retries "${APT_RETRIES}";
Acquire::Queue-Mode "host";
Acquire::http::Timeout "${APT_TIMEOUT}";
Acquire::https::Timeout "${APT_TIMEOUT}";
Acquire::http::Pipeline-Depth "0";
Acquire::https::Pipeline-Depth "0";
Acquire::Languages "none";
EOF

"${SUDO[@]}" install \
  -o root \
  -g root \
  -m 0644 \
  "$tmp_config" \
  "$APT_CONFIG_FILE"

for file in "${source_files[@]}" "$MIRROR_FILE"; do
  [[ -f "$file" ]] || continue

  if grep -qE 'https?://azure\.archive\.ubuntu\.com/ubuntu/' "$file"; then
    error "Azure Ubuntu APT mirror is still referenced by $file"
    exit 1
  fi
done

apt-config dump >/dev/null

notice "APT mirrors configured"
notice "Azure Ubuntu APT mirror disabled"
notice "Primary Ubuntu mirror: ${PRIMARY_URI}"
notice "APT retries: ${APT_RETRIES}; timeout: ${APT_TIMEOUT}s; HTTP pipelining disabled"
