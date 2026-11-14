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
#   GOLDEN_HARNESS_FRESH=1  build the LangArena harness into the work
#                    directory for this run instead of reusing the cached one
# --bless writes each program's result as its golden instead of comparing
# (a program that fails to compile is reported and gets no golden).
#
# Sets (comma list; default all of them, each program once):
#   examples   every examples/*.av except examples/http.av, with the flags
#              the suite uses (add, layout: --cc examples/c/*.c; zlib: --lib z)
#   fixtures   every tests/fixtures/**/*.av that has `fn main`, except
#              tests/fixtures/format/ (formatter inputs, not programs)
#   langarena  the 50 LangArena tasks in `test` mode through the harness
#              (langarena/harness/measure.cr: the checkout's cached binary,
#              tools/la-harness.sh, rebuilt only when its source changed);
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
# its prod runs, writes its stitched sources and binaries to langarena/tmp/
# (one file set per task, named by the task).
#
# Parallel (Stage 28 W16t): GOLDEN_JOBS programs at once (default 16, chosen
# by measurement on the 16-thread, 27 GB machine: the full AVANT_GC_STRESS
# mode's processes stay far inside memory; 1 is the sequential order). Each
# program has its own work directory (GOLDEN_OUT/work/p/N: binary, run
# directory, outputs, TMPDIR); each LangArena task is its own harness
# process (AVANT_TASK_LIST of one task, its own results directory), after
# the harness binary is ready (the cached one, or its build, starts first). A program that names
# a fixed port (serial_reason below) runs under one lock, never beside
# another such program. Results, the report and the summary are written in
# the sequential order, by one aggregation pass: the same files as the
# sequential runner, plus GOLDEN_OUT/times.txt (each program's compile and
# run seconds) and langarena.log as the tasks' harness logs in order.
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
JOBS=${GOLDEN_JOBS:-16}
case "$JOBS" in
  ''|*[!0-9]*|0) echo "run-goldens: GOLDEN_JOBS must be a positive integer" >&2; exit 2 ;;
esac
rm -rf "$work"
mkdir -p "$work/p" "$work/tmp" "$out/diff" || exit 2
TMPDIR=$work/tmp
export TMPDIR
cleanup() { [ "${GOLDEN_KEEP:-0}" = 1 ] || rm -rf "$work"; }
trap cleanup EXIT
trap 'exit 130' INT TERM
: > "$out/results.txt"
: > "$out/differences.txt"
: > "$out/times.txt"
: > "$work/seen"
: > "$work/queue"
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
# A job's result, for the aggregation pass to record in order.
note() { printf '%s\n%s\n%s\n' "$1" "$2" "${3:-}" > "$d/rec"; }

skip_reason() { # PATH -> prints a reason when the program is excluded
  case "$1" in
    examples/http.av) echo "opens a loopback listener and connects to it (no network ports here)" ;;
    tests/fixtures/format/*) echo "formatter input, not a program" ;;
    tests/fixtures/ffi/ceiling.av) echo "size-ceiling probes, run per mode by tests/cases/test_ceiling.av (every mode traps)" ;;
  esac
}
# A program that opens or connects to a network port (a listener, a
# client, or a fixed port such as the bench's 18301 and 18311) never runs
# beside another one (Stage 28 W16t). At W16t only examples/http.av
# matches, and it is skipped above.
serial_reason() { # PATH -> prints a reason when the program runs under the port lock
  grep -qE 'Http\.(listen|get|post)|Ws\.connect|\b183[0-9][0-9]\b' "$root/$1" 2>/dev/null && echo "uses a network port"
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
now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", b - a }'; }

# The queue: one item per line in the sequential order, `prog PATH` or
# `task NAME`; a program is queued once (the first set naming it).
program() { # PATH relative to root
  grep -qxF "$1" "$work/seen" && return
  echo "$1" >> "$work/seen"
  echo "prog $1" >> "$work/queue"
}
langarena() { # TASKS (comma list or empty)
  if [ -n "$1" ]; then
    echo "$1" | tr ',' '\n'
  else
    sed -n 's/^  "\([A-Za-z0-9]*::[A-Za-z0-9]*\)" *=> TaskSpec.*/\1/p' langarena/harness/measure.cr
  fi | while IFS= read -r n; do [ -n "$n" ] && echo "task $n" >> "$work/queue"; done
}

# prog_job PATH, in its own directory d: compile, run, and (unless
# blessing) compare; the outcome is d/rec, the outputs d/view, d/err, d/rrc.
prog_job() {
  p=$1
  r=$(skip_reason "$p")
  if [ -n "$r" ]; then note skip "$p" "$r"; return; fi
  [ -f "$root/$p" ] || { note different "$p" "no such file"; return; }
  k=$(key_of "$p")
  b=$d/prog
  lock=""
  [ -n "$(serial_reason "$p")" ] && lock=$work/port.lock
  if [ -n "$lock" ]; then
    until mkdir "$lock" 2>/dev/null; do sleep 1; done
  fi
  c0=$(now)
  # shellcheck disable=SC2046,SC2086 # ENVW and flags are words
  env -u AVANT_BACKEND TMPDIR="$d/tmp" $ENVW timeout -k 10 120 "$COMPILER" compile "$root/$p" "$b" $MODE $(flags_of "$p") > "$d/clog" 2>&1 < /dev/null
  crc=$?
  c1=$(now)
  if [ $crc -ne 0 ] || [ ! -x "$b" ]; then
    [ -n "$lock" ] && rmdir "$lock"
    echo "compile=$(secs "$c0" "$c1") run=-" > "$d/time"
    { echo "== $p compile rc=$crc"; tail -20 "$d/clog"; } > "$out/diff/$(echo "$k" | tr / _).txt"
    note different "$p" "compile failed: rc=$crc"
    return
  fi
  mkdir "$d/run"
  mv "$b" "$d/run/prog"
  # shellcheck disable=SC2086
  (cd "$d/run" && exec env -u AVANT_BACKEND TMPDIR="$d/tmp" $ENVW timeout -k 10 120 ./prog > "$d/out" 2> "$d/err" < /dev/null)
  rrc=$?
  r1=$(now)
  [ -n "$lock" ] && rmdir "$lock"
  rm -rf "$d/run"
  echo "compile=$(secs "$c0" "$c1") run=$(secs "$c1" "$r1")" > "$d/time"
  view "$p" "$d/out" > "$d/view"
  echo "$rrc" > "$d/rrc"
  if [ $bless -eq 1 ]; then
    note blessed "$p" "exit=$rrc"
    return
  fi
  want=$(status_of "$p")
  if [ -z "$want" ] || [ ! -f "$G/$k.out" ]; then note different "$p" "no golden"; return; fi
  if [ -f "$G/$k.err" ]; then cp "$G/$k.err" "$d/want-err"; else : > "$d/want-err"; fi
  why=""
  [ "$rrc" = "$want" ] || why="exit $rrc, golden $want"
  cmp -s "$d/view" "$G/$k.out" || why="$why${why:+, }stdout"
  cmp -s "$d/err" "$d/want-err" || why="$why${why:+, }stderr"
  if [ -z "$why" ]; then
    note same "$p" "exit=$rrc"
  else
    { echo "== $p: $why"; diff -u --label golden --label got "$G/$k.out" "$d/view" | head -40; diff -u --label golden --label got "$d/want-err" "$d/err" | head -40; } > "$out/diff/$(echo "$k" | tr / _).txt"
    note different "$p" "$why"
  fi
}

# The harness (the first job when a task is queued): the checkout's cached
# binary from tools/la-harness.sh, built only when langarena/harness/measure.cr
# changed (O115); GOLDEN_HARNESS_FRESH=1 builds a fresh one into the work
# directory instead, as every run did before.
build_job() {
  if [ "${GOLDEN_HARNESS_FRESH:-0}" = 1 ]; then
    if timeout -k 10 900 crystal build --no-debug -o "$work/measure" langarena/harness/measure.cr > "$out/harness-build.log" 2>&1 < /dev/null; then
      echo "$work/measure" > "$work/measure.path"; echo ok > "$work/measure.done"
    else
      echo failed > "$work/measure.done"
    fi
  elif sh tools/la-harness.sh > "$work/measure.path" 2> "$out/harness-build.log" < /dev/null; then
    echo ok > "$work/measure.done"
  else
    echo failed > "$work/measure.done"
  fi
  echo "harness: $(cat "$work/measure.path" 2>/dev/null)$([ "${GOLDEN_HARNESS_FRESH:-0}" = 1 ] && echo " (fresh, GOLDEN_HARNESS_FRESH=1)")" >> "$out/harness-build.log"
}

# task_job NAME: one task through the harness in `test` mode, its own
# results directory d/la; waits for the harness build.
task_job() {
  until [ -f "$work/measure.done" ]; do sleep 1; done
  [ "$(cat "$work/measure.done")" = ok ] || return
  t0j=$(now)
  # shellcheck disable=SC2086
  env -u AVANT_SKIP_OK -u AVANT_PROBE_SCALE -u AVANT_TASK_LIST -u AVANT_BACKEND TMPDIR="$d/tmp" $ENVW AVANT_COMPILER="$COMPILER" AVANT_CODEGEN=$codegen \
    AVANT_RESULTS_DIR="$d/la" AVANT_TASK_LIST="$1" AVANT_TASK_TIMEOUT_S=120 AVANT_COMPILE_TIMEOUT_S=120 \
    timeout -k 10 2700 "$(cat "$work/measure.path")" test > "$d/la.log" 2>&1 < /dev/null
  echo "harness rc=$?" >> "$d/la.log"
  echo "total=$(secs "$t0j" "$(now)")" > "$d/time"
}

la_view() { # RAWFILE -> the compared fields and the program's output
  grep -E '^(actual|ok|compile_ok|timed_out|exit_code): ' "$1"
  sed -n '/^--- stdout ---$/,$p' "$1"
}

# Aggregation of item N (in queue order): its record, and for a blessed
# program the golden files; for a task, the comparison of its raw result.
aggregate() { # N KIND ARG
  d=$work/p/$1
  if [ "$2" = prog ]; then
    if [ ! -f "$d/rec" ]; then record different "$3" "job did not finish"; return; fi
    { read -r st; read -r nm; read -r dt; } < "$d/rec"
    [ -f "$d/time" ] && echo "$3 $(cat "$d/time")" >> "$out/times.txt"
    if [ "$st" = blessed ]; then
      k=$(key_of "$3")
      mkdir -p "$(dirname "$G/$k")"
      cp "$d/view" "$G/$k.out"
      if [ -s "$d/err" ]; then cp "$d/err" "$G/$k.err"; else rm -f "$G/$k.err"; fi
      set_status "$3" "$(cat "$d/rrc")"
    fi
    record "$st" "$nm" "$dt"
    return
  fi
  tn=$3
  if [ "$(cat "$work/measure.done" 2>/dev/null)" != ok ]; then
    echo "run-goldens: cannot build the LangArena harness (see $out/harness-build.log)" >&2; exit 2
  fi
  [ -f "$d/la.log" ] && cat "$d/la.log" >> "$out/langarena.log"
  [ -f "$d/time" ] && echo "langarena:$tn $(cat "$d/time")" >> "$out/times.txt"
  slug=$(echo "$tn" | sed 's/::/_/' | tr 'A-Z' 'a-z')
  f=$d/la/raw/$slug.txt
  if [ ! -f "$f" ]; then record different "langarena:$tn" "no harness result"; return; fi
  la_view "$f" > "$d/la-$slug.view"
  if [ $bless -eq 1 ]; then
    cp "$d/la-$slug.view" "$G/langarena/$slug.txt"
    record blessed "langarena:$tn" "$(grep -E '^(actual|ok): ' "$f" | tr '\n' ' ')"
  elif [ ! -f "$G/langarena/$slug.txt" ]; then
    record different "langarena:$tn" "no golden"
  elif cmp -s "$G/langarena/$slug.txt" "$d/la-$slug.view"; then
    record same "langarena:$tn" "$(grep -E '^(actual|ok): ' "$f" | tr '\n' ' ')"
  else
    { echo "== langarena:$tn"; diff -u --label golden --label got "$G/langarena/$slug.txt" "$d/la-$slug.view" | head -60; } > "$out/diff/la-$slug.txt"
    record different "langarena:$tn" "$(diff "$G/langarena/$slug.txt" "$d/la-$slug.view" | grep -c '^[<>]') differing lines"
  fi
}

if [ -n "$explicit" ]; then
  tasks=""
  for x in $explicit; do
    case "$x" in
      *::*) tasks="$tasks${tasks:+,}$x" ;;
      *) program "$(realpath -m --relative-to="$root" "$x")" ;;
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

# The pool: JOBS tokens in a FIFO; each job takes one and gives it back
# when it ends. The aggregation follows the queue order as items finish.
count=$(wc -l < "$work/queue")
mkfifo "$work/pool" || exit 2
exec 3<> "$work/pool"
rm -f "$work/pool"
i=0
while [ $i -lt "$JOBS" ]; do echo >&3; i=$((i + 1)); done
next=1
flush() {
  while [ $next -le "$count" ] && [ -f "$work/p/$next/done" ]; do
    aggregate $next $(sed -n "${next}p" "$work/queue")
    next=$((next + 1))
  done
}
if grep -q '^task ' "$work/queue"; then
  read -r _ <&3
  ( (build_job); echo >&3 ) < /dev/null &
fi
n=0
while IFS= read -r line; do
  n=$((n + 1))
  read -r _ <&3
  mkdir -p "$work/p/$n/tmp"
  (
    d=$work/p/$n
    case "$line" in
      prog\ *) (prog_job "${line#prog }") ;;
      task\ *) (task_job "${line#task }") ;;
    esac
    : > "$d/done"
    echo >&3
  ) < /dev/null &
  flush
done < "$work/queue"
wait
for i in $(seq "$next" "$count"); do : > "$work/p/$i/done"; done
flush
exec 3>&-

t1=$(date +%s)
if [ $bless -eq 1 ]; then
  echo "$same blessed, $different failed, $skipped skipped (wall $((t1 - t0)) s)" | tee "$out/summary.txt"
else
  echo "$same same, $different different, $skipped skipped (wall $((t1 - t0)) s)" | tee "$out/summary.txt"
fi
[ "$different" -eq 0 ]
