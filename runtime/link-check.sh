#!/bin/sh
# link-check.sh — every avant_* symbol a runtime object calls is defined
# globally by a runtime object, so the first program that links them does
# not find a hidden symbol made local.
#
#   sh runtime/link-check.sh DIR [EXPORTS]
#
# DIR holds the runtime's objects (runtime/build): avant_rt.o, the core
# units folded into one object whose global symbols are only the names
# EXPORTS (default runtime/avant_rt.exports beside this script) lists,
# and the others (avant_str.o, avant_json.o, ...). avant_llvm.o, which
# only the compiler links, is not read. For each other object's
# undefined avant_* symbol, some object of DIR must define it globally.
# A symbol avant_rt.o defines but not globally is the usual fault: a
# hidden function of a core unit, called from another object, is made
# local by the merge's `objcopy --keep-global-symbols` because EXPORTS
# does not list it; the merge and its surface check pass, and the link of
# a program that needs the other object fails ("hidden symbol isn't
# defined"). runtime/build.sh runs this after the objects are built.
# Prints one line per missing symbol (the symbol, the object that needs
# it, the reason) on stderr. Status 0 when nothing is missing, 1
# otherwise, 2 on a usage error.
set -u
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: sh runtime/link-check.sh DIR [EXPORTS]" >&2; exit 2; }
dir=$1
exports=${2:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)/avant_rt.exports}
[ -f "$dir/avant_rt.o" ] || { echo "runtime/link-check.sh: no avant_rt.o in $dir" >&2; exit 2; }
set --
for o in "$dir"/*.o; do
  case "$(basename "$o")" in avant_llvm.o) continue ;; esac
  set -- "$@" "$o"
done
# Every global definition (SYMBOL OBJECT), every undefined avant_* of an
# object other than avant_rt.o (SYMBOL OBJECT), and avant_rt.o's local
# definitions of an avant_* name.
LC_ALL=C nm -A -g --defined-only "$@" 2>/dev/null | awk '{ print $NF }' | LC_ALL=C sort -u > "$dir/.link-check.def" || exit 2
LC_ALL=C nm -A -u "$@" 2>/dev/null | awk '{ o = $1; sub(/:$/, "", o); n = split(o, p, "/"); if ($NF ~ /^avant_/ && p[n] != "avant_rt.o") print $NF, p[n] }' | LC_ALL=C sort -u > "$dir/.link-check.und"
LC_ALL=C nm "$dir/avant_rt.o" | awk 'NF == 3 && $2 ~ /^[a-z]$/ && $3 ~ /^avant_/ { print $3 }' | LC_ALL=C sort -u > "$dir/.link-check.loc"
missing=$(awk -v exports="$(basename "$exports")" 'FILENAME == ARGV[1] { d[$1] = 1; next } FILENAME == ARGV[2] { l[$1] = 1; next }
  !($1 in d) {
    if ($1 in l) why = "avant_rt.o defines it, but not globally: " exports " does not list it, so the merge made it local (list it in the runtime-internal section)"
    else why = "no runtime object defines it"
    print "runtime/build.sh: " $1 ", called by " $2 ", is not defined globally by any runtime object: " why
  }' "$dir/.link-check.def" "$dir/.link-check.loc" "$dir/.link-check.und")
rm -f "$dir/.link-check.def" "$dir/.link-check.und" "$dir/.link-check.loc"
[ -z "$missing" ] && exit 0
printf '%s\n' "$missing" >&2
exit 1
