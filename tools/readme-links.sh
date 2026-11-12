#!/bin/sh
# The README's links and anchors, checked (Stage 29, the README rewrite).
#
#   sh tools/readme-links.sh [FILE.md]        (default: README.md)
#
# 1. Every heading's anchor is unique (GitHub's rule: lower case, every
#    character but letters, digits, `-`, `_` and spaces removed, spaces
#    to `-`), so no anchor ever needs a `-1` suffix and a link stays
#    right when a section moves.
# 2. Every `](#anchor)` names a heading of the file.
# 3. Every relative `](path)` is a file or directory beside the file.
# 4. Every `##` and `###` heading is linked from the "Contents" section
#    (the table of contents shows the two levels), when the file has one.
# Code fences are skipped. The last line is `README LINKS PASS` or
# `README LINKS FAIL`; the exit status is 0 only on PASS (2 on usage).
set -u

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
doc=${1:-$root/README.md}
[ -f "$doc" ] || { echo "readme-links: no file $doc" >&2; exit 2; }
dir=$(dirname "$doc")
tmp=$(mktemp "${TMPDIR:-/tmp}/readme-links-XXXXXX") || exit 2
trap 'rm -f "$tmp"' EXIT

awk -v DOC="$doc" '
function slug(s) {
  s = tolower(s)
  gsub(/[^a-z0-9 _-]/, "", s)
  gsub(/ /, "-", s)
  return s
}
BEGIN { infence = 0; bad = 0; nh = 0; nl = 0; in_toc = 0 }
{
  if ($0 ~ /^```/) { infence = !infence; next }
  if (infence) next
  line = $0
  if (line ~ /^#+ /) {
    level = line
    sub(/ .*$/, "", level)
    text = line
    sub(/^#+ +/, "", text)
    sub(/[ \t]+$/, "", text)
    a = slug(text)
    if (a in seen) {
      printf "%s:%d: the heading \"%s\" repeats the anchor #%s of line %d\n", DOC, NR, text, a, seen[a]
      bad++
    } else {
      seen[a] = NR
    }
    nh++
    hl[nh] = length(level)
    ha[nh] = a
    hn[nh] = NR
    ht[nh] = text
    in_toc = (a == "contents")
    next
  }
  rest = line
  while (match(rest, /\]\([^)]*\)/)) {
    target = substr(rest, RSTART + 2, RLENGTH - 3)
    rest = substr(rest, RSTART + RLENGTH)
    nl++
    lt[nl] = target
    ln[nl] = NR
    if (in_toc && substr(target, 1, 1) == "#") toc[substr(target, 2)] = 1
  }
}
END {
  has_toc = ("contents" in seen)
  for (i = 1; i <= nl; i++) {
    t = lt[i]
    if (substr(t, 1, 1) == "#") {
      if (!(substr(t, 2) in seen)) {
        printf "%s:%d: no heading has the anchor %s\n", DOC, ln[i], t
        bad++
      }
    } else if (t !~ /^[a-z][a-z0-9+.-]*:/) {
      sub(/#.*$/, "", t)
      if (t != "") print t "\t" ln[i] > "/dev/stderr"
    }
  }
  if (has_toc) {
    for (i = 1; i <= nh; i++) {
      if ((hl[i] == 2 || hl[i] == 3) && ha[i] != "contents" && !(ha[i] in toc)) {
        printf "%s:%d: the heading \"%s\" (#%s) is not in the Contents\n", DOC, hn[i], ht[i], ha[i]
        bad++
      }
    }
  }
  printf "readme-links: %d headings, %d links\n", nh, nl
  exit (bad > 0 ? 1 : 0)
}
' "$doc" 2> "$tmp"
rc=$?

tab=$(printf '\t')
while IFS=$tab read -r path line; do
  if [ ! -e "$dir/$path" ]; then
    echo "$doc:$line: the link target $path does not exist"
    rc=1
  fi
done < "$tmp"

if [ "$rc" -ne 0 ]; then
  echo "README LINKS FAIL"
  exit 1
fi
echo "README LINKS PASS"
exit 0
