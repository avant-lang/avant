#!/bin/sh
# tools/la-spelling.sh on two fake harnesses. Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/la-spelling.sh WORK
#
# WORK is a new scratch directory. A fake checkout carries the real
# tools/la-spelling.sh and ll-digests.sh, a stub harness (tools/la-harness.sh:
# its prod pass stitches langarena/tasks/*.av with helper.av and run.js and
# asks the compiler to build each, as the real one does) and a stub C'
# (`__emit-ll` writes the program with `newname` read as `oldname`, the
# two spellings of one thing, and refuses a program holding REFUSE).
#   spelling-same: a harness that writes oldname as newname compiles alike:
#     status 0, every program SAME, the line says so, the work place gone;
#   spelling-differ: a task whose text changed reads LL-DIFF with its diff,
#     the others SAME, status 1, the line names it;
#   spelling-only: a task only one side has reads ONLY-NEW;
#   spelling-refused: a program the compiler refuses on one side reads
#     REFUSED with both statuses;
#   spelling-usage: a directory that is not a harness is refused, status 2.
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/la-spelling.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
r=$w/fake/avant
mkdir -p "$r/tools" "$r/bin" "$w/fake/LangArena"
for t in la-spelling.sh ll-digests.sh; do cp "$root/tools/$t" "$r/tools/$t"; done
echo '[{"name": "A::A", "checksum": 1}]' > "$w/fake/LangArena/run.js"
cat > "$r/bin/avant-c-llvm" <<'STUB'
#!/bin/sh
[ "$1" = __emit-ll ] || exit 2
if grep -q REFUSE "$2"; then echo "error: refused"; exit 1; fi
sed 's/newname/oldname/g' "$2" > "$3"
STUB
chmod +x "$r/bin/avant-c-llvm"
cat > "$r/tools/la-harness.sh" <<'STUB'
#!/bin/sh
# The stub harness: its prod pass stitches each task as the real one does.
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
d=$root/langarena/tmp/harness; mkdir -p "$d"
cat > "$d/measure" <<'EOF'
#!/bin/sh
A=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
T=$A/langarena/tmp
[ "$1" = prod ] && [ -f "$A/../LangArena/run.js" ] || exit 2
for f in "$A"/langarena/tasks/*.av; do
  s=$(basename "$f" .av)
  echo "cfg $(cat "$A/../LangArena/run.js")" > "$T/$s.cfg.av"
  { echo "// file: $A/langarena/helper.av"; cat "$A/langarena/helper.av"; echo "// file: $T/$s.cfg.av"; cat "$T/$s.cfg.av"; echo "// file: $f"; cat "$f"; } > "$T/$s.av"
  rm -f "$T/$s"
  "$AVANT_COMPILER" compile "$T/$s.av" "$T/$s" --release --cpu=v3 > /dev/null 2>&1
done
exit 0
EOF
chmod +x "$d/measure"; echo "$d/measure"
STUB
mk() { # DIR HELPER A B [C]: a fake langarena/ directory
  mkdir -p "$1/harness" "$1/tasks"; : > "$1/harness/measure.cr"
  echo "$2" > "$1/helper.av"; echo "$3" > "$1/tasks/a.av"; echo "$4" > "$1/tasks/b.av"
  [ $# -ge 5 ] && echo "$5" > "$1/tasks/c.av"
  return 0
}
mk "$w/old" 'fn oldname' 'a calls oldname' 'b'
mk "$w/new1" 'fn newname' 'a calls newname' 'b'
mk "$w/new2" 'fn newname' 'a calls newname' 'b, changed'
mk "$w/new3" 'fn oldname' 'a calls oldname' 'b' 'c'
mk "$w/new4" 'fn oldname' 'a calls oldname REFUSE' 'b'

o=$(cd "$r" && sh tools/la-spelling.sh "$w/old" "$w/new1" "$w/o1" 2> "$w/o1.err" < /dev/null); rc=$?
[ $rc -eq 0 ] && [ "$(cut -f2 "$w/o1/result.tsv" | tr '\n' ' ')" = "SAME SAME " ] && echo "$o" | grep -q '^la-spelling: 2 programs, every one compiles alike' \
  && [ ! -e "$w/o1/work" ] && [ ! -e "$w/o1/diff" ]
say spelling-same $? "rc=$rc $o $(tr '\n' ';' < "$w/o1/result.tsv" 2>/dev/null) $(tail -n 2 "$w/o1.err" | tr '\n' ';')"
o=$(cd "$r" && sh tools/la-spelling.sh "$w/old" "$w/new2" "$w/o2" 2> "$w/o2.err" < /dev/null); rc=$?
[ $rc -eq 1 ] && grep -qx 'a.av	SAME' "$w/o2/result.tsv" && grep -qx 'b.av	LL-DIFF' "$w/o2/result.tsv" && grep -q '^+b, changed' "$w/o2/diff/b.diff" \
  && echo "$o" | grep -q '^la-spelling: 2 programs, 1 compile alike; 1 do not: b LL-DIFF (result.tsv, diff/)'
say spelling-differ $? "rc=$rc $o $(tr '\n' ';' < "$w/o2/result.tsv" 2>/dev/null)"
o=$(cd "$r" && sh tools/la-spelling.sh "$w/old" "$w/new3" "$w/o3" 2> "$w/o3.err" < /dev/null); rc=$?
[ $rc -eq 1 ] && grep -qx 'c.av	ONLY-NEW' "$w/o3/result.tsv" && [ "$(grep -c '	SAME$' "$w/o3/result.tsv")" = 2 ]
say spelling-only $? "rc=$rc $o $(tr '\n' ';' < "$w/o3/result.tsv" 2>/dev/null)"
o=$(cd "$r" && sh tools/la-spelling.sh "$w/old" "$w/new4" "$w/o4" 2> "$w/o4.err" < /dev/null); rc=$?
[ $rc -eq 1 ] && grep -qx 'a.av	REFUSED (0, 1)' "$w/o4/result.tsv"
say spelling-refused $? "rc=$rc $o $(tr '\n' ';' < "$w/o4/result.tsv" 2>/dev/null)"
o=$(cd "$r" && sh tools/la-spelling.sh "$w/old" "$w/fake" "$w/o5" 2>&1 < /dev/null); rc=$?
[ $rc -eq 2 ] && echo "$o" | grep -q 'is not a langarena/ directory'
say spelling-usage $? "rc=$rc $o"
echo done
