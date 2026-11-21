#!/bin/sh
# test-map.sh — which test files run which lines of the compiler: the
# coverage job's instrumented compiler, its hits taken one test file at a
# time, kept in the base cache beside the tree's entry.
#
#   sh tools/test-map.sh REV        (from the checkout root)
#
# REV's tree gets its base cache entry (tools/base-cache.sh, built when
# missing) and, beside it, ENTRY/testmap/:
#
#   hits/TEST.tsv   one line per source file the test file's run executed
#                   (compiler/... and the library modules the compiler is
#                   built from): PATH <TAB> the lines hit, as ranges
#                   (1-5,9,12-40)
#   files.txt       every path some test file's run executed
#   MANIFEST        the tree, the time of each part; COMPLETE, written last
#
#   sh tools/test-map.sh --pick MAP BEFORE AFTER FILE...
#
# prints the test files of the map MAP whose hits meet the lines the change
# BEFORE..AFTER makes in the FILEs (tools/change-check.sh's use). Every test
# file imports the suite's helper, and compiling it runs much of the
# compiler: a line more than TESTMAP_SHARE of the test files run (default
# 0.5) tells no test file from another and picks none (said on stderr).
#
# How: a copy of the entry's checkout gets the compiler built as the
# coverage job builds it (`AVANT_COVERAGE=1 AVANT_COVERAGE_MAP=MAP`, C'
# compiling compiler/main.av into bin/avant-b, --debug); every
# tests/cases/test_*.av then runs as the job's suite runs it
# (`AVANT_COMPILER=./bin/avant-b ./bin/avant-b test FILE`), each in a mount
# namespace of its own where MAP.hits is a file of its own (the counters
# are one map shared by every process of the instrumented binary), so the
# hits are that file's alone; then `./bin/avant-b help` with
# AVANT_COVERAGE_LCOV writes that namespace's LCOV report, read for the
# lines hit. TESTMAP_JOBS files at once (default 8), the `[test]
# exclusive` files of avant.toml alone afterwards; each under `timeout -k
# 10 1800`, stdin from /dev/null, a TMPDIR of its own. A test file that
# fails still gives its hits (named in MANIFEST). Built once per tree,
# under ENTRY/testmap.lock; run it again after removing ENTRY/testmap to
# refresh it. Read by tools/change-check.sh. Run it under the machine
# lock's shared side. With TESTMAP_RUNNER (the runner's command words, such
# as `python3 -I PATH/ledger run --shared --wave W`) the script is started
# outside the lock and takes it per piece of work, each its own shared run
# named TESTMAP_STEP (default `test map`) plus ` base cache` (when the entry
# is missing), ` build`, ` group K/N` or ` exclusive` (TESTMAP_KIND is their --kind, default new): the test files
# are dealt into groups of about TESTMAP_GROUP_SECONDS of wall each (default
# 240), so no run holds the lock past the shared limit however many files
# there are. Within a group the files start longest first (their times from
# another tree's map in the base cache, TESTMAP_TIMES naming a runs.tsv
# instead), so the last file does not run alone; TESTMAP_JOBS files at once
# (default 16). The hits do not depend on any of this: the same files, the
# same lines.
# Status 0 with the directory on stdout, 1 when the build failed, 2 on a
# usage error or where the machine refuses the namespace.
set -u
runner=${TESTMAP_RUNNER:-}
now() { date +%s.%N; }
# tm_run STEP CMD...: CMD through TESTMAP_RUNNER as its own shared run, else as it is.
tm_run() {
  r_step=$1; shift
  if [ -n "$runner" ]; then
    # shellcheck disable=SC2086
    $runner --step "$r_step" --kind "${TESTMAP_KIND:-new}" -- "$@"
  else "$@"; fi
}

if [ "${1:-}" = --pick ]; then
  # --pick MAP BEFORE AFTER FILE...: the test files of MAP whose hits meet
  # the lines that the change BEFORE..AFTER makes in each FILE (read in
  # MAP's tree: the change's lines in BEFORE carried to that tree through
  # `git diff` of the tree and BEFORE; a line no test file hits, not
  # executable (a signature, a field), counts as the whole function or
  # class it sits in). Prints one tests/cases/ path a line.
  m=$2 before=$3 after=$4; shift 4
  TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/test-map-pick.XXXXXX") || exit 2
  mt=$(sed -n 's/^tree: //p' "$m/MANIFEST" | head -n 1)
  for f in "$@"; do
    grep -q -F -x -- "$f" "$m/files.txt" 2>/dev/null || continue
    git cat-file -e "$mt:$f" 2>/dev/null || continue
    # The changed lines of BEFORE: an insertion reads the lines around it.
    git diff -U0 "$before" "$after" -- "$f" | awk '/^@@/ {
        split($2, o, ","); a = substr(o[1], 2) + 0; b = (2 in o) ? o[2] + 0 : 1
        if (b == 0) { print a; print a + 1 } else for (i = a; i < a + b; i++) print i }' > "$TMPDIR/pick.lines.$$"
    [ -s "$TMPDIR/pick.lines.$$" ] || continue
    # Carried to the map's tree; beside each line, its block there (from a
    # line at the left margin or two spaces in that starts fn, class or
    # struct, to the next one).
    git diff -U0 "$mt" "$before" -- "$f" | awk '/^@@/ { print substr($2, 2) " " substr($3, 2) }' > "$TMPDIR/pick.hunks.$$"
    git show "$mt:$f" | awk '/^ {0,2}(pub +)?(fn|class|struct)[ (]/ { print NR }' > "$TMPDIR/pick.starts.$$"
    awk -v nl="$(git show "$mt:$f" | wc -l)" '
      FILENAME == ARGV[1] { split($1, o, ","); split($2, q, ","); h++
        oa[h] = o[1] + 0; ob[h] = (2 in o) ? o[2] + 0 : 1; na[h] = q[1] + 0; nb[h] = (2 in q) ? q[2] + 0 : 1; next }
      FILENAME == ARGV[2] { st[++ns] = $1; next }
      { L = $1 + 0; d = 0; lo = -1
        for (i = 1; i <= h; i++) {
          if (nb[i] > 0 && L >= na[i] && L < na[i] + nb[i]) { lo = oa[i]; hi = (ob[i] > 0) ? oa[i] + ob[i] - 1 : oa[i] + 1; break }
          if ((nb[i] > 0 && na[i] + nb[i] <= L) || (nb[i] == 0 && na[i] < L)) d += nb[i] - ob[i]
        }
        if (lo < 0) { lo = L - d; hi = lo }
        b = 1; e = nl
        for (j = 1; j <= ns; j++) { if (st[j] <= lo) b = st[j]; if (st[j] > hi) { e = st[j] - 1; break } }
        print lo " " hi " " b " " e }' "$TMPDIR/pick.hunks.$$" "$TMPDIR/pick.starts.$$" "$TMPDIR/pick.lines.$$" > "$TMPDIR/pick.spans.$$"
    # A test file is picked when its hits meet a changed line; a changed
    # line no test file hits (not executable: a signature, a field, a
    # comment) counts as its whole block instead.
    awk -F'\t' -v f="$f" -v share="${TESTMAP_SHARE:-0.5}" -v all="$(ls "$m"/hits | wc -l)" '
      FILENAME == ARGV[1] { split($0, q, " "); n++; lo[n] = q[1]; hi[n] = q[2]; b[n] = q[3]; e[n] = q[4]; next }
      $1 == f { t = FILENAME; sub(/.*\//, "", t); sub(/\.tsv$/, "", t); seen[t] = 1
        k = split($2, r, ","); for (i = 1; i <= k; i++) { split(r[i], x, "-"); a = x[1] + 0; z = (2 in x) ? x[2] + 0 : a
          for (j = 1; j <= n; j++) { if (a <= hi[j] && z >= lo[j]) { ex[j, t] = 1; exn[j] = 1 }; if (a <= e[j] && z >= b[j]) wd[j, t] = 1 } } }
      END { for (j = 1; j <= n; j++) { c = 0; for (t in seen) if ((exn[j] && ex[j, t]) || (!exn[j] && wd[j, t])) c++; cnt[j] = c }
        for (t in seen) for (j = 1; j <= n; j++) if (cnt[j] <= share * all && ((exn[j] && ex[j, t]) || (!exn[j] && wd[j, t]))) { print "tests/cases/" t ".av"; break }
        for (j = 1; j <= n; j++) if (cnt[j] > share * all) common++
        if (common) printf "test-map.sh: %s: %d of %d changed lines run by more than %d%% of the test files (compiling their shared helpers runs them): no test file picked for those\n", f, common, n, share * 100 > "/dev/stderr" }' \
      "$TMPDIR/pick.spans.$$" "$m"/hits/*.tsv
    rm -f "$TMPDIR/pick.lines.$$" "$TMPDIR/pick.hunks.$$" "$TMPDIR/pick.starts.$$" "$TMPDIR/pick.spans.$$"
  done | LC_ALL=C sort -u
  rm -rf "$TMPDIR"
  exit 0
fi

if [ "${1:-}" = --one ]; then
  # --one TEST: one test file in its namespace ($TM_WORK, $TM_OUT).
  t=$2; n=$(basename "$t" .av)
  : > "$TM_OUT/raw/$n.hits"
  mkdir -p "$TM_OUT/raw/$n.tmp"
  t0=$(date +%s.%N)
  # shellcheck disable=SC2016
  unshare --user --map-current-user --mount --keep-caps sh -c '
    mount --bind "$1" "$2/cov.map.hits" || exit 3
    cd "$2/avant" || exit 3
    env TMPDIR="$4" AVANT_COMPILER=./bin/avant-b AVANT_COVERAGE_LCOV=0 AVANT_COVERAGE_REPORT=0 \
      timeout -k 10 1800 ./bin/avant-b test "$3" < /dev/null > "$5.log" 2>&1
    echo $? > "$5.rc"
    env AVANT_COVERAGE_LCOV="$5.lcov" AVANT_COVERAGE_REPORT=0 ./bin/avant-b help < /dev/null > /dev/null 2>&1' \
    test-map "$TM_OUT/raw/$n.hits" "$TM_WORK" "$t" "$TM_OUT/raw/$n.tmp" "$TM_OUT/raw/$n" < /dev/null
  rm -rf "$TM_OUT/raw/$n.tmp" "$TM_OUT/raw/$n.hits"
  # The lines hit, per file, as ranges.
  awk -F'[:,]' -v w="$TM_WORK/avant/" '
    /^SF:/ { f = substr($0, 4); if (index(f, w) == 1) f = substr(f, length(w) + 1); next }
    /^DA:/ && $3 > 0 { l[f] = l[f] " " $2; next }
    END { for (k in l) {
      n = split(substr(l[k], 2), a, " "); s = ""; st = a[1]; pr = a[1]
      for (i = 2; i <= n + 1; i++) {
        if (i <= n && a[i] == pr + 1) { pr = a[i]; continue }
        s = s (s == "" ? "" : ",") (st == pr ? st : st "-" pr)
        if (i <= n) { st = a[i]; pr = a[i] }
      }
      print k "\t" s } }' "$TM_OUT/raw/$n.lcov" 2>/dev/null | LC_ALL=C sort > "$TM_OUT/hits/$n.tsv"
  printf '%s\t%s\t%s\t%s\n' "$t" "$(cat "$TM_OUT/raw/$n.rc" 2>/dev/null || echo none)" \
    "$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')" "$(wc -l < "$TM_OUT/hits/$n.tsv")"
  [ -s "$TM_OUT/hits/$n.tsv" ] && rm -f "$TM_OUT/raw/$n.lcov" "$TM_OUT/raw/$n.log" "$TM_OUT/raw/$n.rc"
  exit 0
fi

[ $# -eq 1 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
unshare --user --map-current-user --mount --keep-caps sh -c 'mount -t tmpfs tmpfs /tmp' < /dev/null > /dev/null 2>&1 \
  || { echo "test-map.sh: this machine refuses a mount namespace: the hits cannot be taken one test file at a time" >&2; exit 2; }
e=$(tm_run "${TESTMAP_STEP:-test map} base cache" sh "$here/base-cache.sh" "$1" < /dev/null) || exit 1
exec 7>> "$e.lock" && flock -s 7
m=$e/testmap
if [ -f "$m/COMPLETE" ]; then echo "$m"; exit 0; fi
exec 9> "$m.lock" || exit 1
if ! flock -n 9; then echo "test-map.sh: waiting for another build of $m" >&2; flock 9 || exit 1; fi
if [ -f "$m/COMPLETE" ]; then echo "$m"; exit 0; fi
rm -rf "$m" "$m.work"
TM_WORK=$m.work TM_OUT=$m
mkdir -p "$TM_WORK" "$TM_OUT/hits" "$TM_OUT/raw" || exit 1
export TM_WORK TM_OUT
log=$TM_OUT/build.log
t0=$(date +%s.%N)
echo "tree: $(basename "$e")" > "$m/MANIFEST"
cp -a "$e/avant" "$TM_WORK/avant" || exit 1
w=$TM_WORK/avant
sed -i "s#^lib: .*#lib: $w/lib#" "$w/bin/avant-c-llvm.versions.txt"
rm -f "$w/bin/avant-b" "$w/bin/avant-b.versions.txt"
tm_run "${TESTMAP_STEP:-test map} build" sh -c 'cd "$1/avant" && exec env AVANT_COVERAGE=1 AVANT_COVERAGE_MAP="$1/cov.map" timeout -k 10 1800 ./bin/avant-c-llvm compile compiler/main.av bin/avant-b --debug runtime/build/avant_llvm.o < /dev/null > "$2" 2>&1' tm-build "$TM_WORK" "$log" \
  || { echo "test-map.sh: the instrumented compiler did not build ($log)" >&2; tail -n 15 "$log" >&2; rm -rf "$TM_WORK"; exit 1; }
: > "$TM_WORK/cov.map.hits"
t1=$(date +%s.%N)
echo "instrumented compiler: $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }') s" >> "$m/MANIFEST"
# The exclusive files alone, after the others.
excl=$(sed -n '/^\[test\]/,/^\[/{ s/^exclusive *= *\[\(.*\)\]/\1/p; }' "$w/avant.toml" | tr ',' '\n' | tr -d ' "')
(cd "$w" && ls tests/cases/test_*.av) | LC_ALL=C sort > "$m/list.txt"
printf '%s\n' "$excl" | sed '/^$/d' | LC_ALL=C sort > "$m/exclusive.txt"
LC_ALL=C comm -23 "$m/list.txt" "$m/exclusive.txt" > "$m/parallel.txt"
# The files' times from another tree's map (the newest one in the cache, or
# TESTMAP_TIMES): the longest start first, and the groups are dealt by them.
times=${TESTMAP_TIMES:-}
if [ -z "$times" ]; then
  for r in $(ls -t "$(dirname "$e")"/*/testmap/runs.tsv 2>/dev/null); do
    case "$r" in "$m"/*) continue ;; esac
    times=$r; break
  done
fi
if [ -n "$times" ] && [ -f "$times" ]; then cut -f1,3 "$times" > "$m/times.tsv"; else : > "$m/times.tsv"; fi
awk -F'\t' 'FILENAME == ARGV[1] { t[$1] = $2; next } { printf "%s\t%s\n", ($1 in t) ? t[$1] : 45, $1 }' "$m/times.tsv" "$m/parallel.txt" \
  | LC_ALL=C sort -t"$(printf '\t')" -k1,1nr -k2,2 > "$m/order.tsv"
total=$(awk -F'\t' '{ s += $1 } END { printf "%d", s }' "$m/order.tsv")
jobs=${TESTMAP_JOBS:-16}
ng=$(awk -v s="$total" -v j="$jobs" -v g="${TESTMAP_GROUP_SECONDS:-240}" 'BEGIN { n = int(s / j / g + 0.999); print (n < 1) ? 1 : n }')
[ -n "$runner" ] || ng=1
mkdir -p "$m/groups"
# Dealt in turn, the longest first, so every group holds long and short files.
awk -F'\t' -v n="$ng" -v d="$m/groups" '{ print $2 > (d "/" (((NR - 1) % n) + 1) ".txt") }' "$m/order.tsv"
step=${TESTMAP_STEP:-test map}
gk=0
for gf in $(ls "$m"/groups/*.txt | LC_ALL=C sort -V); do
  gk=$((gk + 1))
  tg=$(now)
  tm_run "$step group $gk/$ng" sh -c 'xargs -P "$1" -I{} sh "$2" --one {} < "$3" >> "$4"' tm-group "$jobs" "$here/test-map.sh" "$gf" "$m/runs.tsv"
  echo "group $gk/$ng: $(wc -l < "$gf") files, $(awk -v a="$tg" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }') s" >> "$m/MANIFEST"
done
tg=$(now)
[ -s "$m/exclusive.txt" ] && tm_run "$step exclusive" sh -c 'xargs -P 1 -I{} sh "$1" --one {} < "$2" >> "$3"' tm-excl "$here/test-map.sh" "$m/exclusive.txt" "$m/runs.tsv"
echo "exclusive: $(wc -l < "$m/exclusive.txt") files, $(awk -v a="$tg" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }') s" >> "$m/MANIFEST"
LC_ALL=C sort -o "$m/runs.tsv" "$m/runs.tsv"
cut -f1 "$m"/hits/*.tsv | LC_ALL=C sort -u > "$m/files.txt"
t2=$(date +%s.%N)
echo "tests: $(wc -l < "$m/runs.tsv") files, $(awk -v a="$t1" -v b="$t2" 'BEGIN { printf "%.1f", b - a }') s" >> "$m/MANIFEST"
awk -F'\t' '$2 != 0 { print "failed (its hits kept): " $1 " status " $2 }' "$m/runs.tsv" >> "$m/MANIFEST"
awk -F'\t' '$4 == 0 { print "no hits: " $1 }' "$m/runs.tsv" >> "$m/MANIFEST"
rmdir "$m/raw" 2>/dev/null
rm -rf "$TM_WORK"
: > "$m/COMPLETE"
echo "$m"
