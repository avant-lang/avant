#!/bin/sh
# The base cache, the digests, the neutrality from a cache entry and the
# per-commit check, on a fake repository.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/change-check.sh WORK
#
# WORK is a new scratch directory. A git repository in WORK/repo/avant
# (LangArena/run.js beside it) carries the real tools/base-cache.sh,
# ll-digests.sh, gcstats.sh, la-count.sh, neutral.sh and change-check.sh,
# with stubs for what they drive: tools/letters.sh (builds a C' that is a
# shell script, counting its builds), the compiler (`__emit-ll` writes its
# own source text, the program's path and the program; `compile` writes a
# script printing a checksum of the program, its `// file:` lines and a new
# spelling left out, as a task's output depends on neither; `test` fails a
# file holding FAILME) and the
# LangArena harness (stitches langarena/tasks/*.av with helper.av and
# run.js, as the real one does, and asks the compiler to build each).
#   cache-build: the first call builds the entry of the tree (COMPLETE,
#     the corpus's digests, the 72 statistics files); cache-reuse: a second
#     call reads it, no second build; cache-partial: an entry without
#     COMPLETE is built again, never read; cache-concurrent: two calls at
#     once for a new tree build it once; cache-la: the LangArena part
#     holds the stitched programs, their compile arguments, digests and
#     counts;
#   digests-normalized: the same programs in two checkouts read the same
#     digests (each checkout's path written as @ROOT@);
#   neutral-cache: tools/neutral.sh with a cache entry as its base reads
#     the programs the same, and the change's own file apart
#     (NEUTRAL_CHANGED), status 0; neutral-default-la: with two checkouts
#     and the new one's own stitched programs (the default directory),
#     whose paths the stub compiler, as the real one, writes relative to
#     its own checkout, the programs read the same;
#   check-first-kept, check-kept, check-rebuilt, check-runtime,
#     check-failed: change-check's table: a first commit changing a tool
#     only keeps the base's C' and moves no program; a commit changing one
#     test file keeps C' and names that file (a helper beside the tests
#     that names a changed path is not run as a test)
#     and its test; a compiler change rebuilds, moves every program and
#     counts the moved LangArena programs, their output the same; a
#     runtime change counts every LangArena program without moving a .ll;
#     a failing touched test makes the line and the status say so;
#     check-worktree: the work tree is gone afterwards;
#   cache-la-base-refuses, neutral-base-refuses, check-base-refuses: the
#     caller's LangArena tasks in a spelling the base's compiler refuses:
#     the part is complete with refused.txt (how many, the first message),
#     tools/neutral.sh reads the tasks `not-comparable` with a summary line
#     saying so and status 0, the sources still judged; change-check reads
#     their counts as new, not as a failure;
#   prune-dry-run, prune-removes-unread, prune-keeps-read, prune-keeps-in-use,
#     prune-after-reader, prune-days-checked, cache-read-stamp: --prune DAYS
#     on a stub cache: --dry-run lists and removes nothing; an entry unread
#     for longer is removed, its lock file kept, the space said; one read
#     within DAYS (its COMPLETE, or its read stamp) is kept; one whose lock
#     a reader holds shared is kept "in use", and removed once the reader
#     is gone; DAYS must be above 0; a call returning an entry stamps it;
#   map-pick-carried, map-pick-widened, map-pick-unmapped: test-map.sh
#     --pick on a map made by hand: a changed line of a commit whose file
#     moved since the map's tree is carried back to that tree's lines and
#     picks the test file that hit them; a changed line no test hits (a
#     signature) picks those that hit its function; a file the map does
#     not have picks none, and nothing is left in TMPDIR; map-pick-common:
#     a line more than half the test files run (compiling the helper every
#     test imports runs much of the compiler) picks none, said on stderr;
#   check-map: change-check with TEST_MAP runs the map's test file beside
#     those by name and names it apart in the line;
#   check-goldens-example: a changed example selects the test file its
#     dump goldens' row names (tests/goldens/list.txt) and test_examples.av
#     (its tests/expected/STEM.out exists); check-goldens-golden: a changed
#     golden selects its row's test file;
#   check-map-auto: a range changing compiler/ with no map builds the
#     base's (a stub test-map.sh) and runs the test file it picks;
#     check-map-none: TEST_MAP=none builds none and says so;
#   check-runner: with CC_RUNNER each piece of work goes through the
#     runner, one shared run each, named `change-check: base cache`,
#     `change-check: test map build` (test-map.sh's own runs, as the stub
#     one says) and `change-check: SHORT build`, `corpus`,
#     `langarena`, `counts`, `tests`;
#   check-stem-prefix: a compiler pass named for what it handles
#     (compiler/mir/lower_ffi.av) selects the test files named for that
#     (test_ffi, test_ffi_llvm), a library file of the same name none;
#   la-count-groups: tools/la-count.sh with LA_RUNNER and LA_GROUP=1 runs
#     the two programs as two runner calls (`STEP group 1/2`, `2/2`) and
#     writes the table a plain run writes (the instruction counts of a
#     run apart: the stub programs' counts differ a little from run to run);
#   la-count-mem-parallel: with LA_MEM_RUNS=3 each program's peak line holds
#     three runs, started together and read exactly (the file's first line
#     `# peak: exact`, their ru_maxrss in OUT.maxrss.tsv) or, with
#     LA_MEM_PARALLEL=0 and LA_MEM_METHOD=maxrss, one after another and read
#     as GNU time's %M;
#   la-count-seeds: with LA_SEEDS=3 each program is counted under three hash
#     seeds (the stub programs' work depends on the seed), its count the
#     mean, OUT.seeds.tsv the counts with their mean, min and max; 13 seeds
#     are refused;
#   check-hash-seeds: a range that changes runtime/avant_hash.c counts both
#     sides under the 12 seeds and the cell reads `RATIO (12 seeds: LO-HI)`;
#     another runtime change keeps seed 0;
#   check-base-harness: a base that refuses the caller's tasks, with
#     CC_BASE_LANGARENA naming the base's own harness: the base's side is
#     read from it, so the first commit's programs are counted against the
#     base's own (ratios, the same output), a task only the caller's
#     harness has reads new, and cache.txt says so.
# Where `perf stat -e instructions:u` cannot count (a container without
# the counters), the LangArena part and change-check cannot be built: their
# checks print `skip: NAME: why`. Prints one line per check, `ok: NAME`,
# `skip: NAME: why` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/change-check.sh WORK" >&2; exit 2; }
# The caller's change-check settings never reach the fake's runs: a
# runner would take the real machine lock and log into the caller's run
# log, a map or a harness would change what the checks select.
unset CC_RUNNER CC_BASE_LANGARENA TEST_MAP
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
r=$w/repo/avant
mkdir -p "$r/tools" "$r/compiler" "$r/lib/std" "$r/runtime" "$r/examples" "$r/tests/cases" \
  "$r/bin/seed" "$r/langarena/harness" "$r/langarena/tasks" "$w/repo/LangArena"
for t in base-cache.sh ll-digests.sh gcstats.sh la-count.sh neutral.sh change-check.sh test-map.sh; do cp "$root/tools/$t" "$r/tools/$t"; done
cp "$root/tools/test-map.sh" "$r/tools/test-map-real.sh"
for f in heap closures objects; do mkdir -p "$r/tests/fixtures/$f"; echo "churn $f" > "$r/tests/fixtures/$f/churn.av"; done
echo 'v1' > "$r/compiler/main.av"
echo 'r1' > "$r/runtime/rt.txt"
echo 'pub fn m: Int { 1 }' > "$r/lib/std/m.av"
printf 'import std.m\nm\n' > "$r/tests/cases/test_m.av"
echo 'other' > "$r/tests/cases/test_other.av"
echo 'a helper that names compiler/main.av' > "$r/tests/cases/helper.av"
echo 'REFUSE me' > "$r/examples/refused.av"
echo 'example' > "$r/examples/e.av"
echo 'probe' > "$r/tests/probe_matrix.av"
printf '/bin/\n/runtime/build/\n/langarena/\n' > "$r/.gitignore"
printf '#!/bin/sh\nexit 0\n' > "$r/bin/seed/avant"; chmod +x "$r/bin/seed/avant"
: > "$r/bin/re2c"
: > "$r/langarena/harness/measure.cr"
echo 'helper' > "$r/langarena/helper.av"
echo 'task a' > "$r/langarena/tasks/a.av"
echo 'task b' > "$r/langarena/tasks/b.av"
echo '[{"name": "A::A", "checksum": 1}]' > "$w/repo/LangArena/run.js"
# The stub compiler's body; tools/letters.sh puts V (compiler/main.av) above it.
cat > "$r/tools/stub-avc" <<'STUB'
R=$(cat "$(dirname "$0")/../runtime/build/avant_rt.o" 2>/dev/null)
cmd=$1; shift
case $cmd in
__emit-ll)
  if grep -q REFUSE "$1"; then echo "error: refused: $(pwd -P)/$1"; exit 1; fi
  # a location under its own checkout written relative to it, as the real compiler does
  me=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd -P)
  # a spelling only a checkout with lib/spell knows (an older tree refuses it)
  if grep -q NEWSPELL "$1" && [ ! -f "$me/lib/spell" ]; then echo "error: unknown spelling NEWSPELL in $(pwd -P)/$1"; exit 1; fi
  { echo "; compiler $V"; echo "; source $(pwd -P)/$1"; sed "s#^// file: $me/#// file: #" "$1"; } > "$2" ;;
compile)
  me=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd -P)
  if grep -q NEWSPELL "$1" && [ ! -f "$me/lib/spell" ]; then echo "error: unknown spelling NEWSPELL in $(pwd -P)/$1"; exit 1; fi
  # the program's work depends on the hash seed (a loop of seed % 7 steps),
  # its output does not
  printf '#!/bin/sh\nk=$(( ${AVANT_HASH_SEED:-0} %% 7 * 200 )); while [ $k -gt 0 ]; do k=$((k - 1)); done\necho checksum-%s\necho "avant gc: runtime=%s peak_heap=10 pause_young_ns=$$" >&2\n' "$(grep -v 'NEWSPELL\|^// file: ' "$1" | cksum | cut -d' ' -f1)" "$R" > "$2"
  chmod +x "$2" ;;
test)
  f=0 p=0
  for t in "$@"; do
    case $t in -*) continue ;; esac
    case $(basename "$t") in test_*) ;; *) echo "FAIL $t: program needs fn main"; f=$((f + 1)); continue ;; esac
    if grep -q FAILME "$t"; then echo "FAIL $t"; f=$((f + 1)); else echo "ok $t"; p=$((p + 1)); fi
  done
  echo "$f failed, $p passed"; [ $f -eq 0 ] ;;
*) exit 2 ;;
esac
STUB
cat > "$r/tools/letters.sh" <<'STUB'
#!/bin/sh
# The stub letters: C' is tools/stub-avc with compiler/main.av as its V.
default_seed=bin/seed
[ -x bin/seed/avant ] || { echo "no seed"; exit 1; }
[ -n "${STUB_BUILDS:-}" ] && echo build >> "$STUB_BUILDS"
mkdir -p bin runtime/build
cp runtime/rt.txt runtime/build/avant_rt.o
{ echo '#!/bin/sh'; echo "V='$(cat compiler/main.av)'"; cat tools/stub-avc; } > bin/avant-c-llvm
chmod +x bin/avant-c-llvm; cp bin/avant-c-llvm bin/avant-b
for x in avant-b avant-c-llvm; do printf 'commit: stub\nlib: %s/lib\n' "$(pwd -P)" > "bin/$x.versions.txt"; done
sleep 1
STUB
cat > "$r/tools/la-harness.sh" <<'STUB'
#!/bin/sh
# The stub harness: its prod pass stitches each task as the real one does.
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
d=$root/langarena/tmp/harness; mkdir -p "$d"
cat > "$d/measure" <<'EOF'
#!/bin/sh
A=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
T=$A/langarena/tmp
[ "$1" = prod ] && [ -f "$A/../LangArena/run.js" ] || exit 2
for f in "$A"/langarena/tasks/*.av; do
  s=$(basename "$f" .av)
  echo "cfg $(cat "$A/../LangArena/run.js")" > "$T/$s.cfg.av"
  { echo "// file: $A/langarena/helper.av"; cat "$A/langarena/helper.av"; echo "// file: $T/$s.cfg.av"; cat "$T/$s.cfg.av"; echo "// file: $f"; cat "$f"; } > "$T/$s.av"
  rm -f "$T/$s"
  "$AVANT_COMPILER" compile "$T/$s.av" "$T/$s" --release --cpu=v3 > /dev/null 2>&1
done
exit 0
EOF
chmod +x "$d/measure"; echo "$d/measure"
STUB
g() { git -C "$r" -c user.name=t -c user.email=t@t "$@" > /dev/null 2>&1; }
g init -q . && g add -A && g commit -q -m base
export BASE_CACHE="$w/cache" STUB_BUILDS="$w/builds"
: > "$w/builds"
builds() { wc -l < "$w/builds" | tr -d ' '; }
tree=$(git -C "$r" rev-parse 'HEAD^{tree}')
e=$w/cache/$tree

# 1. The entry: built once, then read.
o=$(cd "$r" && sh tools/base-cache.sh HEAD 2> "$w/c1.err" < /dev/null); rc=$?
[ $rc -eq 0 ] && [ "$o" = "$e" ] && [ -f "$e/COMPLETE" ] && [ "$(builds)" = 1 ] && [ "$(wc -l < "$e/ll.tsv")" = 10 ] \
  && [ "$(ls "$e/gcstats" | grep -c '\.\(out\|err\)$')" = 72 ] && grep -q "	1	-	" "$e/ll.tsv"
say cache-build $? "rc=$rc out=$o builds=$(builds) ll=$(wc -l < "$e/ll.tsv" 2>/dev/null) $(tail -n 3 "$w/c1.err" | tr '\n' ';')"
o=$(cd "$r" && sh tools/base-cache.sh HEAD 2> /dev/null < /dev/null); rc=$?
[ $rc -eq 0 ] && [ "$o" = "$e" ] && [ "$(builds)" = 1 ]; say cache-reuse $? "rc=$rc builds=$(builds)"
rm -f "$e/COMPLETE"
o=$(cd "$r" && sh tools/base-cache.sh HEAD 2> /dev/null < /dev/null); rc=$?
[ $rc -eq 0 ] && [ -f "$e/COMPLETE" ] && [ "$(builds)" = 2 ]; say cache-partial $? "rc=$rc builds=$(builds)"

# 2. Two callers at once for a new tree: one build.
echo 'example 2' > "$r/examples/e2.av"; g add -A && g commit -q -m two
(cd "$r" && sh tools/base-cache.sh HEAD > "$w/p1.out" 2> /dev/null < /dev/null) &
p1=$!
(cd "$r" && sh tools/base-cache.sh HEAD > "$w/p2.out" 2> /dev/null < /dev/null) &
p2=$!
wait $p1; r1=$?; wait $p2; r2=$?
[ $r1 -eq 0 ] && [ $r2 -eq 0 ] && cmp -s "$w/p1.out" "$w/p2.out" && [ "$(builds)" = 3 ]
say cache-concurrent $? "rc=$r1,$r2 builds=$(builds)"

# 3. The digests: one program list in two checkouts, the same lines.
(cd "$r" && find examples tests/cases -name '*.av' | LC_ALL=C sort) > "$w/list.txt"
e2=$(cat "$w/p1.out")
sh "$r/tools/ll-digests.sh" "$e2/avant" "$w/list.txt" "$w/d1.tsv" < /dev/null
(cd "$r" && sh tools/letters.sh > /dev/null 2>&1 < /dev/null)
sh "$r/tools/ll-digests.sh" "$r" "$w/list.txt" "$w/d2.tsv" < /dev/null
cmp -s "$w/d1.tsv" "$w/d2.tsv" && [ "$(wc -l < "$w/d1.tsv")" = 6 ] && ! grep -q "$r" "$w/d2.tsv"
say digests-normalized $? "$(diff "$w/d1.tsv" "$w/d2.tsv" | head -n 4 | tr '\n' ';')"

# 3b. --prune: a stub cache of four entries, by their last read.
pc=$w/pcache
mk() { # HEX AGE: an entry read AGE ago (a date for touch -d)
  mkdir -p "$pc/$1/avant" && echo "tree: $1" > "$pc/$1/MANIFEST" && : > "$pc/$1/COMPLETE"
  head -c 2097152 /dev/zero > "$pc/$1/avant/blob"
  touch -d "$2" "$pc/$1/MANIFEST" "$pc/$1/COMPLETE" "$pc/$1"
}
old_e=1111111111111111111111111111111111111111 new_e=2222222222222222222222222222222222222222
use_e=3333333333333333333333333333333333333333 stamp_e=4444444444444444444444444444444444444444
mk $old_e '10 days ago'; mk $new_e '1 day ago'; mk $use_e '10 days ago'; mk $stamp_e '10 days ago'
touch "$pc/$stamp_e.read"
# a reader of the third entry: its lock held shared, as tools/neutral.sh does
mkdir -p "$w/holder"
(exec 7>> "$pc/$use_e.lock"; flock -s 7; : > "$w/holder/up"; while [ ! -e "$w/holder/done" ]; do sleep 0.1; done) &
hp=$!
while [ ! -e "$w/holder/up" ]; do sleep 0.05; done
BASE_CACHE=$pc sh "$r/tools/base-cache.sh" --prune 5 --dry-run < /dev/null > "$w/prune-dry.out" 2>&1; rc=$?
[ $rc -eq 0 ] && [ -d "$pc/$old_e" ] && grep -q "^would remove $old_e: last read .*, 2.0 MB\$" "$w/prune-dry.out" \
  && grep -q 'would remove 1 entries, 2.0 MB; kept 2 read within 5 days, 1 in use$' "$w/prune-dry.out"
say prune-dry-run $? "rc=$rc $(tr '\n' ';' < "$w/prune-dry.out")"
BASE_CACHE=$pc sh "$r/tools/base-cache.sh" --prune 5 < /dev/null > "$w/prune.out" 2>&1; rc=$?
[ $rc -eq 0 ] && [ ! -e "$pc/$old_e" ] && [ -f "$pc/$old_e.lock" ] && grep -q "^removed $old_e: last read .*, 2.0 MB\$" "$w/prune.out" \
  && grep -q 'removed 1 entries, 2.0 MB freed; kept 2 read within 5 days, 1 in use$' "$w/prune.out"
say prune-removes-unread $? "rc=$rc $(tr '\n' ';' < "$w/prune.out")"
[ -f "$pc/$new_e/COMPLETE" ] && [ -f "$pc/$stamp_e/COMPLETE" ] && grep -q "^kept $stamp_e: read .*, within 5 days\$" "$w/prune.out"
say prune-keeps-read $? "$(ls "$pc" | tr '\n' ' ')"
[ -f "$pc/$use_e/avant/blob" ] && grep -q "^kept $use_e: in use" "$w/prune.out"
say prune-keeps-in-use $? "$(grep "$use_e" "$w/prune.out")"
: > "$w/holder/done"; wait $hp
BASE_CACHE=$pc sh "$r/tools/base-cache.sh" --prune 5 < /dev/null > "$w/prune2.out" 2>&1
[ ! -e "$pc/$use_e" ] && grep -q "^removed $use_e" "$w/prune2.out"
say prune-after-reader $? "$(tr '\n' ';' < "$w/prune2.out")"
BASE_CACHE=$pc sh "$r/tools/base-cache.sh" --prune 0 < /dev/null > /dev/null 2>&1; rc=$?
[ $rc -eq 2 ]; say prune-days-checked $? "status $rc for --prune 0"
# every call that returns an entry stamps it
[ -f "$e.read" ] && [ "$(stat -c %Y "$e.read")" -ge "$(stat -c %Y "$e/COMPLETE")" ]
say cache-read-stamp $? "$(ls -l "$e.read" 2>&1)"

# 3c. test-map.sh --pick on a map made by hand: compiler/x.av's two
# functions, each hit by one test file; a later commit moves them down by
# three lines and the next changes a line of the second.
printf 'fn a {\n  1\n}\nfn b {\n  2\n}\n' > "$r/compiler/x.av"; g add -A && g commit -q -m 'x'
xt=$(git -C "$r" rev-parse 'HEAD^{tree}')
pm=$w/pickmap; mkdir -p "$pm/hits"
printf 'tree: %s\n' "$xt" > "$pm/MANIFEST"; : > "$pm/COMPLETE"
printf 'compiler/x.av\t1-3\n' > "$pm/hits/test_a.tsv"; printf 'compiler/x.av\t5\n' > "$pm/hits/test_b.tsv"
printf 'compiler/x.av\n' > "$pm/files.txt"
printf '# a\n# b\n# c\nfn a {\n  1\n}\nfn b {\n  2\n}\n' > "$r/compiler/x.av"; g add -A && g commit -q -m 'x moved'
p1=$(git -C "$r" rev-parse HEAD)
printf '# a\n# b\n# c\nfn a {\n  1\n}\nfn b {\n  22\n}\n' > "$r/compiler/x.av"; g add -A && g commit -q -m 'x b changed'
p2=$(git -C "$r" rev-parse HEAD)
o=$(cd "$r" && TMPDIR=$w sh tools/test-map.sh --pick "$pm" "$p1" "$p2" compiler/x.av compiler/main.av < /dev/null 2>&1)
[ "$o" = tests/cases/test_b.av ]; say map-pick-carried $? "picked: $(echo "$o" | tr '\n' ' ')"
printf '# a\n# b\n# c\nfn a {\n  1\n}\nfn b(x) {\n  22\n}\n' > "$r/compiler/x.av"; g add -A && g commit -q -m 'x b signature'
o=$(cd "$r" && TMPDIR=$w sh tools/test-map.sh --pick "$pm" "$p2" HEAD compiler/x.av < /dev/null 2>&1)
[ "$o" = tests/cases/test_b.av ]; say map-pick-widened $? "picked: $(echo "$o" | tr '\n' ' ')"
o=$(cd "$r" && TMPDIR=$w sh tools/test-map.sh --pick "$pm" "$p2" HEAD compiler/y.av lib/std/m.av < /dev/null 2>&1)
[ -z "$o" ] && [ -z "$(ls -d "$w"/test-map-pick.* 2>/dev/null)" ]; say map-pick-unmapped $? "picked: $(echo "$o" | tr '\n' ' ')"
# a line every test file runs (as compiling their shared helper does) picks none, said on stderr
cp -r "$pm" "$w/pickmap3"; printf 'compiler/x.av\t1-6\n' > "$w/pickmap3/hits/test_c.tsv"; printf 'compiler/x.av\t1-6\n' > "$w/pickmap3/hits/test_a.tsv"
o=$(cd "$r" && TMPDIR=$w sh tools/test-map.sh --pick "$w/pickmap3" "$p1" "$p2" compiler/x.av < /dev/null 2> "$w/pick3.err")
[ -z "$o" ] && grep -q '^test-map.sh: compiler/x.av: 1 of 1 changed lines run by more than 50% of the test files' "$w/pick3.err"
say map-pick-common $? "picked: $(echo "$o" | tr '\n' ' '); $(cat "$w/pick3.err")"

perf_ok=0
perf stat -x, -e instructions:u -o "$w/perf.txt" true > /dev/null 2>&1 && grep -q '^[0-9]' "$w/perf.txt" && perf_ok=1
skip() { for n in "$@"; do echo "skip: $n: perf stat cannot count instructions here"; done; }
if [ $perf_ok = 0 ]; then
  skip check-map cache-la neutral-cache neutral-default-la check-first-kept check-kept check-rebuilt check-runtime check-failed check-worktree \
    cache-la-base-refuses neutral-base-refuses check-base-refuses check-base-harness check-goldens-example check-goldens-golden check-map-none check-map-auto check-runner check-stem-prefix la-count-groups la-count-mem-parallel la-count-seeds check-hash-seeds
  echo done; exit 0
fi

# 4. The LangArena part.
lap=$(cd "$r" && sh tools/base-cache.sh --la HEAD 2> "$w/la.err" < /dev/null); rc=$?
[ $rc -eq 0 ] && [ -f "$lap/COMPLETE" ] && [ "$(ls "$lap/args" | tr '\n' ' ')" = "a b " ] && grep -qx -- '--release' "$lap/args/a" \
  && [ "$(cut -f2 "$lap/ll.tsv" | tr '\n' ' ')" = "0 0 " ] && [ "$(wc -l < "$lap/tasks.tsv")" = 2 ] \
  && awk -F'\t' '$2 !~ /^[0-9]+$/ || $5 !~ /^checksum-/ { bad = 1 } END { exit bad + (NR != 2) }' "$lap/ins.tsv"
say cache-la $? "rc=$rc $(cat "$lap/ins.tsv" 2>/dev/null | tr '\n' ';') $(tail -n 3 "$w/la.err" | tr '\n' ';')"

# 5. The neutrality from the entry: the change's own file read apart.
echo 'other, changed' > "$r/tests/cases/test_other.av"; g add -A && g commit -q -m three
(cd "$r" && sh tools/letters.sh > /dev/null 2>&1 < /dev/null)
git -C "$r" diff --name-only HEAD^ HEAD > "$w/changed.txt"
NEUTRAL_CHANGED="$w/changed.txt" sh "$r/tools/neutral.sh" "$e2" "$r" "$w/neutral" < /dev/null > "$w/neutral.log" 2>&1; rc=$?
[ $rc -eq 0 ] && grep -qx 'changed (LL-DIFF) tests/cases/test_other.av' "$w/neutral/result.txt" \
  && grep -q '^sources: files 13 ll-same 11 LL-DIFF 0 refused-same 1 refused-msg 0 STATUS 0 new 0 changed 1 ' "$w/neutral/summary.txt" \
  && grep -q '^langarena: files 2 ll-same 2 ' "$w/neutral/summary.txt"
say neutral-cache $? "rc=$rc $(tr '\n' ';' < "$w/neutral/summary.txt" 2>/dev/null)"

# 5b. The two-checkout form with its default LangArena directory: the new
# checkout's own stitched programs, which its compiler would locate
# relative to itself and the base's absolutely, read the same.
h=$(cd "$r" && sh tools/la-harness.sh < /dev/null)
(cd "$r" && AVANT_COMPILER="$r/bin/avant-c-llvm" "$h" prod < /dev/null > /dev/null 2>&1)
# its copy goes to TMPDIR and is removed
mkdir -p "$w/tmp"
TMPDIR="$w/tmp" sh "$r/tools/neutral.sh" "$e2/avant" "$r" "$w/neutral2" < /dev/null > "$w/neutral2.log" 2>&1
grep -q '^langarena: files 2 ll-same 2 LL-DIFF 0 ' "$w/neutral2/summary.txt" && [ -z "$(ls "$w/tmp")" ]
say neutral-default-la $? "$(grep '^langarena' "$w/neutral2/summary.txt" 2>/dev/null); left in TMPDIR: $(ls "$w/tmp" | tr '\n' ' ')"

# 6. change-check over five commits after the third.
b=$(git -C "$r" rev-parse HEAD)
echo '# a note' >> "$r/tools/gcstats.sh"; g add -A && g commit -q -m 'a tool changed'
echo 'other, again' > "$r/tests/cases/test_other.av"; g add -A && g commit -q -m 'a test changed'
echo 'v2' > "$r/compiler/main.av"; g add -A && g commit -q -m 'the compiler changed'
echo 'r2' > "$r/runtime/rt.txt"; g add -A && g commit -q -m 'the runtime changed'
printf 'import std.m\nFAILME\n' > "$r/tests/cases/test_m.av"; g add -A && g commit -q -m 'a test fails'
(cd "$r" && TEST_MAP=none sh tools/change-check.sh "$b" HEAD "$w/cc" < /dev/null > "$w/cc.log" 2>&1); rc=$?
t=$w/cc/table.md
row() { grep -F "| $1 |" "$t" 2>/dev/null; }
row 'a tool changed' | grep -q '| 1 | kept | none matched | 0 | - (no rebuild) | - |'
say check-first-kept $? "$(row 'a tool changed')"
row 'a test changed' | grep -q '| 1 | kept | 0 failed, 1 passed: test_other | 1: tests/cases/test_other.av | - (no rebuild) | - |'
say check-kept $? "$(row 'a test changed')"
row 'the compiler changed' | grep -q '| 1 | rebuilt | none matched | 10: .* | 2: a.av, b.av | a [0-9.]*, b [0-9.]* |'
say check-rebuilt $? "$(row 'the compiler changed')"
row 'the runtime changed' | grep -q '| 1 | rebuilt | none matched | 0 | 0 | a [0-9.]*, b [0-9.]* |'
say check-runtime $? "$(row 'the runtime changed')"
[ $rc -eq 1 ] && row 'a test fails' | grep -q '| FAILED (rc=1) 1 failed, 0 passed: test_m |'
say check-failed $? "rc=$rc $(row 'a test fails') $(tail -n 3 "$w/cc.log" | tr '\n' ';')"
[ ! -e "$w/cc/work" ] && [ "$(git -C "$r" worktree list | wc -l)" = 1 ]; say check-worktree $? "$(git -C "$r" worktree list | tr '\n' ';')"

# 7. A base that refuses the caller's LangArena tasks: the tasks gain a
# spelling only a tree with lib/spell knows, and the head adds lib/spell.
nb=$(git -C "$r" rev-parse HEAD)
mkdir -p "$w/oldla"; (cd "$r/langarena" && tar --exclude=./tmp -cf - .) | tar -xf - -C "$w/oldla"
for x in a b; do echo NEWSPELL >> "$r/langarena/tasks/$x.av"; done
: > "$r/lib/spell"; g add -A && g commit -q -m 'a new spelling'
lap=$(cd "$r" && sh tools/base-cache.sh --la "$nb" 2> "$w/la-refused.err" < /dev/null); rc=$?
[ $rc -eq 0 ] && [ -f "$lap/COMPLETE" ] && grep -q '^refused	2	2$' "$lap/refused.txt" \
  && grep -q '^first	a.av	error: unknown spelling NEWSPELL in a.av$' "$lap/refused.txt" && [ "$(grep -c '	compile-failed	' "$lap/ins.tsv")" = 2 ]
say cache-la-base-refuses $? "rc=$rc $(tr '\n' ';' < "$lap/refused.txt" 2>/dev/null) $(tail -n 3 "$w/la-refused.err" | tr '\n' ';')"
(cd "$r" && sh tools/letters.sh > /dev/null 2>&1 < /dev/null)
git -C "$r" diff --name-only "$nb" HEAD > "$w/changed2.txt"
NEUTRAL_CHANGED="$w/changed2.txt" sh "$r/tools/neutral.sh" "$(dirname "$lap")" "$r" "$w/neutral3" < /dev/null > "$w/neutral3.log" 2>&1; rc=$?
[ $rc -eq 0 ] && grep -q '^sources: files 13 ll-same 10 LL-DIFF 0 refused-same 1 refused-msg 0 STATUS 0 new 0 changed 0 not-comparable 2 ' "$w/neutral3/summary.txt" \
  && grep -q '^langarena: files 2 ll-same 0 LL-DIFF 0 refused-same 0 refused-msg 0 STATUS 0 new 0 changed 0 not-comparable 2 ' "$w/neutral3/summary.txt" \
  && grep -qx 'langarena: not comparable: the base refuses 2 of 2 (first message: a.av: error: unknown spelling NEWSPELL in a.av); the sources corpus is judged alone' "$w/neutral3/summary.txt" \
  && [ "$(grep -c '^not-comparable ' "$w/neutral3/la-result.txt")" = 2 ]
say neutral-base-refuses $? "rc=$rc $(tr '\n' ';' < "$w/neutral3/summary.txt" 2>/dev/null) $(tail -n 2 "$w/neutral3.log" | tr '\n' ';')"
(cd "$r" && TEST_MAP=none sh tools/change-check.sh "$nb" HEAD "$w/cc2" < /dev/null > "$w/cc2.log" 2>&1); rc=$?
[ $rc -eq 0 ] && grep -F '| a new spelling |' "$w/cc2/table.md" | grep -q '| 2: a.av, b.av | a new (the base refused it), b new (the base refused it) |'
say check-base-refuses $? "rc=$rc $(grep -F '| a new spelling |' "$w/cc2/table.md" 2>/dev/null) $(tail -n 3 "$w/cc2.log" | tr '\n' ';')"
echo 'task c' > "$r/langarena/tasks/c.av"
(cd "$r" && TEST_MAP=none CC_BASE_LANGARENA="$w/oldla" sh tools/change-check.sh "$nb" HEAD "$w/cc4" < /dev/null > "$w/cc4.log" 2>&1); rc=$?
rm -f "$r/langarena/tasks/c.av"
[ $rc -eq 0 ] && grep -F '| a new spelling |' "$w/cc4/table.md" | grep -q "| 3: a.av, b.av, c.av | a [0-9][0-9.]*, b [0-9][0-9.]*, c new (not a program of the base's harness) |" \
  && ! grep -F '| a new spelling |' "$w/cc4/table.md" | grep -q 'refused\|OUTPUT' \
  && grep -q "^langarena: the base refuses 2 of this checkout's programs; the base's side read from its own harness $w/oldla " "$w/cc4/cache.txt"
say check-base-harness $? "rc=$rc $(grep -F '| a new spelling |' "$w/cc4/table.md" 2>/dev/null) $(cat "$w/cc4/cache.txt" 2>/dev/null | tr '\n' ';') $(tail -n 3 "$w/cc4.log" | tr '\n' ';')"

# 8. change-check with a test map: the map's test file joins the ones by
# name, and the cell says which came from the map.
mkdir -p "$w/map2/hits"; printf 'tree: %s\n' "$(git -C "$r" rev-parse 'HEAD^{tree}')" > "$w/map2/MANIFEST"; : > "$w/map2/COMPLETE"
printf 'compiler/main.av\t1\n' > "$w/map2/hits/test_other.tsv"; printf 'lib/std/m.av\t1\n' > "$w/map2/hits/test_m.tsv"
printf 'compiler/main.av\nlib/std/m.av\n' > "$w/map2/files.txt"
echo 'v3' > "$r/compiler/main.av"; g add -A && g commit -q -m 'the compiler again'
(cd "$r" && TEST_MAP="$w/map2" sh tools/change-check.sh HEAD^ HEAD "$w/cc3" < /dev/null > "$w/cc3.log" 2>&1); rc=$?
[ $rc -eq 0 ] && grep -F '| the compiler again |' "$w/cc3/table.md" | grep -q '| 0 failed, 1 passed: -; by the map, 1 (1 more): test_other |' \
  && grep -q "^test map: $w/map2 " "$w/cc3/cache.txt"
say check-map $? "rc=$rc $(grep -F '| the compiler again |' "$w/cc3/table.md" 2>/dev/null) $(tail -n 3 "$w/cc3.log" | tr '\n' ';')"

# 9. The dump goldens' list and the examples: a changed example selects
# its row's test file and test_examples.av, a changed golden its row's.
mkdir -p "$r/tests/goldens/examples" "$r/tests/expected"
printf '# TEST STEM SOURCE LAYERS\ntest_goldens  examples/e  examples/e.av  tok\n' > "$r/tests/goldens/list.txt"
echo 'tok' > "$r/tests/goldens/examples/e.tok"; echo 'out' > "$r/tests/expected/e.out"
echo 'goldens' > "$r/tests/cases/test_goldens.av"; echo 'examples' > "$r/tests/cases/test_examples.av"
g add -A && g commit -q -m 'goldens and examples'
echo 'example, changed' > "$r/examples/e.av"; g add -A && g commit -q -m 'an example changed'
echo 'tok2' > "$r/tests/goldens/examples/e.tok"; g add -A && g commit -q -m 'a golden changed'
(cd "$r" && TEST_MAP=none sh tools/change-check.sh HEAD~2 HEAD "$w/cc5" < /dev/null > "$w/cc5.log" 2>&1); rc=$?
t=$w/cc5/table.md
row 'an example changed' | grep -q '| 0 failed, 2 passed: test_examples test_goldens |'
say check-goldens-example $? "rc=$rc $(row 'an example changed') $(tail -n 3 "$w/cc5.log" | tr '\n' ';')"
row 'a golden changed' | grep -q '| 0 failed, 1 passed: test_goldens |'
say check-goldens-golden $? "rc=$rc $(row 'a golden changed')"
grep -qx 'test map: none (TEST_MAP=none)' "$w/cc5/cache.txt"
say check-map-none $? "$(tr '\n' ';' < "$w/cc5/cache.txt" 2>/dev/null)"

# 10. A range changing compiler/ with no map: the base's map is built (a
# stub test-map.sh writes one by hand into the entry; --pick is the real
# one) and its test file runs.
cat > "$r/tools/test-map.sh" <<'STUB'
#!/bin/sh
# The stub map: REV's entry gets testmap/ where test_other's run hits
# compiler/main.av's first line; --pick is the real script's.
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
[ "$1" = --pick ] && exec sh "$here/test-map-real.sh" "$@"
e=$(sh "$here/base-cache.sh" "$1" < /dev/null) || exit 1
echo built >> "$STUB_MAPS"
# (the real script takes the lock itself, per piece of work, as TESTMAP_RUNNER says)
[ -n "${TESTMAP_RUNNER:-}" ] && $TESTMAP_RUNNER --step "$TESTMAP_STEP build" --kind "$TESTMAP_KIND" -- true
m=$e/testmap; mkdir -p "$m/hits"
printf 'tree: %s\n' "$(basename "$e")" > "$m/MANIFEST"
printf 'compiler/main.av\t1\n' > "$m/hits/test_other.tsv"; printf 'lib/std/m.av\t1\n' > "$m/hits/test_m.tsv"
printf 'compiler/main.av\nlib/std/m.av\n' > "$m/files.txt"
: > "$m/COMPLETE"; echo "$m"
STUB
g add -A && g commit -q -m 'the stub map'
echo 'v4' > "$r/compiler/main.av"; g add -A && g commit -q -m 'the compiler once more'
# The runner: logs its step and side, then runs the command after --.
cat > "$w/runner" <<'STUB'
#!/bin/sh
step="" side=shared
while [ $# -gt 0 ] && [ "$1" != -- ]; do
  case $1 in --step) step=$2; shift ;; --timed) side=timed ;; esac
  shift
done
shift
echo "$side	$step" >> "$RUNNER_LOG"
exec "$@"
STUB
chmod +x "$w/runner"
: > "$w/maps"
(cd "$r" && STUB_MAPS="$w/maps" RUNNER_LOG="$w/runner.log" CC_RUNNER="$w/runner" sh tools/change-check.sh HEAD^ HEAD "$w/cc6" < /dev/null > "$w/cc6.log" 2>&1); rc=$?
t=$w/cc6/table.md
[ $rc -eq 0 ] && [ "$(wc -l < "$w/maps")" = 1 ] && row 'the compiler once more' | grep -q '| 0 failed, 1 passed: -; by the map, 1 (1 more): test_other |' \
  && grep -q '^test map: built for the base, the range changing compiler/ or lib/ ' "$w/cc6/cache.txt"
say check-map-auto $? "rc=$rc maps=$(wc -l < "$w/maps") $(row 'the compiler once more') $(tr '\n' ';' < "$w/cc6/cache.txt" 2>/dev/null) $(tail -n 3 "$w/cc6.log" | tr '\n' ';')"
s6=$(git -C "$r" rev-parse --short=8 HEAD)
printf 'shared\tchange-check: base cache\nshared\tchange-check: base cache\nshared\tchange-check: test map build\nshared\tchange-check: %s build\nshared\tchange-check: %s corpus\nshared\tchange-check: %s langarena\nshared\tchange-check: %s counts\nshared\tchange-check: %s tests\n' \
  "$s6" "$s6" "$s6" "$s6" "$s6" > "$w/runner.want"
cmp -s "$w/runner.want" "$w/runner.log"
say check-runner $? "$(tr '\n' ';' < "$w/runner.log" 2>/dev/null)"

# 15. A compiler pass named for what it handles: mir/lower_ffi.av selects
# test_ffi.av and test_ffi_llvm.av (the name without lower_ or emit_), a file
# outside compiler/ named lower_ffi selects neither.
mkdir -p "$r/compiler/mir" "$r/lib/std"
echo 'lower' > "$r/compiler/mir/lower_ffi.av"; echo 'lower' > "$r/lib/std/lower_ffi.av"
echo 'ffi test' > "$r/tests/cases/test_ffi.av"; echo 'ffi llvm test' > "$r/tests/cases/test_ffi_llvm.av"
g add -A && g commit -q -m 'the ffi tests'
echo 'lower2' > "$r/compiler/mir/lower_ffi.av"; g add -A && g commit -q -m 'the ffi lowering'
echo 'lower2' > "$r/lib/std/lower_ffi.av"; g add -A && g commit -q -m 'a library file of that name'
(cd "$r" && TEST_MAP=none sh tools/change-check.sh HEAD~2 HEAD "$w/cc7" < /dev/null > "$w/cc7.log" 2>&1)
t=$w/cc7/table.md
row 'the ffi lowering' | grep -q '| 0 failed, 2 passed: test_ffi test_ffi_llvm |' && ! row 'a library file of that name' | grep -q 'test_ffi'
say check-stem-prefix $? "$(row 'the ffi lowering'); $(row 'a library file of that name')"

# 16. la-count.sh with LA_RUNNER: the programs go in groups of LA_GROUP,
# each its own runner call named `STEP group K/N`, and the table is the
# one a single run writes.
cat > "$w/lrunner" <<'STUB'
#!/bin/sh
step="" side=shared
while [ $# -gt 0 ] && [ "$1" != -- ]; do
  case $1 in --step) step=$2; shift ;; --timed) side=timed ;; esac
  shift
done
shift
echo "$side	$step" >> "$RUNNER_LOG"
exec "$@"
STUB
chmod +x "$w/lrunner"
: > "$w/lrunner.log"
(cd "$r" && sh tools/letters.sh > /dev/null 2>&1 < /dev/null)
lap=$(cd "$r" && sh tools/base-cache.sh --la HEAD 2> "$w/la16.err" < /dev/null)
(cd "$r" && sh tools/la-count.sh . "$lap" "$w/lc-plain.tsv" < /dev/null > /dev/null 2>&1)
(cd "$r" && RUNNER_LOG="$w/lrunner.log" LA_RUNNER="$w/lrunner" LA_STEP="count" LA_GROUP=1 sh tools/la-count.sh . "$lap" "$w/lc-grouped.tsv" < /dev/null > /dev/null 2>&1)
[ "$(tr '\n' ';' < "$w/lrunner.log")" = "shared	count group 1/2;shared	count group 2/2;" ] && [ "$(cut -f1,3-5 "$w/lc-plain.tsv")" = "$(cut -f1,3-5 "$w/lc-grouped.tsv")" ] && awk -F'\t' '$2 !~ /^[0-9]+$/ { bad = 1 } END { exit bad }' "$w/lc-grouped.tsv" && [ "$(wc -l < "$w/lc-plain.tsv")" = 2 ]
say la-count-groups $? "$(tr '\n' ';' < "$w/lrunner.log" 2>/dev/null); $(cat "$w/lc-grouped.tsv" 2>/dev/null | tr '\n' ';')"
(cd "$r" && LA_MEM_RUNS=3 sh tools/la-count.sh . "$lap" "$w/lc-mem.tsv" < /dev/null > /dev/null 2>&1)
(cd "$r" && LA_MEM_RUNS=3 LA_MEM_PARALLEL=0 LA_MEM_METHOD=maxrss sh tools/la-count.sh . "$lap" "$w/lc-mem0.tsv" < /dev/null > /dev/null 2>&1)
memok() { awk -F'\t' -v m="$2" 'NR == 1 { if ($0 != "# peak: " m) bad = 1; next } { n = split($5, v, ","); if (n != 3 || $2 !~ /^[0-9]+$/) bad = 1 } END { exit bad + (NR != 3) }' "$1"; }
memok "$w/lc-mem.mem.tsv" exact && memok "$w/lc-mem.maxrss.tsv" maxrss && memok "$w/lc-mem0.mem.tsv" maxrss && [ ! -e "$w/lc-mem0.maxrss.tsv" ]
say la-count-mem-parallel $? "$(cat "$w/lc-mem.mem.tsv" 2>/dev/null | tr '\n' ';'); $(cat "$w/lc-mem0.mem.tsv" 2>/dev/null | tr '\n' ';')"

# 17. Hash seeds: LA_SEEDS=3 counts each program under the list's first
# three seeds, its count their mean, OUT.seeds.tsv the counts with their
# mean, min and max; LA_SEEDS=13 is refused.
(cd "$r" && LA_SEEDS=3 sh tools/la-count.sh . "$lap" "$w/lc-seeds.tsv" < /dev/null > /dev/null 2>&1); rc=$?
(cd "$r" && LA_SEEDS=13 sh tools/la-count.sh . "$lap" "$w/lc-seeds13.tsv" < /dev/null > /dev/null 2>&1); rc13=$?
[ $rc = 0 ] && [ $rc13 = 2 ] && awk -F'\t' 'FILENAME == ARGV[1] { n[$1] = $2; next }
  { k = split($6, c, ","); s = 0; lo = c[1]; hi = c[1]; for (i = 1; i <= k; i++) { s += c[i]; if (c[i] < lo) lo = c[i]; if (c[i] > hi) hi = c[i] }
    if (k != 3 || n[$1] != sprintf("%.0f", s / k) || $2 != n[$1] || $3 != lo || $4 != hi || lo >= hi) bad = 1 }
  END { exit bad + (NR != 4) }' "$w/lc-seeds.tsv" "$w/lc-seeds.seeds.tsv"
say la-count-seeds $? "rc=$rc rc13=$rc13; $(tr '\n' ';' < "$w/lc-seeds.tsv" 2>/dev/null); $(tr '\n' ';' < "$w/lc-seeds.seeds.tsv" 2>/dev/null)"

# 18. change-check turns the seeds on for a range that changes a hash file
# (runtime/avant_hash.c), both sides counted under the 12, the cell with the
# per-seed range; another range keeps seed 0.
echo 'hash v1' > "$r/runtime/avant_hash.c"; g add -A && g commit -q -m 'the hash table file'
echo 'hash v2' > "$r/runtime/avant_hash.c"; g add -A && g commit -q -m 'the hash probing'
(cd "$r" && TEST_MAP=none sh tools/change-check.sh HEAD~1 HEAD "$w/cc18" < /dev/null > "$w/cc18.log" 2>&1)
echo 'r3' > "$r/runtime/rt.txt"; g add -A && g commit -q -m 'another runtime file'
(cd "$r" && TEST_MAP=none sh tools/change-check.sh HEAD~1 HEAD "$w/cc18b" < /dev/null > "$w/cc18b.log" 2>&1)
grep -q '^hash seeds: the counts under 12 seeds of tools/la-count.sh, both sides (the range changes runtime/avant_hash.c' "$w/cc18/cache.txt" \
  && grep 'the hash probing' "$w/cc18/table.md" | grep -q ' [0-9][.][0-9]* (12 seeds: [0-9.]*-[0-9.]*)' \
  && [ "$(awk -F'\t' '{ print split($6, c, ",") }' "$w/cc18/01-"*/ins.seeds.tsv | sort -u)" = 12 ] \
  && grep -q '^hash seeds: seed 0 alone' "$w/cc18b/cache.txt" && ! grep -q 'seeds:' "$w/cc18b/table.md"
say check-hash-seeds $? "$(grep 'hash seeds' "$w/cc18/cache.txt" "$w/cc18b/cache.txt" 2>/dev/null | tr '\n' ';'); $(grep 'the hash probing' "$w/cc18/table.md" | cut -c1-400)"
echo done
