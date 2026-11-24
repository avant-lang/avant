#!/bin/sh
# la-modes.sh — every LangArena program under every collector mode.
#
#   sh tools/la-modes.sh OUT [SLUG...]      (from the checkout root)
#
# OUT is a new directory. The programs are stitched by this checkout's
# harness at a small size (probe: the prod configurations with the work
# scaled by 0.1, the harness's AVANT_PROBE_SCALE default; LA_MODES_SIZE=test
# takes the test configurations, too small for most programs to collect),
# compiled by C' with the arguments the harness gives, then each program is
# run plain and under each collector mode, with hash seed 0, and every
# output is compared with the plain run's: the whole-suite check a change
# to the runtime, the collector or the code generator runs besides its test
# suite's modes, over programs the suite does not hold (the 50 tasks and
# the 14 pure twins).
#
# The modes, in order (LA_MODES, words separated by blanks; `plain` is no
# environment): the plain run, AVANT_GC_STRESS=1 (a collection at every
# allocation), AVANT_GC_VERIFY=1, AVANT_NURSERY=0 and AVANT_YOUNG_MB=1.
# A run is correct when it exits 0 and prints what the plain run printed
# (md5 of stdout); a program whose plain run fails is reported with the
# plain run's status and not compared further.
#
#   OUT/p/                  the stitched programs (p/args/SLUG: the
#                           compile arguments; p/avant/langarena/tmp/SLUG.av)
#   OUT/bin/SLUG(.log)      the compiled programs and the compiler's output
#   OUT/compile.txt         SLUG rc=STATUS, one line per program
#   OUT/runs.txt            SLUG MODE rc=STATUS MD5 SECONDS KB, one line
#                           per run (stdout and stderr in OUT/run/)
#   OUT/summary.txt         the runs and bad ones counted, each bad run,
#                           the slowest of each mode
#
# The last line on stdout is the verdict, `la-modes: N runs, M bad` (the
# bad ones named in summary.txt). Status 0 when every run is correct, 1
# otherwise, 2 on a usage error or when the stitching or a compile failed.
#
# Environment:
#   LA_MODES_ROOT    the checkout whose bin/avant-c-llvm compiles (default
#                    this one)
#   LA_MODES_SIZE    probe (default) or test
#   LA_MODES         the modes (default: plain AVANT_GC_STRESS=1
#                    AVANT_GC_VERIFY=1 AVANT_NURSERY=0 AVANT_YOUNG_MB=1)
#   LA_MODES_LIMIT   seconds a compile or a run may take (default 600)
#   LA_MODES_JOBS    runs at once (default: half the CPUs, at least 1);
#                    compiles at once are a half of it
#   LA_MODES_RUNNER  the runner's command words (such as `python3 -I
#                    PATH/ledger run --shared --`, ending where the command
#                    begins): the script then does each piece of its work
#                    through it, as one run of its own (stitch, compile,
#                    one per mode), so none holds the machine lock past its
#                    limit (about 25 minutes in all here, a mode 2 to 10)
#
# The programs run side by side but each at its own pace; nothing here
# times a program for comparison (`/usr/bin/time` records the seconds and
# the peak only to name the slowest). Stdin from /dev/null.
set -u
step=""
if [ "${1:-}" = --step ]; then step=${2:-}; shift 2; fi
[ $# -ge 1 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
out=$(realpath -m "$1"); shift
slugs="$*"
cc_root=$(realpath "${LA_MODES_ROOT:-$root}") || exit 2
size=${LA_MODES_SIZE:-probe}
case "$size" in probe|test) ;; *) echo "la-modes.sh: LA_MODES_SIZE is probe or test, not '$size'" >&2; exit 2 ;; esac
modes=${LA_MODES:-plain AVANT_GC_STRESS=1 AVANT_GC_VERIFY=1 AVANT_NURSERY=0 AVANT_YOUNG_MB=1}
limit=${LA_MODES_LIMIT:-600}
case "$limit" in ''|*[!0-9]*) echo "la-modes.sh: LA_MODES_LIMIT is a number of seconds" >&2; exit 2 ;; esac
ncpu=$(nproc 2>/dev/null || echo 2)
jobs=${LA_MODES_JOBS:-$((ncpu / 2))}
case "$jobs" in ''|*[!0-9]*|0) jobs=1 ;; esac
cjobs=$((jobs / 2)); [ "$cjobs" -ge 1 ] || cjobs=1
runner=${LA_MODES_RUNNER:-}
p=$out/p
case " $modes " in " plain "*) ;; *) echo "la-modes.sh: LA_MODES must start with plain (every run is compared with it)" >&2; exit 2 ;; esac

# wait_below N: block until fewer than N background jobs of this shell run.
wait_below() { while [ "$(jobs -p | wc -l)" -ge "$1" ]; do sleep 0.2; done; }

# stitch: the harness's own stitching with a compiler that only records its
# arguments, as tools/base-cache.sh does for its prod pass.
do_stitch() {
  rm -rf "$p"; mkdir -p "$p/avant/tools" "$p/args" "$p/stitch" "$p/avant/langarena" || exit 2
  [ -f "$root/langarena/harness/measure.cr" ] || { echo "la-modes.sh: no langarena/ harness in $root" >&2; exit 2; }
  [ -f "$root/../LangArena/run.js" ] || { echo "la-modes.sh: no LangArena/run.js beside $root" >&2; exit 2; }
  (cd "$root/langarena" && tar --exclude=./tmp --exclude=./.git -cf - .) | tar -xf - -C "$p/avant/langarena" || exit 2
  cp "$here/la-harness.sh" "$p/avant/tools/la-harness.sh"
  ln -s "$(realpath "$root/../LangArena")" "$p/LangArena"
  h=$(cd "$p/avant" && timeout -k 10 900 sh tools/la-harness.sh < /dev/null 2> "$p/stitch/harness.log") || { echo "la-modes.sh: the harness did not build ($p/stitch/harness.log)" >&2; exit 2; }
  cat > "$p/stitch/record-args" <<'EOF'
#!/bin/sh
# Records the compile arguments the harness gives (after `compile ROOT
# BIN`), one per line, in $STITCH_ARGS/SLUG; then refuses, so no task runs.
[ "$1" = compile ] || exit 1
slug=$(basename "$3"); shift 3
: > "$STITCH_ARGS/$slug"
for a in "$@"; do printf '%s\n' "$a" >> "$STITCH_ARGS/$slug"; done
exit 1
EOF
  chmod +x "$p/stitch/record-args"
  (cd "$p/avant" && env STITCH_ARGS="$p/args" AVANT_COMPILER="$p/stitch/record-args" AVANT_CODEGEN=release AVANT_RESULTS_DIR="$p/stitch/results" \
    timeout -k 10 900 "$h" "$size" < /dev/null > "$p/stitch/stitch.log" 2>&1)
  n=$(ls "$p/args" | wc -l)
  [ "$n" -gt 0 ] || { echo "la-modes.sh: the harness stitched no program ($p/stitch/stitch.log)" >&2; exit 2; }
  echo "la-modes: $n programs stitched ($size)"
}

# slug_list: the programs to run, one per line.
slug_list() {
  if [ -n "$slugs" ]; then for s in $slugs; do echo "$s"; done; else ls "$p/args"; fi
}

# do_compile: every program compiled by C' with its arguments.
do_compile() {
  t=$p/avant/langarena/tmp
  mkdir -p "$out/bin" || exit 2
  : > "$out/compile.txt"
  for s in $(slug_list); do
    [ -f "$p/args/$s" ] || { echo "la-modes.sh: no program $s ($p/args)" >&2; exit 2; }
    set --; while IFS= read -r a; do set -- "$@" "$a"; done < "$p/args/$s"
    ( cd "$t" && env -u AVANT_ROOT timeout -k 10 "$limit" "$cc_root/bin/avant-c-llvm" compile "$s.av" "$out/bin/$s" "$@" > "$out/bin/$s.log" 2>&1 < /dev/null; echo "$s rc=$?" >> "$out/compile.txt" ) &
    wait_below "$cjobs"
  done
  wait
  bad=$(grep -vc ' rc=0$' "$out/compile.txt")
  echo "la-modes: $(wc -l < "$out/compile.txt") programs compiled, $bad failed"
  [ "$bad" -eq 0 ]
}

# run_one SLUG MODE: one run, its line appended to runs.txt.
run_one() {
  s=$1 m=$2
  case "$m" in plain) mw="" ;; *) mw=$m ;; esac
  d=$out/run; mkdir -p "$d"
  ( cd "$p/avant/langarena/tmp" && /usr/bin/time -f '%e %M' -o "$d/$s.$m.time" env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 $mw timeout -k 5 "$limit" "$out/bin/$s" > "$d/$s.$m.out" 2> "$d/$s.$m.err" < /dev/null
    rc=$?
    echo "$s $m rc=$rc $(md5sum < "$d/$s.$m.out" | cut -c1-12) $(tail -n 1 "$d/$s.$m.time" 2>/dev/null || echo '? ?')" >> "$out/runs.txt" )
}

# do_mode MODE: every compiled program run under MODE.
do_mode() {
  m=$1
  for s in $(grep ' rc=0$' "$out/compile.txt" | cut -d' ' -f1 | LC_ALL=C sort); do
    run_one "$s" "$m" &
    wait_below "$jobs"
  done
  wait
  echo "la-modes: $m: $(grep -c " $m rc=" "$out/runs.txt") runs"
}

# summary: runs.txt against the plain runs.
do_summary() {
  awk -v modes="$modes" -v nprog="$(wc -l < "$out/compile.txt")" '
    { n++; slug[n] = $1; mode[n] = $2; rc[n] = $3; sum[n] = $4; sec[n] = $5
      if ($2 == "plain") { prc[$1] = $3; pmd[$1] = $4 } }
    END {
      for (i = 1; i <= n; i++) {
        cnt[mode[i]]++
        if (rc[i] != "rc=0") { bad++; why[bad] = slug[i] " " mode[i] " " rc[i] " (not rc=0)" }
        else if (mode[i] != "plain" && prc[slug[i]] == "rc=0" && sum[i] != pmd[slug[i]]) { bad++; why[bad] = slug[i] " " mode[i] " (output " sum[i] " differs from the plain run, " pmd[slug[i]] ")" }
        if (rc[i] == "rc=0" && sec[i] + 0 >= top[mode[i]] + 0) { top[mode[i]] = sec[i]; topn[mode[i]] = slug[i] }
      }
      printf "runs %d over %d programs\n", n, nprog
      k = split(modes, ml, " ")
      for (j = 1; j <= k; j++) printf "mode %s: %d runs\n", ml[j], cnt[ml[j]]
      printf "bad %d\n", bad + 0
      for (i = 1; i <= bad; i++) print "bad: " why[i]
      for (j = 1; j <= k; j++) if (ml[j] in topn) printf "slowest %s: %s %s s\n", ml[j], topn[ml[j]], top[ml[j]]
    }' "$out/runs.txt" > "$out/summary.txt"
  nb=$(sed -n 's/^bad \([0-9]*\)$/\1/p' "$out/summary.txt")
  nr=$(sed -n 's/^runs \([0-9]*\) over.*/\1/p' "$out/summary.txt")
  echo "la-modes: $nr runs, $nb bad"
  [ "$nb" -eq 0 ]
}

# The steps, as the caller's runner takes them.
case "$step" in
  stitch) do_stitch; exit $? ;;
  compile) do_compile; exit $? ;;
  mode:*) do_mode "${step#mode:}"; exit $? ;;
  "") ;;
  *) echo "la-modes.sh: no step $step" >&2; exit 2 ;;
esac
[ -x "$cc_root/bin/avant-c-llvm" ] || { echo "la-modes.sh: no compiler $cc_root/bin/avant-c-llvm" >&2; exit 2; }
[ -e "$out" ] && { echo "la-modes.sh: $out exists" >&2; exit 2; }
mkdir -p "$out" || exit 2
# rn STEP: this script's step, through the runner when there is one.
rn() {
  if [ -n "$runner" ]; then
    # shellcheck disable=SC2086
    $runner sh "$0" --step "$1" "$out" $slugs < /dev/null
  else sh "$0" --step "$1" "$out" $slugs < /dev/null; fi
}
rn stitch || exit 2
rn compile || exit 2
: > "$out/runs.txt"
for m in $modes; do rn "mode:$m" || exit 2; done
do_summary
