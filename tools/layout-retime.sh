#!/bin/sh
# layout-retime.sh — does a LangArena task's time follow where its code
# sits? One task compiled twice by this checkout's C', as it is and with
# only its layout shifted, then timed alternately.
#
#   sh tools/layout-retime.sh LADIR SLUG [RUNS] [OUT]   (from the checkout root)
#
# LADIR is a base cache's LangArena part (tools/base-cache.sh --la REV:
# the stitched programs and the harness's compile arguments); SLUG a task's
# file name there (brainfuck_recursion for Brainfuck::Recursion: the name
# with :: as _, lowercased). The shift is an unused C function linked in
# with `--cc` (tools/layout/plt.c, four PLT entries: the program's code
# starts 64 bytes later, nothing else changes; LAYOUT_CC names another C
# file). Both binaries are checked to print the same first line, the
# address of `main` in each is printed, and the two run alternately RUNS
# times each (default 5) from LADIR's directory with an empty environment
# but PATH and HOME, each under /usr/bin/time (wall, user, involuntary
# switches). The last lines are each side's range and median and the
# verdict: "the time follows the layout" when the two ranges do not
# overlap, "not separated" otherwise. A timing difference at equal
# instruction counts is worth this before it is believed: a function or a
# PLT entry added anywhere before the program's code can move a task by
# several percent on some machines. It times: run it under the machine
# lock's exclusive side (`ledger run --timed`). OUT (default a new
# directory under TMPDIR, removed) keeps the binaries and the lines.
# Status 0, 1 when a binary did not build or the outputs differ, 2 on usage.
set -u
[ $# -ge 2 ] && [ $# -le 4 ] || { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
la=$(realpath "$1") || exit 2; s=$2; n=${3:-5}
src=$la/avant/langarena/tmp
[ -f "$src/$s.av" ] && [ -f "$la/args/$s" ] || { echo "layout-retime.sh: $s is not a stitched program of $la" >&2; exit 2; }
[ -x "$root/bin/avant-c-llvm" ] || { echo "layout-retime.sh: $root/bin/avant-c-llvm: no such compiler" >&2; exit 2; }
[ -x /usr/bin/time ] || { echo "layout-retime.sh: /usr/bin/time is missing" >&2; exit 2; }
cc_file=$(realpath "${LAYOUT_CC:-$root/tools/layout/plt.c}") || exit 2
if [ -n "${4:-}" ]; then out=$(realpath -m "$4"); mkdir -p "$out" || exit 2; keep=1
else out=$(mktemp -d "${TMPDIR:-/tmp}/layout-retime.XXXXXX") || exit 2; keep=0; fi
set --
while IFS= read -r a; do set -- "$@" "$a"; done < "$la/args/$s"
(cd "$src" && env -u AVANT_ROOT timeout -k 10 300 "$root/bin/avant-c-llvm" compile "$s.av" "$out/plain" "$@" < /dev/null > "$out/plain.log" 2>&1) \
  || { echo "layout-retime.sh: $s did not compile ($out/plain.log)" >&2; exit 1; }
(cd "$src" && env -u AVANT_ROOT timeout -k 10 300 "$root/bin/avant-c-llvm" compile "$s.av" "$out/shifted" "$@" --cc "$cc_file" < /dev/null > "$out/shifted.log" 2>&1) \
  || { echo "layout-retime.sh: $s did not compile with --cc $cc_file ($out/shifted.log)" >&2; exit 1; }
for b in plain shifted; do echo "$b main at $(nm "$out/$b" 2>/dev/null | awk '$3 == "main" { print $1 }')"; done | tee "$out/result.txt"
one() { # BIN: one run, "wall user invol" on stdout, its first line in OUT/BIN.first
  (cd "$src" && env -i PATH="$PATH" HOME="${HOME:-/}" /usr/bin/time -f '%e %U %c' -o "$out/$1.time" timeout -k 10 900 "$out/$1" < /dev/null > "$out/$1.out" 2> /dev/null)
  head -n 1 "$out/$1.out" > "$out/$1.first"
  cat "$out/$1.time"
}
i=0
while [ $i -lt "$n" ]; do
  for b in plain shifted; do
    set -- $(one "$b"); echo "$s $b wall=$1 user=$2 invol=$3" | tee -a "$out/result.txt"
  done
  if ! cmp -s "$out/plain.first" "$out/shifted.first"; then echo "layout-retime.sh: the two binaries print different first lines" >&2; exit 1; fi
  i=$((i + 1))
done
awk '$2 == "plain" || $2 == "shifted" { split($3, w, "="); v = w[2] + 0; k = $2; c[k]++; x[k, c[k]] = v
    if (!(k in lo) || v < lo[k]) lo[k] = v; if (!(k in hi) || v > hi[k]) hi[k] = v }
  function med(k,   i, j, t, m) { m = c[k]; for (i = 1; i <= m; i++) a[i] = x[k, i]
    for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t }
    return (m % 2) ? a[(m + 1) / 2] : (a[m / 2] + a[m / 2 + 1]) / 2 }
  END { for (k in c) printf "%s: %d runs, %.2f-%.2f s, median %.3f s\n", k, c[k], lo[k], hi[k], med(k)
    if (hi["plain"] < lo["shifted"] || hi["shifted"] < lo["plain"]) printf "verdict: the time follows the layout (median %+.1f%% with the code shifted)\n", (med("shifted") / med("plain") - 1) * 100
    else print "verdict: not separated (the ranges overlap): the layout does not explain the difference here" }' "$out/result.txt" | LC_ALL=C sort | tee -a "$out/result.txt"
[ $keep = 1 ] || rm -rf "$out"
exit 0
