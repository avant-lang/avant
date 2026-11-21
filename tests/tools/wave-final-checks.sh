#!/bin/sh
# tools/wave-final.sh's provenance, litter and reference checks.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/wave-final-checks.sh WORK
#
# WORK is a new scratch directory. The real wave-final.sh runs in fake
# checkouts whose gate and goldens are stubs that pass at once (the later
# steps are red there; each check reads one SUMMARY.txt line). WAVE_TMP
# points the litter check at a directory of WORK instead of /tmp:
#   head-not-git: outside a git checkout the head step is red and says so;
#   head-clean: in a git checkout with a clean tree it names the commit
#     and its tree;
#   tmp-gone-green: an entry removed during the run by its owner is listed,
#     not red; tmp-new-red: an entry the run left is red and named;
#   ref-other-machine: a reference whose machine.txt names another CPU is
#     refused; ref-no-machine: one without machine.txt is refused;
#   ref-cpu-parens: a CPU whose name has its own parentheses
#     (Intel(R) Xeon(R) ...) matches its reference (a fake cpuinfo);
#   ref-default: with WAVE_LR_REFERENCE unset, the directory named in
#     WAVE_LR_DEFAULT is used (and checked); ref-none: no reference at all
#     is red and says how to give one;
#   runner-sides: with WAVE_RUNNER (a stub that logs its arguments and runs
#     the command), only the LangArena timing goes through it `--timed`,
#     every other step `--shared`, each named `final: STEP`;
#   runner-lock-line: SUMMARY.txt says so; runner-none: without
#     WAVE_RUNNER its `lock:` line says the caller holds the lock;
#   runner-wall: through the runner, the gate's wall is its own, the lock's
#     wait beside it;
#   base-merge-base: with WAVE_BASE naming a branch that moved on after
#     the head left it, the base cache and the neutrality are asked for
#     the merge base, not the branch's tip;
#   tmp-locale: under a locale whose order is not the bytes' (en_US.UTF-8,
#     when the machine has it), with one entry gone and one new during the
#     run, only those two are read so, not the entries there throughout;
#   private-tmp: without WAVE_TMP the run is in a private /tmp: an entry
#     its step leaves in /tmp is red, named, and not in the real /tmp
#     afterwards (`skip:` where the machine refuses the namespace);
#     private-tmp-paths: a checkout and OUT under /tmp (as inside the
#     gate's private /tmp) are bound into the run's own;
#   cases-green, cases-red, cases-stdout, cases-stop: the test files go
#     through C' before the gate: green with their count; one the stub C'
#     refuses is red, named with its message, before the gate's line and
#     on stdout at once (the second line, after the lock's); with
#     WAVE_CASES_STOP=1 the run ends there, no gate;
#   lock-old-form, lock-old-form-runner, lock-new-form-quiet: inside an
#     exclusive `ledger run` (LEDGER_LOCK_HELD) the lock line and the first
#     line on stdout say it is the old form, with or without WAVE_RUNNER;
#     outside, nothing is said, nor under another lock file (LEDGER_LOCK
#     naming another path than the one held: lock-other-file);
#   group-one-call, group-lines: with WAVE_GROUP=1 the shared steps are one
#     runner call (`final: shared steps`), the harness build and the timing
#     as before; every step's line is in SUMMARY.txt, and a `shared steps:`
#     line with the call's wall and wait;
#   layout-marked, layout-two-bases: with a LangArena step that passes
#     (stubs) and a base LR table of the base cache entry's tree, a task
#     whose time moved 10% at an equal count reads "layout?" in its
#     `layout:` line and in floor.md's "Layout?" section; with a base table
#     of another tree the step says it is skipped and why.
#   only-stats, only-gate-no-build, only-unknown: WAVE_ONLY runs only the
#     named steps (the others read `skipped`), builds C' first only when the
#     gate is not among them, ends `WAVE PARTIAL GREEN` and never `WAVE FINAL`,
#     and refuses a name that is no step;
#   beside-merged, beside-differs: with WAVE_BESIDE=1 the goldens and the
#     statistics run in a copy beside the gate and their lines (and a
#     build of C') join the summary, the copy removed; when the copy's
#     binaries are not the gate's, a `beside:` line is red;
#   neutral-count: the neutral line counts the programs that moved outside
#     the change (by kind) and names the first five.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/wave-final-checks.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }

# A fake checkout at W/NAME/side/avant, LangArena beside it.
fake() { # NAME
  f=$w/$1/side/avant
  mkdir -p "$f/tools" "$f/langarena/harness" "$w/$1/side/LangArena"
  : > "$f/langarena/harness/measure.cr"
  cp "$root/tools/wave-final.sh" "$f/tools/wave-final.sh"
  printf '#!/bin/sh\necho "GATE PASS (stub)"\n' > "$f/tools/gate.sh"
  cat > "$f/tools/run-goldens.sh" <<'EOF'
#!/bin/sh
# The stub goldens step: during the first mode it removes WF_GONE and
# leaves WF_LITTER when they are set.
if [ "${GOLDEN_MODE:-}" = --debug ]; then
  [ -n "${WF_GONE:-}" ] && rmdir "$WF_GONE"
  [ -n "${WF_LITTER:-}" ] && mkdir "$WF_LITTER"
fi
echo "1 same, 0 different, 0 skipped"
EOF
  echo "$f"
}
# Run wave-final.sh in checkout F into OUT, the environment emptied but for
# the given variables.
final() { # F OUT VAR=VALUE...
  f=$1 o=$2; shift 2
  (cd "$f" && env -i PATH="$PATH" HOME="$w/home" "$@" timeout -k 10 300 sh tools/wave-final.sh "$o" < /dev/null > "$o.log" 2>&1)
}
has() { grep -q "$1" "$2" 2>/dev/null; }
line() { grep "^$1" "$2" 2>/dev/null | head -n 1; }

# This machine as machine.txt says it, and another machine.
cpu=$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1)
n=$(getconf _NPROCESSORS_ONLN)
mkdir -p "$w/ref-here" "$w/ref-other" "$w/ref-none" "$w/home"
printf 'date: 2026-01-01\ncpu: %s (a container)\nthreads: %s\n' "$cpu" "$n" > "$w/ref-here/machine.txt"
printf 'date: 2026-01-01\ncpu: Some Other CPU 9000\nthreads: %s\n' "$n" > "$w/ref-other/machine.txt"
for d in ref-here ref-other ref-none; do printf '# label: X\ntask\tstatus\twall_s\trss_kb\tinprog_s\texit\n' > "$w/$d/x.tsv"; done

# 1. Outside a git checkout (WORK may itself sit inside one: the fake
# checkout is its own directory, never the top of a work tree).
f=$(fake nogit)
mkdir -p "$w/tmp1"
final "$f" "$w/nogit.out" WAVE_TMP="$w/tmp1"
s=$w/nogit.out/SUMMARY.txt
has '^head: red (not a git checkout' "$s"; say head-not-git $? "$(line head: "$s")"

# 2. A clean git checkout: the commit and its tree.
f=$(fake git)
(cd "$f" && git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -q -m t) > /dev/null 2>&1
mkdir -p "$w/tmp2/gone"
final "$f" "$w/git.out" WAVE_TMP="$w/tmp2" WF_GONE="$w/tmp2/gone"
s=$w/git.out/SUMMARY.txt
rev=$(git -C "$f" rev-parse HEAD 2>/dev/null); tree=$(git -C "$f" rev-parse 'HEAD^{tree}' 2>/dev/null)
has "^head: $rev clean (tree $tree)\$" "$s"; say head-clean $? "$(line head: "$s")"
# the entry removed during the run by its owner: listed, not red
has '^tmp: nothing new (1 gone during the run' "$s" && has "$w/tmp2/gone" "$w/git.out/tmp-gone.txt"
say tmp-gone-green $? "$(line tmp: "$s")"

# 3. An entry the run leaves: red, named.
f=$(fake litter)
mkdir -p "$w/tmp3"
final "$f" "$w/litter.out" WAVE_TMP="$w/tmp3" WF_LITTER="$w/tmp3/left"
s=$w/litter.out/SUMMARY.txt
has "^tmp: red (1 new in $w/tmp3, tmp-new.txt: $w/tmp3/left" "$s"; say tmp-new-red $? "$(line tmp: "$s")"

# 4. The LR reference.
f=$(fake la)
side=$w/la/side
final "$f" "$w/ref-other.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_REFERENCE="$w/ref-other"
s=$w/ref-other.out/SUMMARY.txt
has "^langarena: red (the LR reference $w/ref-other was measured on another machine (Some Other CPU 9000" "$s"
say ref-other-machine $? "$(line langarena: "$s")"
final "$f" "$w/ref-nomachine.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_REFERENCE="$w/ref-none"
s=$w/ref-nomachine.out/SUMMARY.txt
has "^langarena: red (the LR reference $w/ref-none has no machine.txt" "$s"; say ref-no-machine $? "$(line langarena: "$s")"
printf 'model name\t: Intel(R) Xeon(R) Fake 1000P-C\n' > "$w/cpuinfo-parens"
mkdir -p "$w/ref-parens"
printf 'date: 2026-01-01\ncpu: Intel(R) Xeon(R) Fake 1000P-C (a container)\nthreads: %s\n' "$n" > "$w/ref-parens/machine.txt"
printf '# label: X\ntask\tstatus\twall_s\trss_kb\tinprog_s\texit\n' > "$w/ref-parens/x.tsv"
final "$f" "$w/ref-parens.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_REFERENCE="$w/ref-parens" WAVE_CPUINFO="$w/cpuinfo-parens"
s=$w/ref-parens.out/SUMMARY.txt
! has 'LR reference' "$s" && has '^langarena:' "$s"
say ref-cpu-parens $? "$(line langarena: "$s")"
echo "$w/ref-here" > "$w/default"
final "$f" "$w/ref-default.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_DEFAULT="$w/default"
s=$w/ref-default.out/SUMMARY.txt
# the reference passed its check: the step ran (and is red only for the
# fake checkout's missing harness), and the reference is recorded
! has 'LR reference' "$s" && has "^$w/ref-here (from $w/default)\$" "$w/ref-default.out/lr-reference.txt"
say ref-default $? "$(line langarena: "$s"); $(cat "$w/ref-default.out/lr-reference.txt" 2>/dev/null)"
final "$f" "$w/ref-unset.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_DEFAULT="$w/no-such-file"
s=$w/ref-unset.out/SUMMARY.txt
has "^langarena: red (no LR reference: set WAVE_LR_REFERENCE, or write this machine's reference directory into $w/no-such-file)" "$s"
say ref-none $? "$(line langarena: "$s")"

# 5. The machine lock through WAVE_RUNNER: a stub runner logs its side and
# step, then runs the command after `--`.
cat > "$w/runner" <<'EOF'
#!/bin/sh
echo "$1 $3" >> "$WF_RUNLOG"
while [ $# -gt 0 ] && [ "$1" != -- ]; do shift; done
shift
exec "$@"
EOF
final "$f" "$w/runner.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$side" WAVE_LR_DEFAULT="$w/default" WAVE_RUNNER="sh $w/runner" WF_RUNLOG="$w/runner.log"
r=$w/runner.log
sides() {
  [ "$(grep -c '^--timed ' "$r" 2>/dev/null)" = 1 ] && grep -q '^--timed final: langarena$' "$r" || return 1
  [ "$(grep -c -v '^--\(timed\|shared\) final: ' "$r")" = 0 ] || return 1
  for t in gate 'goldens debug' 'goldens bare' 'goldens release' 'goldens stress' gcstats smoke 'langarena harness'; do
    grep -q "^--shared final: $t\$" "$r" || return 1
  done
}
sides; say runner-sides $? "$(tr '\n' ';' < "$r" 2>/dev/null)"
s=$w/runner.out/SUMMARY.txt
has '^lock: through WAVE_RUNNER (sh .*runner): shared for every step but the LangArena timing, the whole machine for that$' "$s"
say runner-lock-line $? "$(line lock: "$s")"
s=$w/ref-default.out/SUMMARY.txt
has "^lock: the caller's (WAVE_RUNNER unset" "$s"; say runner-none $? "$(line lock: "$s")"
grep -q '^wall [0-9.]* s (waited [0-9.]* s for the lock)$' "$w/runner.out/gate-wall.txt"
say runner-wall $? "$(cat "$w/runner.out/gate-wall.txt" 2>/dev/null)"

# 6. WAVE_BASE is read as the merge base: the git checkout's first commit
# is the base, a branch from it moves on past it, the head is a child of it.
f=$w/git/side/avant
c1=$(git -C "$f" rev-parse HEAD)
c3=$(cd "$f" && echo moved | git -c user.name=t -c user.email=t@t commit-tree "$c1^{tree}" -p "$c1" 2>/dev/null)
git -C "$f" update-ref refs/heads/moved "$c3"
echo x > "$f/x"; (cd "$f" && git add x && git -c user.name=t -c user.email=t@t commit -q -m head) > /dev/null 2>&1
final "$f" "$w/base.out" WAVE_TMP="$w/tmp1" WAVE_BASE=moved
s=$w/base.out/SUMMARY.txt
has "^neutral: red (tools/base-cache.sh $c1 failed" "$s"; say base-merge-base $? "$(line neutral: "$s")"

# 7. The snapshots' order: entries whose order differs by locale.
if locale -a 2>/dev/null | grep -qx 'en_US.utf8'; then
  mkdir -p "$w/tmp7/.X11-unix" "$w/tmp7/WIozLM" "$w/tmp7/avant-x" "$w/tmp7/.XIM-unix"
  final "$f" "$w/locale.out" WAVE_TMP="$w/tmp7" LC_ALL=en_US.UTF-8 WF_GONE="$w/tmp7/avant-x" WF_LITTER="$w/tmp7/w9a"
  s=$w/locale.out/SUMMARY.txt
  has "^tmp: red (1 new in $w/tmp7, tmp-new.txt: $w/tmp7/w9a ;" "$s" && [ "$(cat "$w/locale.out/tmp-gone.txt")" = "$w/tmp7/avant-x" ]
  say tmp-locale $? "$(line tmp: "$s")"
else echo "skip: tmp-locale: no en_US.utf8 locale here"; fi

# 8. The private /tmp: the stub goldens leave an entry in /tmp.
lit=/tmp/wave-final-checks-litter.$$
if unshare --user --map-current-user --mount --keep-caps sh -c 'mount -t tmpfs tmpfs /tmp' < /dev/null > /dev/null 2>&1; then
  final "$f" "$w/private.out" WF_LITTER="$lit"
  s=$w/private.out/SUMMARY.txt
  has "^tmp: red (1 new in /tmp, tmp-new.txt: $lit ; a tmpfs in a mount namespace of its own)" "$s" && [ ! -e "$lit" ]
  say private-tmp $? "$(line tmp: "$s"); real: $(ls -d "$lit" 2>/dev/null)"
  rmdir "$lit" 2>/dev/null
  # The checkout and OUT under /tmp (as in the gate's own private /tmp):
  # bound into the run's private one, OUT made before it.
  d=$(mktemp -d /tmp/wave-final-checks.XXXXXX)
  cp -r "$w/nogit" "$d/"
  final "$d/nogit/side/avant" "$d/out"
  has '^tmp: unchanged (a tmpfs in a mount namespace of its own)' "$d/out/SUMMARY.txt"
  say private-tmp-paths $? "$(line tmp: "$d/out/SUMMARY.txt"); $(tail -n 2 "$d/out.log" 2>/dev/null | tr '\n' ';')"
  rm -rf "$d"
else echo "skip: private-tmp: this machine refuses the mount namespace"; echo "skip: private-tmp-paths: this machine refuses the mount namespace"; fi

# 9. The test files compile before the gate (tools/cases-check.sh with a
# stub C' that refuses a file holding REFUSEME).
cases_fake() { # NAME: a fake checkout with two test files, a helper and the stub compiler
  f=$(fake "$1")
  cp "$root/tools/cases-check.sh" "$f/tools/cases-check.sh"
  mkdir -p "$f/tests/cases" "$f/bin"
  echo ok > "$f/tests/cases/test_good.av"; echo ok > "$f/tests/cases/test_other.av"; echo 'REFUSEME, a helper' > "$f/tests/cases/harness.av"
  cat > "$f/bin/avant-c-llvm" <<'EOF'
#!/bin/sh
[ "$1" = __emit-ll ] || exit 2
if grep -q REFUSEME "$2"; then echo "error[E4002]: for i in ...: i already names a variable here ($2)"; exit 1; fi
: > "$3"
EOF
  chmod +x "$f/bin/avant-c-llvm"
  echo "$f"
}
f=$(cases_fake cases)
final "$f" "$w/cases.out" WAVE_TMP="$w/tmp1"
s=$w/cases.out/SUMMARY.txt
has '^cases: 2 files compile ([0-9.]* s)$' "$s"; say cases-green $? "$(line cases: "$s")"
echo REFUSEME > "$f/tests/cases/test_other.av"
final "$f" "$w/cases-red.out" WAVE_TMP="$w/tmp1"
s=$w/cases-red.out/SUMMARY.txt
has '^cases: red (1 of 2 refused, [0-9.]* s: tests/cases/test_other.av: error\[E4002\]: for i in ...: i already names a variable here (tests/cases/test_other.av))$' "$s" \
  && [ "$(grep -n '^cases: ' "$s" | cut -d: -f1)" -lt "$(grep -n '^gate: ' "$s" | cut -d: -f1)" ] && has 'WAVE FINAL RED:.* cases' "$s"
say cases-red $? "$(line cases: "$s"); $(line WAVE "$s")"
# printed at once: the first two lines on stdout are the lock and the cases
sed -n '2p' "$w/cases-red.out.log" | grep -q '^wave-final: cases: red (1 of 2 refused'
say cases-stdout $? "$(head -n 3 "$w/cases-red.out.log" | tr '\n' ';')"
final "$f" "$w/cases-stop.out" WAVE_TMP="$w/tmp1" WAVE_CASES_STOP=1
s=$w/cases-stop.out/SUMMARY.txt
has '^WAVE FINAL RED:.* cases (stopped after the cases: WAVE_CASES_STOP=1)$' "$s" && ! has '^gate: ' "$s"
say cases-stop $? "$(tr '\n' ';' < "$s" 2>/dev/null)"

# 10. A run inside an exclusive `ledger run` (the old form) says so in its
# lock line and its first line on stdout; a run outside does not.
f=$w/nogit/side/avant
final "$f" "$w/oldform.out" WAVE_TMP="$w/tmp1" LEDGER_LOCK_HELD="exclusive:$w/machine.lock"
s=$w/oldform.out/SUMMARY.txt
has "^lock: the caller's (WAVE_RUNNER unset: every step runs under the lock the run was started with); but the whole run is inside an exclusive \`ledger run\` (LEDGER_LOCK_HELD), which holds the whole machine for every step: the old form" "$s" \
  && head -n 1 "$w/oldform.out.log" | grep -q '^wave-final: lock: .*the old form; start wave-final.sh outside ledger run, with WAVE_RUNNER'
say lock-old-form $? "$(line lock: "$s"); stdout: $(head -n 1 "$w/oldform.out.log")"
final "$f" "$w/oldform-runner.out" WAVE_TMP="$w/tmp1" LEDGER_LOCK_HELD="exclusive:$w/machine.lock" WAVE_RUNNER="sh $w/runner" WF_RUNLOG="$w/runner-old.log"
has '^lock: through WAVE_RUNNER (sh .*runner): shared for every step but the LangArena timing, the whole machine for that; but the whole run is inside an exclusive' "$w/oldform-runner.out/SUMMARY.txt"
say lock-old-form-runner $? "$(line lock: "$w/oldform-runner.out/SUMMARY.txt")"
! has 'the old form' "$w/runner.out/SUMMARY.txt" && ! grep -q 'old form' "$w/runner.out.log"
say lock-new-form-quiet $? "$(line lock: "$w/runner.out/SUMMARY.txt")"
final "$f" "$w/otherlock.out" WAVE_TMP="$w/tmp1" LEDGER_LOCK_HELD="exclusive:$w/machine.lock" LEDGER_LOCK="$w/another.lock"
! has 'the old form' "$w/otherlock.out/SUMMARY.txt"
say lock-other-file $? "$(line lock: "$w/otherlock.out/SUMMARY.txt")"

# 11. WAVE_GROUP=1: the shared steps in one runner call, the LangArena
# timing still alone and --timed, every step's line as before.
f=$(cases_fake group)
final "$f" "$w/group.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/group/side" WAVE_LR_DEFAULT="$w/default" WAVE_RUNNER="sh $w/runner" WF_RUNLOG="$w/group-runner.log" WAVE_GROUP=1
r=$w/group-runner.log; s=$w/group.out/SUMMARY.txt
[ "$(tr '\n' ';' < "$r")" = "--shared final: shared steps;--shared final: langarena harness;--timed final: langarena;" ]
say group-one-call $? "$(tr '\n' ';' < "$r" 2>/dev/null)"
ok=0
for l in 'head: ' 'lock: through WAVE_RUNNER .*in one runner call (WAVE_GROUP=1)$' 'cases: 2 files compile' 'gate: pass' 'nm: ' 'goldens: ' 'gcstats: ' 'smoke: ' 'neutral: skipped' 'shared steps: one runner call, wall [0-9.]* s (waited [0-9.]* s for the lock)$' 'langarena: ' 'tmp: '; do
  has "^$l" "$s" || { ok=1; echo "missing: $l" >> "$w/group.missing"; }
done
say group-lines $ok "$(cat "$w/group.missing" 2>/dev/null | tr '\n' ';') / $(tr '\n' ';' < "$s" 2>/dev/null)"

# 12. Layout?: a LangArena step that passes (stub harness, LR table and
# counts), a base table of the base cache entry's tree beside it: the task
# whose time moved at an equal count is timed across its layouts (a stub
# layout-check.sh: its four runs span 0.98-1.12 s, so the base's 1.000 s
# lies within them) and named layout? in SUMMARY.txt and floor.md; with
# runs of 1.05-1.12 s it is named beyond.
f=$(fake lay)
cp "$root/tools/layout-flag.py" "$root/tools/floor.py" "$f/tools/"
cat > "$f/tools/layout-check.sh" <<'STUB'
#!/bin/sh
[ "$1" = build ] && { mkdir -p "$3"; echo "$4" > "$3/built"; exit 0; }
r=$2/rounds/raw; i=1
for w in $(cat "$(dirname "$0")/../stub-walls"); do
  mkdir -p "$r/round-$i"
  printf 'id\thead--a_same\nround\t%d\nwall_ns\t%d\nexit\t0\noutput\tok\ntask\tA::Same\nside\thead\nlayout\t%d\nundisturbed\tyes\n' $i $w $((i * 64)) > "$r/round-$i/head--a_same.run"
  i=$((i + 1))
done
STUB
echo "980000000 1050000000 1120000000 1100000000" > "$f/stub-walls"
lc=$w/lay/cache; le=$lc/feedfacefeedfacefeedfacefeedfacefeedface; mkdir -p "$le/la-x" "$le/gcstats"
printf 'a_same\t1000000\tx\t0\tc\nb_other\t1000000\tx\t0\tc\n' > "$le/la-x/ins.tsv"
cat > "$f/tools/base-cache.sh" <<EOF
#!/bin/sh
[ "\$1" = --la ] && { echo "$le/la-x"; exit 0; }
echo "$le"
EOF
printf '#!/bin/sh
mkdir -p "$3"; echo "sources: files 1 ll-same 1" > "$3/summary.txt"; : > "$3/result.txt"
' > "$f/tools/neutral.sh"
printf '#!/bin/sh
printf "a_same\\t1000000\\tx\\t0\\tc\\nb_other\\t1300000\\tx\\t0\\tc\\n" > "$3"
' > "$f/tools/la-count.sh"
printf '#!/bin/sh
mkdir -p "$(dirname "$0")/../langarena/tmp"; h=$(cd "$(dirname "$0")/.." && pwd)/langarena/tmp/stub-measure
printf "#!/bin/sh\\nexit 0\\n" > "$h"; chmod +x "$h"; echo "$h"
' > "$f/tools/la-harness.sh"
cat > "$f/tools/lr-table.sh" <<'EOF'
#!/bin/sh
if [ "$1" = avant ]; then printf 'task	status	wall_s
A::Same	ok	1.100
B::Other	ok	1.300
' > "$4"; exit 0; fi
[ -n "${3:-}" ] && printf 'name\twall_s\trss_mb\tchecksum_ok\nA::Same\t1.1\t1\ttrue\n' > "$3/runtime.tsv"
printf '## Combined (worse of the two multipliers)

| # | Task | Avant s | Best s | Best (time) | Time x | Avant MB | Best MB | Best (mem) | Mem x | vs Crystal t/m | vs Go t/m |
| ---: | --- |
| 1 | A::Same | 1.1 | 1.0 | C | 1.10 | 1 | 1 | C | 1.00 | 1.10 / 1.00 | 1.10 / 1.00 |
'
EOF
mkdir -p "$w/layb/final/la"; printf 'task\tstatus\twall_s\nA::Same\tok\t1.000\nB::Other\tok\t1.000\n' > "$w/layb/final/la/lr-avant.tsv"
cp "$w/layb/final/la/lr-avant.tsv" "$w/layb/final/la/x"; printf 'deadbeef\nfeedfacefeedfacefeedfacefeedfacefeedface\n' > "$w/layb/final/head.txt"
(cd "$w" && sh "$f/tools/lr-table.sh" table > "$w/layb/final/la/table.md")
(cd "$f" && git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -q -m t && git branch -q base) > /dev/null 2>&1
final "$f" "$w/lay.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/lay/side" WAVE_LR_DEFAULT="$w/default" WAVE_BASE=base WAVE_FLOOR_BASE="$w/layb/final/la/table.md"
s=$w/lay.out/SUMMARY.txt
has "^layout: 1 tasks moved at an equal instruction count; 1 within the spread across the head's layouts (layout?): A::Same +10.0% (base 1.000 s, head 0.980-1.120 s across 4 layouts); 0 beyond it (la/layout.tsv)$" "$s" \
  && grep -q '^## Layout?$' "$w/lay.out/floor.md" && grep -q '^| A::Same | 1.000 | 1.100 | +10.0% | +0.000% | 0.980–1.120 | layout? | yes |$' "$w/lay.out/floor.md" \
  && [ "$(cat "$w/lay.out/la/layout-check/built" 2>/dev/null)" = a_same ]
say layout-marked $? "$(line layout: "$s"); $(line langarena: "$s"); $(grep -A 6 '^## Layout' "$w/lay.out/floor.md" 2>/dev/null | tr '\n' ';')"
echo "1050000000 1120000000 1100000000 1080000000" > "$f/stub-walls"
(cd "$f" && git add -A && git -c user.name=t -c user.email=t@t commit -q -m walls) > /dev/null 2>&1
final "$f" "$w/lay3.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/lay/side" WAVE_LR_DEFAULT="$w/default" WAVE_BASE=base WAVE_FLOOR_BASE="$w/layb/final/la/table.md"
has "^layout: 1 tasks moved at an equal instruction count; 0 within the spread across the head's layouts (layout?); 1 beyond it: A::Same +10.0% (base 1.000 s, head 1.050-1.120 s across 4 layouts) (la/layout.tsv)$" "$w/lay3.out/SUMMARY.txt" \
  && grep -q '^| A::Same | 1.000 | 1.100 | +10.0% | +0.000% | 1.050–1.120 | beyond | yes |$' "$w/lay3.out/floor.md"
say layout-beyond $? "$(line layout: "$w/lay3.out/SUMMARY.txt"); $(grep -A 6 '^## Layout' "$w/lay3.out/floor.md" 2>/dev/null | tr '\n' ';')"
printf 'deadbeef\nanothertree\n' > "$w/layb/final/head.txt"
final "$f" "$w/lay2.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/lay/side" WAVE_LR_DEFAULT="$w/default" WAVE_BASE=base WAVE_FLOOR_BASE="$w/layb/final/la/table.md"
has '^layout: skipped (the floor.s base table is of tree anothertree, the base cache.s entry of feedface' "$w/lay2.out/SUMMARY.txt"
say layout-two-bases $? "$(line layout: "$w/lay2.out/SUMMARY.txt")"

# 13. The twins at once: a stub harness whose tasks pass lists two twins,
# each twin's pass writes its row, and a pass that names no task writes the
# reports from the merged rows; a stub LR table writes a twin's row where
# LR_TASKS=0 asks for it alone. With two NUMA nodes or more and numactl,
# the tasks pass leaves the twins out (AVANT_TWINS=0, LR_TWINS=0),
# tools/la-twins.py times them on the nodes, their rows are merged in the
# library's order and the reports written again; with
# WAVE_LA_TWINS_AT_ONCE=0 they stay in the passes, as before.
f=$(fake tw)
cp "$root/tools/la-twins.py" "$root/tools/floor.py" "$f/tools/"
cat > "$f/tools/la-harness.sh" <<'STUB'
#!/bin/sh
d=$(cd "$(dirname "$0")/.." && pwd)/langarena/tmp; mkdir -p "$d"
cat > "$d/stub-measure" <<'EOF'
#!/bin/sh
d=$AVANT_RESULTS_DIR; mkdir -p "$d/raw"
echo "prod twins=${AVANT_TWINS:-} list=${AVANT_TASK_LIST:-}" >> "$STUB_LOG"
case "${AVANT_TASK_LIST:-}" in
  "") printf 'task\tlibrary\ttwin\ttwin_slug\tkind\tcompared\nA::A\tlib\tA::A-Pure\ta_a-pure\tcommon\tC\nB::B\tlib\tB::B-Pure\tb_b-pure\tcommon\tC\n' > "$d/library.tsv"
      echo '[]' > "$d/runtime.json" ;;
  -) echo "pure rows: $(grep -c '"name"' "$d/runtime-pure.json")" > "$d/SUMMARY.md" ;;
  *) echo "[1/1] $AVANT_TASK_LIST run (timeout 900s)"; sleep 0.5
     printf '[{"name": "%s", "checksum_ok": "true"}]\n' "$AVANT_TASK_LIST" > "$d/runtime-pure.json" ;;
esac
EOF
chmod +x "$d/stub-measure"; echo "$d/stub-measure"
STUB
cat > "$f/tools/lr-table.sh" <<'STUB'
#!/bin/sh
if [ "$1" = avant ]; then
  echo "lr tasks=${LR_TASKS:-} twins=${LR_TWINS:-} list=${LR_TWIN_LIST:-}" >> "$STUB_LOG"
  if [ "${LR_TASKS:-}" = 0 ]; then
    printf 'task\tstatus\twall_s\n' > "$4"; printf '# label: Avant pure\ntask\tstatus\twall_s\n%s\tok\t1.000\n' "$LR_TWIN_LIST" > "$(dirname "$4")/lr-avant-pure.tsv"
  else printf 'task\tstatus\twall_s\nA::A\tok\t1.000\n' > "$4"; fi
  exit 0
fi
[ -n "${3:-}" ] && printf 'name\twall_s\trss_mb\tchecksum_ok\nA::A\t1.0\t1\ttrue\n' > "$3/runtime.tsv"
printf '## Combined (worse of the two multipliers)\n\n| # | Task | Avant s | Best s | Best (time) | Time x | Avant MB | Best MB | Best (mem) | Mem x | vs Crystal t/m | vs Go t/m |\n| ---: | --- |\n| 1 | A::A | 1.0 | 1.0 | C | 1.00 | 1 | 1 | C | 1.00 | 1.00 / 1.00 | 1.00 / 1.00 |\n'
STUB
if command -v numactl > /dev/null 2>&1 && [ "$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)" -ge 2 ]; then
  : > "$w/tw.log"
  final "$f" "$w/tw.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/tw/side" WAVE_LR_DEFAULT="$w/default" STUB_LOG="$w/tw.log"
  s=$w/tw.out/SUMMARY.txt; la=$w/tw.out/la
  has '^langarena: prod rc=0 wall=[0-9.]* s; the twins on the NUMA nodes at once: 2 twins, 4 jobs on nodes [0-9,]* in [0-9.]* s' "$s" \
    && [ "$(awk -F'\t' '!/^#/ && $1 != "task" { print $1 }' "$la/lr-avant-pure.tsv" | tr '\n' ' ')" = "A::A-Pure B::B-Pure " ] \
    && [ "$(grep -c '"name"' "$la/runtime-pure.json")" = 2 ] && grep -qx 'pure rows: 2' "$la/SUMMARY.md" \
    && grep -qx 'prod twins=0 list=' "$w/tw.log" && grep -qx 'lr tasks= twins=0 list=' "$w/tw.log" && grep -qx 'prod twins= list=-' "$w/tw.log" \
    && [ "$(grep -c '^[PR] ' "$la/twins.tsv")" = 4 ]
  say twins-at-once $? "$(line langarena: "$s"); $(tr '\n' ';' < "$w/tw.log" 2>/dev/null); $(tr '\n' ';' < "$la/twins.tsv" 2>/dev/null)"
else echo "skip: twins-at-once: this machine has one NUMA node or no numactl"; fi
: > "$w/tw2.log"
final "$f" "$w/tw2.out" WAVE_TMP="$w/tmp1" WAVE_LA=1 WAVE_LA_SIDE="$w/tw/side" WAVE_LR_DEFAULT="$w/default" STUB_LOG="$w/tw2.log" WAVE_LA_TWINS_AT_ONCE=0
s=$w/tw2.out/SUMMARY.txt
has '^langarena: prod rc=0 wall=[0-9.]* s; LR table' "$s" && [ ! -e "$w/tw2.out/la/twins.tsv" ] \
  && [ "$(tr '\n' ';' < "$w/tw2.log")" = "prod twins= list=;lr tasks= twins= list=;" ]
say twins-in-the-passes $? "$(line langarena: "$s"); $(tr '\n' ';' < "$w/tw2.log" 2>/dev/null)"
# 14. WAVE_ONLY: only the named steps run, after a build of C' when the gate
# is not one of them; the last line says the run is partial, never a final.
f=$(cases_fake only)
cat > "$f/tools/letters.sh" <<'STUB'
#!/bin/sh
echo "letters $*" >> "$(dirname "$0")/../letters.log"; mkdir -p "$(dirname "$0")/../runtime/build"; : > "$(dirname "$0")/../runtime/build/avant_rt.o"
STUB
cat > "$f/tools/gcstats.sh" <<'STUB'
#!/bin/sh
mkdir -p "$2"; echo "stub" > "$2/a.out"; echo "stub" > "$2/a.err"
STUB
printf '/bin/\n/runtime/build/\n/letters.log\n' > "$f/.gitignore"
(cd "$f" && git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -q -m t) > /dev/null 2>&1
final "$f" "$w/only.out" WAVE_TMP="$w/tmp1" WAVE_ONLY=stats
s=$w/only.out/SUMMARY.txt
has '^cases: skipped (WAVE_ONLY=stats)$' "$s" && has '^gate: skipped (WAVE_ONLY=stats)$' "$s" && has '^goldens: skipped (WAVE_ONLY=stats)$' "$s" \
  && has '^smoke: skipped (WAVE_ONLY=stats)$' "$s" && has '^neutral: skipped (WAVE_ONLY=stats)$' "$s" && has '^gcstats: 2 files (no base given)$' "$s" \
  && has "^build: C' built by the letters" "$s" && [ "$(cat "$f/letters.log")" = "letters runtime b c-llvm" ] && ! has 'GATE\|^gate: pass' "$s" \
  && has '^WAVE PARTIAL GREEN (only: stats; not a final)$' "$s" && ! has 'WAVE FINAL' "$s"
say only-stats $? "$(tr '\n' ';' < "$s" 2>/dev/null)"
rm -f "$f/letters.log"
final "$f" "$w/only2.out" WAVE_TMP="$w/tmp1" WAVE_ONLY=cases,gate
s=$w/only2.out/SUMMARY.txt
has '^cases: 2 files compile' "$s" && has '^gate: pass' "$s" && has '^gcstats: skipped' "$s" && [ ! -e "$f/letters.log" ] && has '^WAVE PARTIAL GREEN (only: cases,gate; not a final)$' "$s"
say only-gate-no-build $? "$(tr '\n' ';' < "$s" 2>/dev/null)"
final "$f" "$w/only3.out" WAVE_TMP="$w/tmp1" WAVE_ONLY=stats,nonsense
has 'WAVE_ONLY names .nonsense.' "$w/only3.out.log" && [ ! -e "$w/only3.out/SUMMARY.txt" ]
say only-unknown $? "$(head -n 2 "$w/only3.out.log" 2>/dev/null | tr '\n' ';')"

# 15. The neutral line counts what moved outside the change, by kind, and
# names the first five.
f=$(fake neutralcount)
cat > "$f/tools/base-cache.sh" <<'STUB'
#!/bin/sh
mkdir -p "$(dirname "$0")/../cache/feedface"; echo "$(cd "$(dirname "$0")/.." && pwd)/cache/feedface"
STUB
cat > "$f/tools/neutral.sh" <<'STUB'
#!/bin/sh
mkdir -p "$3"; echo "sources: files 9 ll-same 6 LL-DIFF 4" > "$3/summary.txt"
printf 'LL-DIFF a/one.av\nLL-DIFF a/two.av\nSTATUS 0->1 a/three.av\nll-same a/four.av\nLL-DIFF a/five.av\n' > "$3/result.txt"
printf 'LL-DIFF six\nrefused-msg seven\nll-same eight\n' > "$3/la-result.txt"
STUB
(cd "$f" && git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -q -m t && git branch -q base) > /dev/null 2>&1
final "$f" "$w/ncount.out" WAVE_TMP="$w/tmp1" WAVE_BASE=base WAVE_ONLY=neutral
has 'outside the change: 6 moved (4 sources, 2 LangArena), the first five: a/one.av a/two.av a/three.av a/five.av six ' "$w/ncount.out/SUMMARY.txt"
say neutral-count $? "$(line neutral: "$w/ncount.out/SUMMARY.txt")"
# 16. WAVE_BESIDE=1: the goldens and the statistics run in a copy (a detached
# worktree of HEAD with its own build of C') while the gate runs; their lines
# join the summary, and the copy's C' must be the gate's byte for byte.
beside_fake() { # NAME LETTERS_BODY
  f=$(fake "$1")
  mkdir -p "$f/bin"; : > "$f/bin/re2c"
  printf '#!/bin/sh\nsh tools/letters.sh runtime b c-llvm d-llvm\necho "GATE PASS (stub)"\n' > "$f/tools/gate.sh"
  printf '#!/bin/sh\nmkdir -p bin runtime/build\n%s\n' "$2" > "$f/tools/letters.sh"
  printf '#!/bin/sh\nmkdir -p "$2"; echo stub > "$2/a.out"; echo stub > "$2/a.err"\n' > "$f/tools/gcstats.sh"
  printf '/bin/\n/runtime/build/\n' > "$f/.gitignore"
  (cd "$f" && git init -q . && git add -A && git -c user.name=t -c user.email=t@t commit -q -m t) > /dev/null 2>&1
  echo "$f"
}
f=$(beside_fake beside 'for x in avant-b avant-c-llvm avant-d-llvm; do echo "stub $x" > bin/$x; chmod +x bin/$x; done; echo rt > runtime/build/avant_rt.o')
final "$f" "$w/beside.out" WAVE_ONLY=gate,goldens,stats WAVE_BESIDE=1
s=$w/beside.out/SUMMARY.txt
has '^gate: pass' "$s" && has '^goldens: green' "$s" && has '^gcstats: 2 files (no base given)$' "$s" && has "^build: C' built by the letters" "$s" \
  && has "^beside: the copy's B, C' and runtime are the gate's, byte for byte" "$s" && has '^WAVE PARTIAL GREEN (only: gate,goldens,stats; not a final)$' "$s" \
  && [ ! -e "$w/beside.out/beside-tree" ] && [ "$(git -C "$f" worktree list | wc -l)" = 1 ]
say beside-merged $? "$(tr '\n' ';' < "$s" 2>/dev/null | cut -c1-700); worktrees: $(git -C "$f" worktree list 2>/dev/null | wc -l)"
f=$(beside_fake besidediff 'for x in avant-b avant-c-llvm avant-d-llvm; do echo "stub $x built in $(pwd)" > bin/$x; chmod +x bin/$x; done; echo rt > runtime/build/avant_rt.o')
final "$f" "$w/besidediff.out" WAVE_ONLY=gate,goldens,stats WAVE_BESIDE=1
s=$w/besidediff.out/SUMMARY.txt
has "^beside: red (the copy's bin/avant-b bin/avant-c-llvm differ from the gate's" "$s" && has '^WAVE PARTIAL RED: beside' "$s"
say beside-differs $? "$(line beside: "$s"); $(line WAVE "$s")"
echo done
