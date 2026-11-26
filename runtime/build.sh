#!/bin/sh
# Compile the runtime objects into runtime/build/.
# The compiler links these objects; no Crystal is involved.
#
#   sh runtime/build.sh         build the objects
#   sh runtime/build.sh --cc    print the C compiler it would build them with
#
# The C compiler is CC when it is set (CC=gcc builds the runtime with gcc,
# the other compiler it is written for), else the clang of the LLVM the
# compiler links (LLVM_CONFIG's bindir, by default /opt/llvm-22.1.8/bin;
# else clang-22 on the PATH): one toolchain for the program's code and the
# runtime's. Measured against gcc 15.2 over every LangArena task, the
# runtime clang builds runs most of the runtime-heavy tasks faster (a radix
# sort, the suffix sorting, the JSON codec, the allocator's slow path).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
llvm_default=/opt/llvm-22.1.8/bin/llvm-config
[ -x "$llvm_default" ] || llvm_default=llvm-config-22
if [ -n "${CC:-}" ]; then
  cc=$CC
elif bindir=$("${LLVM_CONFIG:-$llvm_default}" --bindir 2> /dev/null) && [ -x "$bindir/clang" ]; then
  cc=$bindir/clang
elif command -v clang-22 > /dev/null 2>&1; then
  cc=clang-22
else
  echo "runtime/build.sh: no clang 22 found (${LLVM_CONFIG:-$llvm_default} --bindir, clang-22): install LLVM 22 with sh tools/install-llvm.sh, or build with gcc: CC=gcc sh runtime/build.sh" >&2
  exit 1
fi
if [ "${1:-}" = --cc ]; then
  echo "$cc"
  exit 0
fi
mkdir -p "$root/build"
# One build of a tree's runtime at a time: the objects are written in
# place, and a second build started meanwhile (another test run of the
# same checkout) would link a runtime with half its units. The second
# waits for the first, on a lock file in runtime/build.
if [ -z "${AVANT_RUNTIME_BUILD_LOCK:-}" ] && command -v flock > /dev/null 2>&1; then
  AVANT_RUNTIME_BUILD_LOCK=1 exec flock "$root/build/.build.lock" sh "$root/build.sh" "$@"
fi
# Code placement: every runtime function starts on a
# 64-byte boundary and loops on 32, as the compiler places a program's own
# code (runtime/avant_llvm.c, place_functions). gcc's default is 16 for
# both, so the runtime growing anywhere moved the hot functions linked
# after it by 16-byte steps, and with them tasks' times (up to 14%;
# tools/placement-probe.sh measures it).
# Each function also gets a section of its own (.text.NAME), so the hot
# ones can be placed apart from the rest (place_hot below); the linker
# keeps the others in the order they had in one .text.
place="-falign-functions=64 -falign-loops=32 -ffunction-sections"
# A source path in an object (`__FILE__` of an `assert`, as picohttpparser's)
# is written relative to the checkout (runtime/...), so a program built in
# one checkout is byte for byte the one built from the same commit in
# another.
prefix="-ffile-prefix-map=$root=runtime"
# The hardening and the format warnings Ubuntu's gcc turns on by default,
# written out, so that clang (whose defaults have none of them) and a gcc
# of another distribution build the same kind of code: stack protectors,
# stack clash probes, control-flow protection (endbr64), the fortified
# libc calls. With Ubuntu's gcc they change no byte of any object. Every
# compile below takes them, the vendored libraries' included.
harden="-fstack-protector-strong -fstack-clash-protection -fcf-protection=full -D_FORTIFY_SOURCE=3 -Wformat -Wformat-security"
cflags="-O2 -std=c11 -pthread -fno-omit-frame-pointer $harden $place $prefix -I$root -I$root/third_party/yyjson -I$root/third_party/base64/include -I$root/third_party/uSockets/src -I$root/third_party/picohttpparser -DLIBUS_NO_SSL"

compile() {
  "$cc" -c $cflags -o "$2" "$1"
}

# The young generation is S+ (avant_gc_sticky.c).
# Candidate E, the evacuating nursery, and the AVANT_GC_CANDIDATE knob
# that chose between the candidates were deleted:
# E's mechanisms that S+ uses stay in S+. Candidate S alone (S+ with its
# copy compiled out, the internal constant GC_STICKY_COPY 0) was deleted
# as a build later: nothing built or tested
# it; its in-place promotion is part of S+.
gc_cflags=

# The core units: the collector and the language traps. They fold into
# one object, avant_rt.o, below (build_core).
core_units="avant_rt_traps avant_gc_heap avant_gc_alloc avant_gc_los avant_gc_remset avant_gc_proof avant_gc_sticky avant_gc_mark avant_gc_defrag avant_gc_verify avant_gc_collect avant_gc_stats avant_gc_roots avant_gc_safepoint"

# The objects every program links (pipeline.av runtime_object_names), into
# OUT: avant_rt.o from the core units, then the others.
# The core units fold into avant_rt.o: the seed's link table names
# avant_rt.o only, so a new core object could not be linked by the old
# seed. The source split is organizational; the link surface is not:
# after the merge only the symbols avant_rt.exports lists stay global, so a
# function or variable the units share with each
# other never becomes part of the object's surface.
build_core() {
  out=$1
  mkdir -p "$out"
  set --
  for name in $core_units; do
    # shellcheck disable=SC2086
    "$cc" -c $cflags $gc_cflags -o "$out/$name.unit.o" "$root/$name.c"
    set -- "$@" "$out/$name.unit.o"
  done
  ld -r -o "$out/avant_rt_merged.o" "$@"
  objcopy --keep-global-symbols="$root/avant_rt.exports" "$out/avant_rt_merged.o"
  # The surface check: the object's global symbols are
  # exactly the exports file's names, the ABI section with default
  # visibility and the runtime-internal section hidden.
  want_abi=$(sed -n '/^# runtime-internal$/q; /^[^#]/p' "$root/avant_rt.exports" | LC_ALL=C sort)
  want_int=$(sed -n '/^# runtime-internal$/,$p' "$root/avant_rt.exports" | sed -n '/^[^#]/p' | LC_ALL=C sort)
  have=$(nm -g --defined-only "$out/avant_rt_merged.o" | awk '{print $3}' | LC_ALL=C sort)
  if [ "$have" != "$(printf '%s\n%s\n' "$want_abi" "$want_int" | sed '/^$/d' | LC_ALL=C sort)" ]; then
    echo "runtime/build.sh: avant_rt.o's global symbols differ from avant_rt.exports" >&2
    exit 1
  fi
  hidden=$(readelf -sW "$out/avant_rt_merged.o" | awk '$5 == "GLOBAL" && $7 != "UND" && $6 == "HIDDEN" {print $8}' | LC_ALL=C sort)
  if [ "$hidden" != "$want_int" ]; then
    echo "runtime/build.sh: avant_rt.o's hidden globals differ from avant_rt.exports' runtime-internal section" >&2
    exit 1
  fi
  # The merged parts are removed, as the zlib and PCRE2 parts are:
  # nothing links them on their own, and tools/package.sh ships every
  # object left here (they were 28 KB of dead weight in the release).
  rm -f "$@"
  mv "$out/avant_rt_merged.o" "$out/avant_rt.o"
  for name in avant_str avant_buf avant_hash avant_array avant_task avant_wide avant_codec avant_io avant_cov; do
    # shellcheck disable=SC2086
    "$cc" -c $cflags -o "$out/$name.o" "$root/$name.c"
  done
  # The stack overflow's unit (avant_stack.c) joins the last object every
  # program links, so its code and its main-thread signal stack (.bss)
  # come after everything else: placed first, or among the core units, it
  # moved the runtime's code and data after it, which measured slows a
  # LangArena task by a few percent (the link table names no new object,
  # so the seed links it too).
  # shellcheck disable=SC2086
  "$cc" -c $cflags -o "$out/avant_stack.unit.o" "$root/avant_stack.c"
  mv "$out/avant_cov.o" "$out/avant_cov.unit.o"
  ld -r -o "$out/avant_cov.o" "$out/avant_cov.unit.o" "$out/avant_stack.unit.o"
  rm -f "$out/avant_cov.unit.o" "$out/avant_stack.unit.o"
  # The hot thread-locals generated code reads (avant_tls.c): their own
  # object, which the compiler links after every other object of a
  # program, so they end the thread-local block with any linker.
  # shellcheck disable=SC2086
  "$cc" -c $cflags -o "$out/avant_tls.o" "$root/avant_tls.c"
}

build_core "$root/build"
# The seed-ABI copy an older build made (a 32-bit avant_int for the
# v0.40.0 seed's B) is gone; a checkout built before keeps none.
rm -rf "$root/build/seed-abi"
for name in avant_json.c avant_http.c avant_base64.c avant_sys.c avant_os.c avant_digest.c; do
  compile "$root/$name" "$root/build/${name%.c}.o"
done

# zlib, vendored at the pin in third_party/zlib/PIN: the library
# files compress and uncompress need, built as zlib's own configure builds
# them (-O3 -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN). It folds into
# avant_zlib.o, the object the compiler's std.zlib row names, so a program
# needs no -lz; only the avant_* symbols stay global, so a program that
# binds a system zlib itself (examples/zlib.av) cannot clash.
zl="$root/third_party/zlib"
zl_out="$root/build/zlib"
rm -rf "$zl_out"
mkdir -p "$zl_out"
for f in adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil; do
  "$cc" -c -O3 -pthread -fno-omit-frame-pointer $harden -D_LARGEFILE64_SOURCE=1 -DHAVE_HIDDEN \
    -I"$zl" -o "$zl_out/$f.o" "$zl/$f.c"
done
"$cc" -c $cflags -I"$zl" -o "$root/build/avant_zlib_own.o" "$root/avant_zlib.c"
ld -r -o "$root/build/avant_zlib.o" "$root/build/avant_zlib_own.o" "$zl_out"/*.o
objcopy -w --keep-global-symbol='avant_*' "$root/build/avant_zlib.o"
rm -rf "$zl_out" "$root/build/avant_zlib_own.o"

# PCRE2, vendored at the pin in third_party/pcre2/PIN: the 8-bit
# library with JIT, configured as the system library std.regex linked
# before (third_party/pcre2/src/config.h). It folds into avant_re.o, the one
# object the compiler's std.regex row names, so a program needs no libpcre2.
# Its symbols are made local after the merge: nothing outside avant_re.c
# calls PCRE2, and a program that binds a system PCRE2 itself cannot clash.
# HEAP_LIMIT (KiB) is the runtime's own default, 256 MiB, in place of
# PCRE2's 20 GB; with PCRE2's match and depth limits (ten million) it is
# what std.regex's Regex.compile defaults to, so a Regex with the default
# limits matches with no match context (avant_re.c, cache entries).
# Its functions get sections of their own too, so its hot ones can be
# placed (runtime/hot.txt, place_hot below).
pcre="$root/third_party/pcre2"
pcre_out="$root/build/pcre2"
rm -rf "$pcre_out"
mkdir -p "$pcre_out"
for f in auto_possess chkdint chartables compile compile_cgroup compile_class config context convert \
  dfa_match error extuni find_bracket jit_compile maketables match match_data match_next newline ord2utf \
  pattern_info script_run serialize string_utils study substitute substring tables ucd valid_utf xclass
do
  "$cc" -c -O2 -pthread -fno-omit-frame-pointer -ffunction-sections $harden -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 \
    -DHEAP_LIMIT=262144 -I"$pcre/src" -o "$pcre_out/pcre2_$f.o" "$pcre/src/pcre2_$f.c"
done
"$cc" -c $cflags -DPCRE2_STATIC -I"$pcre/src" -o "$root/build/avant_re_own.o" "$root/avant_re.c"
ld -r -o "$root/build/avant_re.o" "$root/build/avant_re_own.o" "$pcre_out"/*.o
objcopy -w --localize-symbol='pcre2_*' --localize-symbol='_pcre2_*' "$root/build/avant_re.o"
rm -rf "$pcre_out" "$root/build/avant_re_own.o"

compile "$root/third_party/yyjson/yyjson.c" "$root/build/yyjson.o"
compile "$root/third_party/picohttpparser/picohttpparser.c" "$root/build/picohttpparser.o"

# picohttpparser folds into avant_http.o: the seed's link table names
# avant_http.o only, so a new object could not be linked by the old
# seed. The compiler's std.http row names avant_http.o for the pair.
ld -r -o "$root/build/avant_http_merged.o" "$root/build/avant_http.o" "$root/build/picohttpparser.o"
mv "$root/build/avant_http_merged.o" "$root/build/avant_http.o"
rm -f "$root/build/picohttpparser.o"

# aklomp/base64 e4cc438. LangArena's C port calls this library.
# x86_64 builds the SIMD codecs; other machines get the plain codec.
# The runtime picks a codec from CPUID on first use.
b64="$root/third_party/base64"
rm -f "$b64/lib/config.h" "$b64/lib/libbase64.o" "$b64/lib/exports.build.txt"
simd=""
case "$(uname -m)" in
  x86_64|amd64)
    simd="AVX2_CFLAGS=-mavx2 SSSE3_CFLAGS=-mssse3 SSE41_CFLAGS=-msse4.1 SSE42_CFLAGS=-msse4.2 AVX_CFLAGS=-mavx"
    ;;
esac
# shellcheck disable=SC2086
# The library's Makefile adds its own flags to CFLAGS; function sections,
# so its hot codecs can be placed (runtime/hot.txt, place_hot below).
( cd "$b64" && env CC="$cc" CFLAGS="$harden -ffunction-sections" $simd make lib/libbase64.o )
cp "$b64/lib/libbase64.o" "$root/build/libbase64.o"
# The aklomp object stays separate: the compiler links it only for a
# program that imports std.base64. The SIMD codecs need their own -m
# flags, so they cannot be one translation unit with avant_base64.c.

us="$root/third_party/uSockets/src"
compile "$us/bsd.c" "$root/build/us_bsd.o"
compile "$us/context.c" "$root/build/us_context.o"
compile "$us/loop.c" "$root/build/us_loop.o"
compile "$us/socket.c" "$root/build/us_socket.o"
compile "$us/udp.c" "$root/build/us_udp.o"
compile "$us/eventing/epoll_kqueue.c" "$root/build/us_epoll_kqueue.o"

# The hot functions' own place. A function of runtime/hot.txt (the
# runtime's functions that take a large share of some benchmark program's
# time; the file says how it was chosen) has its section renamed
# .avant_hot.K.NAME. The program's link (compiler/llvm/driver.av,
# llvm_hot_script) gathers these sections, sorted by name, into one output
# section after all other code, on a 4 KiB boundary: each hot function
# then sits after only the hot functions whose names sort before it,
# whatever else the runtime or the program holds, and the program's own
# code does not move. Before, a hot function sat wherever the objects and
# functions linked before it ended: a collector change moved the string
# parser 0x180 to 0x240 bytes, and a task calling it from its loop ran 20%
# slower at equal instructions (its loop and the parser met in the same
# op-cache sets, which repeat every 2 KiB). K is 9 for the collector's
# (avant_rt.o) and 5 for the others, so the code that changes most often
# comes last and moves nothing else. A link without the script (one by
# mold, as the compiler's own build is, or the seed's link of the
# compiler) keeps each name as an output section of its own, where that
# linker puts an unknown code section (mold: before .text, by name), on no
# page boundary.
place_hot() {
  while read -r o f; do
    case "$o" in ''|'#'*) continue ;; esac
    [ -f "$root/build/$o" ] || { echo "runtime/build.sh: runtime/hot.txt names $o, which is not built" >&2; exit 1; }
    k=5
    [ "$o" = avant_rt.o ] && k=9
    # The function's section, or its clones' (gcc names a specialized
    # copy NAME.constprop.0, NAME.isra.0, ...).
    secs=$(readelf -SW "$root/build/$o" | awk -v f="$f" '{ for (i = 1; i <= NF; i++) if ($i == ".text." f || index($i, ".text." f ".") == 1) print $i }')
    [ -n "$secs" ] || { echo "runtime/build.sh: runtime/hot.txt names $f, which $o has no section for" >&2; exit 1; }
    for sec in $secs; do
      objcopy --rename-section "$sec=.avant_hot.$k.${sec#.text.}" "$root/build/$o"
    done
  done < "$root/hot.txt"
  # The other functions' sections go back into one .text per object (and
  # one .text.unlikely, ...), in the order they had: a program's link then
  # reads a few sections per object, not thousands (it cost every link a
  # third more instructions), and lays the code out as before but for the
  # padding between functions.
  printf 'SECTIONS\n{\n  .text.unlikely : { *(.text.unlikely .text.unlikely.*) }\n  .text.exit : { *(.text.exit .text.exit.*) }\n  .text.startup : { *(.text.startup .text.startup.*) }\n  .text.hot : { *(.text.hot .text.hot.*) }\n  .text : { *(.text .text.*) }\n}\n' > "$root/build/text_merge.ld"
  for o in "$root"/build/*.o; do
    # avant_llvm.o is the compiler's alone; libbase64.o's functions keep
    # their library's 16-byte alignment, so one .text would place them
    # otherwise (and it holds a few dozen).
    case "$o" in */avant_llvm.o|*/libbase64.o) continue ;; esac
    ld -r -T "$root/build/text_merge.ld" -o "$o.merged" "$o" || exit 1
    mv "$o.merged" "$o"
  done
  rm -f "$root/build/text_merge.ld"
}
place_hot

# Every avant_* symbol an object calls is defined globally by one of them:
# a hidden function of a core unit that another object calls is made local
# by the merge above unless avant_rt.exports lists it, and the surface
# check cannot see that (the exports file is its reference); the link of
# the first program needing that object would fail instead, far from the
# cause. link-check.sh names the symbol, the object and the reason.
sh "$root/link-check.sh" "$root/build" "$root/avant_rt.exports"

# The bind helper: tools/bind.c against libclang, into
# runtime/build/avant-bind. `avant bind` runs it and builds nothing itself;
# the release ships it beside avant. Without llvm-config or libclang it is
# skipped with a notice, and `avant bind` says how to build it.
# BIND_LLVM_CONFIG names the llvm-config whose libclang it links (default
# llvm-config-20: the helper stays on the distribution's libclang 20, the
# one `avant bind` asks its users to install, while the compiler links
# LLVM's official 22.1.8, whose libclang finds no resource headers when
# loaded by avant-bind, so `avant bind` of a header that includes
# <stdbool.h> would fail with it).
llvm_config=${BIND_LLVM_CONFIG:-llvm-config-20}
bind_src="$root/../tools/bind.c"
bind_out="$root/build/avant-bind"
if command -v "$llvm_config" > /dev/null 2>&1 && [ -f "$bind_src" ]; then
  llvm_cflags=$("$llvm_config" --cflags)
  llvm_libdir=$("$llvm_config" --libdir)
  # shellcheck disable=SC2086
  if "$cc" -O2 -std=c11 $llvm_cflags "$bind_src" -o "$bind_out.part" -L"$llvm_libdir" -lclang -Wl,-rpath,"$llvm_libdir"; then
    mv "$bind_out.part" "$bind_out"
  else
    rm -f "$bind_out.part" "$bind_out"
    echo "runtime/build.sh: skipped avant-bind: libclang did not link (avant bind will not work)" >&2
  fi
else
  rm -f "$bind_out"
  echo "runtime/build.sh: skipped avant-bind: $llvm_config not found (avant bind needs llvm-config and libclang)" >&2
fi

# The compiler's in-process LLVM: tools/build-llvm.sh writes
# runtime/build/avant_llvm.o, which only the compiler links (tools/
# letters.sh). It rebuilds only when LLVM or the glue changed, so a cached
# run costs a few sha256 sums. Without its llvm-config it is skipped with a
# notice, like avant-bind; building the compiler then stops and says why.
if command -v "${LLVM_CONFIG:-$llvm_default}" > /dev/null 2>&1; then
  sh "$root/../tools/build-llvm.sh"
else
  echo "runtime/build.sh: skipped avant_llvm.o: ${LLVM_CONFIG:-$llvm_default} not found (the compiler needs LLVM 22: sh tools/install-llvm.sh; see tools/build-llvm.sh)" >&2
fi
