#!/bin/sh
# Hello-world harness for std.http: one server per row, one server at a
# time on port 18301 (HTTP) / 18311 (WebSocket). Nothing else may run
# during a timed phase: no builds, no suites.
#
# Two modes.
#
# 1. Without METHOD (the mode the README's HTTP/WebSocket table has
#    always been made with):
#      run.sh [avant|h2o|rust|go|crystal|hyper|fasthttp|valk|all]
#    HTTP via `wrk -t4 -c64 -d10s --latency`, one run; WebSocket echo
#    via ws_load.go (8 connections, 10 s, text "hi" round-trips) for
#    the five rows that have a WebSocket server (`all` is those five);
#    RSS is the server's peak VmHWM read from /proc/<pid>/status after
#    the phase (idle RSS before it). Avant also reports allocation
#    bytes per request from AVANT_GC_STATS.
#
# 2. With METHOD=A|A8|B|C|Q9:
#      METHOD=B run.sh [row ...|all]      (all: every row that built)
#    HTTP only, HTTP_BENCH_RUNS (3) rounds; in each round every row
#    gets a fresh server process, one unmeasured warm-up load
#    (HTTP_BENCH_WARMUP seconds, 2, the same wrk line) and one measured
#    load; the row order rotates by one each round. The methods:
#      A   wrk -t4 -c64 -d10s --latency URL
#          servers at their default thread counts, nothing pinned
#          (the first mode's load, kept for continuity)
#      A8  wrk -t8 -c256 -d10s --latency URL
#          as A with a stronger load generator (is wrk the limit in A?)
#      B   wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL -s pipe.lua -- 16
#          900 keep-alive connections, 16 pipelined requests per
#          write, 8 load threads; servers at their default thread
#          counts, nothing pinned
#      C   the same wrk line under `taskset -c 4-7,12-15`; the server
#          limited to 4 threads and under `taskset -c 0-3` (B with
#          the server on 4 pinned threads: four physical cores for the
#          server, the other four with both siblings for wrk, the
#          server cores' siblings 8-11 idle; the CPU lists are for a
#          machine whose siblings are k and k+8:
#          HTTP_BENCH_SERVER_CPUS, HTTP_BENCH_WRK_CPUS)
#      Q9  wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL
#          B without pipelining (depth 1 at 900 connections)
#    HTTP_BENCH_DURATION overrides the seconds of the measured load;
#    HTTP_BENCH_THREADS forces a server thread count under any method
#    (C's default is 4). Thread control per row: avant `--threads N`,
#    h2o and hyper and valk an argument, go and fasthttp GOMAXPROCS,
#    crystal CRYSTAL_WORKERS; the Rust standard-library row has none
#    (a thread per connection) and is bounded by the pinning alone.
#    Three labelled variants run only when named, never under `all`:
#    avant-nosigpipe (the same Avant binary started by
#    `env --ignore-signal=PIPE`: up to v0.41.0 the server was killed by
#    SIGPIPE when a client closed with pipelined answers unread, which
#    is what wrk does at the end of a B or C load; the row stays for
#    measuring a binary built before that was fixed), and hyper-nodate and
#    fasthttp-nodate (those servers with their `Date` header, and
#    fasthttp's `Server` header, switched off: an 84-byte answer like
#    the servers that send neither).
#    Before each load one request is checked (status 200, body
#    `hello`). Kept per run under HTTP_BENCH_OUT/<METHOD>/ (default
#    $BUILD/out): r<round>.<row>.wrk (wrk's output, measured),
#    .warm (wrk's output, warm-up), .time (/usr/bin/time: wrk's own
#    elapsed, user and system seconds), .server.log, and .meta
#    (key=value: the command lines, threads, pinning, the server's
#    CPU seconds during the measured load, idle and peak RSS, the
#    check, Avant's AVANT_GC_STATS line). tables.py turns that
#    directory into the tables; a run whose wrk output reports socket
#    errors, timeouts or non-2xx answers is not a result and
#    tables.py says so.
#
# `run.sh build` only builds. `run.sh check [row ...|all]` is the
# untimed correctness pass: each row's server is started, check.py
# runs against it (the exact response bytes, 16 pipelined requests in
# one write and cut in two at several bytes, 3,200 pipelined requests
# on one connection, 1,000 keep-alive requests, a client that closes
# with 16 answers unread), then the server is
# stopped by its own mechanism (`GET /quit`; SIGTERM for h2o and valk,
# which have no such route) and the port must be free; results in
# HTTP_BENCH_OUT/check/<row>.json and <row>.stop.
# `METHOD=B run.sh syscalls [row ...|all]` counts system calls, also
# untimed: each row's server is started under `strace -f -c`, the
# method's wrk line runs once (HTTP_BENCH_DURATION seconds, 3), the
# server is stopped, and strace's per-call counts are kept beside
# wrk's output in HTTP_BENCH_OUT/syscalls/<METHOD>.<row>.strace*;
# tables.py divides them by the requests wrk completed. strace slows
# the server many times over, so each wake-up finds more work than it
# does at full speed: the calls a server makes per request or per
# batch (receive, send) are what this shows; the share of the waiting
# calls (epoll_wait, io_uring_enter, futex) is a lower bound.
#
# Credit: pipe.lua and the hyper, fasthttp and valk programs are
# adapted from valk-lang/valk examples/bench/http (MIT, (c) 2024
# Lorenz Vandevelde), each file stating what it changed; the pipelined
# methods (B and C) follow that project's published benchmark.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AVANT_ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
BUILD=${HTTP_BENCH_BUILD:-/tmp/http-ws-bench-build}
# h2o (the C row) builds from the workbench clone at refs/h2o;
# libh2o-evloop and its generated flag file live in H2O_BUILD, and
# wslay (h2o's WebSocket dependency) comes from pkg-config when the
# system has it, else from an extraction of the Ubuntu packages
# (WSLAY_ROOT, with a libwslay.pc pointing at it in WSLAY_PC).
H2O_SRC=${H2O_SRC:-$AVANT_ROOT/../refs/h2o}
H2O_BUILD=${H2O_BUILD:-/tmp/h2o-build}
WSLAY_ROOT=${WSLAY_ROOT:-/tmp/wslay/root/usr}
WSLAY_PC=${WSLAY_PC:-/tmp/wslay/pc}
mkdir -p "$BUILD"
COMPILER=${AVANT_BENCH_COMPILER:-$AVANT_ROOT/bin/avant-b}
HPORT=18301
WPORT=18311
METHOD=${METHOD:-}
URL="http://127.0.0.1:$HPORT/"

say() { printf '%s\n' "$*"; }

fresh() {
  [ -x "$1" ] && [ "$1" -nt "$2" ]
}

# Builds libh2o-evloop once (cmake, mruby off so no Ruby runtime is
# linked), then the two harness servers against it. The include set
# is read from the library build's own flags file so the vendored
# dependency headers always match the library that was built.
build_h2o() {
  wslay_system=0
  if pkg-config --exists libwslay 2>/dev/null; then
    wslay_system=1
  elif [ -f "$WSLAY_PC/libwslay.pc" ]; then
    PKG_CONFIG_PATH=$WSLAY_PC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
    export PKG_CONFIG_PATH
  fi
  if [ ! -f "$H2O_BUILD/libh2o-evloop.a" ]; then
    cmake -S "$H2O_SRC" -B "$H2O_BUILD" -DCMAKE_BUILD_TYPE=Release -DWITH_MRUBY=OFF >/dev/null
    cmake --build "$H2O_BUILD" --target libh2o-evloop -j "$(nproc)" >/dev/null
  fi
  h2o_inc=$(sed -n 's/^C_INCLUDES = //p' "$H2O_BUILD/CMakeFiles/libh2o-evloop.dir/flags.make")
  if [ "$wslay_system" = 1 ]; then
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
  # Rust hyper and Go fasthttp: the versions are the committed
  # Cargo.lock's and go.sum's (the first build downloads them).
  cargo build --release --locked --quiet --manifest-path "$HERE/rust_hyper/Cargo.toml" --target-dir "$BUILD/rust_hyper"
  if ! fresh "$BUILD/go_fasthttp" "$HERE/go_fasthttp/main.go" || ! fresh "$BUILD/go_fasthttp" "$HERE/go_fasthttp/go.sum"; then
    (cd "$HERE/go_fasthttp" && go build -o "$BUILD/go_fasthttp" .)
  fi
  # The valk row needs the Valk toolchain; without one it is left out.
  if command -v valk >/dev/null 2>&1; then
    fresh "$BUILD/valk_server" "$HERE/valk_server.valk" || valk build "$HERE/valk_server.valk" --release -o "$BUILD/valk_server" >/dev/null
  else
    say "valk is not installed: the valk row is left out"
  fi
}

port_open() {
  python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(s.connect_ex(('127.0.0.1', $1)))" 2>/dev/null
}

wait_port() {
  i=0
  while [ $i -lt 100 ]; do
    if port_open "$1"; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.1
  done
  say "server on port $1 did not come up"
  return 1
}

# wait_port_free PORT: the port stops accepting within 10 s.
wait_port_free() {
  i=0
  while [ $i -lt 100 ]; do
    if ! port_open "$1"; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.1
  done
  say "port $1 is still in use"
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
  wrk_out=$(wrk -t4 -c64 -d10s --latency "$URL" 2>&1)
  peak_kb=$(vmhwm_kb "$pid")
  reqs=$(printf '%s\n' "$wrk_out" | awk '/requests in/ {print $1}')
  rps=$(printf '%s\n' "$wrk_out" | awk '/Requests\/sec/ {print $2}')
  p50=$(printf '%s\n' "$wrk_out" | awk '/^[[:space:]]*50%/ {print $2}' | head -1)
  p99=$(printf '%s\n' "$wrk_out" | awk '/^[[:space:]]*99%/ {print $2}' | head -1)
  alloc_per=""
  if [ "$name" = "avant" ]; then
    curl -s "${URL}quit" >/dev/null 2>&1 || true
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

# ---- the method mode -------------------------------------------------

# server_cmd ROW THREADS: prints the row's command line, one word per
# line (THREADS empty: the server's default thread count).
server_cmd() {
  case "$1" in
    avant)
      printf '%s\n' env AVANT_GC_STATS=1 "$BUILD/avant_server"
      if [ -n "$2" ]; then printf '%s\n' --threads "$2"; fi ;;
    avant-nosigpipe)
      # The same binary, started with SIGPIPE ignored (an ignored
      # signal stays ignored across exec): see the header.
      printf '%s\n' env --ignore-signal=PIPE AVANT_GC_STATS=1 "$BUILD/avant_server"
      if [ -n "$2" ]; then printf '%s\n' --threads "$2"; fi ;;
    h2o)
      printf '%s\n' "$BUILD/h2o_server" "$HPORT"
      if [ -n "$2" ]; then printf '%s\n' "$2"; fi ;;
    rust)
      printf '%s\n' "$BUILD/rust_server" "$HPORT" ;;
    hyper)
      printf '%s\n' "$BUILD/rust_hyper/release/rust_hyper" "$HPORT"
      if [ -n "$2" ]; then printf '%s\n' "$2"; fi ;;
    hyper-nodate)
      printf '%s\n' "$BUILD/rust_hyper/release/rust_hyper" "$HPORT" "${2:-0}" nodate ;;
    go)
      if [ -n "$2" ]; then printf '%s\n' env "GOMAXPROCS=$2"; fi
      printf '%s\n' "$BUILD/go_server" "$HPORT" ;;
    fasthttp)
      if [ -n "$2" ]; then printf '%s\n' env "GOMAXPROCS=$2"; fi
      printf '%s\n' "$BUILD/go_fasthttp" "$HPORT" ;;
    fasthttp-nodate)
      if [ -n "$2" ]; then printf '%s\n' env "GOMAXPROCS=$2"; fi
      printf '%s\n' "$BUILD/go_fasthttp" "$HPORT" nodate ;;
    crystal)
      if [ -n "$2" ]; then printf '%s\n' env "CRYSTAL_WORKERS=$2"; fi
      printf '%s\n' "$BUILD/crystal_server" "$HPORT" ;;
    valk)
      printf '%s\n' "$BUILD/valk_server" "$HPORT"
      if [ -n "$2" ]; then printf '%s\n' "$2"; fi ;;
    *)
      say "unknown row: $1" >&2
      return 1 ;;
  esac
}

# wrk_line SECONDS: the method's wrk arguments, exactly as the header
# of this file states them.
wrk_line() {
  case "$METHOD" in
    A) printf '%s' "-t4 -c64 -d${1}s --latency $URL" ;;
    A8) printf '%s' "-t8 -c256 -d${1}s --latency $URL" ;;
    B | C) printf '%s' "--latency -d $1 -c 900 -t 8 --timeout 8 $URL -s $HERE/pipe.lua -- 16" ;;
    Q9) printf '%s' "--latency -d $1 -c 900 -t 8 --timeout 8 $URL" ;;
  esac
}

# cpu_ticks PID: "user system" clock ticks of the whole process.
cpu_ticks() {
  sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $12, $13}'
}

# thread_ticks PID: user + system ticks of each thread since the
# process started, largest first (how evenly the load fell on them).
thread_ticks() {
  for t in /proc/"$1"/task/*/stat; do
    sed 's/.*) //' "$t" 2>/dev/null | awk '{print $12 + $13}'
  done | sort -rn | head -64 | tr '\n' ',' | sed 's/,$//'
}

# method_cell ROUND ROW: one fresh server, the check, the warm-up load
# and the measured load; everything kept under $MOUT.
method_cell() {
  round=$1
  row=$2
  cell="$MOUT/r$round.$row"
  rm -f "$cell.wrk" "$cell.warm" "$cell.time" "$cell.meta" "$cell.server.log" "$cell.body"
  idle_kb=0; peak_kb=0; nthreads=0; t0="0 0"; t1="0 0"; tticks=; alive=n/a
  if port_open $HPORT; then
    say "$METHOD r$round $row: port $HPORT is in use, nothing started"
    printf 'method=%s\nrow=%s\nround=%s\nstatus=port-in-use\n' "$METHOD" "$row" "$round" >"$cell.meta"
    return 0
  fi
  # shellcheck disable=SC2046 # one word per line, no spaces in paths
  set -- $(server_cmd "$row" "$THREADS")
  if [ -n "$SERVER_CPUS" ]; then
    set -- taskset -c "$SERVER_CPUS" "$@"
  fi
  server_line=$*
  : >"$BUILD/server.log"
  pid=$(start_server "$@")
  status=ok
  if ! wait_port $HPORT >/dev/null; then
    status=no-listen
  fi
  check=skipped
  if [ "$status" = ok ]; then
    sleep 0.5
    idle_kb=$(vmhwm_kb "$pid")
    code=$(curl -s --max-time 5 -o "$cell.body" -w '%{http_code}' "$URL" 2>/dev/null || true)
    if [ "$code" = 200 ] && [ "$(cat "$cell.body" 2>/dev/null)" = hello ]; then
      check=ok
    else
      check="bad:$code"
      status=bad-answer
    fi
  fi
  if [ "$status" = ok ]; then
    # WRK_PIN and the wrk lines are word lists (no spaces in the paths).
    # shellcheck disable=SC2046,SC2086
    timeout -k 10 $((WARM + 30)) $WRK_PIN wrk $(wrk_line "$WARM") </dev/null >"$cell.warm" 2>&1 || true
    t0=$(cpu_ticks "$pid")
    # shellcheck disable=SC2046,SC2086
    /usr/bin/time -f 'wrk_elapsed=%e\nwrk_user=%U\nwrk_sys=%S' -o "$cell.time" timeout -k 10 $((DUR + 30)) $WRK_PIN wrk $(wrk_line "$DUR") </dev/null >"$cell.wrk" 2>&1 || status=wrk-failed
    t1=$(cpu_ticks "$pid")
    [ -n "$t0" ] || t0="0 0"
    [ -n "$t1" ] || t1="0 0"
    tticks=$(thread_ticks "$pid")
    peak_kb=$(vmhwm_kb "$pid")
    nthreads=$(awk '/^Threads:/ {print $2}' "/proc/$pid/status" 2>/dev/null || echo 0)
    sleep 0.3
    # A server that did not survive the load generator closing its
    # connections is a finding, not a result to hide.
    if kill -0 "$pid" 2>/dev/null; then alive=yes; else alive=no; fi
  fi
  if [ "${row#avant}" != "$row" ] && kill -0 "$pid" 2>/dev/null; then
    # /quit closes the server; the exit prints the AVANT_GC_STATS line.
    curl -s --max-time 5 "${URL}quit" >/dev/null 2>&1 || true
    i=0
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 50 ]; do
      i=$((i + 1))
      sleep 0.1
    done
  fi
  stop_server "$pid"
  wait_port_free $HPORT || status="$status,port-not-freed"
  head -c 1000000 "$BUILD/server.log" >"$cell.server.log"
  {
    printf 'method=%s\nrow=%s\nround=%s\nstatus=%s\ncheck=%s\n' "$METHOD" "$row" "$round" "$status" "$check"
    printf 'date=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'server_cmd=%s\n' "$server_line"
    printf 'server_threads_arg=%s\nserver_cpus=%s\n' "${THREADS:-default}" "${SERVER_CPUS:-all}"
    printf 'wrk_cmd=%s\n' "${WRK_PIN:+$WRK_PIN }wrk $(wrk_line "$DUR")"
    printf 'warmup_s=%s\nduration_s=%s\n' "$WARM" "$DUR"
    printf 'idle_kb=%s\npeak_kb=%s\nserver_os_threads=%s\n' "${idle_kb:-0}" "${peak_kb:-0}" "${nthreads:-0}"
    printf 'alive_after_load=%s\n' "${alive:-n/a}"
    printf 'server_user_ticks=%s\nserver_sys_ticks=%s\n' "$(( ${t1% *} - ${t0% *} ))" "$(( ${t1#* } - ${t0#* } ))"
    printf 'server_cpu_ticks=%s\nclk_tck=%s\n' "$(( ${t1% *} + ${t1#* } - ${t0% *} - ${t0#* } ))" "$(getconf CLK_TCK)"
    printf 'thread_ticks=%s\n' "$tticks"
    cat "$cell.time" 2>/dev/null || true
    if [ "${row#avant}" != "$row" ]; then
      printf 'gc_stats=%s\n' "$(grep -a 'alloc=' "$cell.server.log" | tail -1)"
    fi
  } >"$cell.meta"
  rm -f "$cell.body"
  rps=$(awk '/Requests\/sec/ {print $2}' "$cell.wrk" 2>/dev/null || true)
  errs=$(grep -a -E 'Socket errors|Non-2xx' "$cell.wrk" 2>/dev/null | tr -s ' ' | tr '\n' ';' || true)
  say "$METHOD r$round $row status=$status req/s=${rps:-?} alive_after_load=${alive:-n/a} ${errs:+ERRORS:$errs}"
}

method_mode() {
  THREADS=${HTTP_BENCH_THREADS:-}
  SERVER_CPUS=
  WRK_PIN=
  WARM=${HTTP_BENCH_WARMUP:-2}
  case "$METHOD" in
    A | A8)
      DUR=${HTTP_BENCH_DURATION:-10} ;;
    B | Q9)
      DUR=${HTTP_BENCH_DURATION:-5} ;;
    C)
      DUR=${HTTP_BENCH_DURATION:-5}
      THREADS=${HTTP_BENCH_THREADS:-4}
      SERVER_CPUS=${HTTP_BENCH_SERVER_CPUS:-0-3}
      WRK_PIN="taskset -c ${HTTP_BENCH_WRK_CPUS:-4-7,12-15}" ;;
    *)
      say "unknown METHOD: $METHOD (A, A8, B, C, Q9)"
      exit 2 ;;
  esac
  RUNS=${HTTP_BENCH_RUNS:-3}
  MOUT=${HTTP_BENCH_OUT:-$BUILD/out}/$METHOD
  mkdir -p "$MOUT"
  # shellcheck disable=SC2046 # the row list is split on purpose
  set -- $(expand_rows "$@")
  n=$#
  {
    printf 'method=%s\nrows=%s\nruns=%s\n' "$METHOD" "$*" "$RUNS"
    printf 'date=%s\nkernel=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(uname -r)"
    printf 'cpu=%s\n' "$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)"
    printf 'governor=%s\n' "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    printf 'wrk=%s\n' "$(wrk -v 2>&1 | head -1)"
    printf 'loadavg=%s\n' "$(cat /proc/loadavg)"
  } >"$MOUT/session.meta"
  round=1
  while [ "$round" -le "$RUNS" ]; do
    for row in "$@"; do
      method_cell "$round" "$row"
    done
    # rotate: the first row goes last
    first=$1
    shift
    set -- "$@" "$first"
    round=$((round + 1))
  done
  say "raw files in $MOUT ($n rows, $RUNS rounds); tables: python3 $HERE/tables.py ${HTTP_BENCH_OUT:-$BUILD/out}"
}

# check_mode ROW...: the untimed correctness pass (see the header).
check_mode() {
  COUT=${HTTP_BENCH_OUT:-$BUILD/out}/check
  mkdir -p "$COUT"
  THREADS=${HTTP_BENCH_THREADS:-}
  bad=0
  for row in "$@"; do
    if port_open $HPORT; then
      say "check $row: port $HPORT is in use, nothing started"
      bad=1
      continue
    fi
    # shellcheck disable=SC2046 # one word per line, no spaces in paths
    set -- $(server_cmd "$row" "$THREADS")
    : >"$BUILD/server.log"
    pid=$(start_server "$@")
    if ! wait_port $HPORT; then
      say "check $row: FAIL, the server does not listen (log: $COUT/$row.server.log)"
      stop_server "$pid"
      head -c 1000000 "$BUILD/server.log" >"$COUT/$row.server.log"
      printf 'stop=none listen=FAIL\n' >"$COUT/$row.stop"
      bad=1
      continue
    fi
    python3 "$HERE/check.py" "$row" --port $HPORT --out "$COUT/$row.json" || bad=1
    died=no
    if ! kill -0 "$pid" 2>/dev/null; then
      # The checks killed the server (a finding, already printed); a
      # fresh one is started so that the stop mechanism is still checked.
      died=yes
      wait_port_free $HPORT || true
      pid=$(start_server "$@")
      wait_port $HPORT || true
    fi
    case "$row" in
      h2o | valk)
        stop=sigterm
        bye=n/a
        kill "$pid" 2>/dev/null || true ;;
      *)
        stop=quit
        if [ "$(curl -s --max-time 5 "${URL}quit" 2>/dev/null || true)" = bye ]; then bye=ok; else bye=FAIL; bad=1; fi ;;
    esac
    i=0
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 50 ]; do
      i=$((i + 1))
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then exited=FAIL; bad=1; stop_server "$pid"; else exited=ok; fi
    if wait_port_free $HPORT; then freed=ok; else freed=FAIL; bad=1; fi
    head -c 1000000 "$BUILD/server.log" >"$COUT/$row.server.log"
    printf 'died_during_checks=%s stop=%s bye=%s exited=%s port_free=%s\n' "$died" "$stop" "$bye" "$exited" "$freed" >"$COUT/$row.stop"
    say "$row stop: $(cat "$COUT/$row.stop")"
  done
  return $bad
}

# listener_pid PORT: the pid of the process listening on the port.
listener_pid() {
  ss -ltnpH "sport = :$1" 2>/dev/null | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1
}

# syscalls_mode ROW...: system calls of each server under the method's
# load (see the header). Never beside a timed run.
syscalls_mode() {
  THREADS=${HTTP_BENCH_THREADS:-}
  SERVER_CPUS=
  WRK_PIN=
  case "$METHOD" in
    A | A8 | B | Q9) ;;
    C)
      THREADS=${HTTP_BENCH_THREADS:-4}
      SERVER_CPUS=${HTTP_BENCH_SERVER_CPUS:-0-3}
      WRK_PIN="taskset -c ${HTTP_BENCH_WRK_CPUS:-4-7,12-15}" ;;
    *)
      say "syscalls needs METHOD=A, A8, B, C or Q9"
      exit 2 ;;
  esac
  DUR=${HTTP_BENCH_DURATION:-3}
  TRACER=strace
  SOUT=${HTTP_BENCH_OUT:-$BUILD/out}/syscalls
  mkdir -p "$SOUT"
  for row in "$@"; do
    cell="$SOUT/$METHOD.$row"
    rm -f "$cell.$TRACER" "$cell.$TRACER.wrk" "$cell.$TRACER.meta" "$cell.$TRACER.server.log"
    if port_open $HPORT; then
      say "syscalls $row: port $HPORT is in use, nothing started"
      continue
    fi
    # shellcheck disable=SC2046 # one word per line, no spaces in paths
    set -- $(server_cmd "$row" "$THREADS")
    if [ -n "$SERVER_CPUS" ]; then
      set -- taskset -c "$SERVER_CPUS" "$@"
    fi
    server_line=$*
    : >"$BUILD/server.log"
    tpid=$(start_server strace -f -c -o "$cell.strace" "$@")
    if ! wait_port $HPORT; then
      say "syscalls $row: the server does not listen under $TRACER"
      stop_server "$tpid"
      continue
    fi
    sleep 0.5
    spid=$(listener_pid $HPORT)
    # shellcheck disable=SC2046,SC2086
    timeout -k 10 $((DUR + 60)) $WRK_PIN wrk $(wrk_line "$DUR") </dev/null >"$cell.$TRACER.wrk" 2>&1 || true
    case "$row" in
      h2o | valk) if [ -n "$spid" ]; then kill "$spid" 2>/dev/null || true; fi ;;
      *) curl -s --max-time 30 "${URL}quit" >/dev/null 2>&1 || true ;;
    esac
    i=0
    while kill -0 "$tpid" 2>/dev/null && [ $i -lt 600 ]; do
      i=$((i + 1))
      sleep 0.1
    done
    if kill -0 "$tpid" 2>/dev/null; then
      if [ -n "$spid" ]; then kill "$spid" 2>/dev/null || true; fi
      stop_server "$tpid"
    fi
    wait_port_free $HPORT || true
    head -c 1000000 "$BUILD/server.log" >"$cell.$TRACER.server.log"
    {
      printf 'method=%s\nrow=%s\ntracer=%s\n' "$METHOD" "$row" "$TRACER"
      printf 'date=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      printf 'server_cmd=%s\n' "$server_line"
      printf 'wrk_cmd=%s\n' "${WRK_PIN:+$WRK_PIN }wrk $(wrk_line "$DUR")"
      if [ "${row#avant}" != "$row" ]; then
        printf 'gc_stats=%s\n' "$(grep -a 'alloc=' "$cell.$TRACER.server.log" | tail -1)"
      fi
    } >"$cell.$TRACER.meta"
    say "syscalls $METHOD $row ($TRACER): $(awk '/requests in/ {print $1}' "$cell.$TRACER.wrk") requests; counts in $cell.$TRACER"
  done
}

# expand_rows ROW...: `all` becomes every row that built.
expand_rows() {
  for r in "$@"; do
    if [ "$r" = all ]; then
      r="avant h2o rust hyper go fasthttp crystal"
      if [ -x "$BUILD/valk_server" ]; then r="$r valk"; fi
    fi
    printf '%s ' "$r"
  done
}

if [ "${1:-}" = check ]; then
  shift
  build_all
  if [ $# -eq 0 ]; then set -- all; fi
  # shellcheck disable=SC2046 # the row list is split on purpose
  if check_mode $(expand_rows "$@"); then say "check: every row passed"; else say "check: FAILURES above"; exit 1; fi
  exit 0
fi

if [ "${1:-}" = syscalls ]; then
  shift
  build_all
  if [ $# -eq 0 ]; then set -- all; fi
  # shellcheck disable=SC2046 # the row list is split on purpose
  syscalls_mode $(expand_rows "$@")
  say "done"
  exit 0
fi

if [ "${1:-}" = build ]; then
  build_all
  say "done"
  exit 0
fi

if [ -n "$METHOD" ]; then
  build_all
  if [ $# -eq 0 ]; then set -- all; fi
  method_mode "$@"
  say "done"
  exit 0
fi

ONLY=${1:-all}

want() {
  [ "$ONLY" = "all" ] || [ "$ONLY" = "$1" ]
}

build_all
if want avant; then http_phase avant "$BUILD/avant_server"; ws_phase avant "$BUILD/avant_ws"; fi
if want h2o; then http_phase h2o "$BUILD/h2o_server" $HPORT; ws_phase h2o "$BUILD/h2o_ws" $WPORT; fi
if want rust; then http_phase rust "$BUILD/rust_server" $HPORT; ws_phase rust "$BUILD/rust_ws" $WPORT; fi
if want go; then http_phase go "$BUILD/go_server" $HPORT; ws_phase go "$BUILD/go_ws" $WPORT; fi
if want crystal; then http_phase crystal "$BUILD/crystal_server" $HPORT; ws_phase crystal "$BUILD/crystal_ws" $WPORT; fi
# The rows without a WebSocket server run only when named.
if [ "$ONLY" = hyper ]; then http_phase hyper "$BUILD/rust_hyper/release/rust_hyper" $HPORT; fi
if [ "$ONLY" = fasthttp ]; then http_phase fasthttp "$BUILD/go_fasthttp" $HPORT; fi
if [ "$ONLY" = valk ]; then http_phase valk "$BUILD/valk_server" $HPORT; fi
say "done"
