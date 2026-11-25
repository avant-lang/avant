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
#             under tools/ or tests/tools/; the test file that checks a
#             dump golden (tests/goldens/list.txt: a changed SOURCE of a
#             row, or a changed golden of its STEM, selects the row's TEST,
#             as examples/http.av selects test_goldens.av); test_examples.av
#             for a changed examples/STEM.av or tests/expected/STEM.out
#             when tests/expected/STEM.out exists (the example it runs).
#             Only tests/cases/test_*.av files run (a helper beside them,
#             such as harness.av, is not a test). With a test map
#             (tools/test-map.sh) also every test file whose run executed a
#             line the commit changes, or the function or class around it
#             (`test-map.sh --pick`); the cell then reads `N failed, M
#             passed: NAMES; by the map, K (J more): NAMES`, the first names
#             by name, path and import, the second those only the map
#             chose. The map is TEST_MAP, else the base cache entry's
#             testmap/, built for the base when the range changes a file
#             under compiler/ or lib/ (the compiler's sources: a checker
#             change selects the tests whose runs execute it) and none is
#             there yet, once per base tree (about 8 to 15 minutes here, in
#             shared runs of a few minutes each); TEST_MAP=none reads no map. A changed file none of
#             these reaches is left to the corpus and the gate.
#   corpus    every program of examples/, tests/fixtures/, tests/cases/,
#             tests/probe_matrix.av and compiler/main.av through
#             `__emit-ll` (tools/ll-digests.sh): the programs whose .ll or
#             message moved, by name, and their count
#   langarena the 50 LangArena programs as the harness stitches them (the
#             base cache's LangArena part) through `__emit-ll`: the moved
#             ones, by name. The base's side is the base compiler on this
#             checkout's langarena/; when the base compiler refuses some of
#             those programs (this checkout's harness, or its tasks, use a
#             name or a spelling the base does not know, such as a helper
#             defining a name the base holds as a builtin), the base's
#             side is read from the base's own harness instead:
#             CC_BASE_LANGARENA (a langarena/ directory), else the main
#             worktree's langarena/ when it differs from this one's
#             (cache.txt names the one read). The first commit's LangArena
#             cell then compares two harnesses' programs (each moved, and
#             counted against the base's own)
#   instr     instruction counts (`perf stat -e instructions:u`,
#             tools/la-count.sh) of the LangArena programs whose .ll moved
#             (all 50 when runtime/ changed: the runtime moves the
#             programs' code without moving their .ll), the ratio to the
#             previous count, and whether the program's output stayed the
#             same. A commit that changes the hash function, the hash tables
#             or their probing (a file of hash_files below:
#             runtime/avant_hash.c, compiler/mir/lower_hash.av,
#             compiler/mir/upsert.av) counts under the 12 hash seeds of
#             tools/la-count.sh (LA_SEEDS=12), on both sides: the commit
#             before it is counted again under the same seeds (before the
#             commit is built over it), and the cell reads the ratio of the
#             means with the per-seed ratios' range beside it, `SLUG 1.0012
#             (12 seeds: 0.9990-1.0031)`; one seed can place a task's keys
#             well or badly. The other commits of the range count at seed 0
#             against the seed-0 count of the commit before (a range of eight
#             merges with one hash commit counted every commit under 12 seeds
#             once, about 17 minutes a pass for one program alone).
#             CC_SEEDS=N forces N seeds (1 to 12) on every commit, CC_SEEDS=0
#             seed 0 alone; cache.txt says which commits
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
# the run was stopped). Its seed and bin/re2c are this checkout's. It
# builds, compiles and counts; it does not time: the machine lock's shared
# side. With CC_RUNNER (the runner's command words, such as `python3 -I
# PATH/ledger run`), the script is started outside the lock and each piece
# of work takes it through the runner, one shared run each (`change-check:
# base cache`, `change-check: test map`, then per commit `change-check:
# SHORT build`, `SHORT corpus`, `SHORT langarena`, `SHORT counts`, `SHORT
# tests`; the test map's build and each group of its test files, `test map
# build`, `test map group K/N`, `test map exclusive`, tools/test-map.sh; CC_RUN_KIND their --kind, default new): no run holds the lock
# for the whole range, so a whole-machine run waiting meanwhile goes
# between two of them. Without CC_RUNNER the caller holds the lock (one
# `ledger run` around the script, as before). Stdin from /dev/null.
# Status 0 when every commit's tests passed and every counted program kept
# its output, 1 otherwise, 2 on a usage error.
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
bad=0
# rn STEP CMD...: CMD through CC_RUNNER on the lock's shared side, as
# `change-check: STEP`; without CC_RUNNER, as it is.
runner=${CC_RUNNER:-}
rn() {
  r_step=$1; shift
  if [ -n "$runner" ]; then
    # shellcheck disable=SC2086
    $runner --step "change-check: $r_step" --kind "${CC_RUN_KIND:-new}" -- "$@"
  else "$@"; fi
}
now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", b-a}'; }
log=$out/run.log

# The commit before the first: the base cache of its tree.
first=$(echo "$commits" | head -n 1)
t0=$(now)
e=$(rn "base cache" sh "$here/base-cache.sh" "$first^" 2>> "$log" < /dev/null) || { echo "change-check.sh: the base cache failed (run.log)" >&2; exit 1; }
lap=$(rn "base cache" sh "$here/base-cache.sh" --la "$first^" 2>> "$log" < /dev/null) || { echo "change-check.sh: the base cache's LangArena part failed (run.log)" >&2; exit 1; }
echo "base cache: $e, $lap ($(secs "$t0" "$(now)") s, $(grep -c 'built ' "$log") built)" >> "$out/cache.txt"
# Hash seeds (above): a commit that changes the hash code counts under 12
# seeds, both sides; CC_SEEDS forces a number on every commit.
hash_files="runtime/avant_hash.c compiler/mir/lower_hash.av compiler/mir/upsert.av"
forced=${CC_SEEDS:-}
case "$forced" in ''|[0-9]|1[0-2]) ;; *) echo "change-check.sh: CC_SEEDS is a number from 0 to 12" >&2; exit 2 ;; esac
hash_commits=""
for hc in $commits; do
  # shellcheck disable=SC2086
  [ -n "$(git diff --name-only "$hc^" "$hc" -- $hash_files)" ] && hash_commits="$hash_commits $(git rev-parse --short=8 "$hc")"
done
if [ -n "$forced" ]; then
  if [ "$forced" -gt 0 ]; then echo "hash seeds: every commit under $forced seeds of tools/la-count.sh, both sides (CC_SEEDS=$forced)" >> "$out/cache.txt"
  else echo "hash seeds: seed 0 alone on every commit (CC_SEEDS=0)" >> "$out/cache.txt"; fi
elif [ -n "$hash_commits" ]; then
  echo "hash seeds: the commits that change $hash_files ($(echo $hash_commits | tr ' ' ',')) count under 12 seeds of tools/la-count.sh, both sides; the others at seed 0" >> "$out/cache.txt"
else echo "hash seeds: seed 0 alone (no file of $hash_files changed)" >> "$out/cache.txt"; fi
# The base's side of LangArena: its own harness when the base compiler
# refuses some of this checkout's stitched programs.
blap=$lap
if [ -f "$lap/refused.txt" ]; then
  bla=${CC_BASE_LANGARENA:-}
  if [ -z "$bla" ]; then
    mw=$(git worktree list --porcelain | sed -n '1s/^worktree //p')
    [ -n "$mw" ] && [ -f "$mw/langarena/harness/measure.cr" ] && [ "$(realpath "$mw")" != "$root" ] && bla=$mw/langarena
  fi
  if [ -n "$bla" ]; then
    bla=$(realpath "$bla")
    blap=$(BASE_CACHE_LANGARENA=$bla rn "base cache" sh "$here/base-cache.sh" --la "$first^" 2>> "$log" < /dev/null) \
      || { echo "change-check.sh: the base cache's LangArena part from $bla failed (run.log)" >&2; exit 1; }
    if [ "$blap" = "$lap" ]; then
      echo "langarena: the base refuses $(sed -n '1p' "$lap/refused.txt" | cut -f2) of this checkout's programs, and $bla is this checkout's harness (give the base's own as CC_BASE_LANGARENA): their counts read new" >> "$out/cache.txt"
    else echo "langarena: the base refuses $(sed -n '1p' "$lap/refused.txt" | cut -f2) of this checkout's programs; the base's side read from its own harness $bla ($blap$([ -f "$blap/refused.txt" ] && echo ", which it refuses $(sed -n '1p' "$blap/refused.txt" | cut -f2) of"))" >> "$out/cache.txt"; fi
  else
    echo "langarena: the base refuses $(sed -n '1p' "$lap/refused.txt" | cut -f2) of this checkout's programs, and no harness of its own is known (CC_BASE_LANGARENA): their counts read new" >> "$out/cache.txt"
  fi
fi
# The entry's lock held shared while it is read: tools/base-cache.sh
# --prune removes no entry a run holds.
exec 7>> "$e.lock" && flock -s 7
cp "$e/ll.tsv" "$out/state/corpus.tsv"
# The test map (tools/test-map.sh): TEST_MAP, else the base entry's own,
# built when the range changes the compiler's sources and it is missing.
tmap=${TEST_MAP:-}
if [ "$tmap" = none ]; then tmap=""; tmwhy="none (TEST_MAP=none)"
else
  tmwhy="none (sh tools/test-map.sh $first^ builds the base's; TEST_MAP names another)"
  if [ -z "$tmap" ] && [ ! -f "$e/testmap/COMPLETE" ] && [ -n "$(git diff --name-only "$first^" "$head" -- compiler lib)" ]; then
    t1=$(now)
    # test-map.sh takes the lock itself, one shared run for the build and one
    # for each group of test files (TESTMAP_RUNNER), so none passes the limit.
    if m=$(TESTMAP_RUNNER="$runner" TESTMAP_STEP="change-check: test map" TESTMAP_KIND="${CC_RUN_KIND:-new}" sh "$here/test-map.sh" "$first^" 2>> "$log" < /dev/null) && [ -f "$m/COMPLETE" ]; then
      echo "test map: built for the base, the range changing compiler/ or lib/ ($(secs "$t1" "$(now)") s)" >> "$out/cache.txt"
    else tmwhy="none: its build failed (run.log), so the tests by name, path and import only"; bad=1; fi
  fi
  [ -z "$tmap" ] && [ -f "$e/testmap/COMPLETE" ] && tmap=$e/testmap
fi
if [ -n "$tmap" ] && [ -f "$tmap/COMPLETE" ]; then echo "test map: $tmap (tree $(sed -n 's/^tree: //p' "$tmap/MANIFEST"))" >> "$out/cache.txt"
else tmap=""; echo "test map: $tmwhy" >> "$out/cache.txt"; fi
cp "$blap/ll.tsv" "$out/state/la.tsv"
cp "$blap/ins.tsv" "$out/state/ins.tsv"

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
    # The dump goldens' list: a row's SOURCE, or a golden of its STEM.
    if [ -f "$work/tests/goldens/list.txt" ]; then
      g=""; case "$p" in tests/goldens/*) g=${p#tests/goldens/}; g=${g%.*} ;; esac
      awk -v p="$p" -v g="$g" '!/^#/ && NF >= 3 && ($3 == p || $2 == g) { print "tests/cases/" $1 ".av" }' "$work/tests/goldens/list.txt"
    fi
    # The examples test runs examples/STEM.av against tests/expected/STEM.out.
    case "$p" in examples/*.av|tests/expected/*.out)
      x=$(basename "$p"); x=${x%.*}
      [ -f "$work/tests/expected/$x.out" ] && [ -f "$work/tests/cases/test_examples.av" ] && echo tests/cases/test_examples.av ;;
    esac
    stem=$(basename "$p"); stem=${stem%.*}; stem=${stem#avant_}; stem=${stem#test_}
    if [ ${#stem} -ge 3 ]; then
      for t in "$work"/tests/cases/test_*"$stem"*.av; do [ -f "$t" ] && echo "tests/cases/$(basename "$t")"; done
    fi
    # A compiler pass's file is named for the pass and what it handles
    # (mir/lower_ffi.av lowers the FFI forms): the test files named for
    # what it handles are picked too (test_ffi.av, test_ffi_llvm.av).
    case "$p" in compiler/*)
      s2=${stem#lower_}; [ "$s2" = "$stem" ] && s2=${stem#emit_}
      if [ "$s2" != "$stem" ] && [ ${#s2} -ge 3 ]; then
        for t in "$work"/tests/cases/test_*"$s2"*.av; do [ -f "$t" ] && echo "tests/cases/$(basename "$t")"; done
      fi ;;
    esac
    case "$p" in
      lib/*.av) m=$(echo "${p#lib/}" | sed 's/\.av$//; s#/#.#g'); grep -l -E "^import $m( |\$)" "$work"/tests/cases/*.av 2>/dev/null ;;
      tests/cases/*.av) m=$(basename "$p" .av); case "$m" in test_*) ;; *) grep -l -E "^import $m( |\$)" "$work"/tests/cases/*.av 2>/dev/null ;; esac ;;
      tests/*) n=${p#tests/}; n=${n%.*}; grep -l -F "$n" "$work"/tests/cases/*.av 2>/dev/null ;;
      *) grep -l -F "$p" "$work"/tests/cases/*.av 2>/dev/null ;;
    esac | sed "s#^$work/##"
  done | grep -E '^tests/cases/test_[^/]*\.av$' | LC_ALL=C sort -u
}

printf '| Commit | Subject | Files | C'"'"' | Tests | Corpus moved | LangArena moved | Instructions (new/old) | Time |\n| --- | --- | ---: | --- | --- | --- | --- | --- | ---: |\n' > "$out/table.md"
i=0
for c in $commits; do
  i=$((i + 1)); t0=$(now)
  short=$(git rev-parse --short=8 "$c"); subj=$(git log -1 --format=%s "$c" | sed 's/|/\\|/g')
  d=$out/$(printf '%02d' $i)-$short; mkdir -p "$d"
  git diff --name-only "$c^" "$c" > "$d/changed.txt"
  # The seeds this commit counts under: CC_SEEDS, else 12 when it changes the
  # hash code, else seed 0 alone.
  if [ -n "$forced" ]; then seeds=$forced
  # shellcheck disable=SC2086
  elif [ -n "$(git diff --name-only "$c^" "$c" -- $hash_files)" ]; then seeds=12
  else seeds=0; fi
  seedenv=""; [ "$seeds" -gt 0 ] && seedenv="env LA_SEEDS=$seeds"
  # Under seeds the commit before is counted again under the same seeds, now,
  # while the work tree still holds its C' (the rebuild for this commit
  # overwrites it): every program of the harness, as the commit may move any
  # of them (a runtime change moves them all).
  : > "$d/prev-ins.tsv"; : > "$d/prev-ins.seeds.tsv"
  if [ "$seeds" -gt 0 ]; then
    pl=$lap; [ $i -eq 1 ] && pl=$blap
    # shellcheck disable=SC2086
    rn "$short previous counts" $seedenv sh "$here/la-count.sh" "$work" "$pl" "$d/prev-ins.tsv" < /dev/null 2>> "$log"
  fi
  git -C "$work" checkout -q --detach "$c" < /dev/null 2>> "$log" || { echo "change-check.sh: checkout of $c failed" >&2; cleanup; exit 1; }
  # Rebuild when the compiler's inputs changed since the letters in bin/.
  ctree=$(git rev-parse "$c^{tree}")
  if [ -n "$(git diff --name-only "$ptree" "$ctree" -- compiler lib runtime tools/letters.sh tools/build-llvm.sh)" ]; then
    rebuilt=yes
    rn "$short build" sh -c 'cd "$1" && exec timeout -k 10 1500 sh tools/letters.sh runtime b c-llvm' cc-build "$work" < /dev/null > "$d/letters.log" 2>&1 || { echo "| $short | $subj | $(wc -l < "$d/changed.txt") | build failed | | | | | |" >> "$out/table.md"; bad=1; break; }
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
  [ -s "$d/compile.txt" ] && rn "$short corpus" sh "$here/ll-digests.sh" "$work" "$d/compile.txt" "$d/new.tsv" < /dev/null 2>> "$log"
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
    rn "$short langarena" sh "$here/ll-digests.sh" "$work" "$d/la-list.txt" "$d/la.tsv" "$lap/avant/langarena/tmp" < /dev/null 2>> "$log"
    cmpr "$out/state/la.tsv" "$d/la.tsv" > "$d/la.txt"
    cp "$d/la.tsv" "$out/state/la.tsv"
  else : > "$d/la.txt"; fi
  # Counts: the moved LangArena programs, or all of them after a runtime change.
  if [ $runtime_moved = yes ] && [ $rebuilt = yes ]; then cut -f1 "$out/state/la.tsv" | sed 's/\.av$//' > "$d/count.txt"
  else awk '{ print $NF }' "$d/la.txt" | sed 's/\.av$//' > "$d/count.txt"; fi
  ins="-"
  if [ -s "$d/count.txt" ]; then
    # shellcheck disable=SC2046
    # shellcheck disable=SC2086
    rn "$short counts" $seedenv sh "$here/la-count.sh" "$work" "$lap" "$d/ins.tsv" $(cat "$d/count.txt") < /dev/null 2>> "$log"
    : > "$d/seed-ranges.tsv"
    # What the ratio is taken against: the state (the commit before's counts
    # at seed 0), or under seeds the commit before's counts under the same
    # seeds (its means).
    cp "$out/state/ins.tsv" "$d/old-ins.tsv"
    if [ "$seeds" -gt 0 ]; then
      # Each program's per-seed ratios, new over old, seed by seed.
      awk -F'\t' 'FILENAME == ARGV[1] { o[$1] = $6; next }
        ($1 in o) { n = split($6, a, ","); m = split(o[$1], b, ","); lo = ""; hi = ""
          for (i = 1; i <= n && i <= m; i++) if (b[i] > 0) { r = a[i] / b[i]; if (lo == "" || r < lo) lo = r; if (hi == "" || r > hi) hi = r }
          if (lo != "") printf "%s\t (%d seeds: %.4f-%.4f)\n", $1, n, lo, hi }' "$d/prev-ins.seeds.tsv" "${d}/ins.seeds.tsv" > "$d/seed-ranges.tsv"
      awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $0; next } { if ($1 in n) print n[$1]; else print }' "$d/prev-ins.tsv" "$out/state/ins.tsv" > "$d/old-ins.tsv"
    fi
    ins=$(awk -F'\t' 'FILENAME == ARGV[1] { sr[$1] = $2; next } FILENAME == ARGV[2] { n[$1] = $2; m[$1] = $3; next }
      { r = !($1 in n) && $2 ~ /^[0-9]+$/ ? "new (not a program of the base'"'"'s harness)" : ($2 ~ /^[0-9]+$/ && n[$1] ~ /^[0-9]+$/ && n[$1] > 0) ? sprintf("%.4f", $2 / n[$1]) : (n[$1] == "compile-failed" && $2 ~ /^[0-9]+$/) ? "new (the base refused it)" : "?"
        o = (!($1 in n) || $3 == m[$1] || n[$1] == "compile-failed") ? "" : " OUTPUT CHANGED"
        s = s (s == "" ? "" : ", ") $1 " " r sr[$1] o }
      END { print s }' "$d/seed-ranges.tsv" "$d/old-ins.tsv" "$d/ins.tsv")
    grep -q 'OUTPUT CHANGED\| ?' <<EOF && bad=1
$ins
EOF
    # The state moves to this commit's counts, always at seed 0: the counts
    # under seeds are written with their first (seed 0) count, so the next
    # commit, counted at seed 0, is compared with a seed-0 count.
    if [ "$seeds" -gt 0 ]; then
      awk -F'\t' 'BEGIN { OFS = "\t" } FILENAME == ARGV[1] { split($6, c, ","); if (c[1] ~ /^[0-9]+$/) z[$1] = c[1]; next } { if ($1 in z) $2 = z[$1]; print }' "$d/ins.seeds.tsv" "$d/ins.tsv" > "$d/ins-seed0.tsv"
    else cp "$d/ins.tsv" "$d/ins-seed0.tsv"; fi
    awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $0; next } { if ($1 in n) print n[$1]; else print }' "$d/ins-seed0.tsv" "$out/state/ins.tsv" > "$d/ins-state.tsv"
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
    rn "$short tests" sh -c 'cd "$1" && t=$2 && shift 2 && exec env -u AVANT_COMPILER -u AVANT_BACKEND TMPDIR="$t" timeout -k 10 1800 ./bin/avant-c-llvm test "$@"' cc-tests "$work" "$d/tmp" $(cat "$d/tests.txt") < /dev/null > "$d/tests.log" 2>&1
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
