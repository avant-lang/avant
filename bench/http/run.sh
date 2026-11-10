#!/bin/sh
# Five-language hello-world harness for std.http (Wave 5B step 1).
# Method (same for every language): HTTP via `wrk -t4 -c64 -d10s`
# with latency percentiles; WebSocket echo via ws_load.py (8
# connections, 10 s, text "hi" round-trips); RSS is the server peak
# VmHWM read from /proc/<pid>/status after the phase (idle RSS is
# read before it). Avant also reports allocation bytes per request
# from AVANT_GC_STATS. One server runs at a time on port 18301
# (HTTP) / 18311 (WS). Nothing else may run during a phase: no
# builds, no suites. Usage: run.sh [avant|h2o|rust|go|crystal|all]
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AVANT_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD=${HTTP_BENCH_BUILD:-/tmp/http-ws-bench-build}
# h2o (the C row) builds from the workbench clone at refs/h2o;
# libh2o-evloop and its generated flag file live in H2O_BUILD, and
# wslay (h2o's WebSocket dependency) comes from pkg-config when the
# system has it, else from a /tmp/wslay extraction (WSLAY_ROOT).
H2O_SRC=${H2O_SRC:-$AVANT_ROOT/../refs/h2o}
H2O_BUILD=${H2O_BUILD:-/tmp/h2o-build}
WSLAY_ROOT=${WSLAY_ROOT:-/tmp/wslay/root/usr}
mkdir -p "$BUILD"
COMPILER=${AVANT_BENCH_COMPILER:-$AVANT_ROOT/bin/avant-b}
export AVANT_MYC_LLVM=${AVANT_MYC_LLVM:-$AVANT_ROOT/bin/v0.31.0/myc-llvm}
HPORT=18301
WPORT=18311
ONLY=${1:-all}

say() { printf '%s\n' "$*"; }

fresh() {
  [ -x "$1" ] && [ "$1" -nt "$2" ]
}

# Builds libh2o-evloop once (cmake, mruby off so no Ruby runtime is
# linked), then the two harness servers against it. The include set
# is read from the library build's own flags file so the vendored
# dependency headers always match the library that was built.
build_h2o() {
  if [ ! -f "$H2O_BUILD/libh2o-evloop.a" ]; then
    if ! pkg-config --exists libwslay 2>/dev/null && [ -f /tmp/wslay/pc/libwslay.pc ]; then
      PKG_CONFIG_PATH=/tmp/wslay/pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
      export PKG_CONFIG_PATH
    fi
    cmake -S "$H2O_SRC" -B "$H2O_BUILD" -DCMAKE_BUILD_TYPE=Release -DWITH_MRUBY=OFF >/dev/null
    cmake --build "$H2O_BUILD" --target libh2o-evloop -j "$(nproc)" >/dev/null
  fi
  h2o_inc=$(sed -n 's/^C_INCLUDES = //p' "$H2O_BUILD/CMakeFiles/libh2o-evloop.dir/flags.make")
  if pkg-config --exists libwslay 2>/dev/null; then
    wslay_link="-lwslay"
  else
    wslay_link="-L$WSLAY_ROOT/lib/x86_64-linux-gnu -Wl,-rpath,$WSLAY_ROOT/lib/x86_64-linux-gnu -lwslay"
  fi
  # h2o_inc and wslay_link are flag lists: word splitting is intended.
  # shellcheck disable=SC2086
  fresh "$BUILD/h2o_server" "$HERE/h2o_server.c" || cc -O2 -DH2O_USE_LIBUV=0 -D_GNU_SOURCE -pthread $h2o_inc -o "$BUILD/h2o_server" "$HERE/h2o_server.c" "$H2O_BUILD/libh2o-evloop.a" $wslay_link -lssl -lcrypto -lz -lzstd -lcap -lpthread -lm -ldl
  # shellcheck disable=SC2086
  fresh "$BUILD/h2o_ws" "$HERE/h2o_ws.c" || cc -O2 -DH2O_USE_LIBUV=0 -D_GNU_SOURCE -pthread $h2o_inc -o "$BUILD/h2o_ws" "$HERE/h2o_ws.c" "$H2O_BUILD/libh2o-evloop.a" $wslay_link -lssl -lcrypto -lz -lzstd -lcap -lpthread -lm -ldl
}

build_all() {
  say "building servers into $BUILD"
  if ! fresh "$BUILD/avant_server" "$HERE/avant_server.av" || ! fresh "$BUILD/avant_server" "$AVANT_ROOT/runtime/build/avant_http.o" || ! fresh "$BUILD/avant_server" "$AVANT_ROOT/runtime/build/avant_task.o" || ! fresh "$BUILD/avant_server" "$AVANT_ROOT/runtime/build/avant_io.o"; then
    (cd "$AVANT_ROOT" && "$COMPILER" compile bench/http/avant_server.av "$BUILD/avant_server" --release >/dev/null)
  fi
  if ! fresh "$BUILD/avant_ws" "$HERE/avant_ws.av" || ! fresh "$BUILD/avant_ws" "$AVANT_ROOT/runtime/build/avant_http.o" || ! fresh "$BUILD/avant_ws" "$AVANT_ROOT/runtime/build/avant_task.o" || ! fresh "$BUILD/avant_ws" "$AVANT_ROOT/runtime/build/avant_io.o"; then
    (cd "$AVANT_ROOT" && "$COMPILER" compile bench/http/avant_ws.av "$BUILD/avant_ws" --release >/dev/null)
  fi
  build_h2o
  fresh "$BUILD/rust_server" "$HERE/rust_server.rs" || rustc -O -o "$BUILD/rust_server" "$HERE/rust_server.rs"
  fresh "$BUILD/rust_ws" "$HERE/rust_ws.rs" || rustc -O -o "$BUILD/rust_ws" "$HERE/rust_ws.rs"
  fresh "$BUILD/go_server" "$HERE/go_server.go" || (cd "$HERE" && go build -o "$BUILD/go_server" go_server.go)
  fresh "$BUILD/go_ws" "$HERE/go_ws.go" || (cd "$HERE" && go build -o "$BUILD/go_ws" go_ws.go)
  fresh "$BUILD/ws_load" "$HERE/ws_load.go" || (cd "$HERE" && go build -o "$BUILD/ws_load" ws_load.go)
  # Crystal is single-threaded unless built with -Dpreview_mt (one
  # scheduler thread per core, fibers run truly in parallel); the
  # run.sh freshness check makes a flag change rebuild the binaries.
  if ! fresh "$BUILD/crystal_server" "$HERE/crystal_server.cr" || ! fresh "$BUILD/crystal_server" "$HERE/run.sh"; then
    (cd "$HERE" && crystal build --release -Dpreview_mt -o "$BUILD/crystal_server" crystal_server.cr)
  fi
  if ! fresh "$BUILD/crystal_ws" "$HERE/crystal_ws.cr" || ! fresh "$BUILD/crystal_ws" "$HERE/run.sh"; then
    (cd "$HERE" && crystal build --release -Dpreview_mt -o "$BUILD/crystal_ws" crystal_ws.cr)
  fi
}

wait_port() {
  i=0
  while [ $i -lt 100 ]; do
    if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(s.connect_ex(('127.0.0.1', $1)))" 2>/dev/null; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.1
  done
  say "server on port $1 did not come up"
  return 1
}

vmhwm_kb() {
  awk '/VmHWM/ {print $2}' "/proc/$1/status" 2>/dev/null || echo 0
}

# start_server CMD... -> echoes the server pid (through a pid file:
# setsid may fork, so $! is not reliable); log at $BUILD/server.log
start_server() {
  : >"$BUILD/server.pid"
  setsid sh -c 'echo $$ > "$1"; shift; exec "$@"' _ "$BUILD/server.pid" "$@" </dev/null >>"$BUILD/server.log" 2>&1 &
  i=0
  while [ ! -s "$BUILD/server.pid" ] && [ $i -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
  done
  cat "$BUILD/server.pid"
}

stop_server() {
  kill "$1" 2>/dev/null || true
  i=0
  while [ $i -lt 50 ]; do
    if ! kill -0 "$1" 2>/dev/null; then return 0; fi
    i=$((i + 1))
    sleep 0.1
  done
  kill -9 "$1" 2>/dev/null || true
}

http_phase() {
  name=$1; shift
  : >"$BUILD/server.log"
  if [ "$name" = "avant" ]; then
    pid=$(start_server env AVANT_GC_STATS=1 "$@")
  else
    pid=$(start_server "$@")
  fi
  wait_port $HPORT
  sleep 0.5
  idle_kb=$(vmhwm_kb "$pid")
  wrk_out=$(wrk -t4 -c64 -d10s --latency "http://127.0.0.1:$HPORT/" 2>&1)
  peak_kb=$(vmhwm_kb "$pid")
  reqs=$(printf '%s\n' "$wrk_out" | awk '/requests in/ {print $1}')
  rps=$(printf '%s\n' "$wrk_out" | awk '/Requests\/sec/ {print $2}')
  p50=$(printf '%s\n' "$wrk_out" | awk '/^[[:space:]]*50%/ {print $2}' | head -1)
  p99=$(printf '%s\n' "$wrk_out" | awk '/^[[:space:]]*99%/ {print $2}' | head -1)
  alloc_per=""
  if [ "$name" = "avant" ]; then
    curl -s "http://127.0.0.1:$HPORT/quit" >/dev/null 2>&1 || true
    sleep 1
    alloc_bytes=$(awk '{for (i = 1; i <= NF; i++) if ($i ~ /^alloc=/) {split($i, a, "="); print a[2]}}' "$BUILD/server.log" | tail -1)
    if [ -n "${alloc_bytes:-}" ] && [ -n "${reqs:-}" ] && [ "$reqs" != "0" ]; then
      alloc_per=$(awk -v b="$alloc_bytes" -v r="$reqs" 'BEGIN {printf "%.0f", b / r}')
    fi
  fi
  stop_server "$pid"
  idle_mb=$(awk -v k="$idle_kb" 'BEGIN {printf "%.1f", k / 1024}')
  peak_mb=$(awk -v k="$peak_kb" 'BEGIN {printf "%.1f", k / 1024}')
  say "HTTP $name req/s=$rps p50=${p50:-?} p99=${p99:-?} idleRSS=${idle_mb}MB peakRSS=${peak_mb}MB allocPerReq=${alloc_per:-n/a}"
}

ws_phase() {
  name=$1; shift
  : >"$BUILD/server.log"
  pid=$(start_server "$@")
  wait_port $WPORT
  sleep 0.5
  total=$("$BUILD/ws_load" $WPORT 8 10 2>/dev/null)
  peak_kb=$(vmhwm_kb "$pid")
  stop_server "$pid"
  peak_mb=$(awk -v k="$peak_kb" 'BEGIN {printf "%.1f", k / 1024}')
  msgps=$(awk -v t="$total" 'BEGIN {printf "%.0f", t / 10}')
  say "WS $name msg/s=$msgps peakRSS=${peak_mb}MB"
}

want() {
  [ "$ONLY" = "all" ] || [ "$ONLY" = "$1" ]
}

build_all
if want avant; then http_phase avant "$BUILD/avant_server"; ws_phase avant "$BUILD/avant_ws"; fi
if want h2o; then http_phase h2o "$BUILD/h2o_server" $HPORT; ws_phase h2o "$BUILD/h2o_ws" $WPORT; fi
if want rust; then http_phase rust "$BUILD/rust_server" $HPORT; ws_phase rust "$BUILD/rust_ws" $WPORT; fi
if want go; then http_phase go "$BUILD/go_server" $HPORT; ws_phase go "$BUILD/go_ws" $WPORT; fi
if want crystal; then http_phase crystal "$BUILD/crystal_server" $HPORT; ws_phase crystal "$BUILD/crystal_ws" $WPORT; fi
say "done"
