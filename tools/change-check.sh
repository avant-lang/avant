#!/bin/sh
# change-check.sh — what each commit of a range changed, commit by commit:
# a quick check after every change, not a gate.
#
#   sh tools/change-check.sh BASE HEAD OUT     (from the checkout root)
#
# For each commit of BASE..HEAD (first parents, oldest first), against the
# commit before it:
#
#   tests     the test files its changed files touch, run by its C'
#             (`avant test`, AVANT_COMPILER and AVANT_BACKEND unset, a
#             private TMPDIR): a changed test file itself; a test file
#             whose name holds a changed file's stem (`avant_` and `test_`
#             taken off: compiler/format.av touches test_format.av); a test
#             file that names a changed file's path (tests/fixtures/x/y.av
#             as fixtures/x/y), or imports a changed library module
#             (lib/std/json.av as std.json); test_tools.av for a change
#             under tools/ or tests/tools/. Only tests/cases/test_*.av
#             files run (a helper beside them, such as harness.av, is not a
#             test). With a test map (tools/test-map.sh: TEST_MAP, else the
#             base cache entry's testmap/ when it was built) also every test
#             file whose run executed a line the commit changes, or the
#             function or class around it (`test-map.sh --pick`); the cell
#             then reads `N failed, M passed: NAMES; by the map, K (J more):
#             NAMES`, the first names by name, path and import, the second
#             those only the map chose. A changed file none of these
#             reaches is left to the corpus and the gate.
#   corpus    every program of examples/, tests/fixtures/, tests/cases/,
#             tests/probe_matrix.av and compiler/main.av through
#             `__emit-ll` (tools/ll-digests.sh): the programs whose .ll or
#             message moved, by name, and their count
#   langarena the 50 LangArena programs as the harness stitches them (the
#             base cache's LangArena part) through `__emit-ll`: the moved
#             ones, by name
#   instr     instruction counts (`perf stat -e instructions:u`,
#             tools/la-count.sh) of the LangArena programs whose .ll moved
#             (all 50 when runtime/ changed: the runtime moves the
#             programs' code without moving their .ll), the ratio to the
#             previous count, and whether the program's output stayed the
#             same
#
# One line per commit in OUT/table.md (and on stdout), the details in
# OUT/NN-SHORT/ (changed.txt, tests.txt and the suite's log, corpus.txt and
# la.txt with every moved program's reading, ins.tsv). The commit before
# the first one is read from the base cache (tools/base-cache.sh, built
# when missing: its C', its corpus digests, its LangArena digests and
# counts); every later commit is compared with the one before it, from
# what this run computed. What did not change is reused: C' is rebuilt only
# when compiler/, lib/ or runtime/ (or the build's own scripts) changed,
# and then every program is compiled again; otherwise only the corpus
# programs the commit changed are. A count is deterministic for a binary,
# so a program whose .ll did not move keeps its count.
#
# The commits are checked out in OUT/work/avant, a detached worktree of
# this repository, removed at the end (`git worktree prune` clears it if
# the run was stopped). Its seed and bin/re2c are this checkout's. Run it
# under the machine lock's shared side (it builds, compiles and counts; it
# does not time), with stdin from /dev/null. Status 0 when every commit's
# tests passed and every counted program kept its output, 1 otherwise, 2 on
# a usage error.
set -u

[ $# -eq 3 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd -P)
cd "$root" || exit 2
base=$1 head=$2 out=$(realpath -m "$3")
[ -e "$out" ] && { echo "change-check.sh: $out exists" >&2; exit 2; }
git rev-parse --verify -q "$base^{commit}" > /dev/null || { echo "change-check.sh: $base: no such commit" >&2; exit 2; }
git rev-parse --verify -q "$head^{commit}" > /dev/null || { echo "change-check.sh: $head: no such commit" >&2; exit 2; }
commits=$(git rev-list --reverse --first-parent "$base..$head")
[ -n "$commits" ] || { echo "change-check.sh: no commit in $base..$head" >&2; exit 2; }
mkdir -p "$out/state" || exit 2
now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", b-a}'; }
log=$out/run.log

# The commit before the first: the base cache of its tree.
first=$(echo "$commits" | head -n 1)
t0=$(now)
e=$(sh "$here/base-cache.sh" "$first^" 2>> "$log" < /dev/null) || { echo "change-check.sh: the base cache failed (run.log)" >&2; exit 1; }
lap=$(sh "$here/base-cache.sh" --la "$first^" 2>> "$log" < /dev/null) || { echo "change-check.sh: the base cache's LangArena part failed (run.log)" >&2; exit 1; }
echo "base cache: $e, $lap ($(secs "$t0" "$(now)") s, $(grep -c 'built ' "$log") built)" >> "$out/cache.txt"
# The entry's lock held shared while it is read: tools/base-cache.sh
# --prune removes no entry a run holds.
exec 7>> "$e.lock" && flock -s 7
cp "$e/ll.tsv" "$out/state/corpus.tsv"
# The test map (tools/test-map.sh): TEST_MAP, else the base entry's own.
tmap=${TEST_MAP:-}
[ -z "$tmap" ] && [ -f "$e/testmap/COMPLETE" ] && tmap=$e/testmap
if [ -n "$tmap" ] && [ -f "$tmap/COMPLETE" ]; then echo "test map: $tmap (tree $(sed -n 's/^tree: //p' "$tmap/MANIFEST"))" >> "$out/cache.txt"
else tmap=""; echo "test map: none (sh tools/test-map.sh $first^ builds the base's; TEST_MAP names another)" >> "$out/cache.txt"; fi
cp "$lap/ll.tsv" "$out/state/la.tsv"
cp "$lap/ins.tsv" "$out/state/ins.tsv"

# The work tree, its seed and re2c as this checkout's.
work=$out/work/avant
git worktree add -q --detach "$work" "$first^" > /dev/null 2>> "$log" < /dev/null || { echo "change-check.sh: git worktree add failed (run.log)" >&2; exit 1; }
cleanup() { git -C "$root" worktree remove --force "$work" > /dev/null 2>&1; rm -rf "$out/work"; }
trap 'cleanup; exit 130' INT TERM HUP
seed=$(sed -n 's/^default_seed=//p' tools/letters.sh | head -n 1); seed=${seed%/}
mkdir -p "$work/bin"
case "$seed" in /*|"") ;; *) [ -e "$root/$seed" ] && mkdir -p "$work/$(dirname "$seed")" && ln -s "$(realpath "$root/$seed")" "$work/$seed" ;; esac
[ -f bin/re2c ] && cp bin/re2c "$work/bin/re2c"
# Before the first build: the base cache's letters, as if built here (the
# same tree, so the same lib/; the compiler's record names the work tree's).
mkdir -p "$work/runtime/build"
cp -a "$e/avant/runtime/build/." "$work/runtime/build/"
for x in avant-b avant-c-llvm; do
  cp -a "$e/avant/bin/$x" "$work/bin/$x"
  sed "s#^lib: .*#lib: $(cd "$work" && pwd -P)/lib#" "$e/avant/bin/$x.versions.txt" > "$work/bin/$x.versions.txt"
done
ptree=$(git rev-parse "$first^^{tree}")

touched() { # the changed paths on stdin -> test files of the work tree
  while IFS= read -r p; do
    case "$p" in tests/cases/test_*.av) [ -f "$work/$p" ] && echo "$p" ;; esac
    case "$p" in tools/*|tests/tools/*) [ -f "$work/tests/cases/test_tools.av" ] && echo tests/cases/test_tools.av ;; esac
    stem=$(basename "$p"); stem=${stem%.*}; stem=${stem#avant_}; stem=${stem#test_}
    if [ ${#stem} -ge 3 ]; then
      for t in "$work"/tests/cases/test_*"$stem"*.av; do [ -f "$t" ] && echo "tests/cases/$(basename "$t")"; done
    fi
    case "$p" in
      lib/*.av) m=$(echo "${p#lib/}" | sed 's/\.av$//; s#/#.#g'); grep -l -E "^import $m( |\$)" "$work"/tests/cases/*.av 2>/dev/null ;;
      tests/cases/*.av) m=$(basename "$p" .av); case "$m" in test_*) ;; *) grep -l -E "^import $m( |\$)" "$work"/tests/cases/*.av 2>/dev/null ;; esac ;;
      tests/*) n=${p#tests/}; n=${n%.*}; grep -l -F "$n" "$work"/tests/cases/*.av 2>/dev/null ;;
      *) grep -l -F "$p" "$work"/tests/cases/*.av 2>/dev/null ;;
    esac | sed "s#^$work/##"
  done | grep -E '^tests/cases/test_[^/]*\.av$' | LC_ALL=C sort -u
}

printf '| Commit | Subject | Files | C'"'"' | Tests | Corpus moved | LangArena moved | Instructions (new/old) | Time |\n| --- | --- | ---: | --- | --- | --- | --- | --- | ---: |\n' > "$out/table.md"
bad=0; i=0
for c in $commits; do
  i=$((i + 1)); t0=$(now)
  short=$(git rev-parse --short=8 "$c"); subj=$(git log -1 --format=%s "$c" | sed 's/|/\\|/g')
  d=$out/$(printf '%02d' $i)-$short; mkdir -p "$d"
  git diff --name-only "$c^" "$c" > "$d/changed.txt"
  git -C "$work" checkout -q --detach "$c" < /dev/null 2>> "$log" || { echo "change-check.sh: checkout of $c failed" >&2; cleanup; exit 1; }
  # Rebuild when the compiler's inputs changed since the letters in bin/.
  ctree=$(git rev-parse "$c^{tree}")
  if [ -n "$(git diff --name-only "$ptree" "$ctree" -- compiler lib runtime tools/letters.sh tools/build-llvm.sh)" ]; then
    rebuilt=yes
    (cd "$work" && timeout -k 10 1500 sh tools/letters.sh runtime b c-llvm < /dev/null > "$d/letters.log" 2>&1) || { echo "| $short | $subj | $(wc -l < "$d/changed.txt") | build failed | | | | | |" >> "$out/table.md"; bad=1; break; }
    ptree=$ctree
  else rebuilt=no; fi
  runtime_moved=no
  grep -q '^runtime/' "$d/changed.txt" && runtime_moved=yes
  # The corpus: every program again after a rebuild, else the changed ones.
  (cd "$work" && find examples tests/fixtures tests/cases tests/probe_matrix.av compiler/main.av -name '*.av' 2>/dev/null | LC_ALL=C sort) > "$d/list.txt"
  if [ $rebuilt = yes ]; then cp "$d/list.txt" "$d/compile.txt"
  else
    { grep -F -x -f "$d/list.txt" "$d/changed.txt"; cut -f1 "$out/state/corpus.tsv" | LC_ALL=C sort > "$d/known.txt"; LC_ALL=C comm -23 "$d/list.txt" "$d/known.txt"; } | LC_ALL=C sort -u > "$d/compile.txt"
  fi
  : > "$d/new.tsv"
  [ -s "$d/compile.txt" ] && sh "$here/ll-digests.sh" "$work" "$d/compile.txt" "$d/new.tsv" < /dev/null 2>> "$log"
  # The commit's corpus state: the recompiled rows, the rest carried over, the removed dropped.
  awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $0; next } FILENAME == ARGV[2] { if ($1 in n) print n[$1]; else print; next }' \
    "$d/new.tsv" "$out/state/corpus.tsv" > "$d/carried.tsv"
  { cat "$d/carried.tsv"; awk -F'\t' 'FILENAME == ARGV[1] { k[$1] = 1; next } !($1 in k)' "$out/state/corpus.tsv" "$d/new.tsv"; } \
    | awk -F'\t' 'FILENAME == ARGV[1] { l[$1] = 1; next } ($1 in l)' "$d/list.txt" - | LC_ALL=C sort > "$d/corpus.tsv"
  cmpr() { # OLD NEW -> the moved programs' readings
    awk -F'\t' 'FILENAME == ARGV[1] { rc[$1] = $2; ll[$1] = $3; lg[$1] = $4; next }
      { seen[$1] = 1 }
      !($1 in rc) { print "new " $1; next }
      rc[$1] == 0 && $2 == 0 { if (ll[$1] != $3) print "LL-DIFF " $1; next }
      rc[$1] == $2 { if (lg[$1] != $4) print "refused-msg " $1; next }
      { print "STATUS " rc[$1] "->" $2 " " $1 }
      END { for (p in rc) if (!(p in seen)) print "removed " p }' "$1" "$2" | LC_ALL=C sort -k2
  }
  cmpr "$out/state/corpus.tsv" "$d/corpus.tsv" > "$d/corpus.txt"
  cp "$d/corpus.tsv" "$out/state/corpus.tsv"
  # LangArena: after a rebuild, the stitched programs again.
  if [ $rebuilt = yes ]; then
    ls "$lap/avant/langarena/tmp" | grep '\.av$' | grep -v '\.cfg\.av$' | LC_ALL=C sort > "$d/la-list.txt"
    sh "$here/ll-digests.sh" "$work" "$d/la-list.txt" "$d/la.tsv" "$lap/avant/langarena/tmp" < /dev/null 2>> "$log"
    cmpr "$out/state/la.tsv" "$d/la.tsv" > "$d/la.txt"
    cp "$d/la.tsv" "$out/state/la.tsv"
  else : > "$d/la.txt"; fi
  # Counts: the moved LangArena programs, or all of them after a runtime change.
  if [ $runtime_moved = yes ] && [ $rebuilt = yes ]; then cut -f1 "$out/state/la.tsv" | sed 's/\.av$//' > "$d/count.txt"
  else awk '{ print $NF }' "$d/la.txt" | sed 's/\.av$//' > "$d/count.txt"; fi
  ins="-"
  if [ -s "$d/count.txt" ]; then
    # shellcheck disable=SC2046
    sh "$here/la-count.sh" "$work" "$lap" "$d/ins.tsv" $(cat "$d/count.txt") < /dev/null 2>> "$log"
    ins=$(awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $2; m[$1] = $3; next }
      { r = ($2 ~ /^[0-9]+$/ && n[$1] ~ /^[0-9]+$/ && n[$1] > 0) ? sprintf("%.4f", $2 / n[$1]) : (n[$1] == "compile-failed" && $2 ~ /^[0-9]+$/) ? "new (the base refused it)" : "?"
        o = ($3 == m[$1] || n[$1] == "compile-failed") ? "" : " OUTPUT CHANGED"
        s = s (s == "" ? "" : ", ") $1 " " r o }
      END { print s }' "$out/state/ins.tsv" "$d/ins.tsv")
    grep -q 'OUTPUT CHANGED\| ?' <<EOF && bad=1
$ins
EOF
    awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $0; next } { if ($1 in n) print n[$1]; else print }' "$d/ins.tsv" "$out/state/ins.tsv" > "$d/ins-state.tsv"
    mv "$d/ins-state.tsv" "$out/state/ins.tsv"
  fi
  # The touched tests, with this commit's C': by name, path and import,
  # and by the test map (the tests whose hits meet the changed lines).
  touched < "$d/changed.txt" > "$d/tests-name.txt"
  : > "$d/tests-map.txt"
  if [ -n "$tmap" ]; then
    # shellcheck disable=SC2046
    TMPDIR=$out sh "$here/test-map.sh" --pick "$tmap" "$c^" "$c" $(cat "$d/changed.txt") < /dev/null 2>> "$log" \
      | while IFS= read -r t; do [ -f "$work/$t" ] && echo "$t"; done > "$d/tests-map.txt"
  fi
  LC_ALL=C sort -u "$d/tests-name.txt" "$d/tests-map.txt" > "$d/tests.txt"
  LC_ALL=C comm -13 "$d/tests-name.txt" "$d/tests-map.txt" > "$d/tests-maponly.txt"
  tests="none matched"
  if [ -s "$d/tests.txt" ]; then
    mkdir -p "$d/tmp"
    # shellcheck disable=SC2046
    (cd "$work" && env -u AVANT_COMPILER -u AVANT_BACKEND TMPDIR="$d/tmp" timeout -k 10 1800 ./bin/avant-c-llvm test $(cat "$d/tests.txt") < /dev/null > "$d/tests.log" 2>&1)
    trc=$?
    rm -rf "$d/tmp"
    sn() { if [ -s "$1" ]; then sed 's#tests/cases/##; s#\.av$##' "$1" | tr '\n' ' ' | sed 's/ $//'; else echo -; fi; }
    tests="$(grep -E '^[0-9]+ failed, [0-9]+ passed' "$d/tests.log" | tail -n 1): $(sn "$d/tests-name.txt")"
    [ -n "$tmap" ] && tests="$tests; by the map, $(wc -l < "$d/tests-map.txt") ($(wc -l < "$d/tests-maponly.txt") more): $(sn "$d/tests-maponly.txt")"
    [ $trc -eq 0 ] || { bad=1; tests="FAILED (rc=$trc) $tests"; }
  fi
  names() { # FILE: "N: a, b" (at most 6 names)
    n=$(wc -l < "$1")
    if [ "$n" -eq 0 ]; then echo 0; else echo "$n: $(head -n 6 "$1" | sed 's/^changed .*) //; s/^[^ ]* //; s/^[^ ]*->[^ ]* //' | tr '\n' ',' | sed 's/,$//; s/,/, /g')$([ "$n" -gt 6 ] && echo ", ...")"; fi
  }
  la_col=$( [ $rebuilt = yes ] && names "$d/la.txt" || echo "- (no rebuild)")
  line="| $short | $subj | $(wc -l < "$d/changed.txt") | $([ $rebuilt = yes ] && echo rebuilt || echo kept) | $tests | $(names "$d/corpus.txt") | $la_col | $ins | $(secs "$t0" "$(now)") s |"
  echo "$line" >> "$out/table.md"
  echo "$line"
done
cleanup
trap - INT TERM HUP
exit $bad
