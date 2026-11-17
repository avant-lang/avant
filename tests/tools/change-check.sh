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
# script printing a checksum; `test` fails a file holding FAILME) and the
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
#     test file keeps C' and names that file
#     and its test; a compiler change rebuilds, moves every program and
#     counts the moved LangArena programs, their output the same; a
#     runtime change counts every LangArena program without moving a .ll;
#     a failing touched test makes the line and the status say so;
#     check-worktree: the work tree is gone afterwards.
# Where `perf stat -e instructions:u` cannot count (a container without
# the counters), the LangArena part and change-check cannot be built: their
# checks print `skip: NAME: why`. Prints one line per check, `ok: NAME`,
# `skip: NAME: why` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/change-check.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
r=$w/repo/avant
mkdir -p "$r/tools" "$r/compiler" "$r/lib/std" "$r/runtime" "$r/examples" "$r/tests/cases" \
  "$r/bin/seed" "$r/langarena/harness" "$r/langarena/tasks" "$w/repo/LangArena"
for t in base-cache.sh ll-digests.sh gcstats.sh la-count.sh neutral.sh change-check.sh; do cp "$root/tools/$t" "$r/tools/$t"; done
for f in heap closures objects; do mkdir -p "$r/tests/fixtures/$f"; echo "churn $f" > "$r/tests/fixtures/$f/churn.av"; done
echo 'v1' > "$r/compiler/main.av"
echo 'r1' > "$r/runtime/rt.txt"
echo 'pub fn m: Int { 1 }' > "$r/lib/std/m.av"
printf 'import std.m\nm\n' > "$r/tests/cases/test_m.av"
echo 'other' > "$r/tests/cases/test_other.av"
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
  { echo "; compiler $V"; echo "; source $(pwd -P)/$1"; sed "s#^// file: $me/#// file: #" "$1"; } > "$2" ;;
compile)
  printf '#!/bin/sh\necho checksum-%s\necho "avant gc: runtime=%s peak_heap=10 pause_young_ns=$$" >&2\n' "$(cksum < "$1" | cut -d' ' -f1)" "$R" > "$2"
  chmod +x "$2" ;;
test)
  f=0 p=0
  for t in "$@"; do
    case $t in -*) continue ;; esac
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
[ $rc -eq 0 ] && [ "$o" = "$e" ] && [ -f "$e/COMPLETE" ] && [ "$(builds)" = 1 ] && [ "$(wc -l < "$e/ll.tsv")" = 9 ] \
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
cmp -s "$w/d1.tsv" "$w/d2.tsv" && [ "$(wc -l < "$w/d1.tsv")" = 5 ] && ! grep -q "$r" "$w/d2.tsv"
say digests-normalized $? "$(diff "$w/d1.tsv" "$w/d2.tsv" | head -n 4 | tr '\n' ';')"

perf_ok=0
perf stat -x, -e instructions:u -o "$w/perf.txt" true > /dev/null 2>&1 && grep -q '^[0-9]' "$w/perf.txt" && perf_ok=1
skip() { for n in "$@"; do echo "skip: $n: perf stat cannot count instructions here"; done; }
if [ $perf_ok = 0 ]; then
  skip cache-la neutral-cache neutral-default-la check-first-kept check-kept check-rebuilt check-runtime check-failed check-worktree
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
  && grep -q '^sources: files 12 ll-same 10 LL-DIFF 0 refused-same 1 refused-msg 0 STATUS 0 new 0 changed 1 ' "$w/neutral/summary.txt" \
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
(cd "$r" && sh tools/change-check.sh "$b" HEAD "$w/cc" < /dev/null > "$w/cc.log" 2>&1); rc=$?
t=$w/cc/table.md
row() { grep -F "| $1 |" "$t" 2>/dev/null; }
row 'a tool changed' | grep -q '| 1 | kept | none matched | 0 | - (no rebuild) | - |'
say check-first-kept $? "$(row 'a tool changed')"
row 'a test changed' | grep -q '| 1 | kept | 0 failed, 1 passed: test_other | 1: tests/cases/test_other.av | - (no rebuild) | - |'
say check-kept $? "$(row 'a test changed')"
row 'the compiler changed' | grep -q '| 1 | rebuilt | none matched | 9: .* | 2: a.av, b.av | a [0-9.]*, b [0-9.]* |'
say check-rebuilt $? "$(row 'the compiler changed')"
row 'the runtime changed' | grep -q '| 1 | rebuilt | none matched | 0 | 0 | a [0-9.]*, b [0-9.]* |'
say check-runtime $? "$(row 'the runtime changed')"
[ $rc -eq 1 ] && row 'a test fails' | grep -q '| FAILED (rc=1) 1 failed, 0 passed: test_m |'
say check-failed $? "rc=$rc $(row 'a test fails') $(tail -n 3 "$w/cc.log" | tr '\n' ';')"
[ ! -e "$w/cc/work" ] && [ "$(git -C "$r" worktree list | wc -l)" = 1 ]; say check-worktree $? "$(git -C "$r" worktree list | tr '\n' ';')"
echo done
