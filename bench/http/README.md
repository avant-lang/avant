# HTTP / WebSocket hello-world

One hello-world server per row, with identical behavior: HTTP answers `GET /` with the body `hello` as `text/plain; charset=utf-8` over keep-alive connections, and the WebSocket servers echo each text message back. Servers are measured one at a time and are never run concurrently with a build or test suite.

| File | Row | What it does |
| --- | --- | --- |
| [avant_server.av](avant/bench/http/avant_server.av), [avant_ws.av](avant/bench/http/avant_ws.av) | Avant | `std.http` servers on ports 18301 (HTTP) and 18311 (WebSocket); thread count with `--threads N` |
| [h2o_server.c](avant/bench/http/h2o_server.c), [h2o_ws.c](avant/bench/http/h2o_ws.c) | C | h2o, one event loop per thread; thread count as the second argument |
| [rust_server.rs](avant/bench/http/rust_server.rs), [rust_ws.rs](avant/bench/http/rust_ws.rs) | Rust std | Standard library only, thread per connection; no thread control |
| [rust_hyper/](avant/bench/http/rust_hyper/src/main.rs) | Rust hyper | hyper 1 on tokio's multi-thread runtime, `pipeline_flush(true)` and `writev(true)`; thread count as the second argument; HTTP only |
| [go_server.go](avant/bench/http/go_server.go), [go_ws.go](avant/bench/http/go_ws.go) | Go | `net/http` and a hand-written WebSocket echo; thread count with `GOMAXPROCS` |
| [go_fasthttp/](avant/bench/http/go_fasthttp/main.go) | Go fasthttp | `fasthttp.Server` with its defaults; thread count with `GOMAXPROCS`; HTTP only |
| [crystal_server.cr](avant/bench/http/crystal_server.cr), [crystal_ws.cr](avant/bench/http/crystal_ws.cr) | Crystal | `HTTP::Server`, built with `-Dpreview_mt`; thread count with `CRYSTAL_WORKERS` |
| [valk_server.valk](avant/bench/http/valk_server.valk) | Valk | `http.serve_fast` (Valk's benchmark handler form) on io_uring; worker count as the second argument; HTTP only; built only when `valk` is installed |

The hyper, fasthttp and Valk programs, [pipe.lua](avant/bench/http/pipe.lua) and methods B and C are adapted from Valk's own HTTP benchmark (valk-lang/valk, `examples/bench/http`, tag `0.8.5`, MIT, © 2024 Lorenz Vandevelde); each file's header says what it changed, and Valk's license is [LICENSE-valk](avant/bench/http/LICENSE-valk). `rust_hyper/Cargo.lock` and `go_fasthttp/go.sum` fix the dependency versions; the first build downloads them.

## The README's table (no `METHOD`)

`sh bench/http/run.sh [avant|h2o|rust|go|crystal|all]`. HTTP is measured with `wrk -t4 -c64 -d10s --latency`, one run; WebSocket is measured with [ws_load.go](avant/bench/http/ws_load.go) using 8 connections for 10 seconds. Each phase also reports the server's peak RSS, and the Avant HTTP phase reports allocated bytes per request. `hyper`, `fasthttp` and `valk` can be named too (HTTP only).

## The three methods (`METHOD`)

`METHOD=A|A8|B|C|Q9 sh bench/http/run.sh [row ...|all]` measures HTTP only (`all` is the eight rows above). Every row gets, in each of three rounds (`HTTP_BENCH_RUNS`), a fresh server process, one checked request (status 200, body `hello`), a 2-second warm-up with the method's own `wrk` line (`HTTP_BENCH_WARMUP`) and one measured load; the row order rotates by one each round.

| Method | Load | Server | What it is |
| --- | --- | --- | --- |
| A | `wrk -t4 -c64 -d10s --latency URL` | default thread counts, nothing pinned | The load the README's table has always used: 64 connections, one request per round trip. |
| A8 | `wrk -t8 -c256 -d10s --latency URL` | as A | A with a stronger load generator, to see whether `wrk` is the limit in A. |
| B | `wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL -s pipe.lua -- 16` | default thread counts, nothing pinned | Valk's README method: 900 connections, 16 pipelined requests per write. The server and `wrk` compete for the same cores. |
| C | the same line under `taskset -c 4-7,12-15` | 4 threads, under `taskset -c 0-3` | Valk's chart method, the per-core one: four physical cores for the server, the other four (both siblings) for `wrk`, the server cores' siblings idle. The Rust std row has no thread control and is bounded by the pinning alone. |
| Q9 | `wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL` | as B | B without pipelining: 900 connections, one request per round trip. |

Three labelled variants run only when named, never under `all`, and are never the headline rows: `avant-nosigpipe` is the same Avant binary started by `env --ignore-signal=PIPE` (the server as shipped is killed by SIGPIPE when a client closes with pipelined answers unread, which is what `wrk` does to its connections at the end of a B or C load, so the plain row often does not survive its own warm-up; this variant is how the server's throughput under B and C is measured until that is fixed); `hyper-nodate` and `fasthttp-nodate` are those servers with their `Date` header (and fasthttp's `Server` header) switched off, an 84-byte answer like Avant's and Valk's.

Pipelining at depth 16 is a choice: almost no browser or proxy pipelines HTTP/1.1. Methods B and C measure a server's CPU cost per request with most of the system-call cost amortised away, and should be read as that. `wrk` 4.1.0 records one latency per batch under pipelining and then applies a correction that assumes one request per round trip, so its percentiles under B and C are not per-request latencies (`src/wrk.c` records the latency when a connection's pending count reaches zero and derives the correction's interval from the completed requests).

For every run the harness keeps, under `HTTP_BENCH_OUT/<METHOD>/` (default `$HTTP_BENCH_BUILD/out`): `wrk`'s output for the measured load and for the warm-up, `wrk`'s own elapsed, user and system seconds from `/usr/bin/time` (a load generator using all of its threads is the limit, not the server), and a `.meta` file with both command lines, the server's thread argument and pinning, its CPU seconds during the measured load, its idle and peak RSS (`VmHWM`), its OS thread count, whether it was still running after the load ended, and Avant's `AVANT_GC_STATS` line. `python3 bench/http/tables.py $HTTP_BENCH_OUT` turns that directory into the tables: median, minimum and maximum of the valid runs, the answer's size in bytes and whether it carries `Date` (from the correctness pass), and every run that is not a result (a socket error, a timeout, a non-2xx answer, a failed check) listed with its reason.

## Correctness and instruments (untimed)

- `sh bench/http/run.sh check [row ...|all]` starts each server and runs [check.py](avant/bench/http/check.py) against it: the exact bytes of one answer; 16 pipelined requests in one write; the same 16 cut in two writes at eight different bytes; 200 writes of 16 on one connection; 1,000 keep-alive requests; a client that sends 16 requests and closes without reading (`close_unread`); then the server's stop mechanism (`GET /quit`; SIGTERM for h2o and Valk) must leave the port free. On 2026-11-12 every row passes everything except Avant's `close_unread`: the server process is killed by SIGPIPE when a client closes with at least two pipelined answers unread (record `2026-11-12-s29-wbench-http`).
- `METHOD=B sh bench/http/run.sh syscalls [row ...|all]` runs each server under `strace -f -c` with the method's load (3 seconds) and keeps the per-call counts; `tables.py` divides them by the requests `wrk` completed.
- [spread.py](avant/bench/http/spread.py) `PID PORT CONNECTIONS [TRIALS]` opens idle connections and reads, from `/proc/PID/fdinfo`, how many landed in each event loop's epoll set (how `SO_REUSEPORT` spread them).

## Prerequisites

Binaries are built into `/tmp/http-ws-bench-build` (override with `HTTP_BENCH_BUILD`); `sh bench/http/run.sh build` only builds. Avant is compiled with `bin/avant-b` in `--release`, like the other languages' optimized builds (override with `AVANT_BENCH_COMPILER`). The h2o servers need a clone of `h2o-project/h2o` at the workbench's `refs/h2o` (override with `H2O_SRC`; the library is built into `H2O_BUILD`, default `/tmp/h2o-build`) and the wslay library, either from the system or extracted from the distribution's packages under `/tmp/wslay` (`WSLAY_ROOT` for the extraction's `usr`, `WSLAY_PC` for the directory holding a `libwslay.pc` that points at it). The hyper row needs `cargo`, the fasthttp row Go modules, the Valk row a `valk` on `PATH`; `wrk`, `curl`, `python3`, `taskset` and, for `syscalls`, `strace` are used by the harness itself.
