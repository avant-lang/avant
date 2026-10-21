#!/bin/sh
# Compile runtime/*.o the way src/avant/runtime.cr does.
# The seed path uses this so Crystal is not required to produce the objects.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cc=${CC:-cc}
mkdir -p "$root/build"
cflags="-O2 -std=c11 -pthread -fno-omit-frame-pointer -I$root -I$root/third_party/yyjson -I$root/third_party/base64/include -I$root/third_party/uSockets/src -DLIBUS_NO_SSL"

compile() {
  "$cc" -c $cflags -o "$2" "$1"
}

for name in \
  avant_rt.c avant_str.c avant_buf.c avant_hash.c avant_array.c avant_task.c \
  avant_json.c avant_http.c avant_wide.c avant_codec.c avant_re.c avant_io.c avant_cov.c
do
  compile "$root/$name" "$root/build/${name%.c}.o"
done

compile "$root/third_party/yyjson/yyjson.c" "$root/build/yyjson.o"

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
# Fold the library into avant_codec.o so the link list stays the one
# the compiler already passes. The SIMD codecs need their own -m flags,
# so they cannot be one translation unit with avant_codec.c.
ld -r -o "$root/build/avant_codec.merge.o" "$root/build/avant_codec.o" "$root/build/libbase64.o"
mv "$root/build/avant_codec.merge.o" "$root/build/avant_codec.o"

us="$root/third_party/uSockets/src"
compile "$us/bsd.c" "$root/build/us_bsd.o"
compile "$us/context.c" "$root/build/us_context.o"
compile "$us/loop.c" "$root/build/us_loop.o"
compile "$us/socket.c" "$root/build/us_socket.o"
compile "$us/udp.c" "$root/build/us_udp.o"
compile "$us/eventing/epoll_kqueue.c" "$root/build/us_epoll_kqueue.o"
