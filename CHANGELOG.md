# Avant Language changelog

## Unreleased

Nothing yet.

## 0.13.0 - 2026-10-27

### Added
* Compile-time diagnostics: every compile error carries a stable code (`E1xxx` lexer, `E2xxx` parser, `E3xxx` type resolution, `E4xxx` checker, `E5xxx` module loader, `E6xxx` macro expansion), shows the offending source line with a caret, and the compiler recovers per statement, function, and file to report up to 20 errors in one run (plus a count of any beyond that) instead of stopping at the first.
* Wrapping arithmetic operators: `&+`, `&-`, and `&*` on integers wrap on overflow. They are the explicit spelling of the old unchecked arithmetic.
* `.get(i)` on `Array`, `String`, `Buf`, and `Bytes` returns `T?`: the element or byte when `i` is in bounds, nil when it is not.
* `avant compile` and `avant run` accept `--debug` and `--release`. `--release` is `--final`, the scored codegen mode. A flag wins over `AVANT_MYC_FINAL` and `AVANT_MYC_DEBUG`, which still work.
* `Array.sort` and `Array.sorted` order `Int`, `Int64`, `UInt64`, `Float64`, `String`, `Bytes`, and `Bool` elements. `Float64` NaN sorts last, and `String` and `Bytes` sort in byte order. Any element type sorts with a comparator block, as in `items.sort { |a, b| a.key < b.key }`. `sort` mutates in place. `sorted` returns a new array and leaves the original alone.
* `Array.reverse` reverses in place. `Array.contains(x)` and `Array.index_of(x)` test and locate an element. `Array.min` and `Array.max` return `T?`. `Array.sum` totals a numeric array and is `0` for an empty one.
* `String.split(sep)` splits on a separator string and keeps empty fields, so `"a,,b"` splits into three pieces. `String.fields()` splits on runs of whitespace and drops empty fields. `String.strip()`, `strip_left()`, and `strip_right()` trim whitespace from the ends.
* Hashes have `keys` and `values` (both in unspecified order), `has_key(k)`, `get_or(k, default)`, `inc(k)` and `inc(k, by)` on a `Hash(K, Int)`, and `each { |k, v| ... }` over every entry.
* `Bytes` is an immutable byte string, distinct from `String`. `s.bytes` views a string's bytes without copying. `String.from_utf8(b)` returns `String?`: the text when `b` is valid UTF-8, nil when it is not. `Bytes` has `size`, the same trapping indexing and `.get(i)` as `String`, and `.slice(a, b)`. `Buf.to_bytes` copies a buffer into a `Bytes`, `Bytes.to_buf` copies it back into a fresh `Buf`, and `Buf.push_bytes(b)` appends a `Bytes`.

### Changed
* Indexing an `Array`, `String`, `Buf`, or `Bytes` outside `0 <= i < size` now traps at runtime, for reads and writes. `Bytes` itself is immutable, so only its reads trap. The program prints one diagnostic to stderr (for example `avant runtime: index 10 out of bounds (size 3)`) and exits with status 1. An out-of-bounds read previously returned 0 or whatever byte followed the buffer.
* Integer `+`, `-`, `*`, `/`, and `%` are checked, including compound assignments (`+=`, `-=`, `*=`, `/=`) and `Int64` and `UInt64` arithmetic. Overflow and division by zero trap with a stderr diagnostic and exit status 1, instead of wrapping silently or killing the process with a floating-point exception. Arithmetic the compiler can prove cannot overflow, and indexes it can prove in bounds, carries no check.
* Compile errors print to stderr and the compiler exits with a non-zero status. `avant run` exits with the compiled program's status. `avant format` is unchanged here: its diagnostics stay and it still exits 0.
* An `Array` captured by a `spawn` block is read-only inside the block, and the sender may not mutate it until the handle is joined. Both are compile errors. Mutation crosses a task boundary through a `Mutex(Array)`, a `Channel`, or `.parallel`.

### Fixed
* `==` and `!=` on two `String` values (and now on two `Bytes` values) compile and compare the bytes. They previously reached the backend in a form it rejected, so a program that compared strings that way did not build at all.

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
