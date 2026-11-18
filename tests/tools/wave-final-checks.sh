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
#     line with the call's wall and wait.
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
echo done
