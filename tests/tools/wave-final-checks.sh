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
#     the merge base, not the branch's tip.
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
echo done
