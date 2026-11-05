# Avant Language changelog

## Unreleased

### Added

* `[test] first` in `avant.toml`: the listed test files start before the others, in the listed order.

### Changed

* `avant bind` works in a release install: the `bind` helper (`avant-bind`) ships beside `avant`, and in a checkout `sh runtime/build.sh` builds it into `runtime/build/`. Before, the command built the helper on first use into `bin/` (two files in the repository) and failed in a release install with `bind source is missing`.
* `avant test` starts a file's run as soon as that file has compiled, instead of after every file has compiled, and a free job slot takes a waiting run before a waiting compile.

### Removed

* `compiler/lex_dump.av`. Its token, AST, and checked-tree dumps are now hidden `avant dump --tokens`, `--ast`, and `--check` modes, so the golden tests use the compiler under test instead of building a second compiler.

## 0.32.0 - 2026-11-05

### Added

* `std.testing`: `t.temp_dir` returns a new, empty directory per call, removed when the test program exits.
* `--keep-tmp` on `compile`, `run`, `test`, `fetch`, and `bind` keeps the command's temporary directory and prints its path (`AVANT_KEEP_TMP=1` does the same for a program run on its own).

### Changed

* No more files left in `/tmp` or beside sources: each command keeps its intermediate files in one private `avant-XXXXXX` directory under `$TMPDIR` (or `/tmp`), removed at exit, on errors, and on Ctrl-C or `SIGTERM`. Tools and programs the command starts get it as their `TMPDIR`.
* Objects built from `.c` files passed to `compile` go to that directory instead of beside the C source.
* The coverage report is `coverage/lcov.info` (was `lcov.info`), for `avant test --coverage` and for an instrumented program's default.
* Without `AVANT_COVERAGE_MAP`, an instrumented `compile` writes its slot map beside the binary as `OUT.covmap` (was a stamped file in `/tmp`).
* `avant test --jobs` keeps up to N test processes running and starts the next as soon as one finishes (was batches of N that waited for their slowest member), with no shell scripts in between.

### Fixed

* Ctrl-C or `SIGTERM` on `avant test` now stops every test process; before, tests kept running after the runner was interrupted.

## 0.31.0 - 2026-11-04

### Added

* `import std.http`: HTTP/1.1 server and blocking client (`Http.listen`, `Http.get`, `Http.post`, `Http.request`), linked only by programs that import it.
* WebSockets in `std.http` (`ws://` only): `server.websocket(path)`, `websocket_open`, `websocket_close`, and the `Ws.connect(url)` client.
* `--threads N` / `--threads=N` program flags set the runtime thread count and are stripped from the program's arguments.

### Changed

* The HTTP server runs one event loop per elected thread on one port; `AVANT_THREADS` remains as a deprecated fallback when no `--threads` flag is given.
* The collector keeps a server's heap near a goal set from its live set: benchmark peak RSS drops from about 220 MB to about 9 MB.

### Removed

* The `http_roundtrip` prelude function is removed with no shim. Programs use `std.http`.

### Fixed

* Struct-typed temporaries are now collector roots and the young copy rewrites forwarded pointers left on the stack; the old gap could segfault in allocation-heavy code, including the HTTP server under load.
* A lambda capturing an `if x = expr { }` binding no longer crashes the backend when the payload is a struct.

## 0.30.0 - 2026-11-04

### Added

* `import std.regex`, regex on top of PCRE2 (with re2c scanners for literal patterns), linked only by programs that import it. `Regex.compile(pattern, caseless = false)` returns `Regex | RegexError`; `re.find(text, from = 0)` returns a `Match` value with bounds, indexed and named groups, and no global match cursors; `re.count`, `re.matches`, `re.replace`, and `re.split` complete the surface. A compiled pattern is a handle released with `re.close` or a `re.scoped` block, backed by a bounded cache of compiled patterns.

### Changed

* The `re_compile`, `re_find`, `re_m0`, `re_m1`, `re_c0`, `re_c1`, and `re_count` prelude functions are removed. Programs use `std.regex`; hello no longer links PCRE2.

### Fixed

* A multi-argument call whose later argument was filled from a declared default crashed the backend (the filled argument's temporary was declared with no type). Filled arguments now take the parameter's type.

## 0.29.0 - 2026-11-04

### Changed

* Root registration is batched at frame entry, and pointer reloads after a collecting call are inline. There is no per-local or per-slot call.
* Allocation and the write barrier have inline fast paths in generated code; the runtime call remains the fallback.
* The garbage collector scans only the remembered objects that changed since the last young collection, instead of every remembered object.
* Sparse mature blocks are evacuated during full collections and freed whole.
* Nursery growth is capped by measured survival, and room for the next copy is reserved before allocation. On a binary-trees workload this holds peak heap at 33.7 MB where it previously reached 54.7 MB.
* `AVANT_GC_STATS=1` reports more counters (roots, remembered set, defragmentation, survival, peak composition, large objects). Each new collector behavior can be disabled separately for comparison: `AVANT_GC_REM_FRONTIER=0`, `AVANT_GC_DEFRAG=0`, `AVANT_GC_YOUNG_SIZING=0`, `AVANT_GC_COPY_RESERVE=0`.
* Builds and release assets use the `myc-llvm` from the safepoints v0.5.0 release, downloaded and checked by sha256.

## 0.28.0 - 2026-11-02

### Added

* `import std.json`, JSON on top of yyjson, linked only by programs that import it. `Json.parse(text)` returns `Json | JsonError`. Values walk with `get`/`at`, typed accessors (`int`, `f64`, `str`, `bool`, by key or on a node) return `T?` with a string copied only when a field is taken, `first`/`iter` scan arrays in linear time, `f64s(key)` projects a numeric field of every element into an `Array(Float64)`, `to_value` builds an explicit value tree, and `JsonBuilder` writes JSON into its own buffer or a caller-owned `Buf`. A parsed document is one collector-owned buffer: there is no `close` and no finalizer.
* Type-level functions: `fn self.name(...)` in a type body declares a function called on the type itself, as `Json.parse` is. The body has no `self`; calling a type-level function on a value, or an instance method on a type, is an error.

### Changed

* The `json_*` and `json_free` prelude functions are removed. Programs use `std.json`; hello no longer links yyjson.
* `avant test --junit PATH` again writes one testcase per check, with per-check durations, for files whose checks record them (the compiler suite and `std.testing` both do); files that record nothing keep one file-level case. `avant test` also prints each file's output as soon as that file finishes instead of after the whole run; totals, exit codes, and coverage are unchanged.

### Fixed

* A young collection could lose track of a mature object's nursery children: the remembered set was never scanned, and remembered objects that were not re-kept in a cycle were dropped from it, so fields kept pointing at reused from-space memory and programs segfaulted at small young sizes (`AVANT_YOUNG_MB=1` on a tree workload). Remembered objects are now roots for the young generation. `AVANT_GC_VERIFY=1` enables a post-copy stale-reference checker, and `tests/cases/test_gc_behavior.av` covers trees, cycles, array growth, and string churn across the collector knobs.
* The full-collection trigger no longer waits for a fixed 32 MB of mature growth: it adapts to twice the live set plus slack after each full collection. Churn with a small live set (a parsed JSON document per iteration) peaks at about a third of the former RSS; large-live-set programs keep the previous spacing.
* `avant test` now removes its temporary files (scripts, logs, result files, fragments, test binaries) as soon as their contents are read; suite runs previously leaked them into the temp directory until the filesystem filled.
* GC stats now report `peak_live`, the largest live set seen at any collection, and `AVANT_GC_VERIFY` additionally proves reachability after each young collection and reports leftover C-stack words that still name from-space.
* A zero value can now be emitted for a struct member of a union, so types such as `SomeStruct | SomeError` can be returned.
* `tests/bless_goldens.av` builds its dump tool with `--debug` (as the test harness already did), bringing a golden re-bless from about nine minutes to under a minute with identical output.

## 0.27.0 - 2026-11-02

### Changed

* `base64` moved to stdlib as `std.base64`

## 0.26.0 - 2026-11-02

### Changed

* `zlib` moved to stdlib as `std.zlib`

## 0.25.0 - 2026-11-01

### Changed
* `import std.testing` walks upward from the compiler and uses the first `lib/std/testing.av` it finds. `AVANT_ROOT` is that directory when it is set. The release still ships the file beside `avant`.
* `avant test --coverage` instruments the selected tests, writes one `lcov.info`, and prints one coverage table after the summary. `compile`, `dump`, and `run` no longer take `--coverage`.

## 0.24.2 - 2026-11-01

### Changed
* `avant test` records parallel names during the run that executes the checks, and does not start the file once only to list them.
* The compiler suite returns before its checks when the test runner is only listing parallel names, so each file runs those checks once. `avant test` in CI keeps the default job count. On a 4-CPU pin, `--jobs 1` and `--jobs 2` were slower than four jobs, and on 16 threads `--jobs 1` and `--jobs 8` were slower than the default.

## 0.24.1 - 2026-10-31

### Changed
* The compiler suite names tests and helpers for what they check. The `Channel` and `Mutex` proof is `tests/cases/test_tasks.av`.

## 0.24.0 - 2026-10-31

### Changed
* The compiler suite is `avant test`. Each file in `tests/cases/` is its own program (`test_<stem>.av`) and imports `tests/cases/harness.av`. `tests/run.av` is gone. CI uses the default job count. The instrumented coverage report is written once after the suite, from the shared hit file.

## 0.23.0 - 2026-10-31

### Added
* `avant test` discovers `test_*.av` under `tests/` and runs each file as its own program. `import std.testing` is the check library. `fn main: Int` and `fn run: Int` are the process status. Files run in parallel by default. The compiler suite stays on `tests/run.av` until a later round.

## 0.22.0 - 2026-10-30

### Changed
* The compiler's checker and codegen sources live in `compiler/checker/` and `compiler/codegen/`. A file there is imported by that path (`import checker.facts`, `import codegen.loops`, `import checker.spawn`).

## 0.21.0 - 2026-10-30

### Changed
* A keyword after a dot is a name. `import http.spawn` loads `http/spawn.av`, a clash is qualified as `http.spawn.Server`, and a member is `box.spawn`. A keyword that starts a phrase stays that keyword (`spawn { }`, `import spawn`, a function name, a field label).

## 0.20.0 - 2026-10-30

### Changed
* An import is also looked up in the directory of the file passed to `compile`, `run`, or `dump`, after the importing file's directory and before the nearest `avant.toml`. One path found twice is one file. A dependency still does not search the program that imported it. This is what lets a nested file name another file under that root (`import codegen.calls` from `compiler/checker/use.av` when the root is `compiler/main.av`).

## 0.19.0 - 2026-10-29

### Added
* Nested modules: `import http.server` loads `http/server.av`. The module name is that path. Its `pub` names enter the importer unqualified, and a clash is qualified with the full name (`http.server.Server`).

### Changed
* An import is looked up beside the importing file, then under the nearest `avant.toml`, then in fetched `deps/`. Two different files for one import are an error. A file inside a dependency is named from that checkout, with no package prefix.
* Two modules may each define a type of the same name. The types stay distinct; an unqualified use of both is an error. Emit names stay as they are unless two symbols would otherwise collide, in which case each gains its module.

## 0.18.0 - 2026-10-29

* Memory management and GC improvements on efficiency and correctness, inclusing the split of  emit_while analysis into WhilePlan helpers
* Revision of performance numbers after the changes aimed towards correctness

## 0.17.0 - 2026-10-27

### Added
* Test runner parallelism: `AVANT_TEST_JOBS=N` runs the suite across parallel shard processes (`AVANT_TEST_CASES` selects cases, `AVANT_TEST_SHARD=i/n` runs one shard); isolated cases still run alone, and each shard writes its own JUnit file.
* README Testing section, with its examples exercised by `tests/cases/testing_docs.av`.

### Changed
* Codegen splits by verb: bare `avant run` keeps `--debug`; bare `avant compile` uses myc `default` (75.5% faster at runtime on the 49-task gate scope). Explicit `--debug`/`--release`/`--final` win on either verb; `AVANT_MYC_FINAL` and `AVANT_MYC_DEBUG` no longer select codegen.
* The test suite's internal compiles pin `--debug`.

## 0.16.0 - 2026-10-27

### Added
* Packages: a project may carry `avant.toml` (`[package]` plus git-only `[dependencies]`, no registry), and `avant fetch [DIR] [--update]` clones dependencies into `deps/`, checks out the exact commits recorded in the generated `avant.lock`, and fetches transitive dependencies; the compiler then finds imported modules beside the importer first and in the fetched checkouts second, with two checkouts providing the same module name reported as an ambiguity error instead of a guess.

## 0.15.0 - 2026-10-27

### Added
* `pin(value) { ... }` pins a heap value for the block and unpins it on every way out; pins are counted, so a scoped pin never clears the process pin a C field store takes.
* C callbacks: a `lib` `fun` parameter may be an Avant `fn` type, filled by a function name or a lambda that captures nothing (libc `qsort` with an Avant comparator is the proven shape).

### Changed
* `avant bind` fails the whole header with a diagnostic and a non-zero exit for a C unsigned 32-bit type instead of silently mapping it to `Int`.
* `myc-llvm` is found on `PATH`, then beside the `avant` binary, then through `AVANT_MYC_LLVM`; the first binary found must accept the safepoints GC flags or compilation stops with an error naming the path. Release archives bundle `myc-llvm` beside `avant`, and CI verifies the seed archive by sha256 before using it.

## 0.14.0 - 2026-10-27

### Added
* `for i in a..b { ... }` (end-exclusive, bounds evaluated once) and `for x in xs { ... }` over an array, with `break`, `continue`, and `return` as in `while`; the loop variable exists only inside the loop.
* `v ?? fallback` and `v.or(fallback)` on a `T?` value: the unwrapped value, or the fallback (evaluated only when `v` is nil).
* Raw strings `r"..."`: single-line, no escapes, no interpolation.
* `String.chars` and `Bytes.chars` return one `String` per Unicode codepoint; `Bytes` traps on invalid UTF-8.
* `Array.any { |x| ... }` and `Array.all { |x| ... }` scan with a `Bool`-returning block and stop early.

### Changed
* Predicate methods return `Bool`; `Buf.starts_with` previously returned `Int`, so code comparing its result to `1` must use it as a condition instead.

## 0.13.0 - 2026-10-27

### Added
* Compile-time diagnostics: every error carries a stable code (`E1xxx`–`E6xxx` by phase), shows the source line with a caret, and the compiler reports up to 20 errors per run (plus a count of any beyond) instead of stopping at the first.
* `.get(i)` on `Array`, `String`, `Buf`, and `Bytes` returns `T?` (nil out of bounds); `avant compile` and `avant run` accept `--debug` / `--release` (a flag wins over `AVANT_MYC_FINAL` / `AVANT_MYC_DEBUG`).
* Collections: `Array.sort` / `sorted` (known element types, or any type with a comparator block; NaN last, strings in byte order), `reverse`, `contains`, `index_of`, `min` / `max` (`T?`), and `sum`; `String.split` / `fields` / `strip` family; hash `keys`, `values`, `has_key`, `get_or`, `inc`, and `each { |k, v| ... }`.
* `Bytes`, an immutable byte string distinct from `String`, with `s.bytes`, `String.from_utf8(b)` (`String?`), `size`, trapping indexing and `.get(i)`, `.slice(a, b)`, and `Buf` conversions (`to_bytes`, `to_buf`, `push_bytes`).
* Wrapping integer operators `&+`, `&-`, `&*`: the explicit spelling of unchecked arithmetic.

### Changed
* Checked semantics: integer arithmetic (including `Int64` / `UInt64` and compound assignments) and indexing on `Array`, `String`, `Buf`, and `Bytes` trap on overflow, division by zero, or an out-of-bounds index, with one stderr diagnostic and exit status 1; arithmetic and indexes the compiler proves safe carry no check.
* Compile errors print to stderr and exit non-zero; `avant run` exits with the program's status. An `Array` captured by a `spawn` block is read-only in the block, and the sender may not mutate it until join (mutation crosses through `Mutex(Array)`, `Channel`, or `.parallel`).

### Fixed
* `==` and `!=` on two `String` (and `Bytes`) values compile and compare bytes; they previously did not build at all.

## 0.12.0 - 2026-10-26

### Added
* `Buf` can be indexed. `b[i]` reads one byte as an `Int`. `b[i] = n` writes the low byte. A loop keeps that byte pointer when the buffer cannot be replaced.

### Changed
* A scalar buffer of at least 6144 bytes that is reachable only from the stack stays young. The next young collection frees it when nothing marks it. It becomes mature when a heap object points at it, or when the marked buffers are at least as large as the young limit. A live matrix is not collected before every allocation.

## 0.11.0 - 2026-10-26

### Changed
* A scalar buffer of at least 6144 bytes does not move. A young collection frees it when nothing marks it. A marked one stays and becomes mature. One freed buffer of that size can be reused. A run of live buffers is not collected before every allocation.
* A parallel loop whose numeric buffers do not move keeps those addresses for the whole loop. A buffer a young collection can still move is reloaded. An array of pointers is still reloaded.
* **ci:** `setup-apt-mirrors.sh` to choose more reliable APT mirrors in Github Workflows

## 0.10.0 - 2026-10-24

### Changed
* CI takes `myc-llvm` from the safepoints `v0.4.0` release. A new `avant-linux-x86_64.tar.gz` contains the compiler only. Set `AVANT_MYC_LLVM` to the extracted safepoints binary.
* `Array.fill` on `Int` writes `0` and `-1` as a repeated byte. Any other value is stored in wide chunks.
* A loop keeps an array's buffer across a call that cannot collect and cannot replace that buffer. `to_i`, `to_f`, `next_int`, and `fill` are such calls. `push` still reloads the buffer, because a push can grow it.
* `lcov.info` keeps a relative directory in `SF:`. `compiler/main.av` stays `compiler/main.av`, so tools like Codecov can match the file.
* The native coverage report is the compiler. The hello smoke run does not reuse the compiler's coverage map, so `lcov.info` is not replaced by `examples/hello.av`.

## 0.9.0 - 2026-10-22

### Changed
* Deleting a hash entry closes the gap in the probe sequence. A later lookup stops at an empty slot instead of walking every deleted key. A string key keeps its hash in the table, so a different hash does not compare the bytes.
* A short heap string keeps its length in the header. `String.size` reads that length and does not walk the block table.
* A young collection marks a young large object that a mature object still points at. The object stays. A large string stored in an array is not freed and reused for the next large string.

## 0.8.0 - 2026-10-22

### Changed
* `Array.fill` accepts `Bool` as well as `Int`. A `Bool` fill writes one byte per element.
* A scalar buffer larger than a block starts young. A young collection frees it when nothing marks it. A marked one stays put and becomes mature. One freed byte buffer can be reused for the next allocation of the same size.
* An assignment that allocates, and does not read the local it replaces, drops that pointer before the allocation.
* `Array.new` clears the previous buffer and header slots before it allocates.

## 0.7.0 - 2026-10-22

### Added
* `re_compile` makes a pattern handle. `re_find` searches a string from a byte index. `re_m0` and `re_m1` are the match bounds. `re_c0` and `re_c1` are the first group.
* A string literal at `re_compile` uses re2c. A computed pattern uses PCRE2. `re_count` uses PCRE2.
* `sqrt` takes one `Float64`.
* `json_gen_body` and `json_gen_into` write JSON into a buffer that the caller holds.
* `Buf.starts_with` tests one string prefix.
* `Hash.clear` removes the entries and keeps the hash.
* `Class.bump` allocates one object and does not collect. The result is `T?`.
* `nursery_reclaim` runs one young collection.

### Changed
* A young collection starts at 16 MiB. `AVANT_YOUNG_MB` sets that limit.
* A young collection does not trace mature objects.
* A store of a heap pointer records that object for the next young collection.
* A live group of objects can raise the young limit, up to 192 MiB. The collector does not move that group into mature space.
* A young collection frees a dead large object that it did not mark.
* A string stored in a mature hash buffer stays remembered.
* A worker thread allocates in mature space. A young collection cannot move that buffer.
* With one thread, a pointer-array push, pop, get, or set does not take the world lock.
* A push into spare capacity does not collect. A push that must grow collects after the store.
* `Array.fill` and `Array.clear` do not allocate.
* `sqrt`, `exp`, `re_find`, and `Hash.inc_slice` do not collect.
* A function that does not allocate does not stop for the collector.
* That function can return a heap pointer. The return does not add a root on the caller.
* `Class.bump` returns nil when another thread exists, the nursery is off, or the nursery cannot grow. `Class.new` still uses the normal allocator.
* `nursery_reclaim` frees a dead nursery. A live tree stays until that tree dies.
* `"prefix${n}"` builds one string.

## 0.6.0 - 2026-10-14

### Removed
* The Crystal host: `src/`, `spec/`, `shard.yml`, and `recovery.yml`. A published seed builds the compiler. `myc-llvm` is still built with Crystal from the safepoints shard, which is not in this repository.

### Added
* A native test that an old object keeps a stored field through a later collection (`tests/cases/safepoints.av`).

## 0.5.0 - 2026-10-14

### Changed
* The required `native` build uses the published `v0.4.0` compiler. Crystal `src/` is recovery (`recovery.yml`), not the merge gate.
* A version tag builds its compiler with that same `v0.4.0` seed and `runtime/build.sh`. `myc-llvm` is still built with Crystal.

## 0.4.0 - 2026-10-14

### Added
* `bind` and `b` on the identity compiler. The command drives `tools/bind.c` through libclang. The C visitor is unchanged.

### Changed
* The next tagged release asset is `avant-linux-x86_64.tar.gz`, with the compiler named `avant`. `release.yml` opens a draft after the suite. `v0.1.0` is left as it is.
* GitHub `seed` checks that the `v0.1.0` compiler can build today's sources and pass the suite. `native` still builds A with Crystal.

## 0.3.0 - 2026-10-14

### Added
* Formatter

### Changed
* Daily compiles pass myc-llvm `--debug`. `AVANT_MYC_DEBUG=0` restores the previous codegen. `AVANT_MYC_FINAL=1` is unchanged. Crystal `src/` still builds A at myc's default.

## 0.2.0 - 2026-10-14

### Added
* Named interfaces: a static bound `T: Show`, a nominal opt-in, and no interface values.
