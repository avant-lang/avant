#!/bin/sh
# The differential oracle (Stage 28, D176): builds and runs a set of programs
# once per side and compares what they observably do (stdout, stderr, exit
# status), program by program.
#
#   sh tools/backend-diff.sh [--set examples,fixtures,langarena,scalars,strings,objects,heap] [PROGRAM...]
#
# A side is a compiler plus an environment (VAR=value words, applied to the
# compile and to the run). Both default to bin/avant-c with the seed's
# backend, so today the oracle compares myc with itself:
#   BDIFF_A_COMPILER / BDIFF_B_COMPILER   default bin/avant-c
#   BDIFF_A_ENV / BDIFF_B_ENV             default AVANT_MYC_LLVM=<root>/bin/v0.34.0/myc-llvm
#                                         (a later wave sets e.g. BDIFF_B_ENV="... AVANT_BACKEND=llvm")
#   BDIFF_MODE    compile flag for both sides: --debug (default), --release, or "" (bare)
#   BDIFF_OUT     report directory (default results/backend-diff-YYYYmmdd-HHMMSS)
#   BDIFF_KEEP=1  keep the work directory (binaries, outputs) in BDIFF_OUT/work
#
# Sets (default all three, in this order):
#   examples   every examples/*.av except the exclusions below, with the
#              flags the suite uses (add, layout: --cc examples/c/*.c; zlib: --lib z)
#   fixtures   every tests/fixtures/**/*.av that has `fn main` (run as a program),
#              except tests/fixtures/format/ (formatter inputs, not programs)
#   langarena  the 50 LangArena tasks in `test` mode through the harness
#              (langarena/harness/measure.cr, built once into the work dir),
#              one harness pass per side; compared per task: the checksum,
#              exit status, compile/timeout flags, the program's stdout and
#              stderr (the harness's wall/RSS/compile-time lines are not compared)
#   scalars    not in the default: the programs the new back end supports
#              (Stage 28 W5b, mir.md W5.7): every tests/fixtures/scalars/*.av
#              (fn run programs included), examples/hello.av, fact.av,
#              switch.av, and the fixtures the scalar lowering accepts
#              (scalar_programs below); run it with
#              BDIFF_B_ENV="... AVANT_BACKEND=llvm"
#   strings    not in the default: the heap-string programs (Stage 28 W6a,
#              mir.md W6a.7): every tests/fixtures/strings/*.av and the
#              fixtures the string lowering accepts (string_programs below)
#   objects    not in the default: the struct and class programs (Stage 28
#              W6b, mir.md W6b.7): every tests/fixtures/objects/*.av with
#              `fn main` (refuse_union.av, the L0001 case, has none) and the
#              examples and fixtures the lowering accepts (object_programs)
#   heap       not in the default: the array, hash, and Buf programs (Stage
#              28 W6c, mir.md W6c.7): every tests/fixtures/heap/*.av with
#              `fn main` (refuse_get.av, the L0001 case, has none) and the
#              examples and fixtures the lowering accepts (heap_programs)
# PROGRAM arguments select a subset instead of the sets: a path to a .av
# file (run as a program; an example gets its suite flags) or a LangArena
# task name such as Etc::Sieve. --set limits the sets; both may be combined.
#
# Exclusions and views (nothing else is excluded or normalized):
#   examples/http.av    skipped: opens a loopback listener and connects to it;
#                       the oracle must not depend on any network port
#   tests/fixtures/format/*.av  skipped: formatter inputs (surface.av imports
#                       modules that do not exist), not programs
#   examples/matmul.av  compared on stdout lines 1-2 only: lines 3-4 are the
#                       milliseconds the sequential and spawned passes took
#
# Per program: compile and run each under `timeout -k 10 120` with stdin from
# /dev/null; the run happens in one empty directory with the binary at the
# same path for both sides, so argv[0] and the working directory agree. A
# program that fails to compile on either side (or both) is a difference. The
# harness enforces 120 s per LangArena compile and run itself.
#
# Output: one line per difference on stdout and in BDIFF_OUT/differences.txt
# (details with diffs in BDIFF_OUT/diff/), every program's result in
# BDIFF_OUT/results.txt, and the summary `N same, M different, K skipped`
# with the wall time. Exit 0 when M = 0, 1 when M > 0, 2 on a usage or tool
# error. The work directory is removed on exit unless BDIFF_KEEP=1. Every
# child runs with TMPDIR inside the work directory (the same path for both
# sides), so nothing is written to /tmp. The LangArena harness, like its
# prod runs, writes its stitched sources and binaries to langarena/tmp/.
# Run it with nothing else running when its wall time counts.
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

sets="examples,fixtures,langarena"
explicit=""
while [ $# -gt 0 ]; do
  case "$1" in
    --set) [ $# -ge 2 ] || { echo "backend-diff: --set needs a value" >&2; exit 2; }; sets=$2; shift 2 ;;
    --set=*) sets=${1#--set=}; shift ;;
    -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) echo "backend-diff: unknown option $1" >&2; exit 2 ;;
    *) explicit="$explicit $1"; shift ;;
  esac
done
for a in "$@"; do explicit="$explicit $a"; done
has_set() { case ",$sets," in *",$1,"*) return 0 ;; esac; return 1; }

default_env="AVANT_MYC_LLVM=$root/bin/v0.34.0/myc-llvm"
A_COMPILER=$(realpath -m "${BDIFF_A_COMPILER:-bin/avant-c}")
B_COMPILER=$(realpath -m "${BDIFF_B_COMPILER:-bin/avant-c}")
A_ENV=${BDIFF_A_ENV-$default_env}
B_ENV=${BDIFF_B_ENV-$default_env}
MODE=${BDIFF_MODE---debug}
case "$MODE" in
  --debug) codegen=debug ;;
  --release) codegen=release ;;
  '') codegen=default ;;
  *) echo "backend-diff: BDIFF_MODE must be --debug, --release, or empty" >&2; exit 2 ;;
esac
for c in "$A_COMPILER" "$B_COMPILER"; do
  [ -x "$c" ] || { echo "backend-diff: no compiler at $c" >&2; exit 2; }
done

out=$(realpath -m "${BDIFF_OUT:-results/backend-diff-$(date +%Y%m%d-%H%M%S)}")
work=$out/work
rm -rf "$work"
mkdir -p "$work/a" "$work/b" "$work/run" "$work/tmp" "$out/diff" || exit 2
# Every child (compilers, programs, harness) gets the same private TMPDIR.
TMPDIR=$work/tmp
export TMPDIR
cleanup() { [ "${BDIFF_KEEP:-0}" = 1 ] || rm -rf "$work"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
: > "$out/results.txt"
: > "$out/differences.txt"
t0=$(date +%s)
same=0; different=0; skipped=0

{
  echo "A: $A_COMPILER env: $A_ENV"
  echo "B: $B_COMPILER env: $B_ENV"
  echo "mode: ${MODE:-bare} sets: $sets programs:${explicit:- (all)}"
} | tee "$out/header.txt"

record() { # STATUS NAME DETAIL
  echo "$1 $2${3:+ $3}" >> "$out/results.txt"
  case "$1" in
    same) same=$((same + 1)) ;;
    skip) skipped=$((skipped + 1)); echo "skip $2: $3" ;;
    *) different=$((different + 1)); echo "DIFFERENT $2: $3" | tee -a "$out/differences.txt" ;;
  esac
}

skip_reason() { # PATH -> prints a reason when the program is excluded
  case "$1" in
    examples/http.av) echo "opens a loopback listener and connects to it (no network ports in the oracle)" ;;
    tests/fixtures/format/*) echo "formatter input, not a program" ;;
  esac
}
flags_of() { # PATH -> the extra compile flags the suite uses
  case "$1" in
    examples/add.av) echo "--cc $root/examples/c/add.c" ;;
    examples/layout.av) echo "--cc $root/examples/c/layout.c" ;;
    examples/zlib.av) echo "--lib z" ;;
  esac
}
scalar_programs() { # the scalars set, one path per line
  for f in tests/fixtures/scalars/*.av; do echo "$f"; done
  echo examples/hello.av
  echo examples/fact.av
  echo examples/switch.av
  echo tests/fixtures/macros/hygiene.av
  echo tests/fixtures/macros/quote_fn.av
  echo tests/fixtures/macros/splice_name.av
  echo tests/fixtures/overloading/defaults.av
  echo tests/fixtures/part3/conv.av
  echo tests/fixtures/part3/run.av
}
string_programs() { # the strings set, one path per line
  for f in tests/fixtures/strings/*.av; do echo "$f"; done
  echo tests/fixtures/overloading/overload.av
  echo tests/fixtures/part3/chr.av
  echo tests/fixtures/part3/index.av
  echo tests/fixtures/part3/interp.av
  echo tests/fixtures/part3/slice.av
}
object_programs() { # the objects set, one path per line
  for f in tests/fixtures/objects/*.av; do grep -q '^fn main' "$f" && echo "$f"; done
  echo examples/binarytrees.av
  echo examples/defaults.av
  echo examples/methods.av
  echo examples/trees.av
  echo examples/vec2.av
  echo tests/fixtures/macros/fields.av
  echo tests/fixtures/macros/method.av
}
heap_programs() { # the heap set, one path per line
  for f in tests/fixtures/heap/*.av; do grep -q '^fn main' "$f" && echo "$f"; done
  echo examples/sieve.av
  echo tests/fixtures/modules/main.av
  echo tests/fixtures/part3/buf.av
  echo tests/fixtures/part3/pop.av
}
view() { # PATH FILE -> the compared view of a stdout file
  case "$1" in
    examples/matmul.av) head -n 2 "$2" ;;
    *) cat "$2" ;;
  esac
}

# side SIDE PATH KEY: compile and run PATH with side SIDE; leaves
# $work/SIDE/KEY.{clog,out,err} and sets crc and rrc.
side() {
  if [ "$1" = a ]; then comp=$A_COMPILER; senv=$A_ENV; else comp=$B_COMPILER; senv=$B_ENV; fi
  d=$work/$1
  rm -f "$d/$3.bin"
  # shellcheck disable=SC2046,SC2086 # senv and flags are words
  env $senv timeout -k 10 120 "$comp" compile "$root/$2" "$d/$3.bin" $MODE $(flags_of "$2") > "$d/$3.clog" 2>&1 < /dev/null
  crc=$?
  rrc=""
  : > "$d/$3.out"; : > "$d/$3.err"
  [ $crc -eq 0 ] && [ -x "$d/$3.bin" ] || { [ $crc -eq 0 ] && crc=255; return; }
  rm -rf "$work/run"; mkdir "$work/run"
  cp "$d/$3.bin" "$work/run/prog"
  # shellcheck disable=SC2086
  (cd "$work/run" && exec env $senv timeout -k 10 120 ./prog > "$d/$3.out" 2> "$d/$3.err" < /dev/null)
  rrc=$?
  rm -rf "$work/run"; mkdir "$work/run"
}

program() { # PATH relative to root
  p=$1
  r=$(skip_reason "$p")
  if [ -n "$r" ]; then record skip "$p" "$r"; return; fi
  [ -f "$root/$p" ] || { record different "$p" "no such file"; return; }
  key=$(echo "$p" | tr '/.' '__')
  side a "$p" "$key"; acrc=$crc; arrc=$rrc
  side b "$p" "$key"; bcrc=$crc; brrc=$rrc
  if [ $acrc -ne 0 ] || [ $bcrc -ne 0 ]; then
    { echo "== $p compile A rc=$acrc"; tail -20 "$work/a/$key.clog"; echo "== compile B rc=$bcrc"; tail -20 "$work/b/$key.clog"; } > "$out/diff/$key.txt"
    record different "$p" "compile failed: A rc=$acrc B rc=$bcrc"
    return
  fi
  view "$p" "$work/a/$key.out" > "$work/a/$key.view"
  view "$p" "$work/b/$key.out" > "$work/b/$key.view"
  why=""
  [ "$arrc" = "$brrc" ] || why="exit A=$arrc B=$brrc"
  cmp -s "$work/a/$key.view" "$work/b/$key.view" || why="$why${why:+, }stdout"
  cmp -s "$work/a/$key.err" "$work/b/$key.err" || why="$why${why:+, }stderr"
  if [ -z "$why" ]; then
    record same "$p" "exit=$arrc"
  else
    { echo "== $p: $why"; diff -u --label A --label B "$work/a/$key.view" "$work/b/$key.view" | head -40; diff -u --label A --label B "$work/a/$key.err" "$work/b/$key.err" | head -40; } > "$out/diff/$key.txt"
    record different "$p" "$why"
  fi
}

# LangArena: one harness pass per side over TASKS (comma list, or empty for all).
la_side() { # SIDE TASKS
  if [ "$1" = a ]; then comp=$A_COMPILER; senv=$A_ENV; else comp=$B_COMPILER; senv=$B_ENV; fi
  rm -rf "$work/la-$1"
  list=""
  [ -n "$2" ] && list="AVANT_TASK_LIST=$2"  # the harness reads an empty list as "no task"
  # shellcheck disable=SC2086
  env -u AVANT_SKIP_OK -u AVANT_PROBE_SCALE -u AVANT_TASK_LIST $senv AVANT_COMPILER="$comp" AVANT_CODEGEN=$codegen \
    AVANT_RESULTS_DIR="$work/la-$1" $list AVANT_TASK_TIMEOUT_S=120 AVANT_COMPILE_TIMEOUT_S=120 \
    timeout -k 10 2700 "$work/measure" test > "$out/langarena-$1.log" 2>&1 < /dev/null
  echo "harness $1 rc=$?" >> "$out/langarena-$1.log"
}
la_view() { # RAWFILE -> the compared fields and the program's output
  grep -E '^(actual|ok|compile_ok|timed_out|exit_code): ' "$1"
  sed -n '/^--- stdout ---$/,$p' "$1"
}
langarena() { # TASKS (comma list or empty)
  if [ ! -x "$work/measure" ]; then
    timeout -k 10 900 crystal build --no-debug -o "$work/measure" langarena/harness/measure.cr > "$out/harness-build.log" 2>&1 < /dev/null || {
      echo "backend-diff: cannot build the LangArena harness (see $out/harness-build.log)" >&2; exit 2; }
  fi
  la_side a "$1"
  la_side b "$1"
  if [ -n "$1" ]; then
    names=$(echo "$1" | tr ',' '\n')
  else
    names=$(sed -n 's/^  "\([A-Za-z0-9]*::[A-Za-z0-9]*\)" *=> TaskSpec.*/\1/p' langarena/harness/measure.cr)
  fi
  for n in $names; do
    slug=$(echo "$n" | sed 's/::/_/' | tr 'A-Z' 'a-z')
    fa=$work/la-a/raw/$slug.txt; fb=$work/la-b/raw/$slug.txt
    if [ ! -f "$fa" ] || [ ! -f "$fb" ]; then record different "langarena:$n" "no harness result on one side"; continue; fi
    la_view "$fa" > "$work/a/la-$slug.view"; la_view "$fb" > "$work/b/la-$slug.view"
    if cmp -s "$work/a/la-$slug.view" "$work/b/la-$slug.view"; then
      record same "langarena:$n" "$(grep -E '^(actual|ok): ' "$fa" | tr '\n' ' ')"
    else
      { echo "== langarena:$n"; diff -u --label A --label B "$work/a/la-$slug.view" "$work/b/la-$slug.view" | head -60; } > "$out/diff/la-$slug.txt"
      record different "langarena:$n" "$(diff "$work/a/la-$slug.view" "$work/b/la-$slug.view" | grep -c '^[<>]') differing lines"
    fi
  done
}

if [ -n "$explicit" ]; then
  tasks=""
  for x in $explicit; do
    case "$x" in
      *::*) has_set langarena && tasks="$tasks${tasks:+,}$x" ;;
      *) rel=$(realpath -m --relative-to="$root" "$x"); program "$rel" ;;
    esac
  done
  [ -n "$tasks" ] && langarena "$tasks"
else
  if has_set examples; then
    for f in examples/*.av; do program "$f"; done
  fi
  if has_set fixtures; then
    for f in $(find tests/fixtures -name '*.av' | LC_ALL=C sort); do
      if [ -n "$(skip_reason "$f")" ] || grep -q '^fn main' "$f"; then program "$f"; fi
    done
  fi
  if has_set scalars; then
    for f in $(scalar_programs); do program "$f"; done
  fi
  if has_set strings; then
    for f in $(string_programs); do program "$f"; done
  fi
  if has_set objects; then
    for f in $(object_programs); do program "$f"; done
  fi
  if has_set heap; then
    for f in $(heap_programs); do program "$f"; done
  fi
  has_set langarena && langarena ""
fi

t1=$(date +%s)
echo "$same same, $different different, $skipped skipped (wall $((t1 - t0)) s)" | tee "$out/summary.txt"
[ "$different" -eq 0 ]
