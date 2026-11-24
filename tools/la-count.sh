#!/bin/sh
# la-count.sh — instruction counts of the LangArena programs as the
# harness stitches them, for one compiler.
#
#   sh tools/la-count.sh ROOT LADIR OUT.tsv [SLUG...]
#   sh tools/la-count.sh --method     the counting method's name (below)
#
# ROOT is a checkout with its C' built (bin/avant-c-llvm). LADIR is a base
# cache's LangArena part (tools/base-cache.sh --la): its stitched programs
# in LADIR/avant/langarena/tmp/SLUG.av and the harness's compile arguments
# in LADIR/args/SLUG. Each program (every one in LADIR/args, or the SLUGs
# given) is compiled by ROOT's C' from that directory with its arguments,
# then run once from it under `perf stat -x, -e instructions:u` with an
# empty environment but PATH, HOME and AVANT_HASH_SEED=0 (another AVANT_*
# variable would change the collector's work; the fixed hash seed makes
# every run probe its hashes alike, so the count repeats). Every run starts
# the program through tools/la-run.c (built here with cc), which gives it
# the same argv[0] (its slug) and the same executable name in the
# auxiliary vector wherever OUT and its work directory lie: started from its own path, a program's stack and its argv
# copy in the heap moved with that path's length, and a count could move by
# a whole collector pass from one directory to another (a collector
# benchmark's scenario read 138.1 M or 145.6 M instructions by the length
# of its directory's name; through la-run 138.18 M from every length,
# within 330 instructions), with address randomization off, and ends it
# after 900 seconds itself: with randomization on, and timeout(1) in the
# counted command (about two million instructions of its own), a count
# moved by about two thousand instructions from run to run; this way by
# tens (la-run's own start adds about 126,000 that repeat to a few). The
# counting method's name, printed by --method, changes whenever the way a
# count is taken does, so a cache of counts (tools/base-cache.sh) can tell
# counts taken another way.
# OUT.tsv gets one line per program, sorted:
#
#   SLUG <TAB> INSTRUCTIONS <TAB> MD5 <TAB> STATUS <TAB> FIRST LINE
#
# MD5 is the first 12 hex of its stdout's md5, FIRST LINE its stdout's first
# line (a LangArena program prints its checksum there); a program that does
# not compile reads `compile-failed` for its count. An instruction count is
# not a time: the machine's load does not change it, so it runs under the
# shared lock. LA_JOBS programs at once (default 8); compile under
# `timeout -k 10 300`, run for at most 900 seconds, stdin from /dev/null.
#
# Instruction counts of threaded programs (the Matmul tasks' workers)
# cover every thread; they repeat as closely as the single-threaded ones
# (five runs of each of Matmul::T4/T8/T16 here spread under 0.02%: the
# waiting threads block, nothing spins), so their ratios read alike.
#
# Hash seeds, with LA_SEEDS=N (1 to 12; default unset: seed 0 alone): each
# program's count is taken once under each of the first N seeds of the fixed
# list below (AVANT_HASH_SEED; the same list on both sides of a comparison),
# and its INSTRUCTIONS is their mean, rounded. One seed can place a task's
# keys well or badly in its tables (seed 0 alone read a +0.49% change of
# Compress::LZWEncode that the other seeds did not), so a change to the hash
# function, the hash tables or their probing is judged over the 12. OUT.seeds.tsv
# beside OUT.tsv gets one line per program:
#
#   SLUG <TAB> MEAN <TAB> MIN <TAB> MAX <TAB> SPREAD % <TAB> COUNTS (comma-separated, in the list's order)
#
# A program whose output differs between seeds reads `seeds-differ` as its
# status (a checksum must not depend on the hash seed). The list: 0, then
# the low 32 bits of splitmix64 at 1 to 11.
#
# Peak memory, with LA_MEM_RUNS=N (default 0: none): each program is then
# also run N times, its peak read exactly by tools/la-run.c's -p (built here
# with cc): the resident size read before every system call that can lower
# it and at exit, in the same environment. LA_MEM_METHOD=maxrss reads it as
# the harness does instead (GNU time's %M of the task's own process, the
# kernel's ru_maxrss), which moves in steps of about 256 KB at equal work:
# the kernel folds a process's per-CPU resident-page counts into the total
# by batches of 64 pages, and its high-water mark reads the folded total.
# With the exact method each run's ru_maxrss is kept beside it, in
# OUT.maxrss.tsv, and its anonymous peak (la-run's ANON_KB: without the
# file-backed pages, which follow the page cache's state of the program's
# files, so they move a few pages from run to run when the machine reclaims
# cached files) in OUT.anon.tsv, both of the same shape as OUT.mem.tsv
# (methods `maxrss` and `exact-anon`). The runs are unbound
# (no NUMA binding: a binding's placement hides changes of touched memory
# of a few megabytes) and with
# address randomization off (`setarch -R`: with it on, a task's peak moves
# between two values 64 KB or more apart at equal page faults, from where
# its mappings land, so a few runs read a change that is not there).
# The N runs of a program start together (LA_MEM_PARALLEL=0: one after
# another; a peak is not a time, and a run's file is its own).
# OUT.mem.tsv beside OUT.tsv gets a first line naming its method
# (`# peak: exact` or `# peak: maxrss`), then one line per program:
#
#   SLUG <TAB> MEDIAN KB <TAB> MIN KB <TAB> MAX KB <TAB> RUNS KB (comma-separated) <TAB> ON RUNS KB
#
# ON RUNS are LA_MEM_ON_RUNS more runs (default N; 0: none, and no sixth
# field) with address randomization on, started with the others: with it
# off, a binary keeps one set of mapping addresses, and a change that only
# moves where a mapping lands can read as a moved peak in every run (a
# JSON task read 2.96% higher at equal collector work, and lower with it
# on); tools/la-mem.py calls a peak moved only when these runs agree.
# tools/la-mem.py compares two such files (the 50 tasks' average peak, its
# spread, and the programs to run again).
# LA_RUNNER and LA_GROUP split the programs into shared runs of their own
# (below, where it is read). Status 0, 1 when a program did not compile or
# failed, 2 on usage.
set -u
# The method's name carries la-run.c's digest: its own start is counted
# with every program, so another la-run is another level.
[ "${1:-}" = --method ] && { echo "la-run $(sha256sum < "$(dirname "$0")/la-run.c" | cut -c1-12) argv0=slug execfn=/dev/fd alarm aslr-off"; exit 0; }
LA_SEED_LIST="0 2298633409 479680206 3674312685 3795028682 2743714650 2918178816 1496452567 4019533366 3195035748 2321035210 595603613"

if [ "${1:-}" = --one ]; then
  s=$2
  t=$LAC_TMP/$s
  set --
  while IFS= read -r a; do set -- "$@" "$a"; done < "$LAC_DIR/args/$s"
  if ! (cd "$LAC_SRC" && env -u AVANT_ROOT timeout -k 10 300 "$LAC_ROOT/bin/avant-c-llvm" compile "$s.av" "$t.linked" "$@" < /dev/null > "$t.compile.log" 2>&1); then
    printf '%s\tcompile-failed\t-\t-\t-\n' "$s"; rm -f "$t.linked"; exit 0
  fi
  # The program runs from a copy written by cat: the page cache keeps a file
  # the linker wrote in other folio sizes than one written by write(2), the
  # kernel maps a file's pages a folio at a time, and the same bytes then
  # read up to about 100 KB apart in resident file pages from one compile to
  # the next (Hash::CRC32: 4,476 KB as linked, 4,444 KB from every copy).
  cat "$t.linked" > "$t" && chmod +x "$t" && rm -f "$t.linked"
  if [ -n "${LA_SEEDS:-}" ]; then
    # One count per seed; the mean is the program's count, the counts in
    # OUT.seeds.tsv. Every seed's output must be the first seed's.
    k=0; counts=""; rc=0; first=""
    for sd in $LA_SEED_LIST; do
      [ "$k" -lt "$LA_SEEDS" ] || break
      k=$((k + 1))
      (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED="$sd" perf stat -x, -e instructions:u -o "$t.perf" "$LAC_TMP/la-run" -R -t 900 "$t" < /dev/null > "$t.out.$k" 2> "$t.err")
      r=$?; [ $r -ne 0 ] && rc=$r
      counts="$counts${counts:+,}$(grep 'instructions' "$t.perf" | cut -d, -f1)"
      m=$(md5sum < "$t.out.$k" | cut -c1-12)
      if [ -z "$first" ]; then first=$m; cp "$t.out.$k" "$t.out"; elif [ "$m" != "$first" ] && [ $rc -eq 0 ]; then rc=seeds-differ; fi
      rm -f "$t.out.$k"
    done
    n=$(echo "$counts" | awk -F, '{ for (i = 1; i <= NF; i++) if ($i !~ /^[0-9]+$/) { print "-"; exit } ; s = 0; for (i = 1; i <= NF; i++) s += $i; printf "%.0f\n", s / NF }')
    echo "$counts" > "$LAC_TMP/$s.seeds"
  else
    (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 perf stat -x, -e instructions:u -o "$t.perf" "$LAC_TMP/la-run" -R -t 900 "$t" < /dev/null > "$t.out" 2> "$t.err")
    rc=$?
    n=$(grep 'instructions' "$t.perf" | cut -d, -f1)
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$s" "${n:--}" "$(md5sum < "$t.out" | cut -c1-12)" "$rc" "$(head -n 1 "$t.out" | cut -c1-60)"
  # The memory runs of one program start together (LA_MEM_PARALLEL=0: one
  # after another): a peak is not a time, and each run writes a file of its own.
  i=0; runs=""; onr=""
  nm=${LA_MEM_RUNS:-0}; non=${LA_MEM_ON_RUNS:-$nm}
  [ "$nm" -gt 0 ] || non=0
  while [ "$i" -lt $((nm + non)) ]; do
    i=$((i + 1))
    # Runs past the first N keep address randomization on.
    if [ "$LA_MEM_METHOD" = exact ]; then
      if [ "$i" -le "$nm" ]; then nr=-R; else nr=; fi
      set -- "$LAC_TMP/la-run" $nr -p "$t.rss.$i" "$t"
    else
      if [ "$i" -le "$nm" ]; then nr="setarch -R"; else nr=; fi
      set -- $nr /usr/bin/time -f %M -o "$t.rss.$i" "$LAC_TMP/la-run" "$t"
    fi
    if [ "${LA_MEM_PARALLEL:-1}" = 0 ]; then
      (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 timeout -k 10 900 "$@" < /dev/null > /dev/null 2>&1)
    else
      (cd "$LAC_SRC" && env -i PATH="$PATH" HOME="${HOME:-/}" AVANT_HASH_SEED=0 timeout -k 10 900 "$@" < /dev/null > /dev/null 2>&1) &
    fi
  done
  wait
  # The exact method's file is `EXACT MAXRSS READS ANON`; GNU time's ends in %M.
  i=0; mxr=""; mxo=""; anr=""; ano=""
  while [ "$i" -lt $((nm + non)) ]; do
    i=$((i + 1))
    if [ "$LA_MEM_METHOD" = exact ]; then
      set -- $(head -n 1 "$t.rss.$i" 2>/dev/null)
      v=${1:-}; w=${2:-}; a=${4:-}
    else
      v=$(tail -n 1 "$t.rss.$i" 2>/dev/null); w=; a=
    fi
    if [ "$i" -le "$nm" ]; then
      runs="$runs${runs:+,}$v"; mxr="$mxr${mxr:+,}$w"; anr="$anr${anr:+,}$a"
    else
      onr="$onr${onr:+,}$v"; mxo="$mxo${mxo:+,}$w"; ano="$ano${ano:+,}$a"
    fi
  done
  [ -n "$onr" ] && runs="$runs	$onr"
  [ -n "$runs" ] && echo "$runs" > "$LAC_TMP/$s.mem"
  [ -n "$mxo" ] && mxr="$mxr	$mxo"
  [ -n "$mxr" ] && echo "$mxr" > "$LAC_TMP/$s.maxrss"
  [ -n "$ano" ] && anr="$anr	$ano"
  [ -n "$anr" ] && echo "$anr" > "$LAC_TMP/$s.anon"
  rm -f "$t" "$t.out" "$t.err" "$t.perf" "$t".rss.* "$t.compile.log"
  exit 0
fi

[ $# -ge 3 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
case "${LA_SEEDS:-}" in ''|[1-9]|1[0-2]) ;; *) echo "la-count.sh: LA_SEEDS is a number from 1 to 12, not '$LA_SEEDS'" >&2; exit 2 ;; esac
LA_MEM_METHOD=${LA_MEM_METHOD:-exact}
case "$LA_MEM_METHOD" in exact|maxrss) ;; *) echo "la-count.sh: LA_MEM_METHOD is exact or maxrss, not '$LA_MEM_METHOD'" >&2; exit 2 ;; esac
export LA_SEED_LIST LA_MEM_METHOD
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
runner=${LA_RUNNER:-}
set -- cc -O2 -o "$LAC_TMP/la-run" "$(dirname "$self")/la-run.c"
# shellcheck disable=SC2086
[ -n "$runner" ] && set -- $runner --step "${LA_STEP:-la-count} la-run" --kind "${LA_KIND:-new}" -- "$@"
if ! "$@" < /dev/null; then
  echo "la-count.sh: tools/la-run.c did not build" >&2; rm -rf "$LAC_TMP"; exit 2
fi
# With LA_RUNNER (the runner's command words, such as `python3 -I PATH/ledger
# run --shared --wave W`) the script is started outside the lock and each group
# of programs is its own shared run, `LA_STEP group K/N` (default step
# `la-count`; LA_KIND its --kind, default new): a pass with memory runs over
# every program no longer holds the lock past the shared limit. LA_GROUP
# programs a group (default 24 with memory runs, 64 without); without
# LA_RUNNER one group, as the caller's single run.
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
# One mem file: its method's line, then a line per program, sorted.
mem_file() { # EXT METHOD OUT
  { echo "# peak: $2"
    for f in "$LAC_TMP"/*."$1"; do
      [ -f "$f" ] || continue
      printf '%s\t%s\n' "$(basename "$f" ."$1")" "$(cat "$f")"
    done | awk -F'\t' '{ n = split($2, r, ","); for (i = 1; i <= n; i++) v[i] = r[i] + 0
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (v[j] < v[i]) { x = v[i]; v[i] = v[j]; v[j] = x }
        m = (n % 2) ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
        printf "%s\t%s\t%s\t%s\t%s%s\n", $1, m, v[1], v[n], $2, (NF > 2 ? "\t" $3 : "") }' | LC_ALL=C sort; } > "$3"
}
if [ "${LA_MEM_RUNS:-0}" -gt 0 ]; then
  mem_file mem "$LA_MEM_METHOD" "${out%.tsv}.mem.tsv"
  if [ "$LA_MEM_METHOD" = exact ]; then
    mem_file maxrss maxrss "${out%.tsv}.maxrss.tsv"
    mem_file anon exact-anon "${out%.tsv}.anon.tsv"
  fi
fi
if [ -n "${LA_SEEDS:-}" ]; then
  for f in "$LAC_TMP"/*.seeds; do
    [ -f "$f" ] || continue
    printf '%s\t%s\n' "$(basename "$f" .seeds)" "$(cat "$f")"
  done | awk -F'\t' '{ n = split($2, c, ","); s = 0; lo = ""; hi = ""; ok = 1
      for (i = 1; i <= n; i++) { if (c[i] !~ /^[0-9]+$/) ok = 0; v = c[i] + 0; s += v; if (lo == "" || v < lo) lo = v; if (hi == "" || v > hi) hi = v }
      if (!ok || n == 0) { printf "%s\t-\t-\t-\t-\t%s\n", $1, $2; next }
      m = s / n; printf "%s\t%.0f\t%d\t%d\t%.4f\t%s\n", $1, m, lo, hi, (hi - lo) / m * 100, $2 }' | LC_ALL=C sort > "${out%.tsv}.seeds.tsv"
fi
rm -rf "$LAC_TMP"
mv "$out.part" "$out"
awk -F'\t' '$2 == "compile-failed" || $4 != 0 { bad = 1 } END { exit bad }' "$out"
