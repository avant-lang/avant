#!/bin/sh
# tools/isa-check.sh refuses a release layout that needs a newer x86-64
# than the baseline, and passes one that does not. Run by
# tests/cases/test_tools.av; by hand:
#
#   sh tests/tools/isa-check.sh COMPILER WORK
#
# COMPILER is a baseline compiler binary (the one under test) that stands
# in for the layout's `avant`; WORK is a new scratch directory. The
# runtime objects are small C files built here with cc. Prints one line
# per check, `ok: NAME` or `FAIL: NAME: why`; test_tools.av expects each
# NAME.
set -u
[ $# -eq 2 ] || { echo "usage: sh tests/tools/isa-check.sh COMPILER WORK" >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
compiler=$1; w=$2
mkdir -p "$w"
say() { if [ "$2" = 0 ]; then echo "ok: $1"; else echo "FAIL: $1: $3"; fi; }

# layout NAME: a layout with the compiler and one baseline runtime object
# (a count of trailing zeros: gcc emits tzcnt, which the check allows).
layout() {
  d=$w/$1
  mkdir -p "$d/runtime/build"
  cp "$compiler" "$d/avant"
  printf 'int base_ctz(unsigned x) { return x ? __builtin_ctz(x) : 32; }\n' > "$d/base.c"
  cc -O2 -c "$d/base.c" -o "$d/runtime/build/base.o"
}
check() { # NAME [ELF...]: run the check on layout NAME, its output beside it
  d=$w/$1; shift
  sh "$root/tools/isa-check.sh" "$d" "$@" < /dev/null > "$d.log" 2>&1
}
loop='void scale(float *a, int n) { for (int i = 0; i < n; i++) a[i] = a[i] * 3.0f + 1.0f; }'

# A baseline layout passes.
layout plain
check plain; r=$?
say baseline-passes "$r" "status $r: $(cat "$w/plain.log")"

# An object of AVX2 code fails, naming the object and the function.
layout vex
printf '%s\n' "$loop" > "$w/vex/v.c"
cc -O3 -mavx2 -c "$w/vex/v.c" -o "$w/vex/runtime/build/vec.o"
check vex; r=$?
[ "$r" -ne 0 ]; say vex-fails $? "status $r"
grep -q 'vec.o: instructions beyond the baseline x86-64 in scale (v' "$w/vex.log"; say vex-named $? "$(cat "$w/vex.log")"

# BMI2 code (shlx) fails too.
layout bmi
printf 'unsigned long shl(unsigned long x, unsigned n) { return x << n; }\n' > "$w/bmi/b.c"
cc -O2 -mbmi2 -c "$w/bmi/b.c" -o "$w/bmi/runtime/build/bmi.o"
check bmi; r=$?
[ "$r" -ne 0 ] && grep -q 'bmi.o: .* shl (shlx)' "$w/bmi.log"; say bmi-fails $? "status $r: $(cat "$w/bmi.log")"

# A file whose ISA property needs x86-64-v3 fails, named.
layout note
printf 'int one(int x) { return x + 1; }\n' > "$w/note/n.c"
cc -O2 -march=x86-64-v3 -mneeded -shared -fPIC "$w/note/n.c" -o "$w/note/v3.so"
check note "$w/note/v3.so"; r=$?
[ "$r" -ne 0 ] && grep -q 'v3.so needs x86-64-v2 x86-64-v3' "$w/note.log"; say note-fails $? "status $r: $(cat "$w/note.log")"

# libbase64's AVX2 codec, chosen at run time by cpuid, is allowed.
layout codec
printf '__attribute__((target("avx2"))) %s\n' "$(echo "$loop" | sed 's/scale/base64_stream_encode_avx2/')" > "$w/codec/c.c"
cc -O3 -c "$w/codec/c.c" -o "$w/codec/runtime/build/libbase64.o"
check codec; r=$?
say codec-allowed "$r" "status $r: $(cat "$w/codec.log")"
echo done
