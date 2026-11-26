#!/bin/sh
# The measuring method's rule and loop: tools/rounds.py's verdict (the
# pooled rule and the per-layout rule with its gap), its loop without a
# warm-up, --stop-settled, and the timed hold (tools/hold.py). Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/measure-method.sh WORK
#
# WORK is a new scratch directory.
#   verdict-pooled: a row whose head runs are all above every base run
#     loses beyond its spread;
#   verdict-layout: one above at three layouts of four by 2% (the ranges
#     meet at the fourth) loses by the per-layout rule;
#   verdict-gap: one above at every layout by 0.2% only (under the 0.5%
#     gap) is within its spread, and with --pl-gap 0.001 it loses;
#   verdict-faster: one below at three layouts by 2% gains by it;
#   verdict-sessions: a task timed one layout a session reads the runs of
#     all its sessions;
#   loop-no-warm-up: a fake task counts its runs: N rounds run it N times
#     a side, and session.txt says there is no warm-up;
#   loop-warm-up: --warm-up runs it once more;
#   stop-settled: a task whose runs meet on both sides (each run is fast
#     or slow in turn, the base slow first and the head fast first) leaves the rotation after round 2 of 4, and is
#     named; a task whose head is far slower runs every round;
#   stop-exempt: --stop-exempt keeps the meeting task every round;
#   stop-needs-two: --stop-settled with one side is refused;
#   hold-longest-first: tools/hold.py on two nodes, unpinned, takes the
#     chunks longest first as nodes come free (by the plan's lengths), runs
#     each with LEDGER_NODE set, and logs each in the walls form;
#   hold-after: the after steps run once every chunk has ended, logged
#     node=all;
#   hold-walls: an earlier walls file's lengths win over the plan's;
#   hold-fail: a chunk that fails makes the hold exit 1, and the others
#     still run;
#   hold-usage: a plan line of another form is refused.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/measure-method.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
R="python3 -I -B $root/tools/rounds.py"

# 1. The verdict on fabricated sessions: RUN SESSION ROUND SIDE TASK LAYOUT WALL_NS
run() {
  d=$w/$1/raw/round-$2; mkdir -p "$d"
  slug=$(echo "$4" | tr -d ':' | tr A-Z a-z)
  printf 'id\t%s--%s\nround\t%s\nwall_ns\t%d\nrss_kb\t1000\nexit\t0\noutput\tok\ntask\t%s\nside\t%s\nlayout\t%s\nundisturbed\tyes\n' \
    "$3" "$slug" "$2" "$6" "$4" "$3" "$5" > "$d/$3--$slug.run"
}
mkdir -p "$w/v1"; printf 'short\t0.2\nsides\tbase head\nlayouts\t0 16 32 48\n' > "$w/v1/params.tsv"
k=0
for L in 0 16 32 48; do
  for i in 1 2 3; do
    k=$((k + 1))
    run v1 $k base A::Pooled $L $((1000000000 + i * 1000000)); run v1 $k head A::Pooled $L $((1100000000 + i * 1000000))
    # B: 2% slower at 0, 16, 32; at 48 the head's runs meet the base's.
    if [ $L = 48 ]; then run v1 $k base B::Layout $L $((1000000000 + i * 10000000)); run v1 $k head B::Layout $L $((1005000000 + i * 1000000))
    else run v1 $k base B::Layout $L $((1000000000 + L * 10000000 + i * 1000000)); run v1 $k head B::Layout $L $((1020000000 + L * 10000000 + i * 1000000)); fi
    # C: 0.2% slower at every layout, the ranges apart; the pooled ranges meet through the layouts' own spread.
    run v1 $k base C::Small $L $((1000000000 + L * 10000000 + i * 100000)); run v1 $k head C::Small $L $((1002500000 + L * 10000000 + i * 100000))
    # D: 2% faster at 0, 16, 32.
    if [ $L = 48 ]; then run v1 $k base D::Faster $L $((1000000000 + i * 1000000)); run v1 $k head D::Faster $L $((1001000000 + i * 1000000))
    else run v1 $k base D::Faster $L $((1000000000 + L * 10000000 + i * 1000000)); run v1 $k head D::Faster $L $((980000000 + L * 10000000 + i * 1000000)); fi
  done
done
$R verdict --out "$w/v1out" "$w/v1" < /dev/null > "$w/v1.log" 2>&1; rc=$?
row() { grep "^| $1 " "$w/v1out/layouts.md" | awk -F'|' '{ print $(NF-1) }' | sed 's/^ *//; s/ *$//'; }
[ $rc -eq 0 ] && [ "$(row A::Pooled)" = "loses beyond spread" ]
say verdict-pooled $? "rc=$rc '$(row A::Pooled)' $(cat "$w/v1.log")"
[ "$(row B::Layout)" = "loses by the per-layout rule" ]
say verdict-layout $? "'$(row B::Layout)'"
$R verdict --pl-gap 0.001 --out "$w/v1gap" "$w/v1" < /dev/null > /dev/null 2>&1
g=$(grep "^| C::Small " "$w/v1gap/layouts.md" | awk -F'|' '{ print $(NF-1) }' | sed 's/^ *//; s/ *$//')
[ "$(row C::Small)" = "within spread" ] && [ "$g" = "loses by the per-layout rule" ]
say verdict-gap $? "0.5%: '$(row C::Small)', 0.1%: '$g'"
[ "$(row D::Faster)" = "gains by the per-layout rule" ]
say verdict-faster $? "'$(row D::Faster)'"
# The same B rows, one layout a session.
for L in 0 16 32 48; do
  mkdir -p "$w/s$L"; printf 'short\t0.2\nsides\tbase head\nlayouts\t%s\n' $L > "$w/s$L/params.tsv"
  for i in 1 2 3; do
    if [ $L = 48 ]; then run s$L $i base B::Layout $L $((1000000000 + i * 10000000)); run s$L $i head B::Layout $L $((1005000000 + i * 1000000))
    else run s$L $i base B::Layout $L $((1000000000 + L * 10000000 + i * 1000000)); run s$L $i head B::Layout $L $((1020000000 + L * 10000000 + i * 1000000)); fi
  done
done
$R verdict --layouts 0,16,32,48 --out "$w/sout" "$w/s0" "$w/s16" "$w/s32" "$w/s48" < /dev/null > "$w/s.log" 2>&1; rc=$?
[ $rc -eq 0 ] && grep -q '^| B::Layout .*| 3 / 0 | .* | loses by the per-layout rule |$' "$w/sout/layouts.md"
say verdict-sessions $? "rc=$rc $(grep 'B::Layout' "$w/sout/layouts.md")"

# 2. The loop. Each fake task appends a line to its side's count file at every run.
lp=$w/loop
mkdir -p "$lp/base/0" "$lp/head/0"
for sd in base head; do
  # Fake::Meets: fast and slow in turn, on both sides, so their ranges meet from the second round on.
  # (the base slow first, the head fast first: apart after round 1, meeting after round 2)
  odd=$([ $sd = base ] && echo 0.06 || echo 0.01); even=$([ $sd = base ] && echo 0.01 || echo 0.06)
  printf '#!/bin/sh\necho x >> %s/%s.meets\nn=$(wc -l < %s/%s.meets)\nif [ $((n %% 2)) = 1 ]; then sleep %s; else sleep %s; fi\necho 111\n' \
    "$lp" "$sd" "$lp" "$sd" "$odd" "$even" > "$lp/$sd/0/fake_meets"
  # Fake::Apart: the head 0.3 s slower than the base, every run.
  printf '#!/bin/sh\necho x >> %s/%s.apart\n[ %s = head ] && sleep 0.3\necho 222\n' "$lp" "$sd" "$sd" > "$lp/$sd/0/fake_apart"
done
chmod +x "$lp"/*/0/fake_*
printf '[\n  {\n    "name": "Fake::Meets",\n    "checksum": 111,\n  },\n  {\n    "name": "Fake::Apart",\n    "checksum": 222,\n  },\n]\n' > "$lp/run.js"
loop() { # OUT ROUNDS FLAGS...
  o=$1; n=$2; shift 2; rm -f "$lp"/*.meets "$lp"/*.apart
  $R langarena --rounds "$n" --run-js "$lp/run.js" --out "$lp/$o" --side base="$lp/base" --side head="$lp/head" --layouts 0 --allow-governor \
    --cpu-tolerance 1 --max-involuntary 100000 --max-outside 100 "$@" < /dev/null > "$lp/$o.log" 2>&1
}
loop nowarm 2 Fake::Apart; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$lp/base.apart")" = 2 ] && [ "$(wc -l < "$lp/head.apart")" = 2 ] && grep -qx 'rounds: 2 (no warm-up)' "$lp/nowarm/session.txt"
say loop-no-warm-up $? "rc=$rc runs $(cat "$lp/base.apart" 2>/dev/null | wc -l); $(tail -2 "$lp/nowarm.log" | tr '\n' ';')"
loop warm 2 --warm-up Fake::Apart; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$lp/base.apart")" = 3 ] && grep -qx 'rounds: 2 (after one unmeasured warm-up of every row)' "$lp/warm/session.txt"
say loop-warm-up $? "rc=$rc runs $(cat "$lp/base.apart" 2>/dev/null | wc -l)"
loop stop 4 --stop-settled; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$lp/base.meets")" = 2 ] && [ "$(wc -l < "$lp/head.apart")" = 4 ] \
  && grep -q 'round 2: settled, leaving the rotation: base--fake_meets head--fake_meets' "$lp/stop/session.txt" \
  && [ -e "$lp/stop/raw/round-2/head--fake_meets.run" ] && [ ! -e "$lp/stop/raw/round-3/head--fake_meets.run" ] && [ -e "$lp/stop/raw/round-4/head--fake_apart.run" ] \
  && grep -q '^stop: a row whose verdict is settled' "$lp/stop/session.txt"
say stop-settled $? "rc=$rc meets $(cat "$lp/base.meets" 2>/dev/null | wc -l) apart $(cat "$lp/head.apart" 2>/dev/null | wc -l); $(grep settled "$lp/stop/session.txt" 2>/dev/null)"
loop exempt 4 --stop-settled --stop-exempt Fake::Meets; rc=$?
[ $rc -eq 0 ] && [ "$(wc -l < "$lp/base.meets")" = 4 ] && ! grep -q 'leaving the rotation' "$lp/exempt/session.txt"
say stop-exempt $? "rc=$rc meets $(cat "$lp/base.meets" 2>/dev/null | wc -l)"
$R langarena --rounds 2 --run-js "$lp/run.js" --out "$lp/one" --side base="$lp/base" --layouts 0 --allow-governor --stop-settled < /dev/null > "$lp/one.log" 2>&1; rc=$?
[ $rc -ne 0 ] && grep -q 'stop-settled needs two sides' "$lp/one.log"
say stop-needs-two $? "rc=$rc $(cat "$lp/one.log")"
# 3. The hold.
h=$w/hold; mkdir -p "$h"
cat > "$h/plan" <<PLAN
# two nodes, four chunks and one step after them
chunk a 0.1 -- echo "start a \$LEDGER_NODE" >> $h/order; sleep 0.1
chunk b 0.4 -- echo "start b \$LEDGER_NODE" >> $h/order; sleep 0.4
chunk c 0.2 -- echo "start c \$LEDGER_NODE" >> $h/order; sleep 0.2
chunk d 0.3 -- echo "start d \$LEDGER_NODE" >> $h/order; sleep 0.3
after t16 -- echo "after \${LEDGER_NODE:-none}" >> $h/order
PLAN
python3 -I -B "$root/tools/hold.py" "$h/plan" --nodes 0,1 --pin none --log "$h/walls" < /dev/null > "$h/log" 2>&1; rc=$?
first=$(head -2 "$h/order" | awk '{ print $2 }' | sort | tr '\n' ' ')
[ $rc -eq 0 ] && [ "$first" = "b d " ] && [ "$(sed -n 3p "$h/order" | awk '{ print $2 }')" = c ] && [ "$(sed -n 4p "$h/order" | awk '{ print $2 }')" = a ] \
  && [ "$(grep -c ' node=[01] wall=[0-9]*s rc=0$' "$h/walls")" = 4 ] && grep -q '^start [ab] [01]$' "$h/order"
say hold-longest-first $? "rc=$rc order $(tr '\n' ';' < "$h/order") walls $(tr '\n' ';' < "$h/walls")"
[ "$(tail -1 "$h/order")" = "after none" ] && grep -q '^t16 node=all wall=0s rc=0$' "$h/walls"
say hold-after $? "$(tail -1 "$h/order"); $(grep t16 "$h/walls")"
printf 'layouts a node=2 wall=900s rc=0\nb node=1 wall=0s rc=0\n' > "$h/prev"
python3 -I -B "$root/tools/hold.py" "$h/plan" --walls "$h/prev" --dry-run < /dev/null > "$h/dry" 2>&1; rc=$?
[ $rc -eq 0 ] && [ "$(head -1 "$h/dry")" = "a	900	walls" ] && [ "$(sed -n 2p "$h/dry" | cut -f1,3)" = "d	plan" ] && grep -q '^b	0	walls$' "$h/dry" && grep -q '^estimate: ' "$h/dry"
say hold-walls $? "rc=$rc $(tr '\n' ';' < "$h/dry")"
printf 'chunk ok -- echo ran >> %s/ran\nchunk bad -- exit 3\nafter post -- echo post >> %s/ran\n' "$h" "$h" > "$h/plan2"
python3 -I -B "$root/tools/hold.py" "$h/plan2" --nodes 0 --pin none --log "$h/walls2" < /dev/null > "$h/log2" 2>&1; rc=$?
[ $rc -eq 1 ] && [ "$(cat "$h/ran" | tr '\n' ' ')" = "ran post " ] && grep -q '^bad node=0 wall=0s rc=3$' "$h/walls2" && grep -q 'not 0: bad' "$h/log2"
say hold-fail $? "rc=$rc $(cat "$h/ran" 2>/dev/null | tr '\n' ' '); $(cat "$h/log2")"
printf 'chunk x 0.1 echo no separator\n' > "$h/plan3"
python3 -I -B "$root/tools/hold.py" "$h/plan3" --pin none < /dev/null > "$h/log3" 2>&1; rc=$?
[ $rc -eq 2 ] && grep -q 'plan3:1: not ' "$h/log3"
say hold-usage $? "rc=$rc $(cat "$h/log3")"
echo done
