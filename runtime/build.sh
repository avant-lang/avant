#!/bin/sh
# Compile runtime/*.o the way src/avant/runtime.cr does.
# The seed path uses this so Crystal is not required to produce the objects.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cc=${CC:-cc}
mkdir -p "$root/build"
cflags="-O2 -std=c11 -pthread -fno-omit-frame-pointer -I$root -I$root/third_party/yyjson -I$root/third_party/uSockets/src -DLIBUS_NO_SSL"

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

us="$root/third_party/uSockets/src"
compile "$us/bsd.c" "$root/build/us_bsd.o"
compile "$us/context.c" "$root/build/us_context.o"
compile "$us/loop.c" "$root/build/us_loop.o"
compile "$us/socket.c" "$root/build/us_socket.o"
compile "$us/udp.c" "$root/build/us_udp.o"
compile "$us/eventing/epoll_kqueue.c" "$root/build/us_epoll_kqueue.o"
