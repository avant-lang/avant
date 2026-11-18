#!/bin/sh
# Check that a release layout runs on any x86-64 processor, not only on the
# one that built it.
#
#   sh tools/isa-check.sh DIR [ELF...]      the static checks
#   sh tools/isa-check.sh --run DIR         the static checks, then the run
#
# DIR is an unpacked release layout (tools/package.sh's
# OUT_DIR/avant-linux-x86_64). The archive promises Linux on x86-64, and
# `avant compile` targets x86-64-v2 by default so its programs run
# elsewhere. That holds only if the C toolchain that built the archive
# emitted baseline x86-64 code: a toolchain whose default is a higher level
# (a distribution's x86-64-v3 package variant, for example) puts AVX2 and
# BMI instructions into the runtime objects every program links and into
# the compiler itself, and nothing else in the build notices, since the
# build machine has them.
#
# The static checks, each failing with the file's name:
#   - the "x86 ISA needed" property (readelf -n) of DIR/avant,
#     DIR/avant-bind, DIR/re2c, every DIR/runtime/build/*.o and each ELF
#     given is at most x86-64-baseline (no property at all is baseline);
#   - no function of a runtime object holds a VEX or EVEX instruction (its
#     mnemonic starts with v; mask registers' with k), BMI1/BMI2, ADX,
#     LZCNT, POPCNT or MOVBE, except libbase64.o's AVX and AVX2 codecs,
#     which the library calls only after checking the processor (cpuid).
#     TZCNT is allowed: its encoding is `rep bsf`, which a processor
#     without BMI1 runs as BSF, and gcc emits it for that reason at the
#     baseline level.
# The run (--run; needs qemu-x86_64, the qemu-user package): under
# `qemu-x86_64 -cpu Nehalem` (x86-64-v2: no AVX, no BMI) DIR/avant prints
# its version, and three programs compiled by DIR/avant at the default
# --cpu (allocation, the collector, threads, JSON, regex, zlib, base64,
# SHA-256, a socket) print what they print natively. On a host whose own C
# library is built for a newer x86-64 (a distribution's x86-64-v3 variant),
# the emulated programs would stop in that library: set QEMU_LD_PREFIX to a
# directory holding a baseline libc6 and libgcc-s1 (qemu loads the dynamic
# loader and libraries from there; .github/baseline-sysroot.sh makes one).
set -eu

run=
if [ "${1:-}" = "--run" ]; then
  run=1
  shift
fi
if [ "$#" -lt 1 ]; then
  echo "usage: sh tools/isa-check.sh [--run] DIR [ELF...]" >&2
  exit 2
fi
dir=$(realpath -m "$1")
shift
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

fail() {
  echo "isa-check: $*" >&2
  exit 1
}

[ -x "$dir/avant" ] || fail "no avant in $dir"
command -v readelf > /dev/null || fail "readelf not found (binutils)"
command -v objdump > /dev/null || fail "objdump not found (binutils)"

# The ISA property: "x86 ISA needed: x86-64-baseline, x86-64-v2, ..." lists
# every level the file's objects declared; anything but the baseline fails.
check_note() { # FILE
  needed=$(readelf -n "$1" 2>/dev/null | sed -n 's/.*x86 ISA needed: *//p' | tr -d ' ' | tr ',' '\n' | grep -v '^x86-64-baseline$' || true)
  [ -z "$needed" ] || fail "$1 needs $(echo "$needed" | tr '\n' ' ')(x86 ISA needed, readelf -n): it was built for a newer x86-64 than the baseline"
}

n=0
for f in "$dir/avant" "$dir/avant-bind" "$dir/re2c" "$dir"/runtime/build/*.o "$@"; do
  [ -e "$f" ] || continue
  check_note "$f"
  n=$((n + 1))
done

# The instruction scan of the runtime objects, per function.
objs=0
for o in "$dir"/runtime/build/*.o; do
  [ -e "$o" ] || continue
  objs=$((objs + 1))
  hits=$(objdump -d --no-show-raw-insn "$o" | awk -F'\t' -v obj="$(basename "$o")" '
    /^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn); next }
    NF >= 2 {
      split($2, w, " ")
      m = w[1]
      if (m ~ /^(rep|repz|repe|repnz|repne|lock|notrack|bnd|data16|addr32|cs|ds|es|ss|fs|gs)$/) m = w[2]
      if (m ~ /^(v[a-z0-9]+|k[a-z0-9]+|andn[lq]?|bextr[lq]?|blsi[lq]?|blsmsk[lq]?|blsr[lq]?|bzhi[lq]?|mulx[lq]?|pdep[lq]?|pext[lq]?|rorx[lq]?|sarx[lq]?|shlx[lq]?|shrx[lq]?|adcx[lq]?|adox[lq]?|lzcnt[wlq]?|popcnt[wlq]?|movbe[wlq]?)$/) {
        if (obj == "libbase64.o" && fn ~ /^base64_stream_(en|de)code_avx(2|512)?$/) next
        if (!(fn in seen)) { seen[fn] = m; order[++k] = fn }
      }
    }
    END { for (i = 1; i <= k && i <= 5; i++) printf "%s (%s) ", order[i], seen[order[i]]; if (k > 5) printf "and %d more functions ", k - 5 }')
  [ -z "$hits" ] || fail "$o: instructions beyond the baseline x86-64 in $hits(objdump -d): its C compiler targets a newer x86-64 by default"
done
[ "$objs" -gt 0 ] || fail "no runtime objects in $dir/runtime/build"
echo "isa-check: $n files at the baseline x86-64 ISA, $objs runtime objects without VEX, EVEX or BMI code"

[ -n "$run" ] || exit 0

command -v qemu-x86_64 > /dev/null || fail "qemu-x86_64 not found (install qemu-user)"
cpu=Nehalem
work=$(mktemp -d "${TMPDIR:-/tmp}/avant-isa-XXXXXX")
trap 'rm -rf "$work"' EXIT

v=$(qemu-x86_64 -cpu "$cpu" "$dir/avant" version < /dev/null 2>&1) || fail "$dir/avant does not run on a $cpu (qemu-x86_64 -cpu $cpu): $v"
echo "isa-check: under qemu-x86_64 -cpu $cpu${QEMU_LD_PREFIX:+ (libraries from $QEMU_LD_PREFIX)}: $v"

cat > "$work/mix.av" <<'EOF_AV'
import std.json
import std.regex
import std.zlib
import std.base64
import std.http

fn main {
  if doc = Json.parse("{\"n\": 1}") {
    puts(doc.int("n") ?? 0)
  }
  if re = Regex.compile("a+") {
    puts(re.count("aa"))
  }
  p = "b"
  if re2 = Regex.compile(p + "+") {
    puts(re2.count("bb"))
  }
  if back = "z".bytes.compress().uncompress() {
    puts(back.size)
  }
  puts("a".bytes.to_base64())
  puts(sha256("abc"))
  if server = http.listen(0) {
    server.close()
  }
  h = spawn {
    1
  }
  puts(h.join() ?? -1)
}
EOF_AV
cp "$dir/examples/binarytrees.av" "$dir/examples/digest.av" "$work/"
for t in mix binarytrees digest; do
  ( cd "$work" && env -i HOME="$work" PATH=/usr/bin:/bin TMPDIR="$work" \
      "$dir/avant" compile "$t.av" "$t" < /dev/null > "$t.compile.log" 2>&1 ) || {
    cat "$work/$t.compile.log" >&2
    fail "$dir/avant could not compile $t.av"
  }
  check_note "$work/$t"
  ( cd "$work" && "./$t" < /dev/null > "$t.native" 2>&1 ) || fail "$t: the program failed natively ($(tail -n 3 "$work/$t.native"))"
  rc=0
  ( cd "$work" && qemu-x86_64 -cpu "$cpu" "./$t" < /dev/null > "$t.qemu" 2>&1 ) || rc=$?
  [ "$rc" -eq 0 ] || fail "$t, compiled by $dir/avant at the default --cpu, fails on a $cpu (qemu-x86_64 -cpu $cpu, exit $rc; 132 is an illegal instruction): $(tail -n 3 "$work/$t.qemu")"
  cmp -s "$work/$t.native" "$work/$t.qemu" || fail "$t prints differently under qemu-x86_64 -cpu $cpu"
  echo "isa-check: $t (default --cpu) runs under qemu-x86_64 -cpu $cpu"
done
echo "isa-check: pass"
