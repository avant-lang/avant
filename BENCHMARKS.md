# Avant benchmarks

The full measurements behind the four tables of the README's [Benchmarks](README.md#benchmarks) section: how each number was taken, the per-task tables, where Avant loses, the peak memory read two ways, and the notes needed before quoting any of them.

## Contents

- [LangArena](#langarena)
- [Where Avant loses](#where-avant-loses)
- [HTTP and WebSocket servers](#http-and-websocket-servers)
- [Reading these numbers](#reading-these-numbers)
- [Garbage collector](#garbage-collector)

Each table names its machine and day (all on a server, AMD EPYC 7351P, with boost off). Every language is an optimized build; losses are printed as plainly as wins. Read [Reading these numbers](#reading-these-numbers) before quoting any.

## LangArena

[LangArena](https://github.com/kostya/LangArena) runs the same tasks in many languages. Seconds, lower is better; **bold** is the best of the five. Each task is one process: wall-clock time and peak memory (`ru_maxrss`, and in this release's tables also the exact peak, below), read by `/usr/bin/time` under LangArena's harness for the others, by a spawner without a wrapper's start-up for Avant. The server: AMD EPYC 7351P (Zen 1, 16 cores, 32 threads), boost off at a fixed 2.4 GHz, governor `performance`. Avant: 2026-11-24, release 0.51.0, `--release --cpu=v3`, the median of 5 rounds with the whole machine held and otherwise idle, each round with the task's code at another of five addresses (0, 16, 32, 48 and 448 bytes later: three of them move each loop within its 64-byte line, with innermost loops aligned to 16 bytes there, one moves the code by whole lines; the runtime's C code moves with the program's), so no single address decides a number; all 50 checksums correct; the others: 2026-11-14. Since release 0.50.0's tables the times are taken by the same method; what changed is the peak memory, given two ways (below); the comparison below measures release 0.50.0 by the same method in the same session.

**The Avant build** is release 0.51.0 as users download it: the release job's build on Ubuntu 26.04, LLVM 22.1.8 for the compiler and its clang for the runtime, for the baseline x86-64. The numbers were measured with the same source built on this server with the same toolchain, which builds release 0.50.0's published compiler byte for byte.

**Peak memory, two ways.** `ru_maxrss` is the kernel's high-water mark of a process's resident memory, as every earlier table gave it (the others' from their 2026-11-14 runs, Avant's from the rounds of its times). The exact peak is the largest resident size the process reaches, read by `tools/la-run.c -p`: every system call that can lower the resident size waits until the process's resident size is read, and it is read once more at its exit; each program runs from a `cat` copy of its binary with address randomization off, the median of five runs, every language on 2026-11-24 (the others' binaries those of 2026-11-14). The kernel counts a process's resident pages per CPU and adds each CPU's count to the total only in batches of 64 pages (256 KB), and `ru_maxrss` is read from that total, so it reads below the true peak, by about 150 to 950 KB on most single-threaded tasks and by up to about 5 MB on the threaded `Matmul` tasks. The README's summary gives `ru_maxrss` only, for every language; the exact peak is given here beside it. Go has no exact column yet: in its two LZW tasks its runtime returns memory through tens of thousands of such calls, and waiting at each of them changes how far its heap grows (`Compress::LZWDecode` 0.25 to 1.2 GB against 23 to 34 MB when not measured so); its other 48 tasks read about 1.4 MB above their `ru_maxrss`.

| Language | 50 tasks (s) | Average peak memory (`ru_maxrss`) | Average peak memory (exact) | time × Avant | peak memory × Avant (`ru_maxrss`) | peak memory × Avant (exact) | time, geometric mean × Avant |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Avant | **86.263** | 27.5 MB | 27.9 MB | 1.00× | 1.00× | 1.00× | 1.00× |
| C (Clang) | 138.008 | **23.0 MB** | 23.2 MB | 1.60× | 0.83× | 0.83× | 1.67× |
| C (GCC) | 143.231 | 23.0 MB | **23.1 MB** | 1.66× | 0.84× | 0.83× | 1.74× |
| Rust | 137.709 | 29.3 MB | 29.6 MB | 1.60× | 1.07× | 1.06× | 1.70× |
| Crystal | 194.527 | 65.2 MB | 65.1 MB | 2.26× | 2.37× | 2.33× | 2.57× |
| Go | 311.401 | 62.8 MB | — | 3.61× | 2.29× | — | 3.13× |

The geometric-mean column is the mean over the 50 tasks of each task's time over Avant's, so every task weighs alike; the total is dominated by the longest tasks.

**Against release 0.50.0, in the same session, by the same method and on the same task sources** (0.50.0's source built here, which builds its published compiler byte for byte): 0.99× the time (the geometric mean over the 50 tasks; their total 87.2 s to 86.3 s) at 0.97× the average peak memory (`ru_maxrss` 28.3 MB to 27.5 MB, exact 28.7 MB to 27.9 MB), no task's peak memory above its spread; two tasks are slower beyond their spread, by less than 1%: `CLBG::Spectralnorm` 1.006× and `Base64::Encode` 1.003×. The largest changes: `Json::Generate` 0.87× at 0.64× its memory (an interpolation pushed into a `Buf` is written in place), `Etc::Words` 0.89× (an insert by a slice looks its key up once), `Graph::DFS` 0.91× and `Graph::BFS` 0.93× at about 0.92× their memory (an inner loop keeps its arrays' size and storage in registers; the collector gives back its own work lists' pages), `Graph::AStar` 0.91× its memory, `CSV::Parse` 0.93× (a float's text read once), `Etc::CacheSimulation` 0.98× (a delete as a statement copies no value out).

**Read the total task by task**: ten rows call a library (†), C's two A* rows are not the references' algorithm (‡), and two rows carry collection hints ([Reading these numbers](#reading-these-numbers)).

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Binarytrees::Obj` | **1.104** | 6.487 | 6.849 | 2.925 | 6.155 |
| `Binarytrees::Arena` | 1.003 | 1.039 | **0.908** | 1.653 | 0.953 |
| `Brainfuck::Array` | **2.149** | 3.455 | 4.066 | 7.098 | 3.837 |
| `Brainfuck::Recursion` | **4.395** | 4.721 | 5.276 | 4.689 | 5.132 |
| `Matmul::Single` | **2.091** | 11.101 | 11.558 | 11.829 | 12.492 |
| `Matmul::T4` | **0.782** | 2.924 | 3.015 | 4.757 | 7.838 |
| `Matmul::T8` | **0.648** | 1.587 | 1.644 | 4.544 | 4.070 |
| `Matmul::T16` | **0.532** | 0.830 | 1.065 | 3.608 | 2.139 |
| `Base64::Encode` † | **0.449** | 0.497 | 1.041 | 3.490 | 5.941 |
| `Base64::Decode` † | **0.517** | 0.573 | 0.977 | 3.351 | 3.958 |
| `Json::Generate` | 0.805 | 1.554 | **0.670** | 2.979 | 3.592 |
| `Json::ParseDom` † | 0.299 | **0.292** | 4.273 | 1.710 | 5.128 |
| `Json::ParseMapping` † | 0.778 | **0.746** | 1.157 | 4.371 | 8.027 |
| `CSV::Parse` | **1.624** | 6.257 | 2.789 | 9.475 | 8.802 |
| `Etc::Sieve` | 2.659 | **1.893** | 1.916 | 4.490 | 3.792 |
| `Etc::TextRaytracer` | **2.335** | 3.250 | 2.538 | 3.525 | 4.135 |
| `Etc::NeuralNet` | 2.751 | **2.654** | 2.967 | 3.137 | 4.717 |
| `Etc::CacheSimulation` | **1.257** | 2.243 | 2.077 | 4.198 | 5.057 |
| `Etc::GameOfLife` | 2.838 | 3.577 | 3.606 | 6.274 | **2.488** |
| `Etc::Words` | **1.506** | 4.621 | 2.481 | 2.300 | 2.949 |
| `Etc::LogParser` † | 1.174 | 1.345 | **0.864** | 2.942 | 79.266 |
| `Template::Regex` † | **1.578** | 3.200 | 8.642 | 4.342 | 21.198 |
| `Template::Parse` | **1.090** | 2.479 | 2.802 | 6.547 | 2.551 |
| `Sort::Quick` | 2.762 | 2.651 | **2.640** | 3.635 | 2.915 |
| `Sort::Merge` | 3.306 | **2.394** | 3.303 | 5.566 | 3.589 |
| `Sort::Self` | **0.574** | 4.487 | 1.544 | 2.453 | 3.063 |
| `Graph::BFS` | 3.364 | **2.425** | 2.895 | 4.292 | 5.223 |
| `Graph::DFS` | 2.908 | **2.133** | 2.531 | 3.782 | 3.300 |
| `Graph::AStar` | **3.183** | 5.225 ‡ | 5.525 | 4.678 | 10.289 |
| `Hash::SHA256` | **0.519** | 2.246 | 2.144 | 2.263 | 2.559 |
| `Hash::CRC32` † | **0.032** | 1.943 | 1.950 | 1.940 | 9.272 |
| `Calculator::Ast` | **0.695** | 2.739 | 1.639 | 1.333 | 2.132 |
| `Calculator::Interpreter` | **1.606** | 2.830 | 2.167 | 2.102 | 1.799 |
| `Maze::Generator` | **1.155** | 1.226 | 2.027 | 5.454 | 3.396 |
| `Maze::BFS` | 1.370 | **1.317** | 2.117 | 6.553 | 4.350 |
| `Maze::AStar` | **1.914** | 3.557 ‡ | 2.590 | 7.896 | 7.585 |
| `CLBG::Fannkuchredux` | **2.142** | 2.680 | 2.398 | 2.455 | 2.598 |
| `CLBG::Mandelbrot` | 2.200 | 2.188 | 2.304 | 2.251 | **2.178** |
| `CLBG::Nbody` | 3.512 | 3.516 | **3.280** | 3.566 | 4.008 |
| `CLBG::Spectralnorm` | **1.993** | 2.724 | 2.729 | 2.874 | 2.712 |
| `Compress::BWTEncode` † | 1.219 | 1.717 | **0.979** | 3.335 | 4.607 |
| `Compress::BWTDecode` † | **3.625** | 3.691 | 4.109 | 4.159 | 6.540 |
| `Compress::HuffEncode` | 1.927 | 1.943 | **1.605** | 2.468 | 2.520 |
| `Compress::HuffDecode` | 1.283 | **1.037** | 1.576 | 2.481 | 3.471 |
| `Compress::ArithEncode` | 2.866 | 2.679 | **2.633** | 3.306 | 2.871 |
| `Compress::ArithDecode` | 2.412 | **2.330** | 3.327 | 2.574 | 2.554 |
| `Compress::LZWEncode` | **1.492** | 3.278 | 1.616 | 2.475 | 3.937 |
| `Compress::LZWDecode` † | **0.573** | 1.489 | 1.601 | 1.787 | 5.814 |
| `Distance::Jaro` | 2.068 | **2.020** | 2.090 | 2.837 | 2.047 |
| `Distance::NGram` | **1.200** | 4.216 | 1.210 | 1.779 | 1.856 |

† 10 of the 50 rows rely on a library or on the runtime's C, as an Avant program written for the task would: `Hash::CRC32` on [std.digest](README.md#stddigest) (Chromium's carry-less-multiply CRC-32, vendored C), `Base64::Encode` and `Decode` on [std.base64](README.md#stdbase64) (aklomp/base64, vendored C), `Json::ParseDom` and `ParseMapping` on [std.json](README.md#stdjson) (yyjson), `Template::Regex` and `Etc::LogParser` on [std.regex](README.md#stdregex) (PCRE2 and re2c), `Compress::BWTEncode` and `BWTDecode` on the runtime's C Burrows-Wheeler transform and inverse, `Compress::LZWDecode` on the runtime's C buffer copies.

## Where Avant loses

Avant is the fastest of these five on 29 of 50 tasks. Against the best of *every* language measured on this machine (C under both GCC and Clang included): at or ahead on 29 tasks, the median task 0.98× the best, no task more than 1.5× behind. The worst by time:

| Task | Avant (s) | Best (s) | Behind by |
| --- | ---: | ---: | ---: |
| `Etc::Sieve` | 2.659 | 1.893 (C, Clang) | 1.40× |
| `Graph::BFS` | 3.364 | 2.425 (C, Clang) | 1.39× |
| `Sort::Merge` | 3.306 | 2.394 (C, Clang) | 1.38× |
| `Graph::DFS` | 2.908 | 2.106 (C, GCC) | 1.38× |
| `Etc::LogParser` | 1.174 | 0.864 (Rust) | 1.36× |
| `Compress::BWTEncode` | 1.219 | 0.979 (Rust) | 1.25× |
| `Compress::HuffDecode` | 1.283 | 1.037 (C, Clang) | 1.24× |
| `Json::Generate` | 0.805 | 0.670 (Rust) | 1.20× |

By peak memory (`ru_maxrss`): at or below the best on 13 tasks, the median task at 1.26×, 11 tasks more than 1.5× above; by the exact peak (Go left out): at or below the best on 13, the median task at 1.23×, 10 more than 1.5× above. The worst:

| Task | Avant (MB, `ru_maxrss`) | Best (MB) | Above by | Avant (MB, exact) | Best (MB, exact) | Above by |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `Compress::LZWEncode` | 10.7 | 3.4 (C, Clang) | 3.15× | 11.0 | 3.4 (C, GCC) | 3.22× |
| `Etc::CacheSimulation` | 6.4 | 3.1 (C, Clang) | 2.04× | 6.6 | 3.4 (C, GCC) | 1.95× |
| `Matmul::T4` | 45.9 | 27.4 (C, GCC) | 1.67× | 48.1 | 27.8 (C, GCC) | 1.73× |
| `Matmul::T8` | 45.4 | 27.6 (C, GCC) | 1.65× | 48.4 | 27.8 (C, GCC) | 1.74× |
| `Graph::AStar` | 126.5 | 78.2 (C, GCC) | 1.62× | 126.9 | 78.5 (C, GCC) | 1.62× |
| `Graph::BFS` | 124.6 | 78.6 (C, Clang) | 1.58× | 124.8 | 78.7 (C, GCC) | 1.58× |

Most of the small tasks allocate in every run, as their references do; a small program's young generation starts at 1 MiB and grows only when the program needs it, so they read about 3.5 MB where the references read 2 to 4 MB.

Crystal and Go are compiled and garbage-collected too, so trailing either on any task, in time or memory, is a defect. Avant trails on 10:

| Task | Behind |
| --- | --- |
| `Binarytrees::Arena` | time 1.05× Go |
| `Matmul::Single` | memory 1.12× Crystal |
| `Etc::GameOfLife` | time 1.14× Go; memory 1.55× Go |
| `Graph::BFS` | memory 1.03× Crystal |
| `Graph::DFS` | memory 1.04× Crystal |
| `Calculator::Interpreter` | memory 1.07× Crystal, 1.28× Go |
| `CLBG::Mandelbrot` | time 1.01× Go |
| `Compress::LZWEncode` | memory 1.12× Crystal, 1.08× Go |
| `Distance::Jaro` | time 1.01× Go |
| `Compress::BWTDecode` | memory 1.08× Crystal |

The memory rows are by `ru_maxrss`; by the exact peak the same six tasks are above Crystal's, by 1.02× to 1.10×.

Avant's numbers are the median of five rounds, the others' one run: ratios within a few percent may be noise.

The tables understate allocation-heavy code: the two rows that would be worse are fast because they are hinted ([What allocation costs today](README.md#what-allocation-costs-today)).

## HTTP and WebSocket servers

The most basic server in each language: `GET /` answered with `hello` over keep-alive connections, each an optimized build, all eight in one session on the LangArena server (AMD EPYC 7351P, boost off at 2.4 GHz), 2026-11-24, Avant release 0.51.0. Each row is the median of three runs, each run a fresh server, one checked request, a 2-second warm-up and the measured load, the order of the rows rotating; every server passed the same correctness checks first (pipelined and split requests, a client that closes without reading). Sources and script: `bench/http/`.

| Server | No pipelining (requests per second) | Peak memory | 16 pipelined (requests per second) | Peak memory |
| --- | ---: | ---: | ---: | ---: |
| Avant (`std.http`) | 350,298 | 3.8 MB | **4,174,476** | 4.1 MB |
| C (h2o) | 343,206 | 9.6 MB | 1,584,415 | 10.6 MB |
| Rust (std, thread per connection) | 343,788 | 7.0 MB | 1,746,867 | 19.7 MB |
| Rust (hyper) | **353,983** | 13.1 MB | 4,005,974 | 27.5 MB |
| Go (`net/http`) | 185,177 | 30.6 MB | 430,824 | 80.0 MB |
| Go (fasthttp) | 344,133 | 17.6 MB | 3,388,545 | 28.4 MB |
| Crystal (`HTTP::Server`) | 165,371 | 35.8 MB | 304,741 | 101.8 MB |
| Valk (`http.serve_fast`) | 323,222 | 22.8 MB | 3,799,826 | 82.6 MB |

**Method.** No pipelining: `wrk -t8 -c256 -d10s --latency`. Pipelined: `wrk -t8 -c900 -d5 --timeout 8 -s pipe.lua -- 16`, 900 connections with 16 requests in every write, as Valk's published benchmark. The load generator runs on the same machine, and the servers use their default thread counts.

**The first column is not a ranking**: `wrk`'s eight threads use 7.5 to 8 of their 8 cores for every row from 323,000 requests a second up, so it shows that none of those six servers is the limit at 256 connections, and what each costs in memory. Pipelined, Avant answers the most requests at the least memory. hyper and fasthttp also send a `Date` header (121 and 139 bytes an answer against Avant's 84), Valk does not.

The WebSocket echo, in the same session: messages per second over 8 connections for 10 seconds, the median of three runs. The echo has two speeds from run to run here for Avant and h2o (about 72,000 and 83,000 messages a second), and two of each one's three runs came at the slower one; five more runs right after, each alternating with release 0.50.0's server, read Avant at 84,264 (0.50.0's 80,498, slower in all five rounds) and h2o at 79,319.

| Server | Messages per second | Peak memory |
| --- | ---: | ---: |
| Avant (`std.http`) | 73,321 | 4.6 MB |
| C (h2o) | 72,071 | 9.0 MB |
| Rust (std, thread per connection) | **91,066** | 2.6 MB |
| Go (`net/http`) | 53,626 | 10.9 MB |
| Crystal (`HTTP::Server`) | 63,884 | 11.4 MB |

## Reading these numbers

These notes are about [LangArena](#langarena).

- **Build flags.** Avant: `--release --cpu=v3` (x86-64-v3). C and C++: LangArena's `-O3 -march=native -mtune=native` (Zen 1 here, a superset of v3); the C column is Clang, the faster of the two C compilers (GCC: 143.231 s, 23.0 MB). Rust (`cargo build --release`), Go and Crystal (`--release`): their defaults.
- **The whole process is timed**: start-up, data preparation, warm-up and the timed iterations; LangArena's published numbers count only the timed iterations.
- **Compiled-in task sizes.** Avant compiles the task configuration in as constants, the others read `run.js` at run time, so LLVM can specialize Avant's loops on the sizes.
- **Rows that are not comparable programs.** Avant's tasks now do what their references do (above): each `Matmul` task sums every dot product with one accumulator and stores the whole result matrix, `Compress::LZWEncode` keys its dictionary by the byte sequence, built afresh in every run, `LZWDecode`'s decoder finds each entry by its code, as C's does (an offset and a length where C keeps a pointer and a length), `Hash::SHA256` is written in Avant as its references write it (its "SHA-256" is their eight-chain hash, not FIPS SHA-256, so no library computes it), and `Hash::CRC32` calls std.digest, as an Avant program would (†). **C's two A\* rows** (‡, `Graph::AStar` and `Maze::AStar`, under both C compilers) are not the references' algorithm: C's priority-queue pop compares the root's children with the minimum it has just taken off, which neither is below, so it never sifts down and leaves the last item at the root, and its open set is not kept in cost order; these rows are left out of any comparison of the A\* tasks.
- **Rows on a library** (the † rows: the library's speed, through Avant's bindings): every release also measures a pure-Avant twin of each, the references' algorithm written in Avant (`Hash::CRC32`'s bitwise loop, `Base64::Encode` and `Decode` as Go's encoding/base64, the JSON parse as serde_json's one-pass recursive descent, the regex tasks on Go's regexp (its parser, compiler, backtracker and Pike VM), the BWT inverse and LZW decode as the C reference writes them), and its record gives the totals twice, as written and in Avant alone. Where the references' algorithms differ, a row has two twins: the common one, the majority's algorithm, counts in the pure total, and the clever one, the approach of the reference ranked first on the task in LangArena's results, in a second line beside it. `Compress::BWTEncode`'s twin sorts each doubling round by comparison, as Rust (ranked first) and Go do, with C's two counting sorts measured beside; `Etc::CacheSimulation`'s makes a node at every miss, as C (ranked first) and Go do, where the task keeps Rust's node array; `Json::ParseMapping`'s clever twin parses the whole document and reads x, y and z from it, as C (ranked first) does, and its common twin decodes into types as Rust and Go do. `Compress::LZWEncode`'s clever twin keeps its phrase in one growing buffer looked up in place, making a key only at a miss, as Rust (ranked first) does; the task itself builds a new string at every byte, as C and Go do.
- **Rows that carry collection hints.** `Binarytrees::Obj` and `Calculator::Ast` call `gc.hold` and `gc.collect()` ([Collection hints](README.md#collection-hints)), telling the collector when not to run and when everything is dead; no reference has an equivalent. Unhinted, as measured with release 0.46.0 and the tasks' sources of then, `Binarytrees::Obj` took 2.882 s (Crystal, the table's fastest, 2.925 s) at 1.72× C's memory, and `Calculator::Ast` 1.541 s, 1.16× Crystal's time ([What allocation costs today](README.md#what-allocation-costs-today)).
- **What the references call**: `Json::ParseDom` and `ParseMapping` (yyjson in Avant and C, simdjson in C++, serde in Rust), `Base64::Encode` and `Decode` (the aklomp codec in Avant and C), `Template::Regex` and `Etc::LogParser` (PCRE2 in Avant and C, RE2 in C++, the `regex` crate in Rust). `Sort::Self` is each language's library sort, by the task's definition. In `CSV::Parse` Rust and C++ use CSV libraries; in `Json::Generate` C and C++ use JSON libraries, Avant its own buffer.
- **Scope.** All 50 tasks; `Etc::TextRaytracer` times its whole render.
- **Equivalent programs is the rule**: Avant's tasks follow the C and Rust references' algorithms and data widths; a row that does not is named above until rewritten.

## Garbage collector

Valk's garbage-collector benchmark suite, from [Valk](https://github.com/valk-lang/valk) 0.8.5 (`examples/bench/gc`, where it exists for Valk, Go, D and C#), with Avant, Crystal and Java ports; the programs, the runner and the full method are in `bench/valk-gc/` (its README). Seven scenarios, each isolating one cost of a collector, run in one process in upstream's order; each program prints each scenario's own time (`time_us`), and peak memory is GNU `time`'s `%M` of the process. One unmeasured warm-up round, then seven rounds; in each round every row runs once as a fresh process, the order rotating; every run's verification values checked. Measured on 2026-11-24 on the whole machine, Avant release 0.51.0's compiler:

```
date: 2026-11-24T22:32:43Z
kernel: Linux 7.3.4-zabbly+
cpu: AMD EPYC 7351P 16-Core Processor (32 threads)
governor: performance x32
memory: 128789 MB total, 125901 MB available
avant: release 0.51.0 (avant-c-release, sha256 71f07ff6a853d69f)
valk: valk-0.8.5
go: go version go1.26.0 linux/amd64 GOAMD64=v1
ldc2: LDC - the LLVM D compiler (1.41.0):   based on DMD v2.111.0 and LLVM 19.1.7 
dotnet: SDK 10.0.115, runtime 10.0.15
crystal: Crystal 1.21.1 [73ea5c2b1] (2026-09-26), LLVM 20.1.8
java: openjdk version "25.0.5.0.3" 2026-10-24
perf: perf version 7.0.14
rounds: 7 (after one unmeasured warm-up round), 84 processes a round, rotating order
sets: headline variants iso
```

| # | Scenario | What the program does |
| --- | --- | --- |
| 1 | short-lived | 10,000,000 nodes, each stored into a sink (only the last stays live) |
| 2 | stable collects | a 500,000-node list; then 500 × (one allocation, one forced collection) |
| 3 | build chain | a 5,000,000-node list |
| 4 | free chain | drops that list, then four forced collections |
| 5 | mutate links | a 200,000-node list; 50 × "move the head to the tail" |
| 6 | churn + live | a 500,000-node live list; 5,000,000 sink allocations; one forced collection |
| 7 | tree churn | 400 × build a depth-14 binary tree, count it, drop it; one forced collection |
| 8 | reverse live links | added by Avant, not part of Valk's suite, only in the scenario-isolated builds |

**What the tables do not settle** (the suite's README has every note): a node is 8 bytes in Go, 24 in C# and a 24-byte class instance in D, 16 in the others, so the same number of nodes is not the same bytes; scenario 4's timed region includes returning memory to the system in Go and D only; Valk's compiler puts scenario 7's trees in a region, so they never reach its collector (the `--no-regions` variant below); C# and Java compile while they run, and Java sizes its heap from the machine's memory; each program's forced collection is its language's own (Valk's is a local collection, proportional to what changed).

**Table 1.** The seven scenarios in one process: `time_us` of each scenario, in ms, median (minimum–maximum) of 7 runs, and the process's peak RSS

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | Peak RSS, MB (the process) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 38.5 (38.1–39.8) | 5.11 (5.10–5.14) | 202 (198–204) | 0.306 (0.297–0.315) | 17.0 (17.0–17.0) | 32.0 (31.8–33.1) | 156 (156–161) | 89.9 (89.6–89.9) |
| Avant `--release --cpu=v3` | 38.3 (38.2–39.8) | 5.13 (5.11–5.25) | 199 (194–202) | 0.310 (0.299–0.314) | 17.0 (17.0–17.1) | 32.1 (31.7–34.5) | 155 (155–188) | 89.8 (89.6–89.9) |
| Valk 0.8.5 `--release` | 40.9 (39.7–41.3) | 0.855 (0.778–0.928) | 173 (171–174) | 85.4 (84.5–85.7) | 21.1 (21.0–21.2) | 20.7 (19.2–20.9) | 109 (108–132) | 81.1 (81.0–81.2) |
| Go | 370 (367–373) | 16858 (16571–16913) | 284 (272–294) | 19.0 (16.9–331) | 16.9 (16.8–16.9) | 324 (319–333) | 776 (740–837) | 44.2 (43.3–44.5) |
| D (LDC `-O3 -release -boundscheck=off`) | 452 (449–461) | 5143 (5133–5174) | 465 (459–471) | 28.6 (28.2–31.0) | 23.9 (22.4–24.7) | 303 (302–304) | 794 (786–809) | 160.9 (160.9–161.2) |
| C# (.NET, tiered JIT) | 224 (223–230) | 9375 (9279–9860) | 592 (589–643) | 74.4 (39.8–76.1) | 32.7 (32.4–32.9) | 125 (122–127) | 409 (393–410) | 156.4 (152.6–156.6) |
| Crystal `--release` (new port) | 338 (329–340) | 3755 (3727–4198) | 331 (326–336) | 6.69 (6.30–7.39) | 26.1 (25.4–29.9) | 107 (102–108) | 283 (282–292) | 185.6 (185.4–185.7) |
| Java (G1, new port) | 150 (149–163) | 15971 (15848–16419) | 434 (418–465) | 20.3 (19.9–21.4) | 25.5 (24.9–25.7) | 123 (119–134) | 225 (224–234) | 254.7 (253.2–254.9) |

**Table 2.** Avant against Valk, scenario by scenario (Table 1's runs)

| Scenario | Avant row | Avant, ms | Valk, ms | Avant ÷ Valk (medians) | Verdict (all runs) |
| --- | --- | --- | --- | --- | --- |
| 1 short-lived | Avant `--release` | 38.5 (38.1–39.8) | 40.9 (39.7–41.3) | 0.94 | ranges overlap |
| 1 short-lived | Avant `--release --cpu=v3` | 38.3 (38.2–39.8) | 40.9 (39.7–41.3) | 0.94 | ranges overlap |
| 2 stable collects | Avant `--release` | 5.11 (5.10–5.14) | 0.855 (0.778–0.928) | 5.97 | trails |
| 2 stable collects | Avant `--release --cpu=v3` | 5.13 (5.11–5.25) | 0.855 (0.778–0.928) | 6.00 | trails |
| 3 build chain | Avant `--release` | 202 (198–204) | 173 (171–174) | 1.17 | trails |
| 3 build chain | Avant `--release --cpu=v3` | 199 (194–202) | 173 (171–174) | 1.15 | trails |
| 4 free chain | Avant `--release` | 0.306 (0.297–0.315) | 85.4 (84.5–85.7) | 0.00 | leads |
| 4 free chain | Avant `--release --cpu=v3` | 0.310 (0.299–0.314) | 85.4 (84.5–85.7) | 0.00 | leads |
| 5 mutate links | Avant `--release` | 17.0 (17.0–17.0) | 21.1 (21.0–21.2) | 0.80 | leads |
| 5 mutate links | Avant `--release --cpu=v3` | 17.0 (17.0–17.1) | 21.1 (21.0–21.2) | 0.81 | leads |
| 6 churn + live | Avant `--release` | 32.0 (31.8–33.1) | 20.7 (19.2–20.9) | 1.55 | trails |
| 6 churn + live | Avant `--release --cpu=v3` | 32.1 (31.7–34.5) | 20.7 (19.2–20.9) | 1.55 | trails |
| 7 tree churn | Avant `--release` | 156 (156–161) | 109 (108–132) | 1.43 | trails |
| 7 tree churn | Avant `--release --cpu=v3` | 155 (155–188) | 109 (108–132) | 1.43 | trails |

Peak RSS beside these times (one process runs the seven scenarios, so it has one peak; Table 1's runs, medians): Avant `--release` 89.9 MB (÷ Valk 1.11); Avant `--release --cpu=v3` 89.8 MB (÷ Valk 1.11); Valk 81.1 MB. Per scenario: Table 10.

A verdict is `leads` when Avant's slowest run is faster than Valk's fastest, `trails` when Avant's fastest is slower than Valk's slowest, `ranges overlap` otherwise.

**Table 3.** The labelled variants: `time_us` of each scenario, in ms, median (minimum–maximum), and the process's peak RSS; each variant under the row it is read against

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | Peak RSS, MB (the process) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 38.5 (38.1–39.8) | 5.11 (5.10–5.14) | 202 (198–204) | 0.306 (0.297–0.315) | 17.0 (17.0–17.0) | 32.0 (31.8–33.1) | 156 (156–161) | 89.9 (89.6–89.9) |
| Avant `--release --cpu=baseline` | 38.3 (37.7–39.8) | 5.10 (5.08–5.17) | 199 (192–206) | 0.311 (0.303–0.320) | 17.0 (16.9–17.1) | 31.9 (31.7–32.0) | 154 (154–159) | 89.8 (89.6–89.9) |
| Avant, young collections (not complete collections) | 38.4 (37.9–39.9) | 5.16 (5.13–5.73) | 202 (199–207) | 50.0 (44.1–55.7) | 16.9 (16.9–16.9) | 61.4 (60.2–63.6) | 162 (160–168) | 129.1 (129.0–129.1) |
| Avant `--release --cpu=v3` | 38.3 (38.2–39.8) | 5.13 (5.11–5.25) | 199 (194–202) | 0.310 (0.299–0.314) | 17.0 (17.0–17.1) | 32.1 (31.7–34.5) | 155 (155–188) | 89.8 (89.6–89.9) |
| Avant `--cpu=v3`, young collections (not complete) | 38.4 (38.2–39.9) | 5.25 (5.11–5.46) | 202 (198–204) | 48.3 (46.5–49.8) | 16.9 (16.8–16.9) | 61.2 (60.1–62.8) | 161 (161–163) | 129.0 (128.9–129.1) |
| Avant with hints (`gc.hold`, `gc.collect()`) | 38.5 (38.0–39.9) | 5.10 (5.08–5.16) | 282 (260–292) | 2.41 (2.36–2.43) | 17.0 (17.0–17.0) | 40.4 (40.2–40.6) | 133 (132–133) | 127.8 (127.8–127.9) |
| Avant `--cpu=v3` with hints | 39.7 (38.3–39.8) | 5.12 (5.09–5.28) | 277 (263–285) | 2.40 (2.34–2.42) | 17.0 (17.0–17.0) | 40.3 (40.3–43.0) | 133 (133–134) | 127.8 (127.7–127.9) |
| Valk 0.8.5 `--release` | 40.9 (39.7–41.3) | 0.855 (0.778–0.928) | 173 (171–174) | 85.4 (84.5–85.7) | 21.1 (21.0–21.2) | 20.7 (19.2–20.9) | 109 (108–132) | 81.1 (81.0–81.2) |
| Valk `--release --no-regions` | 41.3 (40.9–42.1) | 0.842 (0.776–0.912) | 171 (169–173) | 85.1 (83.5–85.9) | 21.1 (21.1–21.1) | 20.0 (19.6–21.0) | 217 (213–222) | 81.1 (81.0–81.1) |
| Go | 370 (367–373) | 16858 (16571–16913) | 284 (272–294) | 19.0 (16.9–331) | 16.9 (16.8–16.9) | 324 (319–333) | 776 (740–837) | 44.2 (43.3–44.5) |
| Go, plain sink (no atomic store) | 339 (337–341) | 16850 (16669–17018) | 272 (265–284) | 18.6 (15.4–276) | 16.8 (16.8–16.9) | 313 (305–328) | 833 (771–847) | 43.5 (43.2–44.3) |
| Go, `FreeOSMemory` outside scenario 4's timer | 372 (368–382) | 16815 (16646–16925) | 273 (268–282) | 269 (8.99–356) | 16.9 (16.8–16.9) | 327 (316–329) | 813 (779–850) | 44.1 (42.7–44.7) |
| Go, `GOGC=off` (upstream's README variant) | 364 (359–377) | 16748 (16591–16974) | 179 (178–183) | 17.8 (15.2–18.8) | 16.8 (16.8–16.9) | 224 (220–229) | 754 (739–759) | 225.5 (224.9–226.4) |
| D (LDC `-O3 -release -boundscheck=off`) | 452 (449–461) | 5143 (5133–5174) | 465 (459–471) | 28.6 (28.2–31.0) | 23.9 (22.4–24.7) | 303 (302–304) | 794 (786–809) | 160.9 (160.9–161.2) |
| D, `GC.minimize` outside scenario 4's timer | 454 (450–521) | 5161 (4961–5293) | 464 (462–503) | 3.69 (3.60–3.82) | 23.7 (23.2–24.8) | 305 (300–338) | 791 (781–908) | 161.0 (160.9–161.2) |
| C# (.NET, tiered JIT) | 224 (223–230) | 9375 (9279–9860) | 592 (589–643) | 74.4 (39.8–76.1) | 32.7 (32.4–32.9) | 125 (122–127) | 409 (393–410) | 156.4 (152.6–156.6) |
| C#, `DOTNET_TieredCompilation=0` | 184 (180–187) | 9494 (9395–9734) | 591 (587–593) | 71.2 (69.8–75.8) | 18.2 (17.8–18.4) | 105 (105–107) | 362 (359–386) | 151.4 (150.8–151.6) |
| Java (G1, new port) | 150 (149–163) | 15971 (15848–16419) | 434 (418–465) | 20.3 (19.9–21.4) | 25.5 (24.9–25.7) | 123 (119–134) | 225 (224–234) | 254.7 (253.2–254.9) |
| Java, `-XX:+UseParallelGC` | 200 (200–201) | 13488 (13450–13571) | 35.0 (33.7–36.7) | 10.4 (10.2–11.0) | 25.5 (24.9–25.7) | 64.3 (61.9–66.6) | 276 (276–277) | 365.5 (365.3–366.2) |

**Table 4.** Whole process (the seven scenarios): wall time and peak RSS, median (minimum–maximum)

| Row | Runs | Wall, s (shell clock around the launch) | Elapsed, s (GNU time `%e`) | Peak RSS, MB (GNU time `%M`) |
| --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 7 | 0.536 (0.530–0.537) | 0.52 (0.51–0.52) | 89.9 (89.6–89.9) |
| Avant `--release --cpu=v3` | 7 | 0.533 (0.524–0.563) | 0.52 (0.51–0.55) | 89.8 (89.6–89.9) |
| Valk 0.8.5 `--release` | 7 | 0.541 (0.534–0.560) | 0.52 (0.52–0.54) | 81.1 (81.0–81.2) |
| Go | 7 | 19.083 (18.671–19.276) | 19.07 (18.64–19.26) | 44.2 (43.3–44.5) |
| D (LDC `-O3 -release -boundscheck=off`) | 7 | 7.406 (7.380–7.429) | 7.39 (7.36–7.41) | 160.9 (160.9–161.2) |
| C# (.NET, tiered JIT) | 7 | 11.097 (10.985–11.562) | 11.08 (10.97–11.54) | 156.4 (152.6–156.6) |
| Crystal `--release` (new port) | 7 | 5.018 (4.994–5.462) | 5.00 (4.98–5.44) | 185.6 (185.4–185.7) |
| Java (G1, new port) | 7 | 17.268 (17.116–17.723) | 17.25 (17.10–17.70) | 254.7 (253.2–254.9) |
| Avant `--release --cpu=baseline` | 7 | 0.530 (0.526–0.547) | 0.51 (0.51–0.52) | 89.8 (89.6–89.9) |
| Avant, young collections (not complete collections) | 7 | 0.663 (0.652–0.669) | 0.65 (0.63–0.65) | 129.1 (129.0–129.1) |
| Avant `--cpu=v3`, young collections (not complete) | 7 | 0.660 (0.655–0.664) | 0.64 (0.64–0.65) | 129.0 (128.9–129.1) |
| Avant with hints (`gc.hold`, `gc.collect()`) | 7 | 0.648 (0.624–0.659) | 0.63 (0.61–0.64) | 127.8 (127.8–127.9) |
| Avant `--cpu=v3` with hints | 7 | 0.649 (0.630–0.709) | 0.63 (0.61–0.63) | 127.8 (127.7–127.9) |
| Valk `--release --no-regions` | 7 | 0.647 (0.637–0.652) | 0.63 (0.62–0.63) | 81.1 (81.0–81.1) |
| Go, plain sink (no atomic store) | 7 | 18.954 (18.878–19.127) | 18.94 (18.86–19.11) | 43.5 (43.2–44.3) |
| Go, `FreeOSMemory` outside scenario 4's timer | 7 | 19.091 (18.929–19.307) | 19.07 (18.91–19.29) | 44.1 (42.7–44.7) |
| Go, `GOGC=off` (upstream's README variant) | 7 | 18.671 (18.497–18.868) | 18.65 (18.48–18.85) | 225.5 (224.9–226.4) |
| D, `GC.minimize` outside scenario 4's timer | 7 | 7.451 (7.197–7.668) | 7.43 (7.18–7.65) | 161.0 (160.9–161.2) |
| C#, `DOTNET_TieredCompilation=0` | 7 | 11.109 (11.001–11.348) | 11.09 (10.98–11.33) | 151.4 (150.8–151.6) |
| Java, `-XX:+UseParallelGC` | 7 | 14.395 (14.353–14.470) | 14.38 (14.33–14.45) | 365.5 (365.3–366.2) |

**Table 5.** Scenario-isolated builds, one scenario per process: `time_us`, in ms, median (minimum–maximum), and beside it the median peak RSS of the process that ran the scenario (its range in Table 6). Scenario 8 is added by Avant and is not part of Valk's suite

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | 8 reverse links (added) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 40.3 (37.3–40.4) · 3.4 MB | 1.19 (1.18–1.20) · 12.1 MB | 186 (185–193) · 88.9 MB | 0.702 (0.701–0.736) · 89.6 MB | 16.9 (16.8–16.9) · 5.9 MB | 26.9 (26.8–27.2) · 20.4 MB | 153 (153–156) · 6.6 MB | 23.2 (22.9–23.3) · 7.6 MB |
| Avant `--release --cpu=v3` | 40.2 (38.6–40.3) · 3.4 MB | 1.19 (1.18–1.20) · 12.1 MB | 186 (183–192) · 88.9 MB | 0.709 (0.694–0.792) · 89.6 MB | 16.9 (16.8–16.9) · 5.9 MB | 27.0 (26.8–27.1) · 20.4 MB | 154 (153–155) · 6.6 MB | 23.1 (23.0–23.4) · 7.6 MB |
| Valk 0.8.5 `--release` | 40.0 (38.6–40.7) · 4.2 MB | 0.990 (0.942–1.06) · 9.9 MB | 158 (156–177) · 80.9 MB | 84.7 (84.3–85.2) · 81.2 MB | 21.1 (21.1–21.2) · 5.2 MB | 20.8 (20.1–21.6) · 11.7 MB | 101 (100–123) · 2.9 MB | 315 (314–316) · 7.7 MB |
| Go | 371 (369–373) · 7.6 MB | 16431 (16357–16584) · 7.1 MB | 389 (386–480) · 43.1 MB | 72.6 (66.3–76.4) · 43.9 MB | 16.9 (16.8–17.0) · 4.6 MB | 448 (430–463) · 14.4 MB | 810 (725–848) · 8.3 MB | 19.0 (18.9–21.8) · 4.6 MB |
| D (LDC `-O3 -release -boundscheck=off`) | 455 (451–457) · 4.2 MB | 5124 (5104–5272) · 19.2 MB | 541 (535–567) · 159.2 MB | 30.1 (29.5–34.4) · 161.2 MB | 23.1 (20.8–24.5) · 9.7 MB | 336 (335–338) · 39.4 MB | 739 (727–749) · 8.4 MB | 25.5 (25.3–26.8) · 9.7 MB |
| C# (.NET, tiered JIT) | 222 (219–229) · 39.9 MB | 9129 (9064–9221) · 45.0 MB | 554 (552–565) · 151.1 MB | 40.6 (36.9–41.6) · 151.0 MB | 38.0 (37.2–38.2) · 43.7 MB | 149 (148–151) · 54.3 MB | 386 (381–393) · 38.7 MB | 70.3 (70.1–70.7) · 43.9 MB |
| Crystal `--release` (new port) | 334 (326–338) · 4.0 MB | 3716 (3647–3750) · 19.5 MB | 353 (348–355) · 168.2 MB | 7.99 (6.94–316) · 168.4 MB | 25.9 (24.9–29.5) · 9.6 MB | 177 (175–180) · 37.4 MB | 545 (526–577) · 4.5 MB | 29.0 (28.5–30.8) · 9.7 MB |
| Java (G1, new port) | 149 (148–151) · 176.7 MB | 15784 (15660–16059) · 101.0 MB | 103 (102–104) · 123.2 MB | 28.6 (27.8–29.8) · 156.4 MB | 26.2 (25.4–27.0) · 85.9 MB | 136 (134–138) · 115.9 MB | 355 (352–356) · 287.4 MB | 32.6 (29.4–35.7) · 85.7 MB |

**Table 6.** Scenario-isolated builds: peak RSS of the process that ran the scenario, MB, median (minimum–maximum). Scenario 4's process also ran scenario 3

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | 8 reverse links (added) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 3.4 (3.3–3.4) | 12.1 (12.1–12.1) | 88.9 (88.8–88.9) | 89.6 (89.6–89.6) | 5.9 (5.8–5.9) | 20.4 (20.3–20.4) | 6.6 (6.6–6.6) | 7.6 (7.6–7.6) |
| Avant `--release --cpu=v3` | 3.4 (3.3–3.4) | 12.1 (12.1–12.1) | 88.9 (88.8–88.9) | 89.6 (89.6–89.6) | 5.9 (5.8–5.9) | 20.4 (20.4–20.4) | 6.6 (6.6–6.6) | 7.6 (7.6–7.6) |
| Valk 0.8.5 `--release` | 4.2 (4.1–4.2) | 9.9 (9.9–9.9) | 80.9 (80.9–80.9) | 81.2 (81.1–81.2) | 5.2 (5.1–5.2) | 11.7 (11.6–11.7) | 2.9 (2.9–2.9) | 7.7 (7.6–7.7) |
| Go | 7.6 (7.1–8.0) | 7.1 (6.9–8.1) | 43.1 (42.6–43.6) | 43.9 (43.6–44.1) | 4.6 (4.4–4.6) | 14.4 (13.9–15.1) | 8.3 (7.8–9.0) | 4.6 (4.4–4.6) |
| D (LDC `-O3 -release -boundscheck=off`) | 4.2 (4.1–4.2) | 19.2 (18.9–19.3) | 159.2 (159.1–159.2) | 161.2 (161.0–161.2) | 9.7 (9.7–9.9) | 39.4 (39.2–39.4) | 8.4 (8.2–8.5) | 9.7 (9.7–9.7) |
| C# (.NET, tiered JIT) | 39.9 (39.4–40.2) | 45.0 (44.3–45.4) | 151.1 (151.0–151.4) | 151.0 (150.6–151.2) | 43.7 (43.7–43.9) | 54.3 (53.9–54.4) | 38.7 (38.4–38.9) | 43.9 (43.7–44.1) |
| Crystal `--release` (new port) | 4.0 (3.7–4.0) | 19.5 (19.2–19.7) | 168.2 (167.9–168.5) | 168.4 (167.9–168.6) | 9.6 (9.4–9.9) | 37.4 (37.2–37.5) | 4.5 (4.2–4.5) | 9.7 (9.7–10.0) |
| Java (G1, new port) | 176.7 (176.4–177.2) | 101.0 (100.4–101.4) | 123.2 (123.0–124.1) | 156.4 (156.0–156.9) | 85.9 (85.4–86.4) | 115.9 (115.3–116.1) | 287.4 (286.6–287.7) | 85.7 (84.8–86.5) |

**Table 10.** The scoreboard, isolated (Table 5's runs): Avant against Valk and Go, time and peak RSS per scenario

| Scenario | Avant, ms | Avant, MB | Valk, ms | Valk, MB | Go, ms | Go, MB | ÷ Valk, time | ÷ Valk, memory | against Valk (all runs) | ÷ Go, time | ÷ Go, memory | against Go (all runs) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 short-lived | 40.3 (37.3–40.4) | 3.4 (3.3–3.4) | 40.0 (38.6–40.7) | 4.2 (4.1–4.2) | 371 (369–373) | 7.6 (7.1–8.0) | 1.01 | 0.81 | ranges overlap | 0.11 | 0.44 | leads |
| 2 stable collects | 1.19 (1.18–1.20) | 12.1 (12.1–12.1) | 0.990 (0.942–1.06) | 9.9 (9.9–9.9) | 16431 (16357–16584) | 7.1 (6.9–8.1) | 1.20 | 1.22 | trails | 0.00 | 1.70 | leads |
| 3 build chain | 186 (185–193) | 88.9 (88.8–88.9) | 158 (156–177) | 80.9 (80.9–80.9) | 389 (386–480) | 43.1 (42.6–43.6) | 1.18 | 1.10 | trails | 0.48 | 2.06 | leads |
| 4 free chain | 0.702 (0.701–0.736) | 89.6 (89.6–89.6) | 84.7 (84.3–85.2) | 81.2 (81.1–81.2) | 72.6 (66.3–76.4) | 43.9 (43.6–44.1) | 0.01 | 1.10 | leads | 0.01 | 2.04 | leads |
| 5 mutate links | 16.9 (16.8–16.9) | 5.9 (5.8–5.9) | 21.1 (21.1–21.2) | 5.2 (5.1–5.2) | 16.9 (16.8–17.0) | 4.6 (4.4–4.6) | 0.80 | 1.14 | leads | 1.00 | 1.28 | ranges overlap |
| 6 churn + live | 26.9 (26.8–27.2) | 20.4 (20.3–20.4) | 20.8 (20.1–21.6) | 11.7 (11.6–11.7) | 448 (430–463) | 14.4 (13.9–15.1) | 1.29 | 1.75 | trails | 0.06 | 1.42 | leads |
| 7 tree churn | 153 (153–156) | 6.6 (6.6–6.6) | 101 (100–123) | 2.9 (2.9–2.9) | 810 (725–848) | 8.3 (7.8–9.0) | 1.51 | 2.26 | trails | 0.19 | 0.80 | leads |
| 8 reverse links (added) | 23.2 (22.9–23.3) | 7.6 (7.6–7.6) | 315 (314–316) | 7.7 (7.6–7.7) | 19.0 (18.9–21.8) | 4.6 (4.4–4.6) | 0.07 | 0.99 | leads | 1.22 | 1.66 | trails |

Avant is the `--release` row. Memory is the peak RSS of the process that ran the scenario (scenario 4's also ran 3). The verdicts are Table 2's, on time.

