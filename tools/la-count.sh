#!/bin/sh
# la-count.sh — instruction counts of the LangArena programs as the
# harness stitches them, for one compiler.
#
#   sh tools/la-count.sh ROOT LADIR OUT.tsv [SLUG...]
#
# ROOT is a checkout with its C' built (bin/avant-c-llvm). LADIR is a base
# cache's LangArena part (tools/base-cache.sh --la): its stitched programs
# in LADIR/avant/langarena/tmp/SLUG.av and the harness's compile arguments
# in LADIR/args/SLUG. Each program (every one in LADIR/args, or the SLUGs
# given) is compiled by ROOT's C' from that directory with its arguments,
# then run once from it under `perf stat -x, -e instructions:u` with an
# empty environment but PATH, HOME and AVANT_HASH_SEED=0 (another AVANT_*
# variable would change the collector's work; the fixed hash seed makes
# every run probe its hashes alike, so the count repeats). OUT.tsv gets one line per program, sorted:
#
#   SLUG <TAB> INSTRUCTIONS <TAB> MD5 <TAB> STATUS <TAB> FIRST LINE
#
# MD5 is the first 12 hex of its stdout's md5, FIRST LINE its stdout's first
# line (a LangArena program prints its checksum there); a program that does
# not compile reads `compile-failed` for its count. An instruction count is
# not a time: the machine's load does not change it, so it runs under the
# shared lock. LA_JOBS programs at once (default 8); compile under
# `timeout -k 10 300`, run under `timeout -k 10 900`, stdin from /dev/null.
#
# Instruction counts of threaded programs (the Matmul tasks' workers)
# cover every thread; they repeat as closely as the single-threaded ones
# (five runs of each of Matmul::T4/T8/T16 here spread under 0.02%: the
# waiting threads block, nothing spins), so their ratios read alike.
#
# Peak memory, with LA_MEM_RUNS=N (default 0: none): each program is then
# also run N times as the harness measures it (GNU time's %M of the task's
# own process, the same environment), unbound (no NUMA binding: a binding's
# placement hides changes of touched memory of a few megabytes) and with
# address randomization off (`setarch -R`: with it on, a task's peak moves
# between two values 64 KB or more apart at equal page faults, from where
# its mappings land, so a few runs read a change that is not there).
# OUT.mem.tsv beside OUT.tsv gets one line per program:
#
#   SLUG <TAB> MEDIAN KB <TAB> MIN KB <TAB> MAX KB <TAB> RUNS KB (comma-separated)
#
# tools/la-mem.py compares two such files (the 50 tasks' average peak, its
# spread, and the programs to run again).
# LA_RUNNER and LA_GROUP split the programs into shared runs of their own
# (below, where it is read). Status 0, 1 when a program did not compile or
# failed, 2 on usage.
set -u

if [ "${1:-}" = --one ]; then
  s=$2
  t=$LAC_TMP/$s
  set --
  while IFS= read -r a; do set -- "$@" "$a"; done < "$LAC_DIR/args/$s"
  if ! (cd "$LAC_SRC" && env -u AVANT_ROOT timeout -k 10 300 "$LAC_ROOT/bin/avant-c-llvm" compile "$s.av" "$t" "$@" < /dev/null > "$t.compile.log" 2>&1); then
    printf '%s\tcompile-failed\t-\t-\t-\n' "$s"; exit 0
  fi
  (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 perf stat -x, -e instructions:u -o "$t.perf" timeout -k 10 900 "$t" < /dev/null > "$t.out" 2> "$t.err")
  rc=$?
  n=$(grep 'instructions' "$t.perf" | cut -d, -f1)
  printf '%s\t%s\t%s\t%s\t%s\n' "$s" "${n:--}" "$(md5sum < "$t.out" | cut -c1-12)" "$rc" "$(head -n 1 "$t.out" | cut -c1-60)"
  i=0; runs=""
  while [ "$i" -lt "${LA_MEM_RUNS:-0}" ]; do
    i=$((i + 1))
    (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 timeout -k 10 900 setarch -R /usr/bin/time -f %M -o "$t.rss" "$t" < /dev/null > /dev/null 2>&1)
    runs="$runs${runs:+,}$(tail -n 1 "$t.rss" 2>/dev/null)"
  done
  [ -n "$runs" ] && echo "$runs" > "$LAC_TMP/$s.mem"
  rm -f "$t" "$t.out" "$t.err" "$t.perf" "$t.rss" "$t.compile.log"
  exit 0
fi

[ $# -ge 3 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
LAC_ROOT=$(realpath "$1") || exit 2
LAC_DIR=$(realpath "$2") || exit 2
out=$(realpath -m "$3"); shift 3
LAC_SRC=$LAC_DIR/avant/langarena/tmp
[ -x "$LAC_ROOT/bin/avant-c-llvm" ] || { echo "la-count.sh: $LAC_ROOT/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
[ -d "$LAC_DIR/args" ] && [ -d "$LAC_SRC" ] || { echo "la-count.sh: $LAC_DIR is not a base cache's LangArena part" >&2; exit 2; }
command -v perf > /dev/null 2>&1 || { echo "la-count.sh: perf not found" >&2; exit 2; }
LAC_TMP=$(mktemp -d "$(dirname "$out")/.la-count.XXXXXX") || exit 2
export LAC_ROOT LAC_DIR LAC_SRC LAC_TMP
self=$(realpath "$0")
if [ $# -gt 0 ]; then printf '%s\n' "$@"; else ls "$LAC_DIR/args"; fi | LC_ALL=C sort -u > "$LAC_TMP/slugs.txt"
# With LA_RUNNER (the runner's command words, such as `python3 -I PATH/ledger
# run --shared --wave W`) the script is started outside the lock and each group
# of programs is its own shared run, `LA_STEP group K/N` (default step
# `la-count`; LA_KIND its --kind, default new): a pass with memory runs over
# every program no longer holds the lock past the shared limit. LA_GROUP
# programs a group (default 24 with memory runs, 64 without); without
# LA_RUNNER one group, as the caller's single run.
runner=${LA_RUNNER:-}
ng=1
if [ -n "$runner" ]; then
  per=${LA_GROUP:-$([ "${LA_MEM_RUNS:-0}" -gt 0 ] && echo 24 || echo 64)}
  ng=$(awk -v per="$per" 'END { n = int((NR + per - 1) / per); print (n < 1) ? 1 : n }' "$LAC_TMP/slugs.txt")
fi
# Dealt in turn, so each group has a share of the long programs.
awk -v n="$ng" -v d="$LAC_TMP" '{ print > (d "/group." (((NR - 1) % n) + 1) ".txt") }' "$LAC_TMP/slugs.txt"
: > "$out.part.unsorted"
gk=0
while [ "$gk" -lt "$ng" ]; do
  gk=$((gk + 1))
  g=$LAC_TMP/group.$gk.txt
  [ -f "$g" ] || continue
  if [ -n "$runner" ]; then
    # shellcheck disable=SC2086
    $runner --step "${LA_STEP:-la-count} group $gk/$ng" --kind "${LA_KIND:-new}" -- \
      sh -c 'xargs -P "$1" -I{} sh "$2" --one {} < "$3" >> "$4"' la-group "${LA_JOBS:-8}" "$self" "$g" "$out.part.unsorted"
  else
    xargs -P "${LA_JOBS:-8}" -I{} sh "$self" --one {} < "$g" >> "$out.part.unsorted"
  fi
done
LC_ALL=C sort "$out.part.unsorted" > "$out.part"; rm -f "$out.part.unsorted"
if [ "${LA_MEM_RUNS:-0}" -gt 0 ]; then
  for f in "$LAC_TMP"/*.mem; do
    [ -f "$f" ] || continue
    printf '%s\t%s\n' "$(basename "$f" .mem)" "$(cat "$f")"
  done | awk -F'\t' '{ n = split($2, r, ","); for (i = 1; i <= n; i++) v[i] = r[i] + 0
      for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { x = v[i]; v[i] = v[j]; v[j] = x }
      m = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
      printf "%s\t%s\t%s\t%s\t%s\n", $1, m, v[1], v[n], $2 }' | LC_ALL=C sort > "${out%.tsv}.mem.tsv"
fi
rm -rf "$LAC_TMP"
mv "$out.part" "$out"
awk -F'\t' '$2 == "compile-failed" || $4 != 0 { bad = 1 } END { exit bad }' "$out"
