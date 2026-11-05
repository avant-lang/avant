# HTTP / WebSocket hello-world

One hello-world server per language, with identical behavior: HTTP answers `GET /` with `hello` over keep-alive connections, and the WebSocket servers echo each text message back. Servers are measured one at a time and are never run concurrently with a build or test suite.

| File | Language | What it does |
| --- | --- | --- |
| [avant_server.av](avant/bench/http/avant_server.av), [avant_ws.av](avant/bench/http/avant_ws.av) | Avant | `std.http` servers on ports 18301 (HTTP) and 18311 (WebSocket) |
| [h2o_server.c](avant/bench/http/h2o_server.c), [h2o_ws.c](avant/bench/http/h2o_ws.c) | C | h2o, one event loop per thread |
| [rust_server.rs](avant/bench/http/rust_server.rs), [rust_ws.rs](avant/bench/http/rust_ws.rs) | Rust | Standard library only, thread per connection |
| [go_server.go](avant/bench/http/go_server.go), [go_ws.go](avant/bench/http/go_ws.go) | Go | `net/http` and a hand-written WebSocket echo |
| [crystal_server.cr](avant/bench/http/crystal_server.cr), [crystal_ws.cr](avant/bench/http/crystal_ws.cr) | Crystal | `HTTP::Server` |

Run it with `sh bench/http/run.sh [avant|h2o|rust|go|crystal|all]`. HTTP is measured with `wrk -t4 -c64 -d10s --latency`; WebSocket is measured with [ws_load.go](avant/bench/http/ws_load.go) using 8 connections for 10 seconds. Each phase also reports the server's peak RSS, and the Avant HTTP phase reports allocated bytes per request.

Binaries are built into `/tmp/http-ws-bench-build` (override with `HTTP_BENCH_BUILD`). Avant is compiled with `bin/avant-b` (override with `AVANT_BENCH_COMPILER`). The h2o servers need a clone of `h2o-project/h2o` at the workbench's `refs/h2o` (override with `H2O_SRC`) and the wslay library, either from the system or extracted under `/tmp/wslay` (`WSLAY_ROOT`).
