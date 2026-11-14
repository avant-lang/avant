#!/bin/sh
# tools/package.sh gives the same archive for the same files (O676,
# VALK-TM2). Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/package-repro.sh COMPILER RE2C WORK
#
# WORK is a new scratch directory. Prints one line per check, `ok: NAME`
# or `FAIL: NAME: why`; test_tools.av expects each NAME.
set -u
[ $# -eq 3 ] || { echo "usage: sh tests/tools/package-repro.sh COMPILER RE2C WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
compiler=$1; re2c=$2; w=$3
mkdir -p "$w"
tgz=avant-linux-x86_64.tar.gz
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
pkg() { # DIR [VAR=VALUE...]: package into DIR, the log beside it
  d=$1; shift
  env "$@" TMPDIR="$w" sh "$root/tools/package.sh" "$d" "$compiler" "$re2c" < /dev/null > "$d.log" 2>&1
}
sum() { cut -d' ' -f1 "$1/$tgz.sha256" 2>/dev/null; }
# Every entry's mtime (UTC) and owner, one line each, deduplicated.
stamps() { TZ=UTC tar --full-time --numeric-owner -tvzf "$1/$tgz" 2>/dev/null | awk '{print $2" "$4" "$5}' | sort -u; }

# Two packages of one compiler, one after the other: the same bytes.
pkg "$w/one" -u SOURCE_DATE_EPOCH; r1=$?
pkg "$w/two" -u SOURCE_DATE_EPOCH; r2=$?
say packaged $((r1 + r2)) "status $r1 and $r2 (logs $w/one.log, $w/two.log)"
s1=$(sum "$w/one"); s2=$(sum "$w/two")
[ -n "$s1" ] && [ "$s1" = "$s2" ]; say same-sha256 $? "$s1 against $s2"
cmp -s "$w/one/$tgz" "$w/two/$tgz"; say same-bytes $? "the archives differ"
(cd "$w/one" && sha256sum -c "$tgz.sha256" > /dev/null 2>&1); say sha256-file $? "the .sha256 file does not check"

# Every entry carries the commit's time (0 outside a git checkout) and
# owner 0/0.
t=$(git -C "$root" log -1 --format=%ct HEAD 2>/dev/null) || t=0
[ -n "$t" ] || t=0
want="0/0 $(TZ=UTC date -d "@$t" '+%Y-%m-%d %H:%M:%S')"
got=$(stamps "$w/one")
[ "$got" = "$want" ]; say commit-time $? "entries carry '$(echo "$got" | tr '\n' ';')', want '$want'"

# SOURCE_DATE_EPOCH overrides the commit's time.
pkg "$w/epoch" SOURCE_DATE_EPOCH=86400; r3=$?
got=$(stamps "$w/epoch")
[ $r3 -eq 0 ] && [ "$got" = "0/0 1970-01-02 00:00:00" ]; say source-date-epoch $? "status $r3, entries carry '$(echo "$got" | tr '\n' ';')'"
s3=$(sum "$w/epoch")
[ -n "$s3" ] && [ "$s3" != "$s1" ]; say epoch-changes-sha256 $? "the same sha256 for another time"

# An epoch that is not a number of seconds is refused before any work.
pkg "$w/bad" SOURCE_DATE_EPOCH=soon; r4=$?
[ $r4 -eq 1 ] && grep -q '^package: SOURCE_DATE_EPOCH is not a number of seconds: soon$' "$w/bad.log" && [ ! -e "$w/bad" ]
say bad-epoch $? "status $r4; $(head -c 300 "$w/bad.log")"
echo done
