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
#   b        the seed compiles compiler/main.av into bin/avant-b, --debug
#            (on the old back end: the seed has no other)
#   c-llvm   C', the default compiler (Stage 28 W9, W10): bin/avant-b
#            compiles compiler/main.av into bin/avant-c-llvm with
#            AVANT_BACKEND=llvm, --debug
#   d-llvm   D': bin/avant-c-llvm compiles compiler/main.av into
#            bin/avant-d-llvm with AVANT_BACKEND=llvm, --debug
#   c        the old C (until W11): bin/avant-b compiles compiler/main.av
#            into bin/avant-c with AVANT_BACKEND=myc, --debug
#
# The names keep the letter and the back end that built it: C' and D' are
# c-llvm and d-llvm (the new back end, the default since W10), C is c
# (myc). W11 retires c with the old back end.
#
# The llvm letters (D171, D176): C' is built by B on the new back end and
# D' by C'; C' is judged only by D' and the suite (C' = D' byte for byte,
# and the pre-optimization .ll of compiler/main.av from both, through
# `avant __emit-ll`, is the new fixed point; tools/gate.sh runs it by
# default). The new back end needs no myc-llvm; the
# AVANT_MYC_LLVM variable is still passed (the compiler's myc probe for
# a myc compile inside, e.g. a test, reads it).
#
# The compiler links the in-process LLVM (D172): every compiler build gets
# runtime/build/avant_llvm.o as a link argument (COMPILER_LINK below);
# a user program never does.
#
# Environment:
#   LETTERS_SEED      seed directory with avant and myc-llvm (default
#                     bin/seeds/s28-w2 when present, D189, else bin/v0.34.0); its myc-llvm is passed as AVANT_MYC_LLVM
#                     to both builds, explicitly
#   LETTERS_TIME_DIR  when set, each build's `/usr/bin/time -p` goes to
#                     LETTERS_TIME_DIR/time-build-<step>.txt (b, c,
#                     c-llvm, d-llvm)
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
M=$seed/myc-llvm
time_dir=${LETTERS_TIME_DIR:-}

steps="$*"
[ -n "$steps" ] || steps="runtime b c-llvm"
for s in $steps; do
  case "$s" in
    runtime|b|c|c-llvm|d-llvm) ;;
    *) echo "letters: unknown step $s (runtime, b, c, c-llvm, d-llvm)" >&2; exit 2 ;;
  esac
done

need_llvm() {
  [ -f "$COMPILER_LINK" ] && return 0
  echo "letters: $COMPILER_LINK is missing: run sh runtime/build.sh (it needs llvm-20-dev: llvm-config-20 and the static LLVM archives)" >&2
  exit 1
}

# build LETTER COMPILER OUT BACKEND
build() {
  need_llvm
  [ -x "$2" ] || { echo "letters: no compiler at $2" >&2; exit 1; }
  [ -x "$M" ] || { echo "letters: no myc-llvm at $M" >&2; exit 1; }
  timer=""
  [ -n "$time_dir" ] && timer="/usr/bin/time -p -o $time_dir/time-build-$1.txt"
  # shellcheck disable=SC2086 # timer is words or empty
  AVANT_BACKEND=$4 AVANT_MYC_LLVM=$M timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" --debug "$COMPILER_LINK" < /dev/null
}

for s in $steps; do
  case "$s" in
    runtime) timeout -k 10 300 sh runtime/build.sh < /dev/null ;;
    b) build b "$seed/avant" bin/avant-b myc ;;
    c) build c ./bin/avant-b bin/avant-c myc ;;
    c-llvm) build c-llvm ./bin/avant-b bin/avant-c-llvm llvm ;;
    d-llvm) build d-llvm ./bin/avant-c-llvm bin/avant-d-llvm llvm ;;
  esac
  rc=$?
  [ $rc -eq 0 ] || exit $rc
done
exit 0
