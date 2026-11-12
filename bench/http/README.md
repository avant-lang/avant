# HTTP / WebSocket hello-world

One hello-world server per row, with identical behavior: HTTP answers `GET /` with the body `hello` as `text/plain; charset=utf-8` over keep-alive connections, and the WebSocket servers echo each text message back. Servers are measured one at a time and are never run concurrently with a build or test suite.

| File | Row | What it does |
| --- | --- | --- |
| [avant_server.av](avant/bench/http/avant_server.av), [avant_ws.av](avant/bench/http/avant_ws.av) | `avant` | `std.http` servers on ports 18301 (HTTP) and 18311 (WebSocket); thread count with `--threads N` |
| [h2o_server.c](avant/bench/http/h2o_server.c), [h2o_ws.c](avant/bench/http/h2o_ws.c) | `h2o` | C, h2o, one event loop per thread; thread count as the second argument |
| [rust_server.rs](avant/bench/http/rust_server.rs), [rust_ws.rs](avant/bench/http/rust_ws.rs) | `rust` | Rust, standard library only, thread per connection; no thread control |
| [rust_hyper/](avant/bench/http/rust_hyper/src/main.rs) | `hyper` | Rust, hyper 1 on tokio's multi-thread runtime, `pipeline_flush(true)` and `writev(true)`; thread count as the second argument; HTTP only |
| [go_server.go](avant/bench/http/go_server.go), [go_ws.go](avant/bench/http/go_ws.go) | `go` | Go, `net/http` and a hand-written WebSocket echo; thread count with `GOMAXPROCS` |
| [go_fasthttp/](avant/bench/http/go_fasthttp/main.go) | `fasthttp` | Go, `fasthttp.Server` with its defaults; thread count with `GOMAXPROCS`; HTTP only |
| [crystal_server.cr](avant/bench/http/crystal_server.cr), [crystal_ws.cr](avant/bench/http/crystal_ws.cr) | `crystal` | Crystal, `HTTP::Server`, built with `-Dpreview_mt`; thread count with `CRYSTAL_WORKERS` |
| [valk_server.valk](avant/bench/http/valk_server.valk) | `valk` | Valk, `http.serve_fast` (the library's handler form that writes straight to the response writer), on io_uring; worker count as the second argument; HTTP only; built only when `valk` is installed |

`rust_hyper/Cargo.lock` and `go_fasthttp/go.sum` fix the dependency versions; the first build downloads them.

## One run per server (no `METHOD`)

`sh bench/http/run.sh [avant|h2o|rust|go|crystal|all]`. HTTP is measured with `wrk -t4 -c64 -d10s --latency`, one run; WebSocket is measured with [ws_load.go](avant/bench/http/ws_load.go) using 8 connections for 10 seconds. Each phase also reports the server's peak RSS, and the Avant HTTP phase reports allocated bytes per request. `hyper`, `fasthttp` and `valk` can be named too (HTTP only).

## The methods (`METHOD`)

`METHOD=A|A8|B|C|Q9 sh bench/http/run.sh [row ...|all]` measures HTTP only (`all` is the eight rows above). Every row gets, in each of three rounds (`HTTP_BENCH_RUNS`), a fresh server process, one checked request (status 200, body `hello`), a 2-second warm-up with the method's own `wrk` line (`HTTP_BENCH_WARMUP`) and one measured load; the row order rotates by one each round.

| Method | What it is | Load | Server |
| --- | --- | --- | --- |
| A | 64 connections, no pipelining, 4 load threads | `wrk -t4 -c64 -d10s --latency URL` | default thread counts, nothing pinned |
| A8 | 256 connections, no pipelining, 8 load threads | `wrk -t8 -c256 -d10s --latency URL` | as A |
| B | 900 connections, 16 pipelined requests per write, 8 load threads | `wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL -s pipe.lua -- 16` | default thread counts, nothing pinned: the server and `wrk` compete for the same cores |
| C | B with the server limited to 4 pinned threads | the same line under `taskset -c 4-7,12-15` | 4 threads, under `taskset -c 0-3`: four physical cores for the server, the other four (both siblings) for `wrk`, the server cores' siblings idle; the `rust` row has no thread control and is bounded by the pinning alone |
| Q9 | 900 connections, no pipelining, 8 load threads | `wrk --latency -d 5 -c 900 -t 8 --timeout 8 URL` | as B |

`HTTP_BENCH_DURATION` overrides the seconds of the measured load, `HTTP_BENCH_THREADS` forces a server thread count under any method, and `HTTP_BENCH_SERVER_CPUS` and `HTTP_BENCH_WRK_CPUS` replace method C's CPU lists (the defaults are for a machine with 8 cores whose sibling threads are `k` and `k+8`).

How to read the methods:

- A and A8 differ only in the load generator's strength. When `wrk`'s own CPU use (kept for every run) is close to its thread count, the load generator is the limit and the rows it limits come out close together whatever the servers could do.
- B and C pipeline 16 requests per write, which almost no browser or proxy does with HTTP/1.1. They measure a server's CPU cost per request with most of the system-call cost amortised away (for a server that answers a batch with one write), and should be read as that.
- Under pipelining `wrk` 4.1.0 records one latency per batch, when a connection's pending count reaches zero, and derives its correction's interval from the completed requests (`src/wrk.c`), so its percentiles under B and C are not per-request latencies.

Three labelled variants run only when named, never under `all`, and are not the plain rows:

- `avant-nosigpipe` is the same Avant binary started by `env --ignore-signal=PIPE`. Up to v0.41.0 a client that closed with pipelined answers unread, which is what `wrk` does to its connections at the end of a B or C load, ended the server process with SIGPIPE, so the plain row could have no server left for its measured load, and this variant was how the server's throughput under B and C was measured. The server's writes no longer raise the signal, and `check.py`'s `close_unread` passes on the plain row; the variant stays for measuring a binary built before that.
- `hyper-nodate` and `fasthttp-nodate` are those servers with their `Date` header (and fasthttp's `Server` header) switched off: an 84-byte answer, like the servers that send neither.

## What is kept, and the tables

For every run the harness keeps, under `HTTP_BENCH_OUT/<METHOD>/` (default `$HTTP_BENCH_BUILD/out`): `wrk`'s output for the measured load and for the warm-up; `wrk`'s own elapsed, user and system seconds from `/usr/bin/time`; and a `.meta` file with both command lines, the server's thread argument and pinning, its user and system CPU during the measured load, its idle and peak RSS (`VmHWM`), its OS thread count and per-thread CPU when the load ended, whether it was still running after the load ended, and Avant's `AVANT_GC_STATS` line.

`python3 bench/http/tables.py $HTTP_BENCH_OUT` turns that directory into Markdown tables: per method, each row's median, minimum and maximum requests per second over the valid runs, `wrk`'s p50 and p99, peak RSS, the server's CPU in cores with its system share, `wrk`'s CPU in cores, the answer's size in bytes and whether it carries `Date` (from the correctness pass); Avant's allocation and collection figures; and every run that is not a result (a socket error, a timeout, a non-2xx answer, a failed check, a server that was gone) listed with its reason.

## Correctness and instruments (untimed)

- `sh bench/http/run.sh check [row ...|all]` starts each server and runs [check.py](avant/bench/http/check.py) against it: the exact bytes of one answer; 16 pipelined requests in one write; the same 16 cut in two writes at eight different bytes; 200 writes of 16 on one connection; 1,000 keep-alive requests; a client that sends 16 requests and closes without reading (`close_unread`); then the server's stop mechanism (`GET /quit`; SIGTERM for `h2o` and `valk`, which have no such route) must leave the port free. A failed check is printed and makes the command exit 1; the pass goes on to the next row.
- `METHOD=B sh bench/http/run.sh syscalls [row ...|all]` runs each server under `strace -f -c` with the method's load (3 seconds) and keeps the per-call counts; `tables.py` divides them by the requests `wrk` completed. `strace` slows the server many times over, so the receive and send counts per request are the result; the waiting calls are a lower bound.
- [spread.py](avant/bench/http/spread.py) `PID PORT CONNECTIONS [TRIALS]` opens idle connections and reads, from `/proc/PID/fdinfo`, how many landed in each event loop's epoll set (how `SO_REUSEPORT` spread them).

## Prerequisites

Binaries are built into `/tmp/http-ws-bench-build` (override with `HTTP_BENCH_BUILD`); `sh bench/http/run.sh build` only builds. Avant is compiled with `bin/avant-b` in `--release`, like the other languages' optimized builds (override with `AVANT_BENCH_COMPILER`). The h2o servers need a clone of `h2o-project/h2o` at the workbench's `refs/h2o` (override with `H2O_SRC`; the library is built into `H2O_BUILD`, default `/tmp/h2o-build`) and the wslay library, either from the system or extracted from the distribution's packages under `/tmp/wslay` (`WSLAY_ROOT` for the extraction's `usr`, `WSLAY_PC` for the directory holding a `libwslay.pc` that points at it). The `hyper` row needs `cargo`, the `fasthttp` row Go modules, the `valk` row a `valk` on `PATH`; `wrk`, `curl`, `python3`, `taskset` and, for `syscalls`, `strace` are used by the harness itself.

## Credits

- [rust_hyper/](avant/bench/http/rust_hyper/src/main.rs), [go_fasthttp/](avant/bench/http/go_fasthttp/main.go), [valk_server.valk](avant/bench/http/valk_server.valk) and [pipe.lua](avant/bench/http/pipe.lua) are adapted from valk-lang/valk, `examples/bench/http` (tag `0.8.5`), MIT, © 2024 Lorenz Vandevelde; each file's header says what it changed, and that project's license is [LICENSE-valk](avant/bench/http/LICENSE-valk). The pipelined methods (B and C: 900 keep-alive connections, 16 pipelined requests, 8 load threads; 4 pinned server threads) follow that project's published benchmark.
- [h2o_server.c](avant/bench/http/h2o_server.c) and [h2o_ws.c](avant/bench/http/h2o_ws.c) are adapted from h2o's `examples/libh2o` (MIT, DeNA).
