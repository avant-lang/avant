#!/bin/sh
# runtime/build.sh compiles its translation units at once and gives the same
# bytes as one after another; a unit that fails to compile fails the build.
# Run by tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/runtime-par.sh WORK
#
# WORK is a new scratch directory. The runtime's sources are copied there
# (with tools/bind.c beside them) and built three times with the C compiler
# runtime/build.sh would use, LLVM's object and the bind helper left out
# (LLVM_CONFIG and BIND_LLVM_CONFIG name nothing):
#   par-serial-builds: RUNTIME_BUILD_SERIAL=1, status 0;
#   par-builds: the default (at once), status 0;
#   par-same-bytes: every file of the two build/ directories has the same
#     sha256;
#   par-fail-status: a compiler that refuses avant_str.c: status not 0;
#   par-fail-named: the build says a compile failed, and stops before the
#     merges that read the objects (no avant_zlib.o).
# Prints one line per check, `ok: NAME` or `FAIL: NAME: why`, then `done`.
set -u
[ $# -eq 1 ] || { echo "usage: sh tests/tools/runtime-par.sh WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
w=$(realpath -m "$1")
rm -rf "$w"; mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }
cc=$(sh "$root/runtime/build.sh" --cc) || { say par-serial-builds 1 "no C compiler"; echo done; exit 0; }

# copy NAME: WORK/NAME/runtime, a copy of the sources without build/.
copy() {
  mkdir -p "$w/$1/tools"
  (cd "$root" && tar --exclude=runtime/build -cf - runtime) | tar -xf - -C "$w/$1"
  cp "$root/tools/bind.c" "$w/$1/tools/bind.c"
  # The base64 library's make leaves its objects in its own tree.
  find "$w/$1/runtime/third_party/base64" -name '*.o' -delete
}
run() { # NAME ENV... : build the copy NAME, its log WORK/NAME.log
  n=$1; shift
  env LLVM_CONFIG=/nonexistent/llvm-config BIND_LLVM_CONFIG=/nonexistent/llvm-config CC="$cc" "$@" \
    sh "$w/$n/runtime/build.sh" < /dev/null > "$w/$n.log" 2>&1
}
digests() { (cd "$w/$1/runtime/build" && find . -type f ! -name '.build.lock' | LC_ALL=C sort | xargs sha256sum); }

copy serial; run serial RUNTIME_BUILD_SERIAL=1; rc=$?
say par-serial-builds $rc "status $rc ($w/serial.log)"
copy par; run par; rc=$?
say par-builds $rc "status $rc ($w/par.log)"
digests serial > "$w/serial.sha256"; digests par > "$w/par.sha256"
if [ -s "$w/serial.sha256" ] && cmp -s "$w/serial.sha256" "$w/par.sha256"; then say par-same-bytes 0
else say par-same-bytes 1 "$(diff "$w/serial.sha256" "$w/par.sha256" | head -n 3 | tr '\n' ' ')"; fi

copy fail
cat > "$w/fail-cc" <<EOS
#!/bin/sh
for a in "\$@"; do case "\$a" in */avant_str.c) echo "fail-cc: refused \$a" >&2; exit 1 ;; esac; done
exec "$cc" "\$@"
EOS
chmod +x "$w/fail-cc"
cc_real=$cc; cc=$w/fail-cc; run fail; rc=$?; cc=$cc_real
[ $rc -ne 0 ]; say par-fail-status $? "status 0 with a failing compile"
grep -q 'a compile failed' "$w/fail.log" && [ ! -f "$w/fail/runtime/build/avant_zlib.o" ]
say par-fail-named $? "$(tail -n 2 "$w/fail.log" | tr '\n' ' ')"
rm -rf "$w/serial" "$w/par" "$w/fail"
echo done
