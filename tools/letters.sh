#!/bin/sh
# The letters, in one place (Stage 28): the runtime, B (the seed builds it),
# and the compilers B builds. Never compile the compiler with the compiler
# under test: B always comes from the seed, C' from B, D' from C'.
#
#   sh tools/letters.sh [STEP...]     (any directory; default: runtime b c-llvm)
#   sh tools/letters.sh --print-seed  prints the seed directory the steps use
#                                     (LETTERS_SEED or the default; the one
#                                     place the default is computed, O31)
#
# STEP is one of:
#   runtime  sh runtime/build.sh (it also builds runtime/build/avant_llvm.o
#            through tools/build-llvm.sh, cached)
#   b        the seed compiles compiler/main.av into bin/avant-b, --debug;
#            a B that carries .llvm_stackmaps (built by a seed older than
#            v0.35.0, whose back end emitted statepoints) is refused: the
#            runtime has no stackmap walker since Stage 28 W12r (the
#            refused bin/avant-b is removed)
#   c-llvm   C' (Stage 28 W9, W10): bin/avant-b compiles compiler/main.av
#            into bin/avant-c-llvm, --debug
#   d-llvm   D': bin/avant-c-llvm compiles compiler/main.av into
#            bin/avant-d-llvm, --debug
#   c-release  the compiler a release ships (W10, D208): bin/avant-b
#            compiles compiler/main.av into bin/avant-c-release, --release
#            (built once per release; the letters and the gate stay
#            --debug)
#
# The names keep the letter and the back end that built it (c-llvm,
# d-llvm; the old back end's C, letter c, retired with it at W11). C' is
# built by B and D' by C'; C' is judged only by D' and the suite (C' = D'
# byte for byte, and the pre-optimization .ll of compiler/main.av from
# both, through `avant __emit-ll`, is the fixed point, D171;
# tools/gate.sh runs it). Every compiler B builds, and B's own compiles,
# use the tree's only back end: no AVANT_BACKEND is set (an ambient one is
# removed).
#
# The compiler links the in-process LLVM (D172): every compiler build gets
# runtime/build/avant_llvm.o as a link argument (COMPILER_LINK below);
# a user program never does.
#
# Environment:
#   LETTERS_SEED      seed directory with avant (default bin/v0.41.0, the
#                     release; a seed must emit W15a1's call table, so
#                     v0.35.0 and v0.36.0 no longer build a correct B since
#                     the C-stack scans retired); relative to the checkout.
#                     Seeds older than v0.35.0
#                     (v0.34.0, bin/seeds/s28-w2) are not supported: their B
#                     carries LLVM stack maps the runtime no longer walks.
#   LETTERS_TIME_DIR  when set, each build's `/usr/bin/time -p` goes to
#                     LETTERS_TIME_DIR/time-build-<step>.txt (b, c-llvm,
#                     d-llvm, c-release)
#
# Every child runs under `timeout -k 10` (runtime 300 s, a compiler build
# 900 s) with stdin from /dev/null. Output goes to stdout and stderr. The
# exit status is the first failing step's (124 on a timeout), else 0; 2 on
# a usage error.
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
[ -f compiler/main.av ] && [ -f runtime/build.sh ] || { echo "letters: $root is not an avant checkout" >&2; exit 2; }

COMPILER_LINK="runtime/build/avant_llvm.o"

# The default seed (D178, D189; Stage 28 W12r, W14b2, W16b, then the
# v0.37.0 to v0.41.0 releases): bin/v0.41.0, the release archive unpacked (CI pins the
# same asset). The v0.36.0 release predates W15a1's call table: the code it
# emits holds references in registers across runtime helpers that now
# collect, which only the conservative C-stack pin scan made safe, and W16b
# retired the scan, so a B it builds is wrong (the local seed
# bin/seeds/s28-w16b bridged that gap until v0.37.0). This is the one place
# the default is computed; tools/gate.sh asks --print-seed (O31).
default_seed=bin/v0.41.0
if [ -z "${LETTERS_SEED:-}" ] && [ ! -x "$default_seed/avant" ]; then
  echo "letters: the default seed $default_seed is missing: unpack the v0.41.0 release archive there, or set LETTERS_SEED to a seed built after Stage 28 W15a (v0.36.0's B needs the retired C-stack scan)" >&2
  exit 2
fi
seed=$(realpath -m "${LETTERS_SEED:-$default_seed}")
time_dir=${LETTERS_TIME_DIR:-}

if [ "${1:-}" = --print-seed ]; then
  [ $# -eq 1 ] || { echo "letters: --print-seed takes no steps" >&2; exit 2; }
  echo "$seed"
  exit 0
fi

steps="$*"
[ -n "$steps" ] || steps="runtime b c-llvm"
for s in $steps; do
  case "$s" in
    runtime|b|c-llvm|d-llvm|c-release) ;;
    *) echo "letters: unknown step $s (runtime, b, c-llvm, d-llvm, c-release)" >&2; exit 2 ;;
  esac
done

need_llvm() {
  [ -f "$COMPILER_LINK" ] && return 0
  echo "letters: $COMPILER_LINK is missing: run sh runtime/build.sh (it needs llvm-20-dev: llvm-config-20 and the static LLVM archives)" >&2
  exit 1
}

# build LETTER COMPILER OUT MODE [FLAG]
# FLAG is --cpu=baseline for every compiler the tree's own compilers build
# (Stage 28 W18S, O140): the avant binary stays on plain x86-64 whatever a
# program's artifact default is, so it runs wherever a developer works.
# The seed (step b) predates --cpu and builds plain x86-64 itself.
build() {
  need_llvm
  [ -x "$2" ] || { echo "letters: no compiler at $2" >&2; exit 1; }
  timer=""
  [ -n "$time_dir" ] && timer="/usr/bin/time -p -o $time_dir/time-build-$1.txt"
  # shellcheck disable=SC2086 # timer and the flag are words or empty
  env -u AVANT_BACKEND -u AVANT_ROOT $root_env timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" "$4" ${5:-} "$COMPILER_LINK" < /dev/null
}

# B links the seed's ABI (Stage 29 W-INT1, the bootstrap): the seed's code
# generator has a 32-bit `Int`, so B, its output, links the copy of the
# program objects runtime/build.sh builds with a 32-bit avant_int, laid
# out as an Avant root (runtime/build/seed-abi); every compiler B builds,
# and every program, links runtime/build (a 64-bit `Int`). The seed's
# AVANT_ROOT rule (pipeline.av avant_root) picks the objects and the
# header the seed compiles C against; the compiler imports no std module,
# so nothing else is read from that root. This goes when a release with
# the 64-bit `Int` is the seed.
seed_root="runtime/build/seed-abi"
need_seed_abi() {
  [ -f "$seed_root/runtime/build/avant_rt.o" ] && return 0
  echo "letters: $seed_root is missing: run sh runtime/build.sh (it builds the seed-ABI runtime B links)" >&2
  exit 1
}

# B must carry no LLVM stack maps (the runtime walks none since W12r).
check_no_stackmaps() {
  if readelf -S "$1" 2>/dev/null | grep -q '\.llvm_stackmaps'; then
    echo "letters: $1 carries .llvm_stackmaps: the seed $seed is older than v0.35.0, and the runtime no longer walks stack maps (removed $1)" >&2
    rm -f "$1"
    return 1
  fi
}

for s in $steps; do
  case "$s" in
    runtime) timeout -k 10 300 sh runtime/build.sh < /dev/null ;;
    b) need_seed_abi; root_env="AVANT_ROOT=$root/$seed_root" build b "$seed/avant" bin/avant-b --debug && check_no_stackmaps bin/avant-b ;;
    c-llvm) root_env= build c-llvm ./bin/avant-b bin/avant-c-llvm --debug --cpu=baseline ;;
    d-llvm) root_env= build d-llvm ./bin/avant-c-llvm bin/avant-d-llvm --debug --cpu=baseline ;;
    c-release) root_env= build c-release ./bin/avant-b bin/avant-c-release --release --cpu=baseline ;;
  esac
  rc=$?
  [ $rc -eq 0 ] || exit $rc
done
exit 0
