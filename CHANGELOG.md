# Avant Language changelog

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
