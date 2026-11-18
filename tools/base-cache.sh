#!/bin/sh
# base-cache.sh — what every change is compared against, built once per
# base tree and shared by every checkout on the machine.
#
#   sh tools/base-cache.sh REV        the entry's directory on stdout
#   sh tools/base-cache.sh --la REV   its LangArena part's directory
#
# REV is any revision of this repository (a branch, a commit); the entry
# is keyed by its tree id, so two commits with the same tree share one,
# and a new tree is a new entry: an entry is never used for another tree.
# Built when missing, from this checkout's tools, by the first caller;
# a second caller waits for the first (an flock on ENTRY.lock beside it)
# and then reads what it built. An entry is complete only when its
# COMPLETE file exists, written last: an entry without it (a build that
# was stopped) is removed and built again, never read.
#
# The cache's directory: BASE_CACHE, else the directory named on the first
# line of BASE_CACHE_DEFAULT (default ~/.config/avant/base-cache), else
# ~/.cache/avant/base-cache.
#
# The entry, CACHE/TREE/:
#   avant/       the tree (`git archive`), its runtime, B and C' built by
#                tools/letters.sh with this checkout's copy of the tree's
#                seed (the tree's letters.sh's default_seed, or
#                LETTERS_SEED; linked) and bin/re2c (copied)
#   ll.tsv       every program of the corpus (examples/, tests/fixtures/,
#                tests/cases/, tests/probe_matrix.av, compiler/main.av)
#                through C''s `__emit-ll`: tools/ll-digests.sh's lines,
#                the entry's avant/ written as @ROOT@
#   gcstats/     the GC-statistics set (tools/gcstats.sh)
#   MANIFEST     the tree, the revision asked for, the seed, each part's
#                build time; COMPLETE, written last
# The LangArena part, CACHE/TREE/la-DIGEST/ (DIGEST: the first 12 hex of
# the sha256 of this checkout's langarena/ harness, without tmp/ and .git,
# and of LangArena/run.js beside the checkout; another harness is another
# part):
#   avant/langarena/      the harness, copied; avant/langarena/tmp/ holds
#                         the 50 programs stitched for the prod run (the
#                         harness's own stitching: its prod pass with a
#                         compiler that records its arguments and refuses)
#   args/SLUG             the compile arguments the harness gives (after
#                         `compile ROOT BIN`), one per line
#   ll.tsv                the stitched programs through C''s `__emit-ll`
#                         from avant/langarena/tmp (tools/ll-digests.sh)
#   tasks.tsv             the harness's task files alone (langarena/tasks/,
#                         from avant/: tools/neutral.sh's corpus has them)
#   ins.tsv               SLUG, instructions, the md5 of its stdout, its
#                         status and its first line: each program compiled
#                         by C' with its arguments and run once from
#                         avant/langarena/tmp under `perf stat -e
#                         instructions:u` (a count: the machine's load does
#                         not change it), with an empty environment but
#                         PATH and HOME
#   refused.txt           only when this tree's C' refuses some of the
#                         stitched programs (the caller's harness writes its
#                         tasks in a spelling this tree does not know):
#                         `refused<TAB>N<TAB>OF` and `first<TAB>PROGRAM<TAB>
#                         its first error line`; their ins.tsv lines read
#                         compile-failed. The part is complete all the same:
#                         tools/neutral.sh reads them as not comparable
#   COMPLETE
# Used by tools/neutral.sh (a base given as an entry), tools/change-check.sh
# and tools/wave-final.sh (WAVE_BASE). Every compiler and program runs
# under `timeout`, stdin from /dev/null; the build's output goes to
# ENTRY/build.log (the LangArena part's to its build.log). Run it under the
# machine lock's shared side: it builds and counts, it does not time.
# Exit status 0 with the directory on stdout; 1 when the build failed (the
# log's tail on stderr); 2 on a usage error.
set -u

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
la=0
[ "${1:-}" = --la ] && { la=1; shift; }
[ $# -eq 1 ] || usage
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
rev=$1
tree=$(git -C "$root" rev-parse --verify -q "$rev^{tree}") || { echo "base-cache.sh: $rev: no such revision in $root" >&2; exit 2; }

cache=${BASE_CACHE:-}
if [ -z "$cache" ]; then
  cfg=${BASE_CACHE_DEFAULT:-${HOME:-/nonexistent}/.config/avant/base-cache}
  [ -f "$cfg" ] && cache=$(sed -n '1p' "$cfg")
fi
[ -n "$cache" ] || cache=${HOME:-/nonexistent}/.cache/avant/base-cache
mkdir -p "$cache" || exit 2
cache=$(realpath "$cache")
e=$cache/$tree

now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", b-a}'; }
fail() { # LOG WHY
  echo "base-cache.sh: $2 (log: $1)" >&2
  tail -n 15 "$1" >&2
  exit 1
}

# build_entry: the tree, its letters, the corpus, the statistics (in $e).
build_entry() {
  rm -rf "$e"; mkdir -p "$e/avant/bin" || exit 1
  log=$e/build.log
  {
    echo "tree: $tree"
    echo "asked as: $rev ($(git -C "$root" rev-parse --verify -q "$rev^{commit}" 2>/dev/null || echo 'not a commit'))"
    echo "built by: $root"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$e/MANIFEST"
  t0=$(now)
  git -C "$root" archive --format=tar "$tree" | tar -xf - -C "$e/avant" || fail "$log" "git archive of $tree failed"
  # The tree's seed (its tools/letters.sh's default_seed, or LETTERS_SEED),
  # linked to this checkout's copy.
  seed=${LETTERS_SEED:-$(sed -n 's/^default_seed=//p' "$e/avant/tools/letters.sh" | head -n 1)}
  seed=${seed%/}
  [ -n "$seed" ] || { echo "the tree's tools/letters.sh names no default_seed" > "$log"; fail "$log" "no seed named"; }
  case "$seed" in /*) src=$seed ;; *) src=$root/$seed ;; esac
  [ -x "$src/avant" ] || { echo "no seed at $src (this checkout's copy of the tree's seed $seed)" > "$log"; fail "$log" "this checkout has no seed $seed"; }
  case "$seed" in /*) ;; *) mkdir -p "$e/avant/$(dirname "$seed")"; ln -s "$(realpath "$src")" "$e/avant/$seed" ;; esac
  [ -f "$root/bin/re2c" ] && cp "$root/bin/re2c" "$e/avant/bin/re2c"
  echo "seed: $seed -> $(realpath "$src")" >> "$e/MANIFEST"
  (cd "$e/avant" && timeout -k 10 1500 sh tools/letters.sh runtime b c-llvm < /dev/null > "$log" 2>&1) || fail "$log" "the letters of $tree did not build"
  t1=$(now); echo "letters: $(secs "$t0" "$t1") s" >> "$e/MANIFEST"
  (cd "$e/avant" && find examples tests/fixtures tests/cases tests/probe_matrix.av compiler/main.av -name '*.av' 2>/dev/null | LC_ALL=C sort) > "$e/list.txt"
  timeout -k 10 1800 sh "$here/ll-digests.sh" "$e/avant" "$e/list.txt" "$e/ll.tsv" < /dev/null >> "$log" 2>&1 || fail "$log" "the corpus's __emit-ll failed"
  t2=$(now); echo "corpus: $(wc -l < "$e/ll.tsv") programs, $(secs "$t1" "$t2") s" >> "$e/MANIFEST"
  timeout -k 10 1800 sh "$here/gcstats.sh" "$e/avant" "$e/gcstats" < /dev/null >> "$log" 2>&1 || fail "$log" "a GC-statistics fixture did not compile"
  t3=$(now); echo "gcstats: $(ls "$e/gcstats" | grep -c '\.\(out\|err\)$') files, $(secs "$t2" "$t3") s" >> "$e/MANIFEST"
  echo "entry: $(secs "$t0" "$t3") s" >> "$e/MANIFEST"
  : > "$e/COMPLETE"
}

# The LangArena part's key: the harness and the configuration it reads.
la_digest() {
  [ -f "$root/langarena/harness/measure.cr" ] || { echo "base-cache.sh: no langarena/ harness in $root (copy it in)" >&2; exit 2; }
  [ -f "$root/../LangArena/run.js" ] || { echo "base-cache.sh: no LangArena/run.js beside $root" >&2; exit 2; }
  {
    (cd "$root" && find langarena -path langarena/tmp -prune -o -path langarena/.git -prune -o -type f -print | LC_ALL=C sort | xargs sha256sum)
    sha256sum < "$root/../LangArena/run.js"
  } | sha256sum | cut -c1-12
}

# build_la DIR: the stitched programs, their .ll, their instruction counts.
build_la() {
  p=$1
  rm -rf "$p"; mkdir -p "$p/avant/tools" "$p/args" "$p/stitch" || exit 1
  log=$p/build.log; : > "$log"
  t0=$(now)
  (cd "$root" && tar --exclude=./langarena/tmp --exclude=./langarena/.git -cf - langarena) | tar -xf - -C "$p/avant" || fail "$log" "copying langarena/ failed"
  cp "$here/la-harness.sh" "$p/avant/tools/la-harness.sh"
  ln -s "$(realpath "$root/../LangArena")" "$p/LangArena"
  # The harness built where it stitches (it finds avant/ and LangArena/ from
  # its own source's place), then its prod pass with a compiler that only
  # records what it is given.
  h=$(cd "$p/avant" && timeout -k 10 900 sh tools/la-harness.sh 2>> "$log" < /dev/null) || fail "$log" "the harness did not build"
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
    timeout -k 10 900 "$h" prod < /dev/null >> "$log" 2>&1)
  n=$(ls "$p/args" | wc -l)
  [ "$n" -gt 0 ] || fail "$log" "the harness stitched no program"
  # The harness's own binary is the cache's no longer: only the stitched sources stay.
  rm -rf "$p/avant/langarena/tmp/harness" "$p/stitch/results"
  ls "$p/args" | LC_ALL=C sort | sed 's/$/.av/' > "$p/list.txt"
  t1=$(now); echo "stitch: $n programs, $(secs "$t0" "$t1") s" >> "$p/MANIFEST"
  tmp=$p/avant/langarena/tmp
  timeout -k 10 1800 sh "$here/ll-digests.sh" "$e/avant" "$p/list.txt" "$p/ll.tsv" "$tmp" < /dev/null >> "$log" 2>&1 || fail "$log" "the stitched programs' __emit-ll failed"
  (cd "$p/avant" && find langarena/tasks -name '*.av' | LC_ALL=C sort) > "$p/tasks-list.txt"
  timeout -k 10 1800 sh "$here/ll-digests.sh" "$e/avant" "$p/tasks-list.txt" "$p/tasks.tsv" "$p/avant" < /dev/null >> "$log" 2>&1 || fail "$log" "the task files' __emit-ll failed"
  t2=$(now); echo "ll: $(secs "$t1" "$t2") s" >> "$p/MANIFEST"
  if ! timeout -k 10 3600 sh "$here/la-count.sh" "$e/avant" "$p" "$p/ins.tsv" < /dev/null >> "$log" 2>&1; then
    # A program this tree's C' refuses (its __emit-ll in ll.tsv failed too:
    # the caller's harness writes its tasks in a spelling this older tree
    # does not know) is recorded, in refused.txt, not a failure of the part;
    # any other (a program that failed to run, or to compile when its
    # __emit-ll did not) still is.
    [ -f "$p/ins.tsv" ] || fail "$log" "the instruction counts failed"
    awk -F'\t' 'FILENAME == ARGV[1] { s = $1; sub(/\.av$/, "", s); rc[s] = $2; next }
      $2 == "compile-failed" && rc[$1] != 0 { next }
      $2 == "compile-failed" || $4 != 0 { print; bad = 1 } END { exit bad }' "$p/ll.tsv" "$p/ins.tsv" > "$p/count-failures.txt" \
      || { cat "$p/count-failures.txt" >> "$log"; fail "$log" "the instruction counts failed"; }
  fi
  rm -f "$p/count-failures.txt"
  nref=$(awk -F'\t' '$2 != 0' "$p/ll.tsv" | wc -l)
  if [ "$nref" -gt 0 ]; then
    first=$(awk -F'\t' '$2 != 0 { print $1; exit }' "$p/ll.tsv")
    msg=$(cd "$tmp" && env -u AVANT_ROOT timeout -k 5 300 "$e/avant/bin/avant-c-llvm" __emit-ll "$first" "$p/.refused.ll" < /dev/null 2>&1)
    rm -f "$p/.refused.ll"
    msg=$(printf '%s\n' "$msg" | sed "s#$tmp/##g; s#$e/avant#@ROOT@#g" | grep -m 1 -i error || printf '%s\n' "$msg" | head -n 1)
    printf 'refused\t%s\t%s\nfirst\t%s\t%s\n' "$nref" "$(wc -l < "$p/ll.tsv")" "$first" "$(printf '%s' "$msg" | tr '\t' ' ')" > "$p/refused.txt"
    echo "refused: $nref of $(wc -l < "$p/ll.tsv") programs by this tree's C' (refused.txt)" >> "$p/MANIFEST"
  fi
  t3=$(now); echo "instructions: $(secs "$t2" "$t3") s" >> "$p/MANIFEST"
  : > "$p/COMPLETE"
}

# ensure DIR BUILD: DIR complete, built under DIR.lock when it is not.
ensure() {
  [ -f "$1/COMPLETE" ] && return 0
  exec 9> "$1.lock" || exit 1
  if ! flock -n 9; then
    echo "base-cache.sh: waiting for another build of $1" >&2
    flock 9 || exit 1
  fi
  if [ ! -f "$1/COMPLETE" ]; then
    echo "base-cache.sh: building $1" >&2
    t=$(now)
    "$2" "$1"
    echo "base-cache.sh: built $1 in $(secs "$t" "$(now)") s" >&2
  else
    echo "base-cache.sh: $1 was built meanwhile; reused" >&2
  fi
  exec 9>&-
}

ensure "$e" build_entry
if [ $la -eq 0 ]; then echo "$e"; exit 0; fi
d=$(la_digest) || exit 2
ensure "$e/la-$d" build_la
echo "$e/la-$d"
