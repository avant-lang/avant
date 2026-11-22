#!/bin/sh
# tools/run-goldens.sh's per-program run limits (tests/goldens/runs/limits.txt)
# in a fake checkout whose compiler is a stub: every program it "compiles"
# sleeps 2 s and prints `hi`. Under the stress mode with a 1 s limit:
#   limit-own: a program named in limits.txt with 10 s for the stress mode
#     passes;
#   limit-mode: a program not named keeps the mode's 1 s and times out;
#   limit-other-mode: a line for the plain mode does not raise the stress
#     mode's limit;
#   limit-summary: the summary counts the programs with their own limit.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/golden-limits.sh WORK
#
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/golden-limits.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
f=$w/avant
mkdir -p "$f/tools" "$f/tests/fixtures/heap" "$f/tests/goldens/runs/fixtures/heap" "$f/bin"
cp "$root/tools/run-goldens.sh" "$f/tools/run-goldens.sh"
cat > "$f/bin/stub" <<'EOF'
#!/bin/sh
# compile SRC OUT FLAGS...
printf '#!/bin/sh\nsleep 2\necho hi\n' > "$3"; chmod +x "$3"
EOF
chmod +x "$f/bin/stub"
: > "$f/tests/goldens/runs/status.txt"
for p in own mode other; do
  printf 'fn main {\n}\n' > "$f/tests/fixtures/heap/$p.av"
  echo hi > "$f/tests/goldens/runs/fixtures/heap/$p.out"
  echo "tests/fixtures/heap/$p.av 0" >> "$f/tests/goldens/runs/status.txt"
done
cat > "$f/tests/goldens/runs/limits.txt" <<'EOF'
# PATH MODE SECONDS REASON
tests/fixtures/heap/own.av stress 10 sleeps 2 s
tests/fixtures/heap/other.av plain 10 sleeps 2 s
EOF
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
(cd "$f" && env GOLDEN_COMPILER="$f/bin/stub" GOLDEN_ENV="AVANT_GC_STRESS=1" GOLDEN_RUN_LIMIT_STRESS=1 GOLDEN_OUT="$w/out" \
  timeout -k 10 120 sh tools/run-goldens.sh tests/fixtures/heap/own.av tests/fixtures/heap/mode.av tests/fixtures/heap/other.av < /dev/null > "$w/run.log" 2>&1)
r=$w/out/results.txt
grep -q '^same tests/fixtures/heap/own.av' "$r"; say limit-own $? "$(grep own.av "$r")"
grep -q '^different tests/fixtures/heap/mode.av' "$r"; say limit-mode $? "$(grep mode.av "$r")"
grep -q '^different tests/fixtures/heap/other.av' "$r"; say limit-other-mode $? "$(grep other.av "$r")"
grep -q 'run limit 1 s, 1 with their own)' "$w/out/summary.txt"; say limit-summary $? "$(cat "$w/out/summary.txt")"
echo done
