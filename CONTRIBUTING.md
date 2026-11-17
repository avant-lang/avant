# Contributing to Avant

For working on Avant itself: the compiler, the runtime, the suite and the documentation. Using Avant, from installing it to testing your own code, is in the [README](README.md).

## Contents

- [How the compiler works](#how-the-compiler-works)
- [Building the compiler](#building-the-compiler)
- [The gate and the suite](#the-gate-and-the-suite)
- [Checking a change](#checking-a-change)
- [Checking the documentation](#checking-the-documentation)

## How the compiler works

The compiler is written in Avant (`compiler/*.av`) and compiles itself. It lowers the checked program to its mid-level IR (MIR, `compiler/mir/`), then to LLVM IR (`compiler/llvm/`), compiled by the LLVM 22 linked in (`runtime/avant_llvm.c`); `cc` links the object with the runtime (`runtime/`, C).

```text
.av  →  parser and checker (compiler/*.av)  →  MIR  →  LLVM IR  →  in-process LLVM  →  object  →  cc  →  binary
```

`tests/goldens/runs/` freezes every program the retired back end (emitting [myc](https://github.com/kostya/myc) IR for a `myc-llvm` process) ran. The collector is `runtime/avant_gc_*.c`, one file per concern (listed in `runtime/avant_gc_internal.h`; settings in `runtime/avant_gc.h`). The compiler's sources obey the language's rules, field privacy included.

## Building the compiler

You need:

- a C compiler, the C library's development files and binutils (`ld`, `objcopy`);
- LLVM 22.1.8's static archives (`sh tools/install-llvm.sh` installs LLVM's official release, checked by sha256, into `/opt/llvm-22.1.8`; another LLVM 22 through `LLVM_CONFIG`) and `libzstd-dev`, which `runtime/build.sh` merges into `runtime/build/avant_llvm.o` (through `tools/build-llvm.sh`), linked only by the compiler;
- `re2c` for regex literals (on `PATH`, or the pinned static build: `sh tools/build-re2c.sh bin/re2c`);
- for the `bind` helper, `llvm-config` and libclang 20 (without them the build skips it with a notice).

Releases and CI build on `ubuntu-26.04` runners with the benchmarks' exact toolchain: gcc 15.2 and libclang 20 (for `bind`) from a fixed snapshot of Ubuntu's archive (`.github/install-toolchain.sh`), and LLVM's official 22.1.8 (`tools/install-llvm.sh`). The archive runs on glibc 2.38 and later; CI installs it on an `ubuntu-24.04` runner before each release. LLVM 22 is used over 20.1.8 for compile speed (the compiler's release build 18% faster, its self-compile 19.5%, the 50 LangArena tasks' release compiles 11.8%; a debug build of the compiler 6.7%, about 1 s, slower), with programs as fast (LangArena geometric mean 0.993, same memory) and the archive 4.7% larger; LLVM 20, 21 and 23 also build the compiler (`LLVM_CONFIG` names their `llvm-config`).

The compiler is built by a published release, the seed: **v0.44.0 or later**, unpacked at `bin/v0.46.0` (or wherever `LETTERS_SEED` points); v0.43.0 and older emit calls to runtime functions this runtime no longer exports (`avant_gc_fast_ok`, `avant_gc_block`, `avant_inline_allocs`).

```sh
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o   # rebuild with itself
```

`AVANT_ROOT` overrides where a compiler looks for `runtime/build` and `lib/std`; without it, a compiler built by `tools/letters.sh` reads the `lib/` its `OUT.versions.txt` names, wherever it is copied (if that library is gone, it says so instead of reading another), and any other (a release, a bare copy) the nearest `lib/` in its directory or one above. `avant dump FILE.av` prints a program's MIR. `tools/bind.c` is the `avant bind` translator, built by `runtime/build.sh` into `runtime/build/avant-bind`; a compiler in a checkout runs that one, elsewhere the `avant-bind` beside itself.

## The gate and the suite

| Command | What it proves |
| --- | --- |
| `sh tools/letters.sh` | builds the runtime, B (`bin/avant-b`, built by the seed) and C′ (`bin/avant-c-llvm`, built by B); `b`, `c-llvm`, `d-llvm` (D′, built by C′) or `c-release` (the `--release` compiler a release ships) builds one. Beside each compiler it writes `OUT.versions.txt`, which `avant version` prints (the compiler carries no version, so it stays byte-identical across commits that do not change it); its `lib:` line names the `lib/` the compiler reads ([Building the compiler](#building-the-compiler)) |
| `sh tools/gate.sh` | the full gate: the runtime-C rooting lint (`tools/gc-lint.sh`) and the check that no tracked file names a planning-note identifier (`tools/no-ledger-refs.sh`) before the builds; B, C′, the suite on both; the README checks on `README.md` and `CONTRIBUTING.md` (`tools/readme-check.sh`, `tools/readme-links.sh`) and `avant format --check` over every tracked source (`tools/format-check.sh`); D′ and the fixed point (C′ and D′ emit the same LLVM IR for the compiler and are byte-identical) |
| `sh tools/run-goldens.sh` | compiles and runs every example, runnable fixture and LangArena task and compares stdout, stderr and exit status with its golden (`GOLDEN_MODE=--release`, or empty, for the other modes) |

The suite is the native test runner on the compiler's own tests:

```sh
AVANT_COMPILER=./bin/avant-b ./bin/avant-b test
AVANT_COMPILER=./bin/avant-c-llvm ./bin/avant-c-llvm test
```

`tests/cases/test_probe_matrix.av` checks seven checker rules, each a pair of programs (an unsafe one to refuse, a safe twin to accept) under every statement and expression wrapper, against `tests/probe/matrix.txt`; `AVANT_COMPILER=./bin/avant-c-llvm ./bin/avant-c-llvm run tests/probe_matrix.av` rewrites the table, committed only after its diff is read.

- Each `tests/cases/test_*.av` is one program; helpers shared by several live in `tests/cases/harness.av`. Run one file with `avant test tests/cases/test_ffi.av`, or one slice with `--shard 2/4`.
- The `[test] exclusive` files (they probe the collector, rewrite `PATH`, use git, or write their own coverage report) run alone, in a command without `--shard`; a shard that would include one needs `--skip-exclusive`. `test_fixed_point.av`, which compiles the compiler, is in `[test] first` so it starts first.
- For the compiler's suite, `--junit PATH` writes one case per check, with durations; output is printed per file as each finishes, and the summary totals the run.
- The compiler's own coverage report is an instrumented compiler build's hit file, written once after the processes updating it finish; it is not `avant test --coverage`, which reports the tests.
- Goldens live in `tests/goldens/`.

## Checking a change

These tools compare a change with its base on a machine shared by several checkouts. Each reads its usage from its header (`sed -n '2,/^set -u/p' tools/NAME.sh`).

| Command | What it gives |
| --- | --- |
| `sh tools/base-cache.sh REV` | the base cache's entry of `REV`'s tree, built once on the machine and read by every checkout after: the tree checked out with its runtime, B and C′ built, every corpus program's `__emit-ll` digest and the GC-statistics set (`tools/gcstats.sh`); `--la REV` adds its LangArena part: the 50 programs stitched by the harness, their compile arguments, digests and instruction counts. Keyed by the tree id, so never read for another tree; built under a lock file beside the entry, so two checkouts do not build it twice; complete only when its `COMPLETE` file exists (a stopped build is built again, never read). The cache's directory is `BASE_CACHE`, else the one named in `~/.config/avant/base-cache`, else `~/.cache/avant/base-cache` |
| `sh tools/change-check.sh BASE HEAD OUT` | one line per commit of `BASE..HEAD`, each against the commit before it: the test files its changed files touch (by name, path or import), run by its C′; the corpus programs and LangArena programs whose `__emit-ll` moved, named and counted; the instruction counts of the LangArena programs whose code moved (all of them after a runtime change), as ratios, with their output checked. C′ is rebuilt only when `compiler/`, `lib/` or `runtime/` changed; the commit before the first is read from the base cache. Minutes, not a gate: `OUT/table.md` is the change's record of its own impact |
| `sh tools/neutral.sh BASE NEW OUT` | the neutrality: every corpus program and the 50 stitched LangArena programs through both compilers' `__emit-ll`, the `.ll` compared. `BASE` is a checkout with C′ built, or a base cache entry (only `NEW`'s C′ then runs); `NEUTRAL_CHANGED` names the change's own files, read apart |
| `sh tools/wave-final.sh OUT [BASE_STATS]` | the final evidence: the gate, the letters' sha256 and exports, the goldens in every mode, the GC statistics, the smoke test of the packed C′, the neutrality against the base cache (`WAVE_BASE`, read as its merge base with `HEAD`) and, with `WAVE_LA=1`, the LangArena timing and its table; one line per step in `OUT/SUMMARY.txt` |

**The machine lock.** Instruction counts, builds, suites and the gate give the same verdict on a busy machine, so they run beside other work; only a timing needs the machine to itself. `wave-final.sh` with `WAVE_RUNNER` (the lock runner's command words) runs every step on the lock's shared side but the LangArena prod pass and its re-measure, which alone hold the whole machine; its `lock:` and `langarena:` lines say so and how long the machine was held. Without `WAVE_RUNNER`, the steps run under whatever lock the run was started with. Run `change-check.sh` and `base-cache.sh` on the shared side too.

## Checking the documentation

`sh tools/readme-check.sh` compiles and runs every `avant` block in `README.md` and compares what it prints with what the file says; `sh tools/readme-links.sh` checks every anchor and link, a link to a heading of another Markdown file included. Given a file (`sh tools/readme-check.sh CONTRIBUTING.md`), each checks that one instead. Run both, on the file you changed, after any change to `README.md` or this file; the gate runs them on both.

- A block with `fn main` is a complete program: compiled, run, and expected to exit 0.
- A trailing `// => text` says the program prints that line there; a `text` block marked `output` or `stderr` states a whole output or a diagnostic.
- Any other block carries a mark in an HTML comment on the line before it (`<!-- check: ... -->`): a fragment completed into a program, an expected compile error, an expected trap, a file of a multi-file project, or a block that is shown and not compiled, with its reason. The header of `tools/readme-check.sh` lists the marks.

The programs of the README's [Testing](README.md#testing) are also exercised by `tests/cases/test_testing_docs.av`.
