# Valk's GC benchmark suite, ported to Avant

Seven scenarios that each isolate one cost of a garbage collector, taken from [Valk](https://github.com/valk-lang/valk) (`examples/bench/gc`), where they exist for Valk, Go, D and C#. This directory adds an Avant port, a Crystal port and a Java port, a runner that measures all of them on one machine by one method, and labelled variants that quantify the differences between the shipped programs.

**Credit and license.** The scenarios, their sizes, the output format and the Valk, Go, D and C# programs are Valk's: valk-lang/valk, `examples/bench/gc` and `examples/bench/run-gc.sh`, tag `0.8.5` (commit `332243001c505373a8203cc60abaafcf188c4701`; the same bytes at `c90e059e86ffe928c97d0ed521264c4ae30fc81d`), MIT, © 2024 Lorenz Vandevelde. Valk's `LICENSE` is `upstream/LICENSE`. Numbers published by Valk were measured on another machine and never share a table with numbers measured here.

## What is here

| Path | What | Origin |
| --- | --- | --- |
| `upstream/main.valk`, `main.go`, `main.d`, `main.cs`, `gc-csharp.csproj`, `README.md`, `run-gc.sh`, `LICENSE` | Valk's files | copied byte for byte; never edited |
| `main.av` | the Avant port (the headline row) | ported from `upstream/main.valk` |
| `main.cr` | a Crystal port | ported from `upstream/main.valk` |
| `Main.java` | a Java port | ported from `upstream/main.valk` and `upstream/main.cs` |
| `variants/young/main.av` | Avant with `gc.collect()` (a young collection) where upstream forces a collection | modified from `main.av` |
| `variants/hints/main.av` | Avant with `gc.hold { }` around scenario 3's build and `gc.collect()` after each tree of scenario 7 | modified from `main.av` |
| `variants/go-plain-sink/main.go` | Go with a plain sink variable in place of the atomic pointer | modified from `upstream/main.go` |
| `variants/go-free-outside/main.go` | Go with `debug.FreeOSMemory()` after scenario 4's timed region | modified from `upstream/main.go` |
| `variants/d-minimize-outside/main.d` | D with `GC.minimize()` after scenario 4's timed region | modified from `upstream/main.d` |
| `variants/iso/` | every program with scenario numbers as arguments (one scenario per process) and an eighth scenario added by Avant | modified from the seven programs above |
| `run.sh`, `tables.py` | the runner and the table generator | new code (not a modification of `run-gc.sh`) |
| `trees/` | two LangArena tasks, Binarytrees::Obj and Calculator::Ast, with and without `std.gc`'s hints ([The tree ports](#the-tree-ports-o386)) | LangArena's tasks as Avant's harness builds them; the unhinted forms modified from them |

Every port and every variant begins with a header naming its source, the repository, the commit, the license and what was changed.

## The scenarios

| # | Name printed | What the program does | Timed region |
| --- | --- | --- | --- |
| 1 | `short-lived alloc` | 10,000,000 nodes, each stored into a sink (only the last stays live) | the loop |
| 2 | `stable-heap forced collects` | a 500,000-node list; then 500 × (one allocation, one forced collection) | the 500 rounds |
| 3 | `build long-lived chain` | a 5,000,000-node list | the build |
| 4 | `free long-lived chain` | drops that list, then four forced collections | the drop and the four collections |
| 5 | `mutate live links` | a 200,000-node list; 50 × "move the head to the tail" (2 stores and a 200,000-node walk a round) | the 50 rounds |
| 6 | `short-lived churn with large live set` | a 500,000-node live list; 5,000,000 sink allocations; one forced collection | the loop and the collection |
| 7 | `tree churn` | 400 × build a depth-14 binary tree (32,767 nodes), count it, drop it; one forced collection | all of it |
| 8 | `reverse live links` | **added by Avant, not part of Valk's suite**, only in `variants/iso/`: scenario 5 with the rotation replaced by an in-place reversal (200,000 stores a round) | the 50 rounds |

Each scenario prints `time_us`, `time_ms`, `mem_kb` and `verify`. The verification values are the same in every language and are checked on every run: 2; `OK`; `OK`; 5000000; `OK`; `OK`; 13106800; and `OK` for scenario 8.

## The Avant port

`main.av` does what `main.valk` does, line for line, with these differences, each forced by the language:

- **The sink.** Avant has no mutable module-level variable. Valk's two globals are the two fields of one long-lived `Globals` object made in `main` and passed to each scenario; `g.sink = n` is a store into a heap object, with Avant's write barrier.
- **Forced collections.** Where the other programs force a blocking full collection, `main.av` calls the runtime's full collection, declared through `lib` as `tests/cases/test_gc_behavior.av` does. `std.gc`'s `gc.collect()` is a young collection, not a complete one; `variants/young/` measures it as a labelled row.
- **No hints.** The headline row uses neither `gc.hold { }` nor `gc.collect()`; `variants/hints/` shows what they buy.
- **Construction.** `Tree { left: build(), right: build() }` is `Tree.new(build_tree(), build_tree())` with an `initialize` that takes both children.
- **`mem_kb`** prints 0: there is no public form for the heap in use. Upstream's own README says the column is not comparable between languages; the runner reports peak RSS for every row.

## The tree ports (O386)

Two LangArena tasks, each in two forms: as LangArena builds it, with `std.gc`'s hints, and without them. They are the bars the Valk stage's later waves are read on (VALK-S and VALK-R3: Binarytrees::Obj unhinted; VALK-R2: Calculator::Ast unhinted; VALK-D2), and they are not part of Valk's suite.

| Path | What | Origin |
| --- | --- | --- |
| `trees/helper.av`, `trees/calc_lib.av` | the harness's helper and Calculator's library | copied from Avant's LangArena harness (`langarena` 37367d3: `helper.av`, `tasks/calc_lib.av`), a header added |
| `trees/binarytrees_obj.av`, `trees/calc_ast.av` | Binarytrees::Obj and Calculator::Ast as LangArena builds them, with the hints | copied from `tasks/binarytrees_obj.av` and `tasks/calc_ast.av`, a header added |
| `trees/binarytrees_obj_unhinted.av`, `trees/calc_ast_unhinted.av` | the same programs without the hints | modified from the two above |
| `trees/cfg_binarytrees_obj.av`, `trees/cfg_calc_ast.av` | LangArena's prod configuration | written as the harness writes it from `run.js` (kostya/LangArena 3c9f84d) |

The tasks are kostya/LangArena's (MIT, © 2026 Konstantin Makarchev); the Avant sources are the Avant project's. `run.sh build trees` stitches each program as the harness stitches a task (the helper, Calculator's library, the configuration, the task; each file after a `// file:` line) into `$BUILD/trees/ROW.av` and compiles it with the harness's flags, `--release --cpu=v3` (D284). Each prints only its checksum, LangArena's: 75497436 for Binarytrees::Obj, 1670614456 for Calculator::Ast, checked on every run.

**The unhinted programs, by D222.** An unhinted program is its hinted program with the hints removed and nothing else: the same classes, functions, sizes, iterations and checksum. Binarytrees::Obj loses `import std.gc`, the `gc.collect()` before each tree and the `gc.hold { }` around building and summing it (each round is `tree_sum(make_tree(0, depth))`). Calculator::Ast loses `import std.gc`, the `gc.collect()` after the text is generated, the `gc.hold { }` around each round and the `gc.collect()` at the round's end; the loop that puts the sentinel back in the parsed slots stays (it is the program's own release of the round's trees, as the references drop theirs, not a hint). `diff trees/binarytrees_obj.av trees/binarytrees_obj_unhinted.av` shows the whole difference.

**Rows.** The set `trees` (`binarytrees-obj`, `binarytrees-obj-unhinted`, `calc-ast`, `calc-ast-unhinted`) runs under `check`, `time` and `rounds` when it is named (`VALK_GC_SETS=trees`); `run.sh trees-counts` gives each program's `AVANT_GC_STATS=1` line and its user-mode instructions in `counts`' fixed environment (Table 12). Until VALK-D1 has merged, the set is outside the default sets and the full build, so `check`, `counts`, `cg` and `time` run what they ran before.

## Running

```sh
sh runtime/build.sh && sh tools/letters.sh b c-llvm      # the Avant compiler, bin/avant-c-llvm
bench/valk-gc/run.sh build                               # every program, into $VALK_GC_BUILD (default /tmp/valk-gc-build)
bench/valk-gc/run.sh build avant valk                    # only what the rows avant and valk use (headline and iso)
bench/valk-gc/run.sh check                               # every row once, untimed: verification only
bench/valk-gc/run.sh counts                              # no clock: AVANT_GC_STATS, instruction counts, the other runtimes' collection counts
bench/valk-gc/run.sh build trees                         # the tree ports (O386), into $VALK_GC_BUILD/trees
VALK_GC_SETS=trees bench/valk-gc/run.sh check            # the tree ports once each, checksums checked
bench/valk-gc/run.sh trees-counts                        # no clock: the tree ports' AVANT_GC_STATS and instructions
bench/valk-gc/run.sh cg 2 3 6                            # no clock: exact instructions per function of the Avant program, here for scenarios 2, 3 and 6
bench/valk-gc/run.sh time 7                              # one warm-up round, then seven measured rounds (alone on the machine, governor performance)
bench/valk-gc/run.sh rounds 7                            # the same through tools/rounds.py's loop and spawner (O652)
VALK_GC_SIDES="base=/tmp/vgc-base final=/tmp/vgc-final" bench/valk-gc/run.sh rounds 7   # two build directories, one session
python3 bench/valk-gc/tables.py /tmp/valk-gc-build/out   # every table, regenerated from the raw per-run files
bench/valk-gc/dry-run.sh /tmp/vgc-dry                     # the runner's own test: every mode VALK-B1 added or changed, one round, not evidence
```

Toolchains: `valk` 0.8.5 (`vman`), `go`, `ldc2`, `dotnet`, `crystal`, a JDK, `perf`, GNU `time`, `python3`; `valgrind` for `run.sh cg` only. The build commands of the upstream programs are upstream's (`run-gc.sh`): `valk build --release`, `go build`, `ldc2 -O3 -release -boundscheck=off`, `dotnet publish --configuration Release`. The ports: `avant compile --release` (and `--release --cpu=v3`), `crystal build --release`, `javac`.

## The method

- **Rows.** Headline: the seven scenarios in one process, in upstream's order, for Avant (`--release`, and `--release --cpu=v3` as a second row), Valk, Go, D, C#, Crystal and Java (G1). Variants: the labelled rows below. Isolated: `variants/iso/`, one scenario per process (scenario 4's process also runs 3), which attributes peak RSS to a scenario, removes the effect of the scenarios before it, and carries scenario 8.
- **Runs.** One unmeasured warm-up round, then seven rounds; in each round every row runs once as a fresh process; the order of the rows rotates from round to round and alternates direction. Timed runs are alone on the machine, with the `performance` governor (the runner refuses otherwise) and the governor, kernel, CPU, load and every command line recorded in `raw/session.txt`.
- **What is read.** `time_us` of each scenario from the program's own output (never the truncated `time_ms`); the whole process's wall time (the shell's microsecond clock around the launch, about a millisecond of launcher inside it) and GNU `time`'s elapsed and peak RSS. Tables give the median, the minimum and the maximum, never a best-of. **Peak memory beside every time** (O380, D342 Q5): a table of one-process times carries the process's peak RSS as its last column (seven scenarios in one process have one peak); Table 5 puts beside each isolated scenario's time the peak RSS of the process that ran it; Table 10 is the scoreboard, Avant against Valk and Go per scenario, time and memory with both ratios; the tree ports and the rounds tables give wall time and peak RSS together. The criterion of D324 stays time only; the memory is read beside it.
- **Checks.** Every run: exit status 0, every expected scenario present once, every verification value right, `=== done ===` present; otherwise the session stops and the run is not a result.
- **Rounds through `tools/rounds.py`** (`run.sh rounds [ROUNDS]`, O652): the same units as `time` (`VALK_GC_SETS`, `VALK_GC_ROWS`; `VALK_GC_UNITS` restricts the isolated scenarios), timed by the one loop LangArena's rounds use. `run.sh` writes the rows (command, environment, what to verify) and `measure.py` runs them through `tools/rounds.py`'s `measure()`: one unmeasured warm-up of every row, then ROUNDS rounds, the units' order rotating as `rounds.py` rotates it (each round starts further into the list, every other round walks it backwards) and, with several sides, the side that goes first rotating inside each unit. Each run is started by `rounds.py`'s spawner, a small C program that reads the monotonic clock just before `vfork` and just after `wait4`: no wrapper's start-up is inside the wall time (GNU time's is, in `time`), and peak RSS, user and system time are `wait4`'s rusage of the program's own process. **Sides** (`VALK_GC_SIDES="base=DIR final=DIR"`, default `this=$VALK_GC_BUILD`): each side is a build directory, so a base and a final compiler are timed in one session (O618); the session keeps each side's `binaries.sha256` and `avant-compiler.txt`. Every run is verified as in `time`. Files: `$VALK_GC_OUT/rounds/session.txt`, `raw/round-R/SIDE--SET--ROW--UNIT.run` (the spawner's figures) and `.out` (the program's output, where `tables.py` reads `time_us`); Tables R1 to R4. `time` keeps its own loop until VALK-D1 has merged; `rounds` then replaces it.
- **Counts, with no clock** (`run.sh counts`): for each scenario in its own process, Avant's `AVANT_GC_STATS=1` line and user-mode instructions (`perf stat -e instructions:u`) for the three Avant rows; instructions for the other ahead-of-time programs; and the collection counts Go (`GODEBUG=gctrace=1`), D (`--DRT-gcopt=profile:1`), Crystal (`GC_PRINT_STATS=1`) and Java (`-Xlog:gc`) print themselves. Each counted process runs in a fixed environment: emptied (`env -i`, keeping `PATH` and `HOME`, then only the variables the row needs) and without address-space randomization (`setarch -R`), so the caller's environment never reaches it. With the caller's environment and randomization, ten runs of one binary spread over about 2,900 instructions, and the count sat about 14,500 higher than in an empty environment (the runtime's `getenv` calls walk every variable, about 300 instructions each); in the fixed environment ten runs spread over 2 instructions at start-up and 10 to 31 on scenarios 5 and 1 (VALK-TM2, O648). The executable's path also moves a count (by up to about 1,700 instructions on scenario 5, through the stack's layout), so counts compare only when both were taken this way from one build directory.
- **Instructions per function, with no clock** (`run.sh cg [N...]`): each scenario asked for (every one by default) runs once, in its own process, as the scenario-isolated Avant program under `valgrind --tool=callgrind`. Callgrind counts every instruction the process executes and attributes each to its function, so the listings are exact and repeat from run to run; a sampled profile (`perf record`) attributes a sample to the instruction that is waiting and so over-counts the functions that take cache misses. For each scenario, under `cg/` of the output directory: `sN.excl.txt` (each function's own instructions, largest first), `sN.incl.txt` (with its callees'), `sN.total` (the whole process; it agrees with `perf stat -e instructions:u` to about 0.1%) and `sN.out` (the program's output, verified like any run); `session.txt` records the toolchains and the binary's hash. Scenario 4's process also runs scenario 3. A process runs about fifty times slower under callgrind, so the limit is an hour per scenario (`VALK_GC_CG_TIMEOUT`); `VALK_GC_CG_ROWS` sets how many functions a listing keeps (70). The runtime's reserved address range is set to 2 GiB for these runs (`AVANT_HEAP_RESERVE_GB=2`), which valgrind needs and which changes no count.
- **Build directories** (`run.sh build [ROW...]`). With no row, `build` makes every program and rewrites `binaries.sha256`. With rows (the ids of `VALK_GC_ROWS`), it makes only what those rows use in any set (`avant` is `gc-avant` and `gc-avant-iso`; `go-gogc-off` is `gc-go`) and replaces their lines of `binaries.sha256`, keeping the others. Each build of an Avant row writes `avant-compiler.txt`: the compiler (`AVANT_BENCH_COMPILER`), its sha256, the checkout's HEAD and its changed paths, and the compiler's `versions.txt`. A base and a final compiler are timed against each other from two build directories, each built with its own `AVANT_BENCH_COMPILER`.
- **Raw files.** `raw/<set>/<row>/r<N>.out` (the program's output), `.time` (GNU time), `.wall_us`, `.rc`, `.err`, `.verify`; `tables.py` reads nothing else.

## The labelled variants

| Row | Differs from | In what | Why |
| --- | --- | --- | --- |
| Avant `--cpu=baseline` | Avant | the x86-64 level | Avant's `compile` defaults to x86-64-v2; Go (`GOAMD64=v1`), LDC, Crystal and Valk build for baseline x86-64 when no CPU is named, and the JITs use the host's instructions |
| Avant, young collections | Avant | `gc.collect()` for every forced collection | Avant's "only what changed" collection beside Valk's local collection (scenario 2); **not a complete collection**: it does not reclaim an old structure (scenario 4 frees nothing) |
| Avant with hints | Avant | `gc.hold { }` around scenario 3's build; `gc.collect()` after each tree of scenario 7 | what the two hints of `std.gc` buy; the other programs carry no hints |
| Valk `--no-regions` | Valk | the compiler keeps every object on the GC heap | with regions, scenario 7's trees never reach Valk's collector (`valk build --region-report`) |
| Go, plain sink | Go | `var sink *Node` in place of `atomic.Pointer[Node]` | upstream's Go sink is an atomic exchange per allocation; Valk's, D's and C#'s are plain stores |
| Go, `FreeOSMemory` outside | Go | scenario 4 returns memory to the system after its timer stops | upstream times Go's and D's return of memory and Valk's program returns none |
| Go, `GOGC=off` | Go | only the explicit collections run | the variant upstream's README offers |
| D, `GC.minimize` outside | D | as Go's | as Go's |
| C#, no tiered compilation | C# | `DOTNET_TieredCompilation=0` | the early scenarios otherwise include the JIT's tiering |
| Java, Parallel | Java | `-XX:+UseParallelGC` | the default collector is G1; no heap flags in either row |

## Equivalence notes (what is not equal between the programs)

- **The sink.** Go: an atomic store in a function marked `noinline`. C#: a static field in a `NoInlining` method. D and Java: a static variable in a small function the compiler may inline. Valk and Crystal: a global or class variable stored in the loop. Avant: a field of a heap object stored in the loop, behind the write barrier's inline test.
- **Forced collection.** Valk: `gc.collect()`, its local collection, complete for data no other thread shares, and proportional to what changed. Go: `runtime.GC()`. D: `GC.collect()`. C#: `GC.Collect(MaxGeneration, Forced, blocking)` and `WaitForPendingFinalizers`. Crystal: `GC.collect` (Boehm). Java: `System.gc()`. Avant: `avant_gc_collect`, a full mark and sweep. All but Valk's trace the whole live heap each time.
- **Scenario 4's timed region.** Go's ends with `debug.FreeOSMemory()` and D's with `GC.minimize()`; no other program returns memory to the system inside the timer.
- **Object sizes.** A node is 16 bytes in Valk, Avant and Java (header and one reference; compressed in Java), 8 in Go, 24 in C#, 16 in Crystal (a type id and one reference), and a 24-byte class instance in D before its size class. "The same number of nodes" is not "the same bytes".
- **Trees.** Valk, Go, Avant, Crystal and Java build both children and then construct the parent from them; D and C# allocate the parent first and store the children into it.
- **Regions.** Valk's compiler puts scenario 7's trees in a region reused at each round of the loop; they never reach its collector. The other programs give every tree node to their collector.
- **JIT.** C# and Java compile while they run; their early scenarios include it. Java sizes its heap from the machine's memory (a quarter of it at most), so a scenario may run with no collection at all.
- **Collectors.** Valk: ownership-based ("stateful"), not moving, not generational. Go: concurrent mark and sweep, not moving, not generational. D: conservative mark and sweep. C#: generational, compacting, concurrent workstation collector. Crystal: Boehm's, conservative, not generational, not moving. Java: G1 (generational, regional, compacting) or Parallel. Avant: sticky Immix with an opportunistic copy for the young generation, a full mark and sweep with defragmentation, precise roots.
- **`mem_kb`** is a different quantity in every language (and 0 in Avant); it is not compared. Peak RSS comes from GNU `time`.
- **One process, seven scenarios.** In the headline rows each scenario inherits the heap the ones before it left; the isolated rows remove that.

## Results

This directory holds the programs, the runner and the method. It carries no result tables: run `./run.sh time 7` and `python3 tables.py <output directory>` to produce them on your machine.
