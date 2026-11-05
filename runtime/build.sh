#!/bin/sh
# Compile the runtime objects into runtime/build/.
# The compiler links these objects; no Crystal is involved.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cc=${CC:-cc}
mkdir -p "$root/build"
cflags="-O2 -std=c11 -pthread -fno-omit-frame-pointer -I$root -I$root/third_party/yyjson -I$root/third_party/base64/include -I$root/third_party/uSockets/src -I$root/third_party/picohttpparser -DLIBUS_NO_SSL"

compile() {
  "$cc" -c $cflags -o "$2" "$1"
}

for name in \
  avant_rt.c avant_rt_traps.c avant_gc_stackmap.c avant_str.c avant_buf.c avant_hash.c avant_array.c avant_task.c \
  avant_json.c avant_http.c avant_wide.c avant_codec.c avant_zlib.c avant_base64.c avant_re.c avant_io.c avant_cov.c
do
  compile "$root/$name" "$root/build/${name%.c}.o"
done

compile "$root/third_party/yyjson/yyjson.c" "$root/build/yyjson.o"
compile "$root/third_party/picohttpparser/picohttpparser.c" "$root/build/picohttpparser.o"

# picohttpparser folds into avant_http.o: the seed's link table names
# avant_http.o only, so a new object could not be linked by the old
# seed. The compiler's std.http row names avant_http.o for the pair.
ld -r -o "$root/build/avant_http_merged.o" "$root/build/avant_http.o" "$root/build/picohttpparser.o"
mv "$root/build/avant_http_merged.o" "$root/build/avant_http.o"

# avant_rt_traps.o folds into avant_rt.o: the seed's link table names
# avant_rt.o only, so a new core object could not be linked by the old
# seed. The source split is organizational; the link surface is not.
ld -r -o "$root/build/avant_rt_merged.o" "$root/build/avant_rt.o" "$root/build/avant_rt_traps.o" "$root/build/avant_gc_stackmap.o"
mv "$root/build/avant_rt_merged.o" "$root/build/avant_rt.o"

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
( cd "$b64" && env $simd make lib/libbase64.o )
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

# The bind helper (D135, D141): tools/bind.c against libclang, into
# runtime/build/avant-bind. `avant bind` runs it and builds nothing itself;
# the release ships it beside avant. Without llvm-config or libclang it is
# skipped with a notice, and `avant bind` says how to build it.
llvm_config=${LLVM_CONFIG:-llvm-config}
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
