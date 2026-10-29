# Avant Language changelog

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
