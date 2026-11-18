#!/bin/sh
# The timing tools' undisturbed rule (tools/rounds.py, tools/lr-table.sh).
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/measure-rule.sh WORK
#
# WORK is a new scratch directory. The judge reads spawner results written
# here (an idle run, a time-shared one, one with many involuntary switches,
# one beside other work). Two fake tasks in a fake run.js: one that computes
# for about 20 ms (whatever the machine's load, its counters are recorded)
# and one that sleeps 60 ms before it prints (its CPU time is far under its
# wall, so the rule calls every run of it disturbed, deterministically).
# rounds.py measures both over two rounds with one retry; lr-table.sh
# re-measures them with one retry and tables them against a fake
# reference; a library.tsv names both as library-backed and gives the busy
# one a pure twin (a copy of its binary), which the re-measure times beside
# them and the table and pure table mark. Prints one line per check, `ok: NAME` or `FAIL: NAME: why`,
# then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/measure-rule.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w/bin" "$w/res"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
R="python3 -I $root/tools/rounds.py"

# 1. The judge, on results in the spawner's form: wall_ns maxrss utime_us
# stime_us status nvcsw nivcsw busy_mine busy_all cpus hz (ticks at 100 Hz).
judge() { # NAME LINE EXPECT
  echo "$2" > "$w/res/$1"
  got=$($R judge "$w/res/$1" < /dev/null); rc=$?
  case "$3:$rc:$got" in yes:0:*"	yes"|no:3:*"	no: "*) say "$1" 0 ;; *) say "$1" 1 "status $rc: $got" ;; esac
}
judge judge-idle "1000000000 2048 995000 2000 0 1 5 100 100 32 100" yes
judge judge-timeshare "1000000000 2048 500000 0 0 1 180 100 100 32 100" no
judge judge-switches "1000000000 2048 990000 5000 0 900 1000 100 100 32 100" no
judge judge-outside "1000000000 2048 995000 2000 0 1 5 200 200 32 100" no

# 2. The fake tasks and run.js.
cat > "$w/task.c" <<'EOF'
#include <stdio.h>
#include <time.h>
#include <unistd.h>
int main(int argc, char **argv) {
#ifdef SLEEP
  usleep(60000);
#else
  volatile unsigned long x = 0;
  clock_t end = clock() + CLOCKS_PER_SEC / 50;
  while (clock() < end) x++;
#endif
  puts(CHECKSUM);
  return 0;
}
EOF
cc -O1 -DCHECKSUM='"111"' -o "$w/bin/fake_busy" "$w/task.c" && cc -O1 -DSLEEP -DCHECKSUM='"222"' -o "$w/bin/fake_sleep" "$w/task.c" \
  || { echo "FAIL: rounds-counters: cannot compile the fake tasks"; echo done; exit 0; }
cat > "$w/run.js" <<'EOF'
[
  {
    "name": "Fake::Busy",
    "checksum": 111,
  },
  {
    "name": "Fake::Sleep",
    "checksum": 222,
  },
]
EOF

# 3. rounds.py: two rounds, one retry.
ROUNDS_RULE=retries=1 $R langarena --rounds 2 --run-js "$w/run.js" --out "$w/rounds" --side x="$w/bin" --allow-governor < /dev/null > "$w/rounds.log" 2>&1
d=$w/rounds/raw
grep -q '^nivcsw	' "$d/round-1/x--fake_busy.run" && grep -q '^outside_mine_s	' "$d/round-1/x--fake_busy.run" && grep -q '^undisturbed	' "$d/round-1/x--fake_busy.run"
say rounds-counters $? "$(tail -3 "$w/rounds.log")"
[ -f "$d/round-1/x--fake_sleep.disturbed-1" ] && [ -f "$d/round-2/x--fake_sleep.disturbed-1" ] && [ ! -f "$d/round-1/x--fake_sleep.disturbed-2" ]
say rounds-retried $? "$(ls "$d/round-1" 2>/dev/null | tr '\n' ' ')"
grep -q '^undisturbed	no$' "$d/round-1/x--fake_sleep.run" && grep -q '^disturbed	CPU time .* under the wall' "$d/round-1/x--fake_sleep.run"
say rounds-kept-disturbed $? "$(tail -4 "$d/round-1/x--fake_sleep.run" 2>/dev/null | tr '\n' ' ')"
! grep -q 'Fake::Sleep' "$w/rounds/rounds.tsv"; say rounds-table-excludes $? "$(grep Sleep "$w/rounds/rounds.tsv")"
grep -q '^## Disturbed runs' "$w/rounds/rounds.md" && grep -q '^| x--fake_sleep | 1 |' "$w/rounds/rounds.md" \
  && grep -q 'Rows short of undisturbed runs.*x Fake::Sleep: 0 undisturbed of 2 rounds' "$w/rounds/rounds.md"
say rounds-table-lists $? "$(grep -c fake_sleep "$w/rounds/rounds.md" 2>/dev/null) lines"
grep -q '^Fake::Sleep	disturbed	' "$w/rounds/x.lr-avant.tsv"; say rounds-lr-disturbed $? "$(grep Sleep "$w/rounds/x.lr-avant.tsv")"

# 4. lr-table.sh: the re-measure's counters, one retry, the table's notes;
# the twin's row, the library marks and the pure table.
cp "$w/bin/fake_busy" "$w/bin/fake_busy-pure"
printf 'task\tlibrary\ttwin\ttwin_slug\nFake::Busy\tlibbusy (C)\tFake::Busy-Pure\tfake_busy-pure\nFake::Sleep\tlibsleep (C)\t-\t-\n' > "$w/bin/library.tsv"
LR_RETRIES=1 sh "$root/tools/lr-table.sh" avant "$w/bin" "$w/run.js" "$w/lr/lr-avant.tsv" < /dev/null > "$w/lr.log" 2>&1 || mkdir -p "$w/lr"
mkdir -p "$w/lr"
awk -F'\t' '$1 == "task" { exit !(NF == 13 && $7 == "user_s" && $13 == "undisturbed") }' "$w/lr/lr-avant.tsv" \
  && awk -F'\t' '$1 == "Fake::Busy" { exit !(NF == 13 && $2 == "ok" && $10 ~ /^[0-9]+$/) }' "$w/lr/lr-avant.tsv"
say lr-columns $? "$(sed -n '/^task/,$p' "$w/lr/lr-avant.tsv" 2>/dev/null | tr '\t\n' ' ;')"
awk -F'\t' '$1 == "Fake::Sleep" { exit !($2 == "ok" && $12 == 2 && $13 ~ /^no: CPU time/) }' "$w/lr/lr-avant.tsv" \
  && [ -f "$w/lr/lr-avant.tsv.raw/fake_sleep.1.res" ] && [ -f "$w/lr/lr-avant.tsv.raw/fake_sleep.2.res" ]
say lr-retried $? "$(grep Sleep "$w/lr/lr-avant.tsv" 2>/dev/null)"
mkdir -p "$w/ref"
printf '# label: Other\ntask\tstatus\twall_s\trss_kb\tinprog_s\texit\nFake::Busy\tok\t0.0100\t1000\t-\t0\nFake::Sleep\tok\t0.0100\t1000\t-\t0\n' > "$w/ref/other.tsv"
sh "$root/tools/lr-table.sh" table "$w/ref" "$w/lr/lr-avant.tsv" < /dev/null > "$w/table.md" 2> "$w/table.err"
grep 'Fake::Sleep' "$w/table.md" | head -n 1 | grep -q 'Avant: disturbed (2 attempts);'
say lr-table-notes $? "$(grep -m 1 'Fake::Sleep' "$w/table.md")"
awk -F'\t' '$1 == "Fake::Busy-Pure" { exit !(NF == 13 && $2 == "ok") }' "$w/lr/lr-avant-pure.tsv" && cmp -s "$w/bin/library.tsv" "$w/lr/library.tsv"
say lr-pure-measured $? "$(grep Pure "$w/lr/lr-avant-pure.tsv" 2>/dev/null)"
grep -q '^Avant source: .* 2 of the 2 rows rely on a library' "$w/table.md" && grep -q '^Avant as written: wall sum ' "$w/table.md" \
  && grep 'Fake::Busy' "$w/table.md" | head -n 1 | grep -q '| Fake::Busy | .* library: libbusy (C); |$'
say lr-table-library $? "$(head -n 3 "$w/table.md" | tr '\n' ' ')"
sh "$root/tools/lr-table.sh" pure "$w/ref" "$w/lr/lr-avant.tsv" < /dev/null > "$w/pure.md" 2> "$w/pure.err"
grep -q '^Avant as written: .* Pure Avant: wall sum ' "$w/pure.md" \
  && grep 'Fake::Busy' "$w/pure.md" | head -n 1 | grep -q '| Fake::Busy | .* pure twin of libbusy (C); |$' \
  && grep 'Fake::Sleep' "$w/pure.md" | head -n 1 | grep -q 'still on a library: libsleep (C); |$'
say lr-table-pure $? "$(cat "$w/pure.err") $(grep -m 1 'Fake::Busy' "$w/pure.md")"
mkdir -p "$w/refrun"
sh "$root/tools/lr-table.sh" ref Fake "$w/bin" "$w/refrun/fake.tsv" "$w/run.js" /bin/echo < /dev/null > "$w/refrun.log" 2>&1
grep -q "^cpu: $(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)\$" "$w/refrun/machine.txt" \
  && grep -q "^threads: $(getconf _NPROCESSORS_ONLN)\$" "$w/refrun/machine.txt"
say lr-machine $? "$(tr '\n' ';' < "$w/refrun/machine.txt" 2>/dev/null)"
echo done
