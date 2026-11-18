#!/bin/sh
# ll-digests.sh — one compiler's `__emit-ll` of a list of programs, kept as
# digests, so two compilers' outputs compare without keeping either .ll.
#
#   sh tools/ll-digests.sh ROOT LIST OUT.tsv [DIR]
#
# ROOT is a checkout with its C' built (bin/avant-c-llvm, a compiler that
# reads the lib/ of the checkout it was built in). LIST holds one program
# path per line, relative to DIR (default ROOT), where each compile runs.
# OUT.tsv gets one line per program, sorted by path:
#
#   PATH <TAB> STATUS <TAB> LL <TAB> LOG
#
# STATUS is the compiler's exit status, LL the sha256 of the .ll (`-` when
# there is none), LOG the sha256 of the compiler's output. Both are taken
# after DIR's and then ROOT's physical paths are written as @ROOT@, so the
# same program compiled in two checkouts reads the same when nothing but
# the checkout's place differs (the source paths the .ll and the messages
# carry).
# LL_JOBS programs at once (default 8); every compile under
# `timeout -k 5 300`, AVANT_ROOT unset, stdin from /dev/null. LL_KEEP=DIR
# keeps each normalized .ll and output as DIR/KEY.ll and DIR/KEY.log
# (KEY the path with `/` and `.` made `_`). Status 0, or 2 on a usage error.
set -u

if [ "${1:-}" = --one ]; then
  # --one PATH: one program (the script calling itself through xargs).
  f=$2
  k=$(echo "$f" | tr '/.' '__')
  t=$LLD_TMP/$k
  (cd "$LLD_DIR" && env -u AVANT_ROOT timeout -k 5 300 "$LLD_ROOT/bin/avant-c-llvm" __emit-ll "$f" "$t.ll" > "$t.log" 2>&1 < /dev/null)
  rc=$?
  sed -i "s#$LLD_DIR#@ROOT@#g; s#$LLD_ROOT#@ROOT@#g" "$t.log"
  lg=$(sha256sum < "$t.log" | cut -c1-64)
  ll=-
  if [ -s "$t.ll" ]; then
    sed -i "s#$LLD_DIR#@ROOT@#g; s#$LLD_ROOT#@ROOT@#g" "$t.ll"
    ll=$(sha256sum < "$t.ll" | cut -c1-64)
  fi
  if [ -n "${LL_KEEP:-}" ]; then
    mv "$t.log" "$LL_KEEP/$k.log"; [ -e "$t.ll" ] && mv "$t.ll" "$LL_KEEP/$k.ll"
  fi
  rm -f "$t.ll" "$t.log"
  printf '%s\t%s\t%s\t%s\n' "$f" "$rc" "$ll" "$lg"
  exit 0
fi

[ $# -ge 3 ] && [ $# -le 4 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2; }
LLD_ROOT=$(realpath "$1") || exit 2
list=$(realpath "$2") || exit 2
out=$(realpath -m "$3")
LLD_DIR=$(realpath "${4:-$1}") || exit 2
[ -x "$LLD_ROOT/bin/avant-c-llvm" ] || { echo "ll-digests.sh: $LLD_ROOT/bin/avant-c-llvm: no such compiler (build C' first)" >&2; exit 2; }
if [ -n "${LL_KEEP:-}" ]; then LL_KEEP=$(realpath -m "$LL_KEEP"); mkdir -p "$LL_KEEP" || exit 2; export LL_KEEP; fi
LLD_TMP=$(mktemp -d "$(dirname "$out")/.ll-digests.XXXXXX") || exit 2
export LLD_ROOT LLD_DIR LLD_TMP
self=$(realpath "$0")
xargs -P "${LL_JOBS:-8}" -I{} sh "$self" --one {} < "$list" | LC_ALL=C sort > "$out.part"
rm -rf "$LLD_TMP"
mv "$out.part" "$out"
