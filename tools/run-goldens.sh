#!/bin/sh
# The run goldens (Stage 28 W11): builds and runs a set of programs with one
# compiler and compares what each observably does (stdout, stderr, exit
# status) with its golden in tests/goldens/runs/. It replaces the
# differential oracle tools/backend-diff.sh (D176) when the old back end
# retired: the oracle's every program was frozen as a golden from the old
# back end (W11 item 1), so what the old back end checked as a second
# implementation is still checked.
#
#   sh tools/run-goldens.sh [--bless] [--set SETS] [PROGRAM...]
#
# Settings (environment):
#   GOLDEN_COMPILER  the compiler (default bin/avant-c-llvm, the gate's C')
#   GOLDEN_ENV       VAR=value words applied to the compile and the run
#                    (default none); any AVANT_BACKEND of the caller's
#                    environment is removed first
#   GOLDEN_MODE      compile flag: --debug (default), --release, or "" (bare)
#   GOLDEN_OUT       report directory (default results/run-goldens-YYYYmmdd-HHMMSS)
#   GOLDEN_KEEP=1    keep the work directory (binaries, outputs) in GOLDEN_OUT/work
# --bless writes each program's result as its golden instead of comparing
# (a program that fails to compile is reported and gets no golden).
#
# Sets (comma list; default all of them, each program once):
#   examples   every examples/*.av except examples/http.av, with the flags
#              the suite uses (add, layout: --cc examples/c/*.c; zlib: --lib z)
#   fixtures   every tests/fixtures/**/*.av that has `fn main`, except
#              tests/fixtures/format/ (formatter inputs, not programs)
#   langarena  the 50 LangArena tasks in `test` mode through the harness
#              (langarena/harness/measure.cr, built once into the work dir);
#              compared per task: the checksum, ok, exit status,
#              compile/timeout flags, the program's stdout and stderr (the
#              harness's wall/RSS/compile-time lines are not compared)
#   scalars, strings, objects, heap, types, closures, ffi, threads
#              the back-end subsets of Stage 28 (W5b to W8b; the lists
#              below): mostly fixtures and examples already above, plus the
#              tests/fixtures/scalars/*.av programs with `fn run` only
# PROGRAM arguments select a subset instead of the sets: a path to a .av
# file or a LangArena task name such as Etc::Sieve.
#
# Goldens (tests/goldens/runs/; a program's key is its path without the
# leading tests/ and without .av):
#   KEY.out         stdout (examples/matmul.av: lines 1-2 only, since lines
#                   3-4 are the milliseconds its two passes took;
#                   examples/zlib.av: a version line reads `<zlib version>`,
#                   since it prints the system zlib's version, 1.3.1 here
#                   and 1.3 on the CI runner)
#   KEY.err         stderr, present only when it is not empty
#   status.txt      one line per program: its path and exit status, sorted
#   langarena/SLUG.txt  the compared view of a LangArena task's raw result
# Where they came from: every program the oracle compared was blessed from
# the old back end (AVANT_BACKEND=myc, --debug) on 2026-11-06 and equals the
# new back end's output in --debug, bare and --release; the five expected
# differences of D196 (the old back end could not compile or run them
# correctly: types/coerce.av, ffi/byval.av, ffi/fresh.av, closures/fresh.av,
# threads/fresh.av) were blessed from the new back end, and the suite
# checks their outputs by hand as before.
#
# Per program: compile and run under `timeout -k 10 120` with stdin from
# /dev/null; the run happens in an empty directory as ./prog. A program that
# fails to compile is a difference. Every child runs with TMPDIR inside the
# work directory, so nothing is written to /tmp; the LangArena harness, as in
# its prod runs, writes its stitched sources and binaries to langarena/tmp/.
#
# Output: one line per difference on stdout and in GOLDEN_OUT/differences.txt
# (diffs in GOLDEN_OUT/diff/), every program's result in
# GOLDEN_OUT/results.txt, and the summary `N same, M different, K skipped`
# (with --bless: `N blessed, M failed`). Exit 0 when M = 0, 1 when M > 0, 2
# on a usage or tool error.
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2

all_sets="examples,fixtures,scalars,strings,objects,heap,types,closures,ffi,threads,langarena"
sets=$all_sets
explicit=""
bless=0
while [ $# -gt 0 ]; do
  case "$1" in
    --bless) bless=1; shift ;;
    --set) [ $# -ge 2 ] || { echo "run-goldens: --set needs a value" >&2; exit 2; }; sets=$2; shift 2 ;;
    --set=*) sets=${1#--set=}; shift ;;
    -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) echo "run-goldens: unknown option $1" >&2; exit 2 ;;
    *) explicit="$explicit $1"; shift ;;
  esac
done
for a in "$@"; do explicit="$explicit $a"; done
has_set() { case ",$sets," in *",$1,"*) return 0 ;; esac; return 1; }

COMPILER=$(realpath -m "${GOLDEN_COMPILER:-bin/avant-c-llvm}")
ENVW=${GOLDEN_ENV:-}
MODE=${GOLDEN_MODE---debug}
case "$MODE" in
  --debug) codegen=debug ;;
  --release) codegen=release ;;
  '') codegen=default ;;
  *) echo "run-goldens: GOLDEN_MODE must be --debug, --release, or empty" >&2; exit 2 ;;
esac
[ -x "$COMPILER" ] || { echo "run-goldens: no compiler at $COMPILER" >&2; exit 2; }
G=$root/tests/goldens/runs
mkdir -p "$G/langarena" || exit 2
[ -f "$G/status.txt" ] || : > "$G/status.txt"

out=$(realpath -m "${GOLDEN_OUT:-results/run-goldens-$(date +%Y%m%d-%H%M%S)}")
work=$out/work
rm -rf "$work"
mkdir -p "$work/bin" "$work/run" "$work/tmp" "$out/diff" || exit 2
TMPDIR=$work/tmp
export TMPDIR
cleanup() { [ "${GOLDEN_KEEP:-0}" = 1 ] || rm -rf "$work"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
: > "$out/results.txt"
: > "$out/differences.txt"
: > "$work/seen"
t0=$(date +%s)
same=0; different=0; skipped=0

{
  echo "compiler: $COMPILER env: ${ENVW:-(none)}"
  echo "mode: ${MODE:-bare} sets: $sets programs:${explicit:- (all)} bless: $bless"
} | tee "$out/header.txt"

record() { # STATUS NAME DETAIL
  echo "$1 $2${3:+ $3}" >> "$out/results.txt"
  case "$1" in
    same|blessed) same=$((same + 1)) ;;
    skip) skipped=$((skipped + 1)); echo "skip $2: $3" ;;
    *) different=$((different + 1)); echo "DIFFERENT $2: $3" | tee -a "$out/differences.txt" ;;
  esac
}

skip_reason() { # PATH -> prints a reason when the program is excluded
  case "$1" in
    examples/http.av) echo "opens a loopback listener and connects to it (no network ports here)" ;;
    tests/fixtures/format/*) echo "formatter input, not a program" ;;
  esac
}
flags_of() { # PATH -> the extra compile flags the suite uses
  case "$1" in
    examples/add.av) echo "--cc $root/examples/c/add.c" ;;
    examples/layout.av) echo "--cc $root/examples/c/layout.c" ;;
    examples/zlib.av) echo "--lib z" ;;
    tests/fixtures/ffi/*) echo "--cc $root/tests/fixtures/ffi/ffi.c" ;;
  esac
}
scalar_programs() {
  for f in tests/fixtures/scalars/*.av; do echo "$f"; done
  for f in examples/hello.av examples/fact.av examples/switch.av tests/fixtures/macros/hygiene.av tests/fixtures/macros/quote_fn.av \
    tests/fixtures/macros/splice_name.av tests/fixtures/overloading/defaults.av tests/fixtures/part3/conv.av tests/fixtures/part3/run.av; do echo "$f"; done
}
string_programs() {
  for f in tests/fixtures/strings/*.av; do echo "$f"; done
  for f in tests/fixtures/overloading/overload.av tests/fixtures/part3/chr.av tests/fixtures/part3/index.av tests/fixtures/part3/interp.av tests/fixtures/part3/slice.av; do echo "$f"; done
}
mains() { for f in "$@"; do grep -q '^fn main' "$f" && echo "$f"; done; }
object_programs() {
  mains tests/fixtures/objects/*.av
  for f in examples/binarytrees.av examples/defaults.av examples/methods.av examples/trees.av examples/vec2.av tests/fixtures/macros/fields.av tests/fixtures/macros/method.av; do echo "$f"; done
}
heap_programs() {
  mains tests/fixtures/heap/*.av
  for f in examples/sieve.av tests/fixtures/modules/main.av tests/fixtures/part3/buf.av tests/fixtures/part3/pop.av; do echo "$f"; done
}
type_programs() {
  mains tests/fixtures/types/*.av
  for f in examples/errors.av tests/fixtures/overloading/generic.av tests/fixtures/overloading/operator.av tests/fixtures/part3/argv.av tests/fixtures/part3/env.av tests/fixtures/part3/exists.av; do echo "$f"; done
}
closure_programs() { mains tests/fixtures/closures/*.av; echo examples/collect.av; }
ffi_programs() { mains tests/fixtures/ffi/*.av; for x in add digest json layout sqrt zlib; do echo "examples/$x.av"; done; }
thread_programs() { mains tests/fixtures/threads/*.av tests/fixtures/coverage/*.av; echo examples/spawn.av; echo examples/matmul.av; }
view() { # PATH FILE -> the compared view of a stdout file
  case "$1" in
    examples/matmul.av) head -n 2 "$2" ;;
    examples/zlib.av) sed -E 's/^[0-9]+([.][0-9]+)+$/<zlib version>/' "$2" ;;
    *) cat "$2" ;;
  esac
}
key_of() { p=${1#tests/}; echo "${p%.av}"; }
status_of() { awk -v p="$1" '$1 == p { print $2 }' "$G/status.txt"; }
set_status() { # PATH RC
  { awk -v p="$1" '$1 != p' "$G/status.txt"; echo "$1 $2"; } | LC_ALL=C sort > "$work/status.new" && mv "$work/status.new" "$G/status.txt"
}

program() { # PATH relative to root
  p=$1
  grep -qxF "$p" "$work/seen" && return
  echo "$p" >> "$work/seen"
  r=$(skip_reason "$p")
  if [ -n "$r" ]; then record skip "$p" "$r"; return; fi
  [ -f "$root/$p" ] || { record different "$p" "no such file"; return; }
  k=$(key_of "$p")
  b=$work/bin/prog
  rm -f "$b"
  # shellcheck disable=SC2046,SC2086 # ENVW and flags are words
  env -u AVANT_BACKEND $ENVW timeout -k 10 120 "$COMPILER" compile "$root/$p" "$b" $MODE $(flags_of "$p") > "$work/clog" 2>&1 < /dev/null
  crc=$?
  if [ $crc -ne 0 ] || [ ! -x "$b" ]; then
    { echo "== $p compile rc=$crc"; tail -20 "$work/clog"; } > "$out/diff/$(echo "$k" | tr / _).txt"
    record different "$p" "compile failed: rc=$crc"
    return
  fi
  rm -rf "$work/run"; mkdir "$work/run"
  cp "$b" "$work/run/prog"
  # shellcheck disable=SC2086
  (cd "$work/run" && exec env -u AVANT_BACKEND $ENVW timeout -k 10 120 ./prog > "$work/out" 2> "$work/err" < /dev/null)
  rrc=$?
  rm -rf "$work/run"; mkdir "$work/run"
  view "$p" "$work/out" > "$work/view"
  if [ $bless -eq 1 ]; then
    mkdir -p "$(dirname "$G/$k")"
    cp "$work/view" "$G/$k.out"
    if [ -s "$work/err" ]; then cp "$work/err" "$G/$k.err"; else rm -f "$G/$k.err"; fi
    set_status "$p" "$rrc"
    record blessed "$p" "exit=$rrc"
    return
  fi
  want=$(status_of "$p")
  if [ -z "$want" ] || [ ! -f "$G/$k.out" ]; then record different "$p" "no golden"; return; fi
  if [ -f "$G/$k.err" ]; then cp "$G/$k.err" "$work/want-err"; else : > "$work/want-err"; fi
  why=""
  [ "$rrc" = "$want" ] || why="exit $rrc, golden $want"
  cmp -s "$work/view" "$G/$k.out" || why="$why${why:+, }stdout"
  cmp -s "$work/err" "$work/want-err" || why="$why${why:+, }stderr"
  if [ -z "$why" ]; then
    record same "$p" "exit=$rrc"
  else
    { echo "== $p: $why"; diff -u --label golden --label got "$G/$k.out" "$work/view" | head -40; diff -u --label golden --label got "$work/want-err" "$work/err" | head -40; } > "$out/diff/$(echo "$k" | tr / _).txt"
    record different "$p" "$why"
  fi
}

la_view() { # RAWFILE -> the compared fields and the program's output
  grep -E '^(actual|ok|compile_ok|timed_out|exit_code): ' "$1"
  sed -n '/^--- stdout ---$/,$p' "$1"
}
langarena() { # TASKS (comma list or empty)
  if [ ! -x "$work/measure" ]; then
    timeout -k 10 900 crystal build --no-debug -o "$work/measure" langarena/harness/measure.cr > "$out/harness-build.log" 2>&1 < /dev/null || {
      echo "run-goldens: cannot build the LangArena harness (see $out/harness-build.log)" >&2; exit 2; }
  fi
  rm -rf "$work/la"
  list=""
  [ -n "$1" ] && list="AVANT_TASK_LIST=$1"  # the harness reads an empty list as "no task"
  # shellcheck disable=SC2086
  env -u AVANT_SKIP_OK -u AVANT_PROBE_SCALE -u AVANT_TASK_LIST -u AVANT_BACKEND $ENVW AVANT_COMPILER="$COMPILER" AVANT_CODEGEN=$codegen \
    AVANT_RESULTS_DIR="$work/la" $list AVANT_TASK_TIMEOUT_S=120 AVANT_COMPILE_TIMEOUT_S=120 \
    timeout -k 10 2700 "$work/measure" test > "$out/langarena.log" 2>&1 < /dev/null
  echo "harness rc=$?" >> "$out/langarena.log"
  if [ -n "$1" ]; then
    names=$(echo "$1" | tr ',' '\n')
  else
    names=$(sed -n 's/^  "\([A-Za-z0-9]*::[A-Za-z0-9]*\)" *=> TaskSpec.*/\1/p' langarena/harness/measure.cr)
  fi
  for n in $names; do
    slug=$(echo "$n" | sed 's/::/_/' | tr 'A-Z' 'a-z')
    f=$work/la/raw/$slug.txt
    if [ ! -f "$f" ]; then record different "langarena:$n" "no harness result"; continue; fi
    la_view "$f" > "$work/la-$slug.view"
    if [ $bless -eq 1 ]; then
      cp "$work/la-$slug.view" "$G/langarena/$slug.txt"
      record blessed "langarena:$n" "$(grep -E '^(actual|ok): ' "$f" | tr '\n' ' ')"
    elif [ ! -f "$G/langarena/$slug.txt" ]; then
      record different "langarena:$n" "no golden"
    elif cmp -s "$G/langarena/$slug.txt" "$work/la-$slug.view"; then
      record same "langarena:$n" "$(grep -E '^(actual|ok): ' "$f" | tr '\n' ' ')"
    else
      { echo "== langarena:$n"; diff -u --label golden --label got "$G/langarena/$slug.txt" "$work/la-$slug.view" | head -60; } > "$out/diff/la-$slug.txt"
      record different "langarena:$n" "$(diff "$G/langarena/$slug.txt" "$work/la-$slug.view" | grep -c '^[<>]') differing lines"
    fi
  done
}

if [ -n "$explicit" ]; then
  tasks=""
  for x in $explicit; do
    case "$x" in
      *::*) tasks="$tasks${tasks:+,}$x" ;;
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
  has_set scalars && for f in $(scalar_programs); do program "$f"; done
  has_set strings && for f in $(string_programs); do program "$f"; done
  has_set objects && for f in $(object_programs); do program "$f"; done
  has_set heap && for f in $(heap_programs); do program "$f"; done
  has_set types && for f in $(type_programs); do program "$f"; done
  has_set closures && for f in $(closure_programs); do program "$f"; done
  has_set ffi && for f in $(ffi_programs); do program "$f"; done
  has_set threads && for f in $(thread_programs); do program "$f"; done
  has_set langarena && langarena ""
fi

t1=$(date +%s)
if [ $bless -eq 1 ]; then
  echo "$same blessed, $different failed, $skipped skipped (wall $((t1 - t0)) s)" | tee "$out/summary.txt"
else
  echo "$same same, $different different, $skipped skipped (wall $((t1 - t0)) s)" | tee "$out/summary.txt"
fi
[ "$different" -eq 0 ]
