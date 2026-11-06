#!/bin/sh
# The letters, in one place (Stage 28): the runtime, B (the seed builds it),
# and C (B builds it). Never compile the compiler with the compiler under
# test: B always comes from the seed, C from B.
#
#   sh tools/letters.sh [STEP...]     (any directory; default: runtime b c)
#
# STEP is one of:
#   runtime  sh runtime/build.sh (it also builds runtime/build/avant_llvm.o
#            through tools/build-llvm.sh, cached)
#   b        the seed compiles compiler/main.av into bin/avant-b, --debug
#   c        bin/avant-b compiles compiler/main.av into bin/avant-c, --debug
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
#                     LETTERS_TIME_DIR/time-build-{b,c}.txt
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
[ -n "$steps" ] || steps="runtime b c"
for s in $steps; do
  case "$s" in
    runtime|b|c) ;;
    *) echo "letters: unknown step $s (runtime, b, c)" >&2; exit 2 ;;
  esac
done

need_llvm() {
  [ -f "$COMPILER_LINK" ] && return 0
  echo "letters: $COMPILER_LINK is missing: run sh runtime/build.sh (it needs llvm-20-dev: llvm-config-20 and the static LLVM archives)" >&2
  exit 1
}

# build LETTER COMPILER OUT
build() {
  need_llvm
  [ -x "$2" ] || { echo "letters: no compiler at $2" >&2; exit 1; }
  [ -x "$M" ] || { echo "letters: no myc-llvm at $M" >&2; exit 1; }
  timer=""
  [ -n "$time_dir" ] && timer="/usr/bin/time -p -o $time_dir/time-build-$1.txt"
  # shellcheck disable=SC2086 # timer is words or empty
  AVANT_MYC_LLVM=$M timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" --debug "$COMPILER_LINK" < /dev/null
}

for s in $steps; do
  case "$s" in
    runtime) timeout -k 10 300 sh runtime/build.sh < /dev/null ;;
    b) build b "$seed/avant" bin/avant-b ;;
    c) build c ./bin/avant-b bin/avant-c ;;
  esac
  rc=$?
  [ $rc -eq 0 ] || exit $rc
done
exit 0
