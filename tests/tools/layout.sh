#!/bin/sh
# tools/layout-flag.py, tools/layout-retime.sh, tools/layout-cc.sh,
# tools/layout-runtime.sh, tools/la-mem.py,
# tools/layout-spread.py and tools/rounds.py --layouts. Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/layout.sh WORK
#
# WORK is a new scratch directory.
#   flag-equal-counts: a task whose time moved 10% at an equal instruction
#     count is named "layout?", with its changes, in the line and the TSV;
#   flag-counts-moved: one whose count moved 2% too is not;
#   flag-small: one whose time moved 1% (or 0.01 s) is not;
#   retime-follows: tools/layout-retime.sh in a fake checkout whose stub
#     C' makes the shifted binary (`--cc`) slower: the runs alternate and
#     the verdict is that the time follows the layout;
#   retime-overlap: with both binaries alike (each run 0.05 s or 0.25 s
#     in turn, so their ranges overlap whatever the noise), "not separated";
#   retime-args: the stub saw the harness's compile arguments and the
#     shift's C file, and nothing is left in TMPDIR;
#   cc-shifts: tools/layout-cc.sh as CC links a C program as asked and once
#     per shift: shift 0 is the same bytes, and main sits 64 and 192 bytes
#     later in the others, each of which runs;
#   cc-passthrough: a compile (-c) and a link without LAYOUT_DIR pass
#     through untouched; cc-bad-shift: a shift not a multiple of 64 fails;
#   rounds-layouts: rounds.py --layouts runs round r at the
#     ((r - 1) mod K)-th shift, names it in each raw file and lists every
#     layout's binary;
#   spread-verdicts: layout-spread.py calls a task whose layouts differ
#     beyond its noise `layout`, one alike at every layout `ok`, and one
#     whose copies of shift 0 differ as much `noisy`;
#   spread-compare: compare reads two spread tables;
#   flag-spread: layout-flag.py --spread keeps a task whose base time lies
#     within the head's layouts layout?, and calls one outside them beyond.
#   check-rounds-many: tools/layout-check.sh time with four tasks and a
#     rounds argument (a stub rounds.py records its arguments) asks for
#     that many rounds and passes every task, the fourth one included;
#     without the argument, one round per shift.
#   check-build-subline: tools/layout-check.sh build in a fake checkout
#     whose stub C' places main AVANT_CODE_SHIFT bytes into its line: the
#     whole-line shifts are linked as before, each sub-line shift compiled
#     with AVANT_CODE_SHIFT set, every layout's binary runs, and
#     shifts.txt lists them all;
#   check-build-subline-checked: a stub that ignores AVANT_CODE_SHIFT is
#     caught (main not where the shift puts it), the build fails;
#   check-build-subline-bad: a sub-line shift of 64 is refused;
#   check-build-loops-bad: a loop alignment other than 16 or 64 is refused;
#   cc-c-asm-same: a compile through layout-cc.sh's assembly path with no
#     shift is the plain compile's object byte for byte;
#   cc-c-shift: LAYOUT_C_SHIFT=16 puts every function 16 bytes into its
#     line and leaves no patchable-entry section;
#   cc-runtime: LAYOUT_RUNTIME links a runtime object from its directory,
#     and a missing one fails the link with status 2;
#   check-build-runtime-moving: layout-check.sh build (its default method)
#     in a fake checkout with a runtime: each sub-line layout's binary
#     links the runtime built at its shift (avant_alloc 16 and 32 bytes
#     into its line, 32l16 included), shift 0 the checkout's own;
#   check-build-runtime-fixed: LAYOUT_CHECK_RUNTIME=fixed keeps the
#     runtime at offset 0 at a sub-line layout;
#   runtime-shift0-same: tools/layout-runtime.sh --check at shift 0
#     rebuilds the checkout's runtime objects byte for byte;
#   mem-compare: tools/la-mem.py reads a lower average over the tasks (the
#     twins left out), a program whose ranges part as moved, and one whose
#     runs take two values as one to run again.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/layout.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }

# 1. The flag.
printf '# label: Avant\ntask\tstatus\twall_s\n' > "$w/head-lr.tsv"; cp "$w/head-lr.tsv" "$w/base-lr.tsv"
printf 'A::Same\tok\t1.100\nB::Moved\tok\t1.100\nC::Small\tok\t1.010\nD::Tiny\tok\t0.110\n' >> "$w/head-lr.tsv"
printf 'A::Same\tok\t1.000\nB::Moved\tok\t1.000\nC::Small\tok\t1.000\nD::Tiny\tok\t0.100\n' >> "$w/base-lr.tsv"
printf 'a_same\t1000000\tx\t0\t1\nb_moved\t1020000\tx\t0\t1\nc_small\t1000000\tx\t0\t1\nd_tiny\t1000000\tx\t0\t1\n' > "$w/head-ins.tsv"
printf 'a_same\t1000000\tx\t0\t1\nb_moved\t1000000\tx\t0\t1\nc_small\t1000000\tx\t0\t1\nd_tiny\t1000000\tx\t0\t1\n' > "$w/base-ins.tsv"
o=$(python3 -I "$root/tools/layout-flag.py" "$w/head-lr.tsv" "$w/base-lr.tsv" "$w/head-ins.tsv" "$w/base-ins.tsv" "$w/layout.tsv" 2>&1)
echo "$o" | grep -q '^layout: 1 of 4 tasks layout? (.*): A::Same +10.0% (instructions +0.000%); one layout experiment' \
  && [ "$(sed -n '2p' "$w/layout.tsv" | cut -f1,2,5)" = "A::Same	a_same	+10.0" ] && [ "$(wc -l < "$w/layout.tsv")" = 2 ]
say flag-equal-counts $? "$o / $(tr '\n' ';' < "$w/layout.tsv" 2>/dev/null)"
! echo "$o" | grep -q 'B::Moved'; say flag-counts-moved $? "$o"
! echo "$o" | grep -q 'C::Small\|D::Tiny'; say flag-small $? "$o"

# 2. The re-timing, in a fake checkout with a stub C'.
f=$w/fake/avant; mkdir -p "$f/tools/layout" "$f/bin"
cp "$root/tools/layout-retime.sh" "$f/tools/"; cp "$root/tools/layout/plt.c" "$f/tools/layout/"
cat > "$f/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
# compile SRC OUT ARGS...: a program that sleeps, longer with --cc when
# STUB_SLOW_SHIFT=1; the arguments are logged. With STUB_SPREAD=1 each
# binary's runs sleep 0.05 s and 0.25 s in turn (its own counter), so the
# two ranges overlap by construction: a noisy machine cannot separate
# two alike binaries by chance.
[ "$1" = compile ] || exit 2
src=$2 out=$3; shift 3
echo "$*" >> "$STUB_ARGS"
d=0.05
case " $* " in *" --cc "*) [ "${STUB_SLOW_SHIFT:-0}" = 1 ] && d=0.4 ;; esac
if [ "${STUB_SPREAD:-0}" = 1 ]; then
  printf '#!/bin/sh\nn=$(cat "%s.n" 2>/dev/null || echo 0); echo $((n + 1)) > "%s.n"\nif [ $((n %% 2)) = 1 ]; then sleep 0.25; else sleep 0.05; fi\necho checksum-1\n' "$out" "$out" > "$out"
else
  printf '#!/bin/sh\nsleep %s\necho checksum-1\n' "$d" > "$out"
fi
chmod +x "$out"
STUB
chmod +x "$f/bin/avant-c-llvm"
la=$w/part; mkdir -p "$la/avant/langarena/tmp" "$la/args"
echo 'task' > "$la/avant/langarena/tmp/a_task.av"; printf -- '--release\n--cpu=v3\n' > "$la/args/a_task"
mkdir -p "$w/tmp"
(cd "$f" && env TMPDIR="$w/tmp" STUB_ARGS="$w/args1.log" STUB_SLOW_SHIFT=1 sh tools/layout-retime.sh "$la" a_task 3 < /dev/null > "$w/retime1.out" 2>&1); rc=$?
[ $rc -eq 0 ] && [ "$(grep -c '^a_task plain wall=' "$w/retime1.out")" = 3 ] && [ "$(grep -c '^a_task shifted wall=' "$w/retime1.out")" = 3 ] \
  && grep -q '^verdict: the time follows the layout (median +[0-9.]*% with the code shifted)$' "$w/retime1.out"
say retime-follows $? "rc=$rc $(tr '\n' ';' < "$w/retime1.out")"
(cd "$f" && env TMPDIR="$w/tmp" STUB_ARGS="$w/args2.log" STUB_SPREAD=1 sh tools/layout-retime.sh "$la" a_task 3 < /dev/null > "$w/retime2.out" 2>&1); rc=$?
[ $rc -eq 0 ] && grep -q '^verdict: not separated' "$w/retime2.out"
say retime-overlap $? "rc=$rc $(tr '\n' ';' < "$w/retime2.out")"
grep -qx -- '--release --cpu=v3' "$w/args1.log" && grep -qx -- "--release --cpu=v3 --cc $f/tools/layout/plt.c" "$w/args1.log" && [ -z "$(ls "$w/tmp")" ]
say retime-args $? "$(tr '\n' ';' < "$w/args1.log"); left: $(ls "$w/tmp" | tr '\n' ' ')"
# 3. The CC wrapper, on a real link.
c=$w/cc; mkdir -p "$c"
printf '#include <stdio.h>\nint main(void) { puts("42"); return 0; }\n' > "$c/p.c"
cc -c -O1 -falign-functions=64 -o "$c/p.o" "$c/p.c"
(cd "$c" && env CC= LAYOUT_DIR="$c/lay" LAYOUT_SHIFTS="0 64 192" sh "$root/tools/layout-cc.sh" p.o -o prog < /dev/null > "$c/log" 2>&1); rc=$?
addr() { nm "$1" 2>/dev/null | awk '$3 == "main" { print $1 }'; }
a0=$(addr "$c/lay/0/prog"); a1=$(addr "$c/lay/64/prog"); a2=$(addr "$c/lay/192/prog")
[ $rc -eq 0 ] && cmp -s "$c/prog" "$c/lay/0/prog" && [ $((0x$a1 - 0x$a0)) = 64 ] && [ $((0x$a2 - 0x$a0)) = 192 ] \
  && [ "$("$c/lay/64/prog")" = 42 ] && [ "$("$c/lay/192/prog")" = 42 ]
say cc-shifts $? "rc=$rc main at $a0 $a1 $a2; $(cat "$c/log")"
(cd "$c" && env LAYOUT_DIR="$c/lay2" LAYOUT_SHIFTS="64" sh "$root/tools/layout-cc.sh" -c -O1 -o q.o p.c < /dev/null \
  && env -u LAYOUT_DIR sh "$root/tools/layout-cc.sh" q.o -o q < /dev/null) > "$c/log2" 2>&1; rc=$?
[ $rc -eq 0 ] && [ -f "$c/q.o" ] && [ -x "$c/q" ] && [ ! -e "$c/lay2" ]
say cc-passthrough $? "rc=$rc $(cat "$c/log2")"
(cd "$c" && env LAYOUT_DIR="$c/lay3" LAYOUT_SHIFTS="0 48" sh "$root/tools/layout-cc.sh" p.o -o r < /dev/null > "$c/log3" 2>&1); rc=$?
[ $rc -eq 2 ] && grep -q 'shift 48 is not a multiple of 64' "$c/log3"
say cc-bad-shift $? "rc=$rc $(cat "$c/log3")"

# 4. rounds.py --layouts: two fake layouts of one fake task.
r=$w/rl; mkdir -p "$r/bin/0" "$r/bin/64"
printf '#!/bin/sh\necho 111\n' > "$r/bin/0/fake_one"; printf '#!/bin/sh\n# moved\necho 111\n' > "$r/bin/64/fake_one"
chmod +x "$r/bin/0/fake_one" "$r/bin/64/fake_one"
printf '[\n  {\n    "name": "Fake::One",\n    "checksum": 111,\n  },\n]\n' > "$r/run.js"
python3 -I "$root/tools/rounds.py" langarena --rounds 3 --run-js "$r/run.js" --out "$r/s" --side x="$r/bin" --layouts 0,64 --allow-governor \
  --cpu-tolerance 1 --max-involuntary 100000 --max-outside 100 < /dev/null > "$r/log" 2>&1; rc=$?
[ $rc -eq 0 ] && grep -qx 'layout	0' "$r/s/raw/round-1/x--fake_one.run" && grep -qx 'layout	64' "$r/s/raw/round-2/x--fake_one.run" \
  && grep -qx 'layout	0' "$r/s/raw/round-3/x--fake_one.run" && grep -q '  x@64--fake_one$' "$r/s/binaries.sha256" \
  && grep -q '  x@0--fake_one$' "$r/s/binaries.sha256" && grep -q '^Layouts: round r ran' "$r/s/rounds.md" && grep -q '^layouts: shifts 0 64' "$r/s/session.txt"
say rounds-layouts $? "rc=$rc $(tail -2 "$r/log" | tr '\n' ';')"

# 5. layout-spread.py on fabricated sessions.
sp=$w/sp; mkdir -p "$sp"
run() { # ROUND SIDE TASK WALL
  mkdir -p "$sp/s/raw/round-$1"
  printf 'id\t%s--x\nround\t%s\nwall_ns\t%d\nexit\t0\noutput\tok\ntask\t%s\nside\t%s\nundisturbed\tyes\n' "$2" "$1" "$4" "$3" "$2" \
    > "$sp/s/raw/round-$1/$2--$(echo "$3" | tr -d ':' | tr A-Z a-z).run"
}
for k in 1 2 3; do
  run $k s0 A::Moves 1000000000; run $k s64 A::Moves 1100000000; run $k s0c1 A::Moves 1001000000
  run $k s0 B::Still 1000000000; run $k s64 B::Still 1002000000; run $k s0c1 B::Still 1001000000
  run $k s0 C::Noisy 1000000000; run $k s64 C::Noisy 1050000000; run $k s0c1 C::Noisy 1100000000
done
o=$(python3 -I "$root/tools/layout-spread.py" spread --out "$sp/out" "$sp/s" 2>&1); rc=$?
v() { awk -F'\t' -v t="$1" '$1 == t { print $9 }' "$sp/out/spread.tsv"; }
[ $rc -eq 1 ] && [ "$(v A::Moves)" = layout ] && [ "$(v B::Still)" = ok ] && [ "$(v C::Noisy)" = noisy ] \
  && echo "$o" | grep -q '^layout spread: 3 tasks; 1 over the bar (2.0%) and their noise, 1 noisy at one layout;'
say spread-verdicts $? "rc=$rc $o; $(tr '\n' ';' < "$sp/out/spread.tsv" 2>/dev/null)"
o=$(python3 -I "$root/tools/layout-spread.py" compare "$sp/out/spread.tsv" "$sp/out/spread.tsv" "$sp/cmp.md" 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$o" | grep -q '^compare: 3 tasks; median ratio new/base geometric mean 1.0000' && grep -q '^| A::Moves |' "$sp/cmp.md"
say spread-compare $? "rc=$rc $o"

# 6. layout-flag.py --spread on the flag's TSV and a head's layout runs.
fs=$w/fs; mkdir -p "$fs/rounds/raw/round-1" "$fs/rounds/raw/round-2"
printf 'task\tslug\tbase_s\thead_s\ttime_pct\tins_pct\nA::In\ta_in\t1.000\t1.100\t+10.0\t+0.000\nB::Out\tb_out\t1.000\t1.100\t+10.0\t+0.000\n' > "$fs/layout.tsv"
for k in 1 2; do
  printf 'task\tA::In\nround\t%d\nwall_ns\t%d\nexit\t0\nlayout\t%d\nundisturbed\tyes\n' $k $((900000000 + k * 100000000)) $((k * 64)) > "$fs/rounds/raw/round-$k/head--a_in.run"
  printf 'task\tB::Out\nround\t%d\nwall_ns\t%d\nexit\t0\nlayout\t%d\nundisturbed\tyes\n' $k $((1050000000 + k * 10000000)) $((k * 64)) > "$fs/rounds/raw/round-$k/head--b_out.run"
done
o=$(python3 -I "$root/tools/layout-flag.py" --spread "$fs/layout.tsv" "$fs/rounds" 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$o" | grep -q "1 within the spread across the head's layouts (layout?): A::In +10.0% (base 1.000 s, head 1.000-1.100 s across 2 layouts); 1 beyond it: B::Out" \
  && [ "$(awk -F'\t' '$1 == "B::Out" { print $10 }' "$fs/layout.tsv")" = beyond ] && [ "$(awk -F'\t' '$1 == "A::In" { print $10 }' "$fs/layout.tsv")" = 'layout?' ]
say flag-spread $? "rc=$rc $o"

# 7. layout-check.sh time with four candidates: the rounds argument is not
# a task's name. A fake checkout's rounds.py is the real one when imported
# (the task names) and, run, records its arguments.
lc=$w/lc; mkdir -p "$lc/root/tools" "$lc/d/layouts/0" "$lc/d/layouts/64"
cp "$root/tools/layout-check.sh" "$lc/root/tools/"
cat > "$lc/root/tools/rounds.py" <<'PY'
import sys
if __name__ == "__main__":
    open(sys.argv[0] + ".args", "a").write(" ".join(sys.argv[1:]) + "\n")
    sys.exit(0)
exec(open(REAL).read())
PY
sed -i "s#REAL#'$root/tools/rounds.py'#" "$lc/root/tools/rounds.py"
echo '0 64' > "$lc/d/shifts.txt"
printf '[\n' > "$lc/run.js"
for t in A::One B::Two C::Three D::Four; do
  s=$(echo "$t" | sed 's/::/_/' | tr A-Z a-z)
  for k in 0 64; do printf '#!/bin/sh\necho 1\n' > "$lc/d/layouts/$k/$s"; chmod +x "$lc/d/layouts/$k/$s"; done
  printf '  {\n    "name": "%s",\n    "checksum": 1,\n  },\n' "$t" >> "$lc/run.js"
done
printf ']\n' >> "$lc/run.js"
sh "$lc/root/tools/layout-check.sh" time "$lc/d" "$lc/run.js" 3 < /dev/null > "$lc/log" 2>&1; rc=$?
a=$(cat "$lc/root/tools/rounds.py.args" 2>/dev/null)
[ $rc -eq 0 ] && echo "$a" | grep -q -- '--rounds 3 ' && echo "$a" | grep -q ' A::One B::Two C::Three D::Four$'
r1=$?
rm -f "$lc/root/tools/rounds.py.args"
sh "$lc/root/tools/layout-check.sh" time "$lc/d" "$lc/run.js" < /dev/null >> "$lc/log" 2>&1; rc2=$?
b=$(cat "$lc/root/tools/rounds.py.args" 2>/dev/null)
[ $r1 -eq 0 ] && [ $rc2 -eq 0 ] && echo "$b" | grep -q -- '--rounds 2 ' && echo "$b" | grep -q ' A::One B::Two C::Three D::Four$'
say check-rounds-many $? "rc=$rc/$rc2 args: $a / $b; $(tail -2 "$lc/log" | tr '\n' ';')"

# 8. layout-check.sh build with sub-line shifts, in a fake checkout whose
# stub C' links a program whose main sits AVANT_CODE_SHIFT bytes into its
# 64-byte line (what the real C' does to every function), through $CC
# (tools/layout-cc.sh for the whole-line shifts).
lb=$w/lb; mkdir -p "$lb/root/tools" "$lb/root/bin" "$lb/la/avant/langarena/tmp" "$lb/la/args"
cp "$root/tools/layout-check.sh" "$root/tools/layout-cc.sh" "$lb/root/tools/"
cat > "$lb/root/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
out=$3
k=${AVANT_CODE_SHIFT:-0}; [ -n "${STUB_IGNORE_SHIFT:-}" ] && k=0
printf '.text\n.p2align 6\n.skip %d, 0x90\n.globl main\nmain:\n xorl %%eax, %%eax\n ret\n.section .note.GNU-stack,"",@progbits\n' "$k" > "$out.s"
${CC:-cc} -o "$out" "$out.s"; rc=$?
rm -f "$out.s"; exit $rc
STUB
chmod +x "$lb/root/bin/avant-c-llvm"
echo 'fn main {}' > "$lb/la/avant/langarena/tmp/x_one.av"; printf -- '--release\n' > "$lb/la/args/x_one"
LAYOUT_CHECK_RUNTIME=fixed LAYOUT_CHECK_SHIFTS="0 64" LAYOUT_CHECK_SUBLINE="16 32" sh "$lb/root/tools/layout-check.sh" build "$lb/la" "$lb/d" x_one < /dev/null > "$lb/log" 2>&1; rc=$?
ok=0
for k in 0 64 16 32; do "$lb/d/layouts/$k/x_one" < /dev/null || ok=1; done
m16=$(nm "$lb/d/layouts/16/x_one" 2>/dev/null | awk '$3 == "main" { print $1 }')
[ $rc -eq 0 ] && [ $ok -eq 0 ] && [ "$(cat "$lb/d/shifts.txt")" = "0 64 16 32" ] && [ -n "$m16" ] && [ $((0x$m16 % 64)) -eq 16 ]
say check-build-subline $? "rc=$rc runs=$ok main@16=$m16 shifts=$(cat "$lb/d/shifts.txt" 2>/dev/null); $(tail -2 "$lb/log" | tr '\n' ';')"
STUB_IGNORE_SHIFT=1 LAYOUT_CHECK_RUNTIME=fixed LAYOUT_CHECK_SHIFTS="0" LAYOUT_CHECK_SUBLINE="16" sh "$lb/root/tools/layout-check.sh" build "$lb/la" "$lb/d2" x_one < /dev/null > "$lb/log2" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q 'at sub-line shift 16: main is at .*, not 16 bytes into its line' "$lb/log2"
say check-build-subline-checked $? "rc=$rc $(tr '\n' ';' < "$lb/log2")"
LAYOUT_CHECK_SUBLINE="64" sh "$lb/root/tools/layout-check.sh" build "$lb/la" "$lb/d3" x_one < /dev/null > "$lb/log3" 2>&1; rc=$?
[ $rc -eq 2 ] && grep -q 'sub-line shift 64 is not below 64' "$lb/log3"
say check-build-subline-bad $? "rc=$rc $(tr '\n' ';' < "$lb/log3")"
LAYOUT_CHECK_RUNTIME=fixed LAYOUT_CHECK_SUBLINE="16l32" sh "$lb/root/tools/layout-check.sh" build "$lb/la" "$lb/d4" x_one < /dev/null > "$lb/log4" 2>&1; rc=$?
[ $rc -eq 2 ] && grep -q "the loops' alignment is 16 or 64" "$lb/log4"
say check-build-loops-bad $? "rc=$rc $(tr '\n' ';' < "$lb/log4")"

# 9. The C code's own shift (layout-cc.sh's LAYOUT_C_SHIFT, LAYOUT_C_ASM,
# LAYOUT_RUNTIME) and the runtime moving with the program at the sub-line
# layouts (tools/layout-runtime.sh, layout-check.sh's default method).
lr=$w/lr; mkdir -p "$lr"
printf 'int f(int x) { int s = 0; for (int i = 0; i < x; i++) s += i * x; return s; }\nint g(int x) { return x + 1; }\n' > "$lr/u.c"
cc -c -O2 -falign-functions=64 -o "$lr/plain.o" "$lr/u.c"
LAYOUT_C_ASM=1 sh "$root/tools/layout-cc.sh" -c -O2 -falign-functions=64 -o "$lr/asm.o" "$lr/u.c"; rc=$?
[ $rc -eq 0 ] && cmp -s "$lr/plain.o" "$lr/asm.o"
say cc-c-asm-same $? "rc=$rc: the compile through assembly differs from the plain one"
LAYOUT_C_SHIFT=16 sh "$root/tools/layout-cc.sh" -c -O2 -falign-functions=64 -o "$lr/s16.o" "$lr/u.c"; rc=$?
fa=$(nm "$lr/s16.o" | awk '$3 == "f" { print $1 }'); ga=$(nm "$lr/s16.o" | awk '$3 == "g" { print $1 }')
[ $rc -eq 0 ] && [ $((0x$fa % 64)) -eq 16 ] && [ $((0x$ga % 64)) -eq 16 ] \
  && [ "$(objdump -d --no-show-raw-insn "$lr/s16.o" | grep -c -E '^\s+[0-9a-f]+:\s+[a-z]' )" -gt 0 ] \
  && ! readelf -S "$lr/s16.o" | grep -q patchable
say cc-c-shift $? "rc=$rc f@$fa g@$ga"
mkdir -p "$lr/x/runtime/build" "$lr/rt"
printf 'int avant_alloc(int x) { return x * 3; }\n' > "$lr/rt.c"
cc -c -o "$lr/x/runtime/build/avant_rt.o" "$lr/rt.c"; cc -c -DMOVED -o "$lr/rt/avant_rt.o" "$lr/rt.c"
printf 'int avant_alloc(int);\nint main(void) { return avant_alloc(0); }\n' > "$lr/m.c"; cc -c -o "$lr/m.o" "$lr/m.c"
LAYOUT_RUNTIME="$lr/rt" sh "$root/tools/layout-cc.sh" -o "$lr/m1" "$lr/m.o" "$lr/x/runtime/build/avant_rt.o"; rc=$?
rm -f "$lr/rt/avant_rt.o"
LAYOUT_RUNTIME="$lr/rt" sh "$root/tools/layout-cc.sh" -o "$lr/m2" "$lr/m.o" "$lr/x/runtime/build/avant_rt.o" 2> "$lr/m2.err"; rc2=$?
[ $rc -eq 0 ] && "$lr/m1" && [ $rc2 -eq 2 ] && grep -q 'no such runtime object' "$lr/m2.err"
say cc-runtime $? "rc=$rc rc2=$rc2 $(cat "$lr/m2.err")"
# A fake checkout: a runtime whose build.sh compiles one unit through $CC
# into runtime/build/avant_rt.o, and a stub C' that links its program
# with that object through $CC, as the real one does.
lm=$w/lm; mkdir -p "$lm/root/tools" "$lm/root/bin" "$lm/root/runtime" "$lm/la/avant/langarena/tmp" "$lm/la/args"
cp "$root/tools/layout-check.sh" "$root/tools/layout-cc.sh" "$root/tools/layout-runtime.sh" "$lm/root/tools/"
cp "$lr/rt.c" "$lm/root/runtime/rt.c"
cat > "$lm/root/runtime/build.sh" <<'RT'
#!/bin/sh
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
mkdir -p "$root/build"
cd "$root" && ${CC:-cc} -c -O2 -falign-functions=64 -o build/avant_rt.o rt.c
RT
sh "$lm/root/runtime/build.sh"
cat > "$lm/root/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
out=$3
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
k=${AVANT_CODE_SHIFT:-0}
printf '.text\n.p2align 6\n.skip %d, 0x90\n.globl main\nmain:\n xorl %%edi, %%edi\n call avant_alloc\n ret\n.section .note.GNU-stack,"",@progbits\n' "$k" > "$out.s"
${CC:-cc} -o "$out" "$out.s" "$root/runtime/build/avant_rt.o"; rc=$?
rm -f "$out.s"; exit $rc
STUB
chmod +x "$lm/root/bin/avant-c-llvm"
echo 'fn main {}' > "$lm/la/avant/langarena/tmp/x_one.av"; printf -- '--release\n' > "$lm/la/args/x_one"
LAYOUT_CHECK_SHIFTS="0" LAYOUT_CHECK_SUBLINE="16 32l16" sh "$lm/root/tools/layout-check.sh" build "$lm/la" "$lm/d" x_one < /dev/null > "$lm/log" 2>&1; rc=$?
a16=$(nm "$lm/d/layouts/16/x_one" 2>/dev/null | awk '$3 == "avant_alloc" { print $1 }')
a32=$(nm "$lm/d/layouts/32l16/x_one" 2>/dev/null | awk '$3 == "avant_alloc" { print $1 }')
a0=$(nm "$lm/d/layouts/0/x_one" 2>/dev/null | awk '$3 == "avant_alloc" { print $1 }')
[ $rc -eq 0 ] && [ $((0x$a0 % 64)) -eq 0 ] && [ $((0x$a16 % 64)) -eq 16 ] && [ $((0x$a32 % 64)) -eq 32 ] \
  && [ "$(cat "$lm/d/method.txt")" = "runtime: moving" ] && "$lm/d/layouts/16/x_one" < /dev/null; r=$?
say check-build-runtime-moving $r "rc=$rc alloc@0=$a0 @16=$a16 @32l16=$a32 $(tail -2 "$lm/log" | tr '\n' ';')"
LAYOUT_CHECK_RUNTIME=fixed LAYOUT_CHECK_SHIFTS="0" LAYOUT_CHECK_SUBLINE="16" sh "$lm/root/tools/layout-check.sh" build "$lm/la" "$lm/d2" x_one < /dev/null > "$lm/log2" 2>&1; rc=$?
a16=$(nm "$lm/d2/layouts/16/x_one" 2>/dev/null | awk '$3 == "avant_alloc" { print $1 }')
[ $rc -eq 0 ] && [ $((0x$a16 % 64)) -eq 0 ] && [ "$(cat "$lm/d2/method.txt")" = "runtime: fixed" ]
say check-build-runtime-fixed $? "rc=$rc alloc@16=$a16"
sh "$lm/root/tools/layout-runtime.sh" --check "$lm/root/runtime" 0 "$lm/rt0" < /dev/null; rc=$?
[ $rc -eq 0 ]
say runtime-shift0-same $? "rc=$rc"

# 10. tools/la-mem.py on two sides' peak memory (la-count.sh's mem files).
lmm=$w/lmm; mkdir -p "$lmm"
printf 'a_one\t1000\t1000\t1000\t1000,1000,1000\nb_two\t4236\t4236\t4492\t4236,4492,4236\nc_three\t9000\t9000\t9000\t9000,9000,9000\nc_three-pure\t50\t50\t50\t50,50,50\n' > "$lmm/base.tsv"
printf 'a_one\t900\t900\t900\t900,900,900\nb_two\t4492\t4236\t4492\t4492,4236,4492\nc_three\t8000\t8000\t8000\t8000,8000,8000\nc_three-pure\t90\t90\t90\t90,90,90\n' > "$lmm/head.tsv"
o=$(python3 -I "$root/tools/la-mem.py" "$lmm/base.tsv" "$lmm/head.tsv" 2>&1)
echo "$o" | grep -q '^average peak over 3 tasks: .*: lower$' && echo "$o" | grep -q '^a_one.*moved$' \
  && echo "$o" | grep -q '^b_two.*repeat' && echo "$o" | grep -q '^to run again: b_two$'
say mem-compare $? "$(echo "$o" | tr '\n' ';')"
echo done
