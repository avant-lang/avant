#!/bin/sh
# Code placement probe: how much does each LangArena task's time depend on
# where its code lands? From the checkout root laid out for the harness
# (langarena/ copied in, LangArena/ beside the checkout):
#
#   sh tools/placement-probe.sh build OUT [TASK...]
#   sh tools/placement-probe.sh plan OUT [MINUTES]
#   sh tools/placement-probe.sh time OUT SESSION [TASK...]
#   sh tools/placement-probe.sh report OUT
#   sh tools/placement-probe.sh OUT [TASK...]       (all three, one call)
#
# TASK is a harness task name (Etc::Sieve); the default is every task of
# the harness's run.js. The steps:
#
#   build   the harness (measure.cr prod, AVANT_TASK_LIST) compiles and
#           runs each task once with PROBE_COMPILER, through
#           tools/layout-cc.sh, which links every program once more per
#           shift in PROBE_SHIFTS into OUT/layouts/SHIFT/ (a block of SHIFT
#           bytes before the program's object: the program and the runtime
#           start SHIFT bytes later, no instruction changes; shift 0 is the
#           harness's own binary, byte for byte); PROBE_COPIES more copies
#           of shift 0 (OUT/layouts/0cN/, the same bytes in other files)
#           measure the noise at one layout. No timing: a shared run (the
#           harness's one run of each task checks its checksum; the timed
#           runs check every layout's).
#   plan    groups of tasks whose timing fits MINUTES (default 9) each,
#           from the harness's own times (one line per group: the task
#           names, comma-separated): one `time` call per group keeps each
#           timed run short.
#   time    tools/rounds.py's LangArena loop with one side per layout
#           (s0, s64, ..., s0c1, ...): PROBE_ROUNDS rounds (default 3)
#           after one unmeasured warm-up, the layouts' order rotated per
#           round, every run's checksum checked, only undisturbed runs
#           counted, single-threaded tasks pinned to PROBE_PIN (default:
#           the second CPU this process may run on). Timed: run it under
#           `ledger run --timed` (or `--timed --slot`; never compare the
#           two). OUT/sessions/SESSION/.
#   report  tools/layout-spread.py over every session: OUT/spread.tsv and
#           OUT/spread.md, each task's median over the layouts, its spread
#           across them and its noise at one layout, worst first.
#
# Status: 0 when every task's spread is at most PROBE_BAR percent or within
# its noise, 1 when one is over both (named on stderr; report only), 2 on a
# usage error or a red step (a checksum, a link, the governor not
# `performance`).
#
# Environment: PROBE_COMPILER (bin/avant-c-llvm), PROBE_SHIFTS ("0 64 192
# 448 1216 2752": multiples of 64, since every function is 64-byte
# aligned), PROBE_COPIES (2), PROBE_ROUNDS (3), PROBE_PIN, PROBE_BAR
# (2.0). Every child runs under timeout with stdin from /dev/null; nothing
# is written outside OUT except the harness's own langarena/tmp (as every
# harness run).
set -u
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ $# -ge 1 ] || usage
cmd=$1
case "$cmd" in
  build|plan|report) shift ;;
  time) shift; [ $# -ge 2 ] || usage ;;
  -*) usage ;;
  *) cmd=all ;;
esac
out=${1:-}
[ -n "$out" ] || usage
shift
shifts=${PROBE_SHIFTS:-0 64 192 448 1216 2752}
copies=${PROBE_COPIES:-2}
rounds=${PROBE_ROUNDS:-3}
bar=${PROBE_BAR:-2.0}
run_js=$root/../LangArena/run.js
mkdir -p "$out" || exit 2
out=$(realpath "$out")

gov_ok() {
  gov=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ')
  [ "$gov" = "performance " ] || { echo "placement-probe: the governor is not performance: $gov" >&2; exit 2; }
}

# The sides: s<shift> per shift, then s0c<N> per copy.
sides() {
  for s in $shifts; do printf 's%s=%s/layouts/%s\n' "$s" "$out" "$s"; done
  n=1; while [ "$n" -le "$copies" ]; do printf 's0c%d=%s/layouts/0c%d\n' "$n" "$out" "$n"; n=$((n + 1)); done
}

build() {
  [ -f langarena/harness/measure.cr ] || { echo "placement-probe: langarena/harness/measure.cr is missing (copy the harness in)" >&2; exit 2; }
  [ -f "$run_js" ] || { echo "placement-probe: $run_js is missing (LangArena/ beside the checkout)" >&2; exit 2; }
  comp=$(realpath -m "${PROBE_COMPILER:-bin/avant-c-llvm}")
  [ -x "$comp" ] || { echo "placement-probe: compiler $comp is missing" >&2; exit 2; }
  case " $shifts " in *" 0 "*) ;; *) echo "placement-probe: PROBE_SHIFTS must include 0" >&2; exit 2 ;; esac
  rm -rf "$out/la" "$out/layouts"; mkdir -p "$out/la" "$out/layouts"
  list=""
  [ $# -gt 0 ] && { list=$(printf '%s,' "$@"); list=${list%,}; }
  printf 'compiler: %s\nsha256: %s\nshifts: %s\ncopies: %s\ntasks: %s\nhead: %s\ndate: %s\n' "$comp" \
    "$(sha256sum "$comp" | cut -d' ' -f1)" "$shifts" "$copies" "${list:-all}" \
    "$(git -C "$root" rev-parse HEAD 2>/dev/null) tree $(git -C "$root" rev-parse 'HEAD^{tree}' 2>/dev/null)" "$(date -Is)" > "$out/build.txt"
  env CC="$root/tools/layout-cc.sh" LAYOUT_DIR="$out/layouts" LAYOUT_SHIFTS="$shifts" AVANT_COMPILER="$comp" \
    AVANT_CODEGEN=release ${list:+AVANT_TASK_LIST="$list"} AVANT_RESULTS_DIR="$out/la" \
    timeout -k 10 2700 crystal run langarena/harness/measure.cr -- prod < /dev/null > "$out/la/la-prod.log" 2>&1 \
    || { echo "placement-probe: the harness run failed (OUT/la/la-prod.log)" >&2; exit 2; }
  # The harness's own crystal build goes through the wrapper too: every
  # file that is not a task's program is dropped.
  for s in $shifts; do
    for b in "$out/layouts/$s"/*; do [ -f "$out/la/raw/$(basename "$b").txt" ] || rm -f "$b"; done
  done
  n=1; while [ "$n" -le "$copies" ]; do
    mkdir -p "$out/layouts/0c$n"; cp "$out"/layouts/0/* "$out/layouts/0c$n/" || exit 2; n=$((n + 1))
  done
  # The harness ran shift 0 and checked its checksum; every timed run of
  # every layout checks its own (tools/rounds.py), the warm-up first.
  red=0
  for f in "$out"/la/raw/*.txt; do
    slug=$(basename "$f" .txt)
    [ "$(sed -n 's/^checksum_match: //p' "$f")" = true ] || { echo "placement-probe: $slug: the harness's checksum differs" >&2; red=1; }
    for side in $(sides); do
      [ -x "${side#*=}/$slug" ] || { echo "placement-probe: $slug has no ${side%%=*} binary" >&2; red=1; }
    done
  done
  # The shift itself, read from the binaries: main's address per layout.
  for f in "$out"/la/raw/*.txt; do
    slug=$(basename "$f" .txt); printf '%s' "$slug"
    for s in $shifts; do printf '\t%s' "$(nm "$out/layouts/$s/$slug" 2>/dev/null | awk '$3 == "main" { print $1 }')"; done; echo
  done > "$out/main-addresses.tsv"
  [ "$red" -eq 0 ] || exit 2
  echo "placement-probe: built $(ls "$out"/la/raw/*.txt | wc -l) tasks at $(echo $shifts | wc -w) layouts and $copies copies"
}

plan() {
  mins=${1:-9}
  nsides=$(sides | wc -l)
  awk -F'\t' -v lim="$((mins * 60))" -v k="$((nsides * (rounds + 1)))" '
    NR > 1 && $3 == "true" { t = $8 * k + 0.05 * k
      if (acc > 0 && acc + t > lim) { print g; g = ""; acc = 0 }
      g = g (g == "" ? "" : ",") $1; acc += t }
    END { if (g != "") print g }' "$out/la/runtime.tsv"
}

timing() {
  session=$1; shift
  gov_ok
  [ -d "$out/layouts/0" ] || { echo "placement-probe: run build first" >&2; exit 2; }
  mkdir -p "$out/sessions"
  pin=${PROBE_PIN:-$(python3 -I -c 'import os; c = sorted(os.sched_getaffinity(0)); print(c[1] if len(c) > 1 else c[0])')}
  set -- --rounds "$rounds" --run-js "$run_js" --out "$out/sessions/$session" --pin "$pin" $(sides | sed 's/^/--side /') "$@"
  timeout -k 10 3600 python3 -I "$root/tools/rounds.py" langarena "$@" < /dev/null > "$out/sessions/$session.log" 2>&1 \
    || { echo "placement-probe: the rounds failed (OUT/sessions/$session.log)" >&2; tail -3 "$out/sessions/$session.log" >&2; exit 2; }
  echo "placement-probe: session $session done (OUT/sessions/$session/rounds.md)"
}

report() {
  [ -d "$out/sessions" ] || { echo "placement-probe: no session yet" >&2; exit 2; }
  python3 -I "$root/tools/layout-spread.py" spread --bar "$bar" --out "$out" "$out"/sessions/*/ < /dev/null
}

case "$cmd" in
  build) build "$@" ;;
  plan) plan "$@" ;;
  time) timing "$@" ;;
  report) report ;;
  all) build "$@" && timing all "$@" && report ;;
esac
