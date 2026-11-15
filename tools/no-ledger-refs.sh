#!/bin/sh
# No tracked file names an identifier of the project's planning notes,
# which live outside this repository: the code, the comments, the tests,
# the tools and the documents state the fact or the reason itself.
#
#   sh tools/no-ledger-refs.sh [--count] [PATH...]     (stdin unused)
#
# Scans every file `git ls-files` lists (under PATH when given), except
# results/ (working notes that are not tracked as part of the language),
# binary files, the allowlist (whose reasons quote the false matches they
# allow) and this script (whose text names what it reports). A line is
# reported when it holds, outside the spans the allowlist permits:
#   - a label of one capital D, O, Q, P, R or F followed by digits, as a
#     whole word (with an optional lower-case letter after the digits);
#   - a wave name: W followed by digits and letters, W- followed by a
#     capital, VALK- followed by a letter or digit, or the
#     word Wave followed by a capital or a digit;
#   - the word Stage followed by a number;
#   - a dated path of the notes, results/ or records/ followed by a date;
#   - the name of one of the notes' documents (gc-design.md, mir.md,
#     gc-audit.md, floor.md, operating.md, philosophy.md, decisions.md,
#     questions.md, roadmap.md, syntax.md, synthesis.md, triage.md,
#     plan.md, gc.md, benchmarks.md), or a path into the notes'
#     repository (avant-ledgers, research/valk/).
# The allowlist (tools/no-ledger-refs.allow) holds the legitimate
# matches: one entry a line, `GLOB REGEX REASON...`; in the files whose
# path matches GLOB (fnmatch, `*` crossing `/`), every span REGEX (a
# Python regular expression without spaces; `\s` where one is needed)
# matches is removed before the line is searched. Each entry says why.
#
# With no PATH, an entry that removes nothing from any reported line is
# stale and fails the check, so the list stays as short as the tree needs.
#
# Prints `path:line: text` for every reported line, then
# `NO LEDGER REFS PASS: N files` or `NO LEDGER REFS FAIL: M lines in
# K files`; --count prints only the number of reported lines. The exit
# status is 0 when nothing is reported, 1 otherwise, 2 on usage.
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
count=0
if [ "${1:-}" = --count ]; then count=1; shift; fi
case "${1:-}" in -*) echo "usage: sh tools/no-ledger-refs.sh [--count] [PATH...]" >&2; exit 2 ;; esac
git rev-parse --is-inside-work-tree > /dev/null 2>&1 || { echo "no-ledger-refs: not a git checkout: $root" >&2; exit 2; }
command -v python3 > /dev/null 2>&1 || { echo "no-ledger-refs: python3 is needed" >&2; exit 2; }

prog=$(cat <<'EOF'
import fnmatch, re, subprocess, sys

count = sys.argv[1] == "1"
paths = sys.argv[2:]
REF = re.compile(
    r"\b[DOQPRF][0-9]+[a-z]?\b"
    r"|\bW[0-9]+[A-Za-z0-9]*\b"
    r"|\bW-[A-Z][A-Za-z0-9]*"
    r"|\bVALK-[A-Za-z0-9]+"
    r"|\bWave\s+[A-Z0-9]"
    r"|\bStage\s+[0-9]+"
    r"|\b(?:results|records)/[0-9]{4}-[0-9]{2}-[0-9]{2}"
    r"|\b(?:gc-design|mir|gc-audit|floor|operating|philosophy|decisions"
    r"|questions|roadmap|syntax|synthesis|triage|plan|gc|benchmarks)\.md\b"
    r"|\bavant-ledgers\b|\bresearch/valk/"
)

allow = []
with open("tools/no-ledger-refs.allow", encoding="utf-8") as f:
    for n, line in enumerate(f, 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            print(f"no-ledger-refs: tools/no-ledger-refs.allow:{n}: an entry is GLOB REGEX REASON", file=sys.stderr)
            sys.exit(2)
        try:
            allow.append((parts[0], re.compile(parts[1]), [0]))
        except re.error as e:
            print(f"no-ledger-refs: tools/no-ledger-refs.allow:{n}: {e}", file=sys.stderr)
            sys.exit(2)

files = subprocess.run(["git", "ls-files", "-z", "--"] + paths, check=True,
                       stdout=subprocess.PIPE).stdout.decode("utf-8", "surrogateescape").split("\0")
files = [p for p in files if p and not p.startswith("results/") and p not in ("tools/no-ledger-refs.allow", "tools/no-ledger-refs.sh")]
reported = 0
bad_files = 0
scanned = 0
for p in files:
    try:
        with open(p, "rb") as f:
            data = f.read()
    except (IsADirectoryError, FileNotFoundError):
        continue  # a submodule, or a file deleted in the work tree
    if b"\0" in data[:8192]:
        continue
    scanned += 1
    rules = [a for a in allow if fnmatch.fnmatchcase(p, a[0])]
    hit = False
    for n, line in enumerate(data.decode("utf-8", "replace").split("\n"), 1):
        if not REF.search(line):
            continue
        rest = line
        for _, rx, used in rules:
            rest, k = rx.subn(" ", rest)
            used[0] += k
        if REF.search(rest):
            reported += 1
            hit = True
            if not count:
                print(f"{p}:{n}: {line.strip()[:200]}")
    bad_files += hit
stale = []
if not paths:
    stale = [g + " " + rx.pattern for g, rx, used in allow if used[0] == 0]
    for e in stale:
        print(f"no-ledger-refs: stale allowlist entry (nothing to allow): {e}")
if count:
    print(reported)
elif reported or stale:
    print(f"NO LEDGER REFS FAIL: {reported} lines in {bad_files} files, {len(stale)} stale allowlist entries")
else:
    print(f"NO LEDGER REFS PASS: {scanned} files")
sys.exit(1 if reported or stale else 0)
EOF
)
exec python3 -I -c "$prog" "$count" "$@"
