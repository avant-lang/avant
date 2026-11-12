# Valk's general benchmark suite, tier 1, measured as shipped

The eight single-threaded programs of Valk's `examples/bench` (`binary-tree`, `merkletrees`, `lru`, `json`, `json-serde`, `nsieve`, `spectral-norm`, `fannkuch-redux`), run on this machine in Valk, Go and Rust exactly as Valk ships them, with a written equivalence note per program. This is the baseline that Avant's own ports of the suite and the two-column table (as shipped, levelled) will be measured against. There is no Avant program here yet.

Credit: the programs, their inputs and the idea of a one-command, self-checking runner are Valk's (https://github.com/valk-lang/valk, MIT, © 2024 Lorenz Vandevelde). Valk's published numbers come from another machine and never share a table with numbers measured here.

## What is here, and what is not

- **Not here: Valk's, Go's and Rust's programs.** They are read from a checkout of valk-lang/valk at the pinned tag `0.8.5` (commit `332243001c505373a8203cc60abaafcf188c4701`), built by the Valk compiler of the same version. They are not copied into this tree: the Go and Rust files carry no licence or credit line of their own, and their origin is not confirmed.
- `run.sh`: the runner (`versions`, `build`, `shipped`, `check`, `counts`, `time`). New code; it calls Valk's own `examples/bench/run.sh` unchanged for the "as shipped" table and repeats its three build commands for everything else.
- `measure.py`: `check`, `counts` and `time` (called by `run.sh`).
- `tables.py`: regenerates every table from the raw per-run files.
- `expected/<program>.txt`: the output of each program with the suite's arguments. One file per program for every language and variant: all of them print the same bytes.
- `equivalence.md`: the notes, one per program, with the verdict "equivalent" or "not comparable as shipped" and the lines that show it.
- `variants/`: two levelled twins of a few lines, adapted from Valk's files, each with a header naming its source and what was changed, and Valk's licence in `variants/LICENSE-valk`: `lru-plain/main.valk` (the eviction as three plain map operations instead of `HashMap.get_or_replace`) and `nsieve-checked/main.valk` (a checked array owned by the collector instead of `@unsafe` manual memory).

## Running it

```sh
git clone https://github.com/valk-lang/valk <dir> && git -C <dir> checkout 0.8.5
ln -s "$(readlink -f "$(command -v valk)")" <dir>/valk     # Valk 0.8.5; Valk's run.sh wants the compiler at the repository root
export VALK_SRC=<dir> VALK_BENCH_OUT=<where the raw files go>
bash bench/valk/run.sh versions
bash bench/valk/run.sh build        # 58 rows; cargo downloads the crates of lru, json and json-serde once
bash bench/valk/run.sh check        # every row once, untimed, against expected/
bash bench/valk/run.sh counts       # perf stat -e instructions:u per row
bash bench/valk/run.sh shipped 5    # Valk's own run.sh, unchanged (timed: alone on the machine)
bash bench/valk/run.sh time 7       # the warm-up and seven rotating rounds (timed: alone on the machine)
python3 bench/valk/tables.py "$VALK_BENCH_OUT"
```

It needs `go`, `rustc` and `cargo`, GNU `/usr/bin/time`, `perf`, `python3` and `bash`. With a Go older than 1.27 the runner exports `GOEXPERIMENT=jsonv2`, because `json` and `json-serde` import `encoding/json/v2`. `shipped` and `time` refuse to run unless every CPU's governor is `performance` (`VALK_BENCH_ALLOW_GOVERNOR=1` makes a dry run, marked "disturbed, not evidence" in its session file and in the tables).

## The rows

| Set | Rows | Built with |
| --- | --- | --- |
| `shipped` (23) | each program in Valk, Go and Rust (`fannkuch-redux` has no Go program) | the three commands of Valk's `run.sh`: `valk build main.valk --release`; `go build`; `cargo build --release` where there is a `Cargo.toml`, else `rustc -C opt-level=3 -C debuginfo=0` |
| `switch` (2) | `binary-tree` and `merkletrees` in Valk without regions | `--release --no-regions` |
| `levelled` (2) | `lru:valk-plain`, `nsieve:valk-checked` | `--release` on the files in `variants/` |
| `v3` (23) | every shipped row with the compiler told to target x86-64-v3 | `--cpu x86-64-v3`; `GOAMD64=v3`; `-C target-cpu=x86-64-v3` (through `RUSTFLAGS` for cargo) |
| `lto` (8) | Rust with fat LTO and one code-generation unit | `CARGO_PROFILE_RELEASE_LTO=fat CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1`; `-C lto=fat -C codegen-units=1` |

The arguments are those of Valk's `run.sh` (`benchmark_args`): `binary-tree 19`, `merkletrees 18`, `lru 1000 11000000`, `json 2000000`, `json-serde sample 50000`, `nsieve 13`, `spectral-norm 5500`. `fannkuch-redux` is in no runner of Valk's; it runs with 11, the default of Valk's program. The shipped rows are built into `$VALK_SRC/debug/benchmarks/`, the directory Valk's `run.sh` builds into, so both methods time the same binaries (`build/sha256.txt` is checked after Valk's `run.sh` has rebuilt them).

Only the `shipped` set is a headline. The other sets are labelled variants: they answer what a difference costs (`research/valk/benchmarks.md` §6.1, questions 2, 3 and 4) and never replace a shipped row.

## The method

- **As shipped (Table 1):** Valk's `examples/bench/run.sh --runs 5`, unchanged, for the seven programs it knows: one warm-up, the median of five fresh processes by GNU time's `%e` (hundredths of a second) and `%M`, Valk always first.
- **This workbench's method (Tables 2 and 3), over the same binaries:** one unmeasured warm-up of every row, then seven rounds of one fresh process per row. The order rotates: each round starts further into the list and every other round walks it backwards. Wall time is a nanosecond monotonic clock around the process (the process started is GNU time, so its start-up, under a millisecond, is inside the interval and the same for every row); peak RSS, user and system time are GNU time's. Every run's output is compared with `expected/`; a wrong output, a non-zero exit or a timeout stops the session and is not a result. One raw file per run. The tables give the median, the minimum and the maximum, never a best-of.
- **Instructions:** `perf stat -e instructions:u`, one run per row, for the rows that time cannot separate (within 5%); the count covers every thread of the process.
- **Recorded beside every table:** the versions of Valk, Go, Rust and the tools, the kernel, the CPU, the date, the governor, the load average before and after, and the build command of every binary.

## What the notes say, in short

Of the eight programs, three are equivalent across the languages that have them (`binary-tree`, `spectral-norm`, and `json-serde` as a library row), `json` is equivalent as a task and is a library row, and four are not comparable as shipped in at least one column (`merkletrees` against Go, `lru` and `nsieve` against both, `fannkuch-redux` against Rust). `equivalence.md` has the lines and the counts. An Avant port follows Valk's program in each case, never something cleverer, and where the shipped programs differ the levelled twin is the comparable row.

## Avant's closest LangArena tasks (for orientation only)

They are different programs with different inputs; no comparison is drawn from them. `binary-tree` and `merkletrees`: `Binarytrees::Obj` (with `gc.hold` hints, which a port of Valk's unhinted program must not use). `lru`: `Etc::CacheSimulation`. `json`: `Json::ParseDom`. `json-serde`: `Json::ParseMapping` (a DOM, not a typed decoder). `nsieve`: `Etc::Sieve`. `spectral-norm`: `CLBG::Spectralnorm`. `fannkuch-redux`: `CLBG::Fannkuchredux`.
