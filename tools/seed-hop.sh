#!/bin/sh
# The seed hop (Stage 28 W16b, D252): CI's released seed is v0.36.0, whose
# generated code predates W15a1's call table (it keeps references in
# registers across runtime helpers that now collect; only the C-stack pin
# scan, retired in W16b, made that safe). So a B that v0.36.0 builds from
# this tree is wrong. The hop builds a seed that carries the current call
# table from v0.36.0, without trusting any unpublished binary:
#
#   1. the hop commit HOP (72c6826, the W16t merge: the last tree whose
#      runtime still has the pin scan, gated with the v0.36.0 seed) is
#      exported to a scratch directory, and its runtime is built there;
#   2. v0.36.0 builds that tree's B, and B builds its C' (tools/letters.sh
#      b c-llvm of HOP, with AVANT_ROOT at the scratch tree);
#   3. that C' is the seed: OUT/avant.
#
# On the machine that built the local seed bin/seeds/s28-w16b, OUT/avant
# is byte-identical to it (sha256 below); elsewhere a different LLVM 20
# patch release may change the bytes, which is reported, not an error:
# the seed's correctness comes from step 2, not from its bytes.
#
#   sh tools/seed-hop.sh SEED_DIR OUT_DIR
#
# SEED_DIR holds the released `avant` (v0.36.0). Needs git with the hop
# commit reachable (fetched from origin when the clone is shallow). Remove
# this script, and the workflow steps that call it, once a release built
# after W15a1 is the pinned seed.
set -eu

HOP=72c68261cb9d6f560435238a14efa8a348a4ba2c
HOP_LOCAL_SHA256=3797fdc99e7a4c27edda771725f280d9d4ae2c3f77dd1727fa85272e20a64442

[ $# -eq 2 ] || { echo "seed-hop: usage: sh tools/seed-hop.sh SEED_DIR OUT_DIR" >&2; exit 2; }
seed=$(realpath -m "$1")
out=$(realpath -m "$2")
[ -x "$seed/avant" ] || { echo "seed-hop: no avant in $seed" >&2; exit 2; }

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
if ! git -C "$root" cat-file -e "$HOP^{commit}" 2>/dev/null; then
  git -C "$root" fetch --quiet --depth=1 origin "$HOP"
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/avant-seed-hop.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
git -C "$root" archive "$HOP" | tar -x -C "$work"

echo "seed-hop: building the runtime of $HOP"
(cd "$work" && AVANT_ROOT="$work" timeout -k 10 900 sh runtime/build.sh < /dev/null > "$work/runtime.log" 2>&1) || { tail -20 "$work/runtime.log" >&2; exit 1; }
echo "seed-hop: $seed/avant builds B of $HOP, and B builds its C'"
(cd "$work" && AVANT_ROOT="$work" LETTERS_SEED="$seed" sh tools/letters.sh b c-llvm < /dev/null)

mkdir -p "$out"
cp "$work/bin/avant-c-llvm" "$out/avant"
sum=$(sha256sum "$out/avant" | cut -d' ' -f1)
echo "seed-hop: $out/avant sha256 $sum"
if [ "$sum" = "$HOP_LOCAL_SHA256" ]; then
  echo "seed-hop: byte-identical to the local seed bin/seeds/s28-w16b"
else
  echo "seed-hop: differs from the local seed bin/seeds/s28-w16b ($HOP_LOCAL_SHA256); expected where LLVM 20's patch release differs"
fi
