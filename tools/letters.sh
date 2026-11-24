#!/bin/sh
# The letters, in one place: the runtime, B (the seed builds it),
# and the compilers B builds. Never compile the compiler with the compiler
# under test: B always comes from the seed, C' from B, D' from C'.
#
#   sh tools/letters.sh [STEP...]     (any directory; default: runtime b c-llvm)
#   sh tools/letters.sh --print-seed  prints the seed directory the steps use
#                                     (LETTERS_SEED or the default; the one
#                                     place the default is computed)
#
# STEP is one of:
#   runtime  sh runtime/build.sh (it also builds runtime/build/avant_llvm.o
#            through tools/build-llvm.sh, cached)
#   b        the seed compiles compiler/main.av into bin/avant-b, --debug;
#            a B that carries .llvm_stackmaps (built by a seed older than
#            v0.35.0, whose back end emitted statepoints) is refused: the
#            runtime has no stackmap walker (the
#            refused bin/avant-b is removed)
#   c-llvm   C': bin/avant-b compiles compiler/main.av
#            into bin/avant-c-llvm, --debug
#   d-llvm   D': bin/avant-c-llvm compiles compiler/main.av into
#            bin/avant-d-llvm, --debug
#   c-release  the compiler a release ships: bin/avant-b
#            compiles compiler/main.av into bin/avant-c-release, --release
#            (built once per release; the letters and the gate stay
#            --debug)
#
# Each compiler step also writes OUT.versions.txt beside the compiler it
# built (record below): what `avant version` prints for it.
#
# The names keep the letter and the back end that built it (c-llvm,
# d-llvm; the old back end's C, letter c, retired with it). C' is
# built by B and D' by C'; C' is judged only by D' and the suite (C' = D'
# byte for byte, and the pre-optimization .ll of compiler/main.av from
# both, through `avant __emit-ll`, is the fixed point;
# tools/gate.sh runs it). Every compiler B builds, and B's own compiles,
# use the tree's only back end: no AVANT_BACKEND is set (an ambient one is
# removed).
#
# The compiler links the in-process LLVM: every compiler build gets
# runtime/build/avant_llvm.o as a link argument (COMPILER_LINK below);
# a user program never does.
#
# Every compiler build runs in the verify mode (AVANT_VERIFY=1: the MIR
# verifier at stage final and LLVM's verifier, compiler/llvm/driver.av).
#
# The compilers link with mold when it is installed (LETTERS_MOLD, else
# `mold` on PATH; LETTERS_MOLD=none for GNU ld) and the compiler it linked
# starts (`OUT version`); a compiler that does not start is linked again
# with GNU ld, cc's default, which every later step then uses. The first
# step says which linker. mold reads the 115 MB LLVM object in a fifth of
# GNU ld's time; lld cannot link the runtime (it puts its thread-local
# common symbols outside the TLS block). The compilers keep linking user
# programs with cc's default. LETTERS_MOLD_CHECK=fail makes the start
# check fail (to test the fallback).
#
# Environment:
#   LETTERS_SEED      seed directory with avant (default bin/v0.49.0, the
#                     release; a seed must emit the runtime call table, so
#                     v0.35.0 and v0.36.0 no longer build a correct B since
#                     the C-stack scans retired); relative to the checkout.
#                     Seeds older than v0.35.0
#                     (v0.34.0, bin/seeds/s28-w2) are not supported: their B
#                     carries LLVM stack maps the runtime no longer walks.
#                     A seed must also have the 64-bit `Int` (v0.41.0 or
#                     later): the runtime has one ABI, and a
#                     32-bit-`Int` seed's B would call it with the wrong
#                     widths. And it must be v0.44.0 or later: the code
#                     older seeds emit calls avant_gc_fast_ok,
#                     avant_gc_block and avant_inline_allocs, which the
#                     runtime no longer exports, so B does not link.
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
# One run of the letters in a checkout at a time (they write bin/ and
# runtime/build in place): a second run waits for the first.
if [ -z "${AVANT_LETTERS_LOCK:-}" ] && command -v flock > /dev/null 2>&1; then
  mkdir -p bin
  AVANT_LETTERS_LOCK=1 exec flock "$root/bin/.letters.lock" sh "$root/tools/letters.sh" "$@"
fi

COMPILER_LINK="runtime/build/avant_llvm.o"

# The default seed (a release since v0.37.0): bin/v0.49.0, the release
# archive unpacked (CI pins the same asset). The v0.36.0 release predates the runtime call table: the code it
# emits holds references in registers across runtime helpers that now
# collect, which only the conservative C-stack pin scan made safe, and the
# scan is retired, so a B it builds is wrong (the local seed
# bin/seeds/s28-w16b bridged that gap until v0.37.0). This is the one place
# the default is computed; tools/gate.sh asks --print-seed.
default_seed=bin/v0.49.0
if [ -z "${LETTERS_SEED:-}" ] && [ ! -x "$default_seed/avant" ]; then
  echo "letters: the default seed $default_seed is missing: unpack the v0.49.0 release archive there, or set LETTERS_SEED to a seed that emits the runtime call table (v0.36.0's B needs the retired C-stack scan)" >&2
  exit 2
fi
# A seed under the checkout keeps its path under the checkout even when
# bin/vX is a symbolic link to another checkout's seed (symbolic links are
# not resolved, given relative or absolute, as tools/gate.sh passes it):
# the compiler links the runtime of the tree it is started from only when
# its path lies under the working directory (compiler/pipeline.av,
# avant_root), and B must link this tree's runtime, which may export
# functions the seed's own runtime does not have yet.
seed_arg=${LETTERS_SEED:-$default_seed}
case $seed_arg in
  /*) seed=$(realpath -m -s "$seed_arg") ;;
  *) seed=$(realpath -m -s "$(pwd -P)/$seed_arg") ;;
esac
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
  echo "letters: $COMPILER_LINK is missing: run sh runtime/build.sh (it needs LLVM 22: sh tools/install-llvm.sh)" >&2
  exit 1
}

# The compilers' linker: CC (read by every compiler's link, the seed's
# too) names bin/.letters-ld/cc, which runs cc (or the CC given) with
# -fuse-ld=mold and the chosen mold first on its program path.
mold=${LETTERS_MOLD:-$(command -v mold || echo none)}
link_env=""
link_said=""
link_setup() {
  [ -n "$link_said" ] && return 0
  link_said=1
  if [ "$mold" != none ] && "$mold" --version 2> /dev/null | grep -q '^mold '; then
    d=$root/bin/.letters-ld
    mkdir -p "$d"
    ln -sf "$mold" "$d/ld.mold"
    printf '#!/bin/sh\nexec %s -B%s -fuse-ld=mold "$@"\n' "${CC:-cc}" "$d" > "$d/cc"
    chmod 755 "$d/cc"
    link_env="CC=$d/cc"
    echo "letters: the compilers link with mold ($("$mold" --version | cut -d ' ' -f 1-2))"
  else
    echo "letters: the compilers link with GNU ld (cc's default; no mold)"
  fi
}

# starts OUT: the compiler just built runs.
starts() {
  [ "${LETTERS_MOLD_CHECK:-}" = fail ] && return 1
  env -u AVANT_ROOT timeout -k 5 60 "$1" version > /dev/null 2>&1 < /dev/null
}

# build LETTER COMPILER OUT MODE [FLAG]
# FLAG is --cpu=baseline for every compiler: the
# avant binary stays on plain x86-64 whatever a program's artifact default
# is, so it runs wherever a developer works. B too since the v0.41.0 seed
# (older seeds predated --cpu and built plain x86-64 themselves).
build() {
  need_llvm
  link_setup
  [ -x "$2" ] || { echo "letters: no compiler at $2" >&2; exit 1; }
  timer=""
  [ -n "$time_dir" ] && timer="/usr/bin/time -p -o $time_dir/time-build-$1.txt"
  rm -f "$3.versions.txt"
  # shellcheck disable=SC2086 # timer, the flag and link_env are words or empty
  env -u AVANT_BACKEND -u AVANT_ROOT AVANT_VERIFY=1 $link_env timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" "$4" ${5:-} "$COMPILER_LINK" < /dev/null || return $?
  if [ -n "$link_env" ] && ! starts "$3"; then
    echo "letters: $3 linked by mold does not start: linked again with GNU ld, which the later steps use"
    link_env=""
    # shellcheck disable=SC2086 # timer and the flag are words or empty
    env -u AVANT_BACKEND -u AVANT_ROOT AVANT_VERIFY=1 timeout -k 10 900 $timer "$2" compile compiler/main.av "$3" "$4" ${5:-} "$COMPILER_LINK" < /dev/null || return $?
  fi
  record "$3" "$1"
}

# record OUT STEP: writes OUT.versions.txt, the build's record that
# `avant version` prints: avant.toml's version,
# the commit (unknown outside git), `dirty: yes` when compiler/, lib/ or
# runtime/ differ from that commit (untracked files there included), and
# the step, and `lib:` the checkout's lib/ (physical path) that the
# compiler reads its standard library from when AVANT_ROOT is not set
# (so a kept or copied compiler keeps its own library, or
# says it is gone). The compiler carries none of it, so a compiler stays
# byte-identical across commits that do not change it.
record() {
  v=$(sed -n 's/^version = "\(.*\)"$/\1/p' avant.toml | head -n 1)
  c=$(git rev-parse HEAD 2>/dev/null) || c=unknown
  {
    [ -z "$v" ] || echo "version: $v"
    echo "avant: $c"
    if [ "$c" != unknown ] && [ -n "$(git status --porcelain -- compiler lib runtime 2>/dev/null)" ]; then
      echo "dirty: yes"
    fi
    echo "built: tools/letters.sh $2"
    echo "lib: $(pwd -P)/lib"
  } > "$1.versions.txt"
}

# B must carry no LLVM stack maps (the runtime walks none).
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
    b) build b "$seed/avant" bin/avant-b --debug --cpu=baseline && check_no_stackmaps bin/avant-b ;;
    c-llvm) build c-llvm ./bin/avant-b bin/avant-c-llvm --debug --cpu=baseline ;;
    d-llvm) build d-llvm ./bin/avant-c-llvm bin/avant-d-llvm --debug --cpu=baseline ;;
    c-release) build c-release ./bin/avant-b bin/avant-c-release --release --cpu=baseline ;;
  esac
  rc=$?
  [ $rc -eq 0 ] || exit $rc
done
exit 0
