# HTTP / WebSocket hello-world harness

Durable copy of the wave-5 five-language harness, recreated for Wave 5B step 1 (the wave-5 copy lived in `/tmp/http-ws-bench/` and did not survive). One basic hello-world server per language, identical behavior before any number is compared: keep-alive on, body `hello`, no per-response `Connection: close` (a server that closes per response is not measuring the same thing as the keep-alive servers — this bug already bit once in wave 5). Avant WebSocket is not built yet (wave 8), so the Avant WS cell stays `—`.

| File | Language | Shape preserved from wave 5 |
| --- | --- | --- |
| [avant_server.av](/home/coghi/Projects/avant-workbench/avant/bench/http/avant_server.av) | Avant `std.http` | `Http.listen` on 18301, `GET /` → `hello`, `GET /quit` closes |
| [c_server.c](/home/coghi/Projects/avant-workbench/avant/bench/http/c_server.c) | C | Hand-written single-threaded epoll, keep-alive, pipelining |
| [rust_server.rs](/home/coghi/Projects/avant-workbench/avant/bench/http/rust_server.rs) | Rust | Std-only thread-per-connection, keep-alive, `TCP_NODELAY` |
| [go_server.go](/home/coghi/Projects/avant-workbench/avant/bench/http/go_server.go) | Go | `net/http` |
| [crystal_server.cr](/home/coghi/Projects/avant-workbench/avant/bench/http/crystal_server.cr) | Crystal | `HTTP::Server` |
| [c_ws.c](/home/coghi/Projects/avant-workbench/avant/bench/http/c_ws.c), [rust_ws.rs](/home/coghi/Projects/avant-workbench/avant/bench/http/rust_ws.rs), [go_ws.go](/home/coghi/Projects/avant-workbench/avant/bench/http/go_ws.go), [crystal_ws.cr](/home/coghi/Projects/avant-workbench/avant/bench/http/crystal_ws.cr) | WS | Text-echo round-trip (`hi`), same loader for all |

Method: HTTP is `wrk -t4 -c64 -d10s --latency`; WebSocket is [ws_load.go](/home/coghi/Projects/avant-workbench/avant/bench/http/ws_load.go) (a dependency-free Python loader, [ws_load.py](/home/coghi/Projects/avant-workbench/avant/bench/http/ws_load.py), is kept for cross-checks but caps near 60k msg/s on this machine, so the harness counts with the compiled one) with 8 connections for 10 s counting text round-trips; RSS is the server peak VmHWM from `/proc/<pid>/status` after the phase, with idle VmHWM read before it (check idle before believing a peak — the C server's first wave-5 RSS reading was a 1 GiB static array being touched page by page). Avant also reports allocation bytes per request from `AVANT_GC_STATS`. Runs are one server at a time on ports 18301/18311, never concurrent with a build or a suite, and before/after comparisons interleave runs of the two builds.

Run: `sh bench/http/run.sh [avant|c|rust|go|crystal|all]`. Binaries land in `/tmp/http-ws-bench-build` (override with `HTTP_BENCH_BUILD`); the Avant compiler is `bin/avant-b` (override with `AVANT_BENCH_COMPILER`, backend pinned through `AVANT_MYC_LLVM`).
