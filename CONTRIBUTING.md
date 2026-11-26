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

- a C compiler for the link (`cc`), the C library's development files and binutils (`ld`, `objcopy`);
- LLVM 22.1.8's static archives and its clang (`sh tools/install-llvm.sh` installs LLVM's official release, checked by sha256, into `/opt/llvm-22.1.8`; another LLVM 22 through `LLVM_CONFIG`): `runtime/build.sh` compiles the runtime with that clang, so a program's own code and the runtime's come from one LLVM (`CC=gcc sh runtime/build.sh` builds it with gcc instead; `sh runtime/build.sh --cc` names the compiler it would use) and `libzstd-dev`, which `runtime/build.sh` merges into `runtime/build/avant_llvm.o` (through `tools/build-llvm.sh`), linked only by the compiler;
- `re2c` for regex literals (on `PATH`, or the pinned static build: `sh tools/build-re2c.sh bin/re2c`);
- for the `bind` helper, `llvm-config` and libclang 20 (without them the build skips it with a notice).

Releases and CI build on `ubuntu-26.04` runners with the benchmarks' exact toolchain: gcc 15.2 (the link and the compiler's LLVM glue) and libclang 20 (for `bind`) from a fixed snapshot of Ubuntu's archive (`.github/install-toolchain.sh`), and LLVM's official 22.1.8 with its clang, which builds the runtime (`tools/install-llvm.sh`). The archive runs on glibc 2.38 and later; CI installs it on an `ubuntu-24.04` runner before each release. LLVM 22 is used over 20.1.8 for compile speed (the compiler's release build 18% faster, its self-compile 19.5%, the 50 LangArena tasks' release compiles 11.8%; a debug build of the compiler 6.7%, about 1 s, slower), with programs as fast (LangArena geometric mean 0.993, same memory) and the archive 4.7% larger; LLVM 20, 21 and 23 also build the compiler (`LLVM_CONFIG` names their `llvm-config`).

The compiler is built by a published release, the seed: **v0.51.0 or later**, unpacked at `bin/v0.51.0` (or wherever `LETTERS_SEED` points); older releases do not link the runtime's thread-local object (`runtime/build/avant_tls.o`) by themselves, so the compiler they build does not link.

```sh
sh runtime/build.sh
/path/to/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o
./bin/avant run examples/hello.av
./bin/avant compile compiler/main.av bin/avant --debug runtime/build/avant_llvm.o   # rebuild with itself
```

`AVANT_ROOT` overrides where a compiler looks for `runtime/build` and `lib/std`; without it, a compiler built by `tools/letters.sh` reads the `lib/` its `OUT.versions.txt` names and links and compiles against the `runtime/` it names (its `lib:` and `runtime:` lines), wherever it is copied (if one is gone, it says so instead of reading another), and any other (a release, a bare copy) the nearest `lib/` in its directory or one above, and the runtime of the checkout around it or of its own install. `avant dump FILE.av` prints a program's MIR. `tools/bind.c` is the `avant bind` translator, built by `runtime/build.sh` into `runtime/build/avant-bind`; a compiler in a checkout runs that one, elsewhere the `avant-bind` beside itself.

## The gate and the suite

| Command | What it proves |
| --- | --- |
| `sh tools/letters.sh` | builds the runtime, B (`bin/avant-b`, built by the seed) and C′ (`bin/avant-c-llvm`, built by B); `b`, `c-llvm`, `d-llvm` (D′, built by C′) or `c-release` (the `--release` compiler a release ships) builds one. Beside each compiler it writes `OUT.versions.txt`, which `avant version` prints (the compiler carries no version, so it stays byte-identical across commits that do not change it); its `lib:` line names the `lib/` the compiler reads ([Building the compiler](#building-the-compiler)) |
| `sh tools/gate.sh` | the full gate: the runtime-C rooting lint (`tools/gc-lint.sh`) and the check that no tracked file names a planning-note identifier (`tools/no-ledger-refs.sh`) before the builds; B, C′, the suite on both; the README checks on `README.md`, `CONTRIBUTING.md` and `DESIGN.md` (`tools/readme-check.sh`, `tools/readme-links.sh`) and `avant format --check` over every tracked source (`tools/format-check.sh`); D′ and the fixed point (C′ and D′ emit the same LLVM IR for the compiler and are byte-identical) |
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
| `sh tools/base-cache.sh REV` | the base cache's entry of `REV`'s tree, built once on the machine and read by every checkout after: the tree checked out with its runtime, B and C′ built, every corpus program's `__emit-ll` digest and the GC-statistics set (`tools/gcstats.sh`); `--la REV` adds its LangArena part: the 50 programs stitched by the harness, their compile arguments, digests and instruction counts. Keyed by the tree id, so never read for another tree; built under a lock file beside the entry, so two checkouts do not build it twice; complete only when its `COMPLETE` file exists (a stopped build is built again, never read). The cache's directory is `BASE_CACHE`, else the one named in `~/.config/avant/base-cache`, else `~/.cache/avant/base-cache`. When the base's C′ refuses some of the stitched programs (the harness writes the tasks in a spelling the base does not know), the part is complete all the same and names them in its `refused.txt`. `--prune DAYS` removes the entries unread for that long (`--dry-run` lists them): every call stamps the entry it returns, the tools that read one hold its lock shared, and an entry whose lock is held is kept |
| `sh tools/test-map.sh REV` | the test map of `REV`'s tree, beside its base cache entry: which lines of the compiler each test file's run executes, from the coverage job's instrumented compiler with the hits taken one test file at a time (about ten minutes). `change-check.sh` reads it when it is there |
| `sh tools/cases-check.sh OUT` | every `tests/cases/test_*.av` through C′'s `__emit-ll`, in a few seconds: a test file the compiler now refuses is named with its message before any suite runs |
| `sh tools/change-check.sh BASE HEAD OUT` | one line per commit of `BASE..HEAD`, each against the commit before it: the test files its changed files touch (by name, path or import), run by its C′, and with a test map (`tools/test-map.sh`) also those whose run executed a line the commit changes or the function around it, named apart; the corpus programs and LangArena programs whose `__emit-ll` moved, named and counted; the instruction counts of the LangArena programs whose code moved (all of them after a runtime change), as ratios, with their output checked. C′ is rebuilt only when `compiler/`, `lib/` or `runtime/` changed; the commit before the first is read from the base cache. Minutes, not a gate: `OUT/table.md` is the change's record of its own impact |
| `sh tools/neutral.sh BASE NEW OUT` | the neutrality: every corpus program and the 50 stitched LangArena programs through both compilers' `__emit-ll`, the `.ll` compared. `BASE` is a checkout with C′ built, or a base cache entry (only `NEW`'s C′ then runs); `NEUTRAL_CHANGED` names the change's own files, read apart; LangArena programs the base refuses read not comparable, and the sources are judged alone |
| `sh tools/layout-retime.sh LADIR SLUG [RUNS]` | one LangArena task compiled by C′ as it is and with only its code moved (an unused C function linked in, `tools/layout/plt.c`), timed alternately: whether the time follows where the code sits. Worth one run before a timing difference at an equal instruction count is believed; `wave-final.sh` names such tasks "layout?" (`tools/layout-flag.py`) and times each across its own layouts (`tools/layout-check.sh`). It times: run it alone |
| `sh tools/placement-probe.sh build\|time\|report OUT` | every LangArena task built once and linked again with its code at several shifts (`tools/layout-cc.sh`, a C compiler wrapper), timed by `tools/rounds.py` with one side per layout, and `tools/layout-spread.py`'s table: each task's median over its layouts, its spread across them and its noise at one layout. The regression check of a change to how code is linked or aligned; `plan` splits the timing into runs of a few minutes. It times: run `time` alone |
| `sh tools/wave-final.sh OUT [BASE_STATS]` | the final evidence: the gate, the letters' sha256 and exports, the goldens in every mode, the GC statistics, the smoke test of the packed C′, the neutrality against the base cache (`WAVE_BASE`, read as its merge base with `HEAD`) and, with `WAVE_LA=1`, the LangArena timing and its table; one line per step in `OUT/SUMMARY.txt`. Its first step compiles every test file (`tools/cases-check.sh`; `WAVE_CASES_STOP=1` ends the run there when one is refused). It runs in a private `/tmp` (a mount namespace, as the gate), so its litter check sees only what it leaves |

**The machine lock.** Instruction counts, builds, suites and the gate give the same verdict on a busy machine, so they run beside other work; only a timing needs the machine to itself. `wave-final.sh` with `WAVE_RUNNER` (the lock runner's command words) runs every step on the lock's shared side but the LangArena prod pass and its re-measure, which alone hold the whole machine; its `lock:` and `langarena:` lines say so and how long the machine was held. Without `WAVE_RUNNER`, the steps run under whatever lock the run was started with; a run started inside an exclusive lock runner (the whole machine for every step) says so in its first line and its `lock:` line. `WAVE_GROUP=1` takes the shared lock once for the shared steps instead of once a step: another checkout's timing that arrives meanwhile then waits for all of them instead of going first at the next step. Run `change-check.sh` and `base-cache.sh` on the shared side too.

## Checking the documentation

`sh tools/readme-check.sh` compiles and runs every `avant` block in `README.md` and compares what it prints with what the file says; `sh tools/readme-links.sh` checks every anchor and link, a link to a heading of another Markdown file included. Given a file (`sh tools/readme-check.sh CONTRIBUTING.md`), each checks that one instead. Run both, on the file you changed, after any change to `README.md`, `DESIGN.md` or this file; the gate runs them on all three (`readme-check.sh` on `DESIGN.md` once it holds a code block).

- A block with `fn main` is a complete program: compiled, run, and expected to exit 0.
- A trailing `// => text` says the program prints that line there; a `text` block marked `output` or `stderr` states a whole output or a diagnostic.
- Any other block carries a mark in an HTML comment on the line before it (`<!-- check: ... -->`): a fragment completed into a program, an expected compile error, an expected trap, a file of a multi-file project, or a block that is shown and not compiled, with its reason. The header of `tools/readme-check.sh` lists the marks.

The programs of the README's [Testing](README.md#testing) are also exercised by `tests/cases/test_testing_docs.av`.
