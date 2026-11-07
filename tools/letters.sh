#!/bin/sh
# The letters, in one place (Stage 28): the runtime, B (the seed builds it),
# and the compilers B builds. Never compile the compiler with the compiler
# under test: B always comes from the seed, C' from B, D' from C'.
#
#   sh tools/letters.sh [STEP...]     (any directory; default: runtime b c-llvm)
#
# STEP is one of:
#   runtime  sh runtime/build.sh (it also builds runtime/build/avant_llvm.o
#            through tools/build-llvm.sh, cached)
#   b        the seed compiles compiler/main.av into bin/avant-b, --debug,
#            on the seed's own default back end (a v0.34.0-style seed
#            compiles through the myc-llvm beside it, see LETTERS_SEED)
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
#   LETTERS_SEED      seed directory with avant (default bin/seeds/s28-w2
#                     when present, D189, else bin/v0.34.0). A seed from
#                     before Stage 28 W10 (v0.34.0, s28-w2) compiles only
#                     through myc-llvm: when the directory holds one, it is
#                     passed as AVANT_MYC_LLVM to step b, and only there. A
#                     myc-free seed (a release from W11 on) needs none.
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

# Default seed (D189): the local seed when present, else the released one.
default_seed=bin/v0.34.0
[ -x bin/seeds/s28-w2/avant ] && default_seed=bin/seeds/s28-w2
seed=$(realpath -m "${LETTERS_SEED:-$default_seed}")
# The seed's myc-llvm (D178: until the re-seed), for step b only.
seed_env=""
[ -x "$seed/myc-llvm" ] && seed_env="AVANT_MYC_LLVM=$seed/myc-llvm"
time_dir=${LETTERS_TIME_DIR:-}

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

# build LETTER COMPILER OUT MODE [ENV]   (ENV: VAR=value words or empty)
build() {
  need_llvm
  [ -x "$2" ] || { echo "letters: no compiler at $2" >&2; exit 1; }
  timer=""
  [ -n "$time_dir" ] && timer="/usr/bin/time -p -o $time_dir/time-build-$1.txt"
  # shellcheck disable=SC2086 # ENV and timer are words or empty
  env -u AVANT_BACKEND -u AVANT_MYC_LLVM ${5:-} timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" "$4" "$COMPILER_LINK" < /dev/null
}

for s in $steps; do
  case "$s" in
    runtime) timeout -k 10 300 sh runtime/build.sh < /dev/null ;;
    b) build b "$seed/avant" bin/avant-b --debug "$seed_env" ;;
    c-llvm) build c-llvm ./bin/avant-b bin/avant-c-llvm --debug ;;
    d-llvm) build d-llvm ./bin/avant-c-llvm bin/avant-d-llvm --debug ;;
    c-release) build c-release ./bin/avant-b bin/avant-c-release --release ;;
  esac
  rc=$?
  [ $rc -eq 0 ] || exit $rc
done
exit 0
