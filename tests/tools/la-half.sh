#!/bin/sh
# tools/la-half.sh and tools/la-half.py: the second half screened by
# instruction counts. Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/la-half.sh WORK
#
# WORK is a new scratch directory. A fake langarena checkout carries a stub
# of the half's builder (second-half/refs/run.py: `avant prod` asks the
# compiler named by AVANT_COMPILER to compile each unit, as the real one does,
# a task and a twin here) and the real tools/la-half.sh runs on it:
#   half-stitch: every unit is stitched by the builder into avant/langarena/
#     tmp/SLUG.av of the part, its compile arguments are in args/SLUG, and
#     names.tsv names each; nothing is written in the langarena checkout;
#   half-stitch-short: a builder that names a unit it does not stitch is an
#     error, status 1;
#   half-stitch-usage: a directory that is not a langarena checkout is
#     refused, status 2;
#   half-stitch-real: the real builder, when this checkout has the langarena
#     repository (langarena/second-half), stitches as many programs as it
#     has tasks and twins (`skip:` without it);
#   half-table: two count files read as a table: each program's counts and
#     ratio, the geometric mean, the programs beyond 0.1% each way, status 0;
#   half-table-red: an output that differs, a program that did not compile
#     and one only one side has are red: status 1, each named;
#   half-screen: with perf and two stub C' (the head's program runs a loop
#     more than the base's), `screen` stitches, counts both sides and writes
#     table.md with the head's program counting more (`skip:` without perf).
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/la-half.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
rm -rf "$w"
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }

# 1. A fake langarena checkout with a stub builder.
la=$w/langarena
mkdir -p "$la/second-half/refs/tasks" "$la/second-half/tasks" "$la/tasks"
echo 'fn helper_one { 1 }' > "$la/helper.av"
echo 'fn helper_two { 2 }' > "$la/second-half/tasks/helper2.av"
echo 'fn json_pure_reader { 3 }' > "$la/tasks/json_pure.av"
echo 'fn main { puts("task a") }' > "$la/second-half/tasks/task_a.av"
echo 'fn main { puts("task b") }' > "$la/second-half/tasks/task_b.av"
echo 'fn main { puts("twin b") }' > "$la/second-half/tasks/task_b_pure.av"
cat > "$la/second-half/refs/run.py" <<'PY'
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SH = os.path.dirname(HERE); LA = os.path.dirname(SH)
units = [("Task::A", "task_a.av", []), ("Task::B", "task_b.av", []), ("Task::B-Pure", "task_b_pure.av", ["../../tasks/json_pure.av"])]
if os.path.exists(os.path.join(SH, "tasks", "short.flag")):
    units.append(("Task::C", "task_c.av", []))
d = os.path.join(SH, "tmp", "prod"); os.makedirs(d, exist_ok=True)
for name, f, extras in units:
    sl = name.replace("::", "_").replace("-", "_").lower()
    if not os.path.exists(os.path.join(SH, "tasks", f)):
        print("avant compile %s (prod): FAILED in 0.0s" % name); continue
    parts = [os.path.join(LA, "helper.av"), os.path.join(SH, "tasks", "helper2.av")] + [os.path.join(SH, "tasks", e) for e in extras] + [os.path.join(SH, "tasks", f)]
    root = os.path.join(d, sl + ".av")
    with open(root, "w") as fh:
        for p in parts:
            fh.write("// file: %s\n%s\n" % (p, open(p).read()))
    r = subprocess.run([os.environ["AVANT_COMPILER"], "compile", root, os.path.join(d, sl), "--release", "--cpu=v3"], cwd=d)
    print("avant compile %s (prod): %s in 0.0s" % (name, "ok" if r.returncode == 0 else "FAILED"))
sys.exit(1)
PY
before=$(cd "$la" && find . -type f | LC_ALL=C sort | tr '\n' ' ')
sh "$root/tools/la-half.sh" stitch "$la" "$w/part" > "$w/stitch.out" 2>&1; rc=$?
ls "$w/part/avant/langarena/tmp" | tr '\n' ' ' | grep -q '^task_a.av task_b.av task_b_pure.av $' \
  && [ "$(cat "$w/part/args/task_a")" = "--release
--cpu=v3" ] \
  && [ "$(cat "$w/part/names.tsv")" = "task_a	Task::A
task_b	Task::B
task_b_pure	Task::B-Pure" ] \
  && grep -q '^fn helper_one' "$w/part/avant/langarena/tmp/task_a.av" && grep -q '^fn json_pure_reader' "$w/part/avant/langarena/tmp/task_b_pure.av" \
  && ! grep -q '^fn json_pure_reader' "$w/part/avant/langarena/tmp/task_a.av" \
  && [ "$before" = "$(cd "$la" && find . -type f | LC_ALL=C sort | tr '\n' ' ')" ] && [ $rc -eq 0 ]
say half-stitch $? "rc=$rc: $(cat "$w/stitch.out" | tr '\n' ';'); $(ls "$w/part/avant/langarena/tmp" 2>&1 | tr '\n' ' '); $(cd "$la" && find . -type f | LC_ALL=C sort | tr '\n' ' ')"
touch "$la/second-half/tasks/short.flag"
sh "$root/tools/la-half.sh" stitch "$la" "$w/part-short" > "$w/short.out" 2>&1; rc=$?
[ $rc -eq 1 ] && grep -q 'the builder named 4 programs and 3 were stitched' "$w/short.out"
say half-stitch-short $? "rc=$rc: $(tr '\n' ';' < "$w/short.out")"
rm -f "$la/second-half/tasks/short.flag"
sh "$root/tools/la-half.sh" stitch "$w" "$w/part-none" > "$w/none.out" 2>&1; rc=$?
[ $rc -eq 2 ] && grep -q 'is not a langarena checkout' "$w/none.out"
say half-stitch-usage $? "rc=$rc: $(tr '\n' ';' < "$w/none.out")"
if [ -f "$root/langarena/second-half/refs/run.py" ]; then
  n=$(ls "$root/langarena/second-half/refs/tasks"/*.json | wc -l)
  t=$(grep -c '"file"' "$root/langarena/second-half/refs/tasks"/*.json | awk -F: '{ s += $2 } END { print s }')
  sh "$root/tools/la-half.sh" stitch "$root/langarena" "$w/part-real" > "$w/real.out" 2>&1; rc=$?
  got=$(ls "$w/part-real/avant/langarena/tmp" | wc -l)
  [ $rc -eq 0 ] && [ "$got" -ge "$n" ] && [ "$got" -le "$t" ]
  say half-stitch-real $? "rc=$rc, $got programs for $n tasks: $(tr '\n' ';' < "$w/real.out")"
else
  echo "skip: half-stitch-real: this checkout has no langarena/second-half"
fi

# 2. The table.
printf 'task_a\t1000000\taaa\t0\t11\ntask_b\t2000000\tbbb\t0\t22\ntask_b_pure\t3000000\tccc\t0\t33\n' > "$w/base.tsv"
printf 'task_a\t990000\taaa\t0\t11\ntask_b\t2000100\tbbb\t0\t22\ntask_b_pure\t3300000\tccc\t0\t33\n' > "$w/head.tsv"
python3 -I "$root/tools/la-half.py" table "$w/head.tsv" "$w/base.tsv" "$w/part/names.tsv" > "$w/table.md"; rc=$?
[ $rc -eq 0 ] && grep -q '^| Task::A | 1000000 | 990000 | 0.9900 | same |$' "$w/table.md" \
  && grep -q '^| Task::B | 2000000 | 2000100 | 1.0001 | same |$' "$w/table.md" \
  && grep -q '^3 programs counted on both sides; geometric mean of head / base 1.0288; 1 fewer instructions beyond 0.1%, 1 more.$' "$w/table.md" \
  && grep -q '^Fewer: Task::A 0.9900$' "$w/table.md" && grep -q '^More: Task::B-Pure 1.1000$' "$w/table.md" \
  && grep -q '^Every output is the same on both sides, and every program ran.$' "$w/table.md"
say half-table $? "rc=$rc: $(tail -n 6 "$w/table.md" | tr '\n' ';')"
printf 'task_a\t990000\tXXX\t0\t11\ntask_b\tcompile-failed\t-\t-\t-\ntask_b_pure\t3300000\tccc\t0\t33\ntask_d\t5\tddd\t0\t1\n' > "$w/head2.tsv"
python3 -I "$root/tools/la-half.py" table "$w/head2.tsv" "$w/base.tsv" "$w/part/names.tsv" > "$w/table2.md"; rc=$?
[ $rc -eq 1 ] && grep -q '^| Task::A | 1000000 | 990000 | 0.9900 | differs |$' "$w/table2.md" \
  && grep -q '^| Task::B | 2000000 | - | - | not run (head compile-failed / base ok) |$' "$w/table2.md" \
  && grep -q '^| task_d | - | 5 | - | only head |$' "$w/table2.md" \
  && grep -q '^Red (the output differs, or a program did not compile or run): Task::A: differs; Task::B: not run (head compile-failed / base ok); task_d: only head$' "$w/table2.md"
say half-table-red $? "rc=$rc: $(tail -n 3 "$w/table2.md" | tr '\n' ';')"

# 3. screen end to end with two stub compilers, when perf can count.
if command -v perf > /dev/null 2>&1 && perf stat -e instructions:u true > /dev/null 2>&1; then
  for side in base head; do
    mkdir -p "$w/$side/bin"
    extra=0; [ $side = head ] && extra=200000
    cat > "$w/$side/bin/avant-c-llvm" <<STUB
#!/bin/sh
# compile SRC OUT ARGS...: a program that counts to a number and prints one line.
[ "\$1" = compile ] || exit 2
printf '#!/bin/sh\ni=0; while [ \$i -lt %s ]; do i=\$((i + 1)); done\necho checksum-1\n' $((100000 + extra)) > "\$3"
chmod +x "\$3"
STUB
    chmod +x "$w/$side/bin/avant-c-llvm"
  done
  LA_RUN_DIR="$w" sh "$root/tools/la-half.sh" screen "$w/screen" "$la" "$w/base" "$w/head" task_a task_b > "$w/screen.out" 2>&1; rc=$?
  [ $rc -eq 0 ] && [ -s "$w/screen/table.md" ] && grep -q '^| Task::A | [0-9]* | [0-9]* | [2-9]\.[0-9]* | same |$' "$w/screen/table.md" \
    && grep -q '^2 programs counted on both sides;' "$w/screen/table.md" && grep -q 'Every output is the same' "$w/screen/table.md"
  say half-screen $? "rc=$rc: $(tail -n 8 "$w/screen.out" | tr '\n' ';'); $(cat "$w/screen/base.log" 2>/dev/null | tail -n 2 | tr '\n' ';')"
else
  echo "skip: half-screen: perf cannot count instructions here"
fi
echo done
