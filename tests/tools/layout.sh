#!/bin/sh
# tools/layout-flag.py and tools/layout-retime.sh. Run by
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
#     shift's C file, and nothing is left in TMPDIR.
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
echo done
