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

[LangArena](https://github.com/kostya/LangArena) runs the same tasks in many languages. Seconds, lower is better; **bold** is the best of the five. Each task is one process: wall-clock time and peak memory (`ru_maxrss`, and the exact peak beside it, below), read by `/usr/bin/time` under LangArena's harness for the others, by a spawner without a wrapper's start-up for Avant. The server: AMD EPYC 7351P (Zen 1, 16 cores, 32 threads), boost off at a fixed 2.4 GHz, governor `performance`. Avant: 2026-11-26, release 0.52.0, `--release --cpu=v3`, the median of 5 rounds with the whole machine held and otherwise idle, each round with the task's code at another of five addresses (0, 16, 32, 48 and 448 bytes later: three of them move each loop within its 64-byte line, with innermost loops aligned to 16 bytes there, one moves the code by whole lines; the runtime's C code moves with the program's), so no single address decides a number; all 50 checksums correct; the others: 2026-11-14. Since release 0.51.0's tables the times are taken by the same method; what changed is the peak memory's exact column, which now has Go (below); the comparison below measures release 0.51.0 by the same method in the same session.

**The Avant build** is release 0.52.0 as users download it: the release job's build on Ubuntu 26.04, LLVM 22.1.8 for the compiler and its clang for the runtime, for the baseline x86-64. The numbers were measured with the same source built on this server with the same toolchain, which builds release 0.51.0's published compiler byte for byte.

**Peak memory, two ways.** `ru_maxrss` is the kernel's high-water mark of a process's resident memory, as every earlier table gave it (the others' from their 2026-11-14 runs, Avant's from the rounds of its times). The exact peak is the largest resident size the process reaches, read by `tools/la-run.c -p`: every system call that can lower the resident size waits until the process's resident size is read, and it is read once more at its exit; each program runs from a `cat` copy of its binary with address randomization off, the median of five runs, every language on 2026-11-26 (the others' binaries those of 2026-11-14). The kernel counts a process's resident pages per CPU and adds each CPU's count to the total only in batches of 64 pages (256 KB), and `ru_maxrss` is read from that total, so it reads below the true peak, by about 150 to 950 KB on most single-threaded tasks and by up to about 5 MB on the threaded `Matmul` tasks. The README's summary gives `ru_maxrss` only, for every language; the exact peak is given here beside it. Go's exact column is new: the reader now keeps each thread's `statm` file open, so a reading costs a third of what it did, and Go's two LZW tasks, whose runtime returns memory through tens of thousands of such calls, now read close to their unmeasured runs (`Compress::LZWDecode` 21.9 to 30.3 MB, median 23.0 MB, against 21.6 to 22.3 MB unmeasured, on the otherwise idle machine; 0.25 to 1.2 GB with the earlier reader); that task's peak also moves with the machine's load (about 23 MB, or 55 to 95 MB on a loaded machine, measured or not), so its median of five is read with that in mind.

| Language | 50 tasks (s) | Average peak memory (`ru_maxrss`) | Average peak memory (exact) | time × Avant | peak memory × Avant (`ru_maxrss`) | peak memory × Avant (exact) | time, geometric mean × Avant |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Avant | **85.560** | 26.7 MB | 27.2 MB | 1.00× | 1.00× | 1.00× | 1.00× |
| C (Clang) | 138.008 | **23.0 MB** | 23.2 MB | 1.61× | 0.86× | 0.86× | 1.68× |
| C (GCC) | 143.231 | 23.0 MB | **23.1 MB** | 1.67× | 0.86× | 0.85× | 1.75× |
| Rust | 137.709 | 29.3 MB | 29.6 MB | 1.61× | 1.10× | 1.09× | 1.70× |
| Crystal | 194.527 | 65.2 MB | 65.7 MB | 2.27× | 2.44× | 2.42× | 2.58× |
| Go | 311.401 | 62.8 MB | 64.8 MB | 3.64× | 2.35× | 2.39× | 3.14× |

The geometric-mean column is the mean over the 50 tasks of each task's time over Avant's, so every task weighs alike; the total is dominated by the longest tasks.

**Against release 0.51.0, in the same session and by the same method** (0.51.0's source built here, which builds its published compiler byte for byte; 0.51.0 on its own task sources, 15 of the 64 programs respelled since with no change in what they compute): 0.99× the time (the geometric mean over the 50 tasks, within its spread; their total 86.3 s to 85.6 s) at 0.97× the average peak memory (`ru_maxrss` 27.5 MB to 26.7 MB, exact 27.9 MB to 27.2 MB); no task is slower beyond its spread. The largest changes: `Etc::GameOfLife` 0.86× (a structure that survives mostly, not wholly, no longer promotes its young generation whole), at 1.02× its memory; `Calculator::Interpreter` 0.81× its memory and `Graph::AStar`, `DFS` and `BFS` 0.90× to 0.93× (the young generation grows past 8 MiB only when a full collection shows it pays), `Graph::DFS` 0.97× and `Graph::BFS` 0.96× the time; `Json::Generate` 0.95×. `Json::ParseMapping`'s `ru_maxrss` is 1.01× above its spread by one batch of the kernel's count (its anonymous peak 8 KB higher).

**Read the total task by task**: ten rows call a library (†), C's two A* rows are not the references' algorithm (‡), and two rows carry collection hints ([Reading these numbers](#reading-these-numbers)).

| Task | Avant | C | Rust | Crystal | Go |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Binarytrees::Obj` | **1.081** | 6.487 | 6.849 | 2.925 | 6.155 |
| `Binarytrees::Arena` | 1.014 | 1.039 | **0.908** | 1.653 | 0.953 |
| `Brainfuck::Array` | **2.152** | 3.455 | 4.066 | 7.098 | 3.837 |
| `Brainfuck::Recursion` | **4.389** | 4.721 | 5.276 | 4.689 | 5.132 |
| `Matmul::Single` | **2.099** | 11.101 | 11.558 | 11.829 | 12.492 |
| `Matmul::T4` | **0.772** | 2.924 | 3.015 | 4.757 | 7.838 |
| `Matmul::T8` | **0.674** | 1.587 | 1.644 | 4.544 | 4.070 |
| `Matmul::T16` | **0.541** | 0.830 | 1.065 | 3.608 | 2.139 |
| `Base64::Encode` † | **0.448** | 0.497 | 1.041 | 3.490 | 5.941 |
| `Base64::Decode` † | **0.517** | 0.573 | 0.977 | 3.351 | 3.958 |
| `Json::Generate` | 0.772 | 1.554 | **0.670** | 2.979 | 3.592 |
| `Json::ParseDom` † | 0.299 | **0.292** | 4.273 | 1.710 | 5.128 |
| `Json::ParseMapping` † | 0.790 | **0.746** | 1.157 | 4.371 | 8.027 |
| `CSV::Parse` | **1.624** | 6.257 | 2.789 | 9.475 | 8.802 |
| `Etc::Sieve` | 2.660 | **1.893** | 1.916 | 4.490 | 3.792 |
| `Etc::TextRaytracer` | **2.335** | 3.250 | 2.538 | 3.525 | 4.135 |
| `Etc::NeuralNet` | 2.752 | **2.654** | 2.967 | 3.137 | 4.717 |
| `Etc::CacheSimulation` | **1.255** | 2.243 | 2.077 | 4.198 | 5.057 |
| `Etc::GameOfLife` | **2.431** | 3.577 | 3.606 | 6.274 | 2.488 |
| `Etc::Words` | **1.501** | 4.621 | 2.481 | 2.300 | 2.949 |
| `Etc::LogParser` † | 1.173 | 1.345 | **0.864** | 2.942 | 79.266 |
| `Template::Regex` † | **1.586** | 3.200 | 8.642 | 4.342 | 21.198 |
| `Template::Parse` | **1.107** | 2.479 | 2.802 | 6.547 | 2.551 |
| `Sort::Quick` | 2.760 | 2.651 | **2.640** | 3.635 | 2.915 |
| `Sort::Merge` | 3.306 | **2.394** | 3.303 | 5.566 | 3.589 |
| `Sort::Self` | **0.568** | 4.487 | 1.544 | 2.453 | 3.063 |
| `Graph::BFS` | 3.244 | **2.425** | 2.895 | 4.292 | 5.223 |
| `Graph::DFS` | 2.813 | **2.133** | 2.531 | 3.782 | 3.300 |
| `Graph::AStar` | **3.124** | 5.225 ‡ | 5.525 | 4.678 | 10.289 |
| `Hash::SHA256` | **0.519** | 2.246 | 2.144 | 2.263 | 2.559 |
| `Hash::CRC32` † | **0.032** | 1.943 | 1.950 | 1.940 | 9.272 |
| `Calculator::Ast` | **0.688** | 2.739 | 1.639 | 1.333 | 2.132 |
| `Calculator::Interpreter` | **1.594** | 2.830 | 2.167 | 2.102 | 1.799 |
| `Maze::Generator` | **1.157** | 1.226 | 2.027 | 5.454 | 3.396 |
| `Maze::BFS` | 1.357 | **1.317** | 2.117 | 6.553 | 4.350 |
| `Maze::AStar` | **1.905** | 3.557 ‡ | 2.590 | 7.896 | 7.585 |
| `CLBG::Fannkuchredux` | **2.142** | 2.680 | 2.398 | 2.455 | 2.598 |
| `CLBG::Mandelbrot` | 2.247 | 2.188 | 2.304 | 2.251 | **2.178** |
| `CLBG::Nbody` | 3.508 | 3.516 | **3.280** | 3.566 | 4.008 |
| `CLBG::Spectralnorm` | **1.988** | 2.724 | 2.729 | 2.874 | 2.712 |
| `Compress::BWTEncode` † | 1.245 | 1.717 | **0.979** | 3.335 | 4.607 |
| `Compress::BWTDecode` † | **3.628** | 3.691 | 4.109 | 4.159 | 6.540 |
| `Compress::HuffEncode` | 1.925 | 1.943 | **1.605** | 2.468 | 2.520 |
| `Compress::HuffDecode` | 1.289 | **1.037** | 1.576 | 2.481 | 3.471 |
| `Compress::ArithEncode` | 2.772 | 2.679 | **2.633** | 3.306 | 2.871 |
| `Compress::ArithDecode` | 2.381 | **2.330** | 3.327 | 2.574 | 2.554 |
| `Compress::LZWEncode` | **1.509** | 3.278 | 1.616 | 2.475 | 3.937 |
| `Compress::LZWDecode` † | **0.571** | 1.489 | 1.601 | 1.787 | 5.814 |
| `Distance::Jaro` | 2.103 | **2.020** | 2.090 | 2.837 | 2.047 |
| `Distance::NGram` | 1.213 | 4.216 | **1.210** | 1.779 | 1.856 |

† 10 of the 50 rows rely on a library or on the runtime's C, as an Avant program written for the task would: `Hash::CRC32` on [std.digest](README.md#stddigest) (Chromium's carry-less-multiply CRC-32, vendored C), `Base64::Encode` and `Decode` on [std.base64](README.md#stdbase64) (aklomp/base64, vendored C), `Json::ParseDom` and `ParseMapping` on [std.json](README.md#stdjson) (yyjson), `Template::Regex` and `Etc::LogParser` on [std.regex](README.md#stdregex) (PCRE2 and re2c), `Compress::BWTEncode` and `BWTDecode` on the runtime's C Burrows-Wheeler transform and inverse, `Compress::LZWDecode` on the runtime's C buffer copies.

## Where Avant loses

Avant is the fastest of these five on 29 of 50 tasks. Against the best of *every* language measured on this machine (C under both GCC and Clang included): at or ahead on 30 tasks, the median task 0.98× the best, no task more than 1.5× behind. The worst by time:

| Task | Avant (s) | Best (s) | Behind by |
| --- | ---: | ---: | ---: |
| `Etc::Sieve` | 2.660 | 1.893 (C, Clang) | 1.41× |
| `Sort::Merge` | 3.306 | 2.394 (C, Clang) | 1.38× |
| `Etc::LogParser` | 1.173 | 0.864 (Rust) | 1.36× |
| `Graph::BFS` | 3.244 | 2.425 (C, Clang) | 1.34× |
| `Graph::DFS` | 2.813 | 2.106 (C, GCC) | 1.34× |
| `Compress::BWTEncode` | 1.245 | 0.979 (Rust) | 1.27× |
| `Compress::HuffDecode` | 1.289 | 1.037 (C, Clang) | 1.24× |
| `Compress::HuffEncode` | 1.925 | 1.605 (Rust) | 1.20× |

By peak memory (`ru_maxrss`): at or below the best on 13 tasks, the median task at 1.24×, 8 tasks more than 1.5× above; by the exact peak: at or below the best on 13, the median task at 1.21×, 7 more than 1.5× above. The worst:

| Task | Avant (MB, `ru_maxrss`) | Best (MB) | Above by | Avant (MB, exact) | Best (MB, exact) | Above by |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `Compress::LZWEncode` | 10.6 | 3.4 (C, Clang) | 3.14× | 11.0 | 3.4 (C, GCC) | 3.22× |
| `Etc::CacheSimulation` | 6.4 | 3.1 (C, Clang) | 2.03× | 6.6 | 3.4 (C, GCC) | 1.95× |
| `Matmul::T4` | 45.7 | 27.4 (C, GCC) | 1.67× | 47.6 | 27.8 (C, GCC) | 1.72× |
| `Matmul::T8` | 44.9 | 27.6 (C, GCC) | 1.63× | 48.4 | 27.8 (C, GCC) | 1.74× |
| `Matmul::Single` | 43.2 | 27.4 (C, Clang) | 1.58× | 43.6 | 27.6 (C, GCC) | 1.58× |
| `Etc::GameOfLife` | 46.4 | 29.5 (Go) | 1.57× | 46.6 | 31.4 (C, GCC) | 1.48× |

Most of the small tasks allocate in every run, as their references do; a small program's young generation starts at 1 MiB and grows only when the program needs it, so they read about 3.5 MB where the references read 2 to 4 MB.

Crystal and Go are compiled and garbage-collected too, so trailing either on any task, in time or memory, is a defect. Avant trails on 8:

| Task | Behind |
| --- | --- |
| `Binarytrees::Arena` | time 1.06× Go |
| `Matmul::Single` | memory 1.12× Crystal |
| `Etc::GameOfLife` | memory 1.57× Go |
| `Calculator::Interpreter` | memory 1.04× Go |
| `CLBG::Mandelbrot` | time 1.03× Go |
| `Compress::BWTDecode` | memory 1.08× Crystal |
| `Compress::LZWEncode` | memory 1.11× Crystal, 1.07× Go |
| `Distance::Jaro` | time 1.03× Go |

The memory rows are by `ru_maxrss`; by the exact peak three of them are above Crystal's (`Matmul::Single` 1.11×, `Compress::BWTDecode` 1.06×, `Compress::LZWEncode` 1.02×) and one above Go's (`Etc::GameOfLife` 1.45×).

Avant's numbers are the median of five rounds, the others' one run: ratios within a few percent may be noise.

The tables understate allocation-heavy code: the two rows that would be worse are fast because they are hinted ([What allocation costs today](README.md#what-allocation-costs-today)).

## HTTP and WebSocket servers

The most basic server in each language: `GET /` answered with `hello` over keep-alive connections, each an optimized build, all eight in one session on the LangArena server (AMD EPYC 7351P, boost off at 2.4 GHz), 2026-11-26, Avant release 0.52.0. Each row is the median of three runs, each run a fresh server, one checked request, a 2-second warm-up and the measured load, the order of the rows rotating; every server passed the same correctness checks first (pipelined and split requests, a client that closes without reading). Sources and script: `bench/http/`.

| Server | No pipelining (requests per second) | Peak memory | 16 pipelined (requests per second) | Peak memory |
| --- | ---: | ---: | ---: | ---: |
| Avant (`std.http`) | 349,322 | 3.8 MB | **4,146,750** | 4.1 MB |
| C (h2o) | 342,333 | 9.6 MB | 1,613,897 | 10.6 MB |
| Rust (std, thread per connection) | 343,102 | 6.7 MB | 1,760,869 | 19.7 MB |
| Rust (hyper) | **350,926** | 13.3 MB | 3,943,840 | 28.5 MB |
| Go (`net/http`) | 185,580 | 30.0 MB | 433,562 | 85.6 MB |
| Go (fasthttp) | 342,326 | 18.2 MB | 3,443,280 | 28.5 MB |
| Crystal (`HTTP::Server`) | 162,479 | 33.6 MB | 304,384 | 101.1 MB |
| Valk (`http.serve_fast`) | 321,096 | 22.9 MB | 3,743,528 | 83.2 MB |

**Method.** No pipelining: `wrk -t8 -c256 -d10s --latency`. Pipelined: `wrk -t8 -c900 -d5 --timeout 8 -s pipe.lua -- 16`, 900 connections with 16 requests in every write, as Valk's published benchmark. The load generator runs on the same machine, and the servers use their default thread counts.

**The first column is not a ranking**: `wrk`'s eight threads use 7.5 to 8 of their 8 cores for every row from 321,000 requests a second up, so it shows that none of those six servers is the limit at 256 connections, and what each costs in memory. Pipelined, Avant answers the most requests at the least memory. hyper and fasthttp also send a `Date` header (121 and 139 bytes an answer against Avant's 84), Valk does not.

The WebSocket echo, in the same session: messages per second over 8 connections for 10 seconds, the median of three runs. The echo has two speeds from run to run on this machine (about 72,000 and 83,000 messages a second; h2o's three runs here read 72,842 to 84,543), so five more runs right after, each alternating with release 0.51.0's server, compare the two: Avant at 83,933 (0.51.0's 79,749, slower in all five rounds) and h2o at 73,419.

| Server | Messages per second | Peak memory |
| --- | ---: | ---: |
| Avant (`std.http`) | 82,193 | 4.6 MB |
| C (h2o) | 79,087 | 9.1 MB |
| Rust (std, thread per connection) | **90,969** | 2.6 MB |
| Go (`net/http`) | 53,648 | 11.4 MB |
| Crystal (`HTTP::Server`) | 63,764 | 11.3 MB |

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

Valk's garbage-collector benchmark suite, from [Valk](https://github.com/valk-lang/valk) 0.8.5 (`examples/bench/gc`, where it exists for Valk, Go, D and C#), with Avant, Crystal and Java ports; the programs, the runner and the full method are in `bench/valk-gc/` (its README). Seven scenarios, each isolating one cost of a collector, run in one process in upstream's order; each program prints each scenario's own time (`time_us`), and peak memory is GNU `time`'s `%M` of the process. One unmeasured warm-up round, then seven rounds; in each round every row runs once as a fresh process, the order rotating; every run's verification values checked. Measured on 2026-11-26 on the whole machine, Avant release 0.52.0's compiler:

```
date: 2026-11-26T14:21:16Z
kernel: Linux 7.3.4-zabbly+
cpu: AMD EPYC 7351P 16-Core Processor (32 threads)
governor: performance x32
memory: 128789 MB total, 126314 MB available
avant: release 0.52.0 (avant-c-release, sha256 b82122cb093d48a6)
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
| Avant `--release` (x86-64-v2, the compiler's default) | 39.8 (38.4–39.9) | 5.12 (5.11–5.14) | 182 (180–186) | 0.449 (0.438–0.643) | 16.9 (16.9–16.9) | 34.4 (33.9–34.6) | 156 (155–158) | 85.8 (85.8–85.9) |
| Avant `--release --cpu=v3` | 39.8 (38.3–39.9) | 5.12 (5.10–5.16) | 183 (180–186) | 0.449 (0.445–0.466) | 16.9 (16.9–16.9) | 34.5 (34.3–34.7) | 155 (154–156) | 85.8 (85.8–85.8) |
| Valk 0.8.5 `--release` | 40.8 (39.6–41.0) | 0.898 (0.799–0.908) | 173 (170–175) | 84.0 (83.1–85.3) | 21.1 (21.1–21.2) | 19.8 (19.1–20.8) | 117 (109–132) | 81.1 (80.9–81.2) |
| Go | 369 (367–372) | 16804 (16705–16915) | 269 (262–286) | 18.2 (15.3–281) | 16.9 (16.8–17.0) | 325 (315–334) | 809 (728–857) | 43.5 (43.1–43.9) |
| D (LDC `-O3 -release -boundscheck=off`) | 454 (447–457) | 5165 (5104–5169) | 462 (459–465) | 28.3 (27.6–28.9) | 23.7 (23.0–24.4) | 303 (302–339) | 793 (789–798) | 161.0 (160.9–161.2) |
| C# (.NET, tiered JIT) | 226 (223–230) | 9359 (9299–9419) | 592 (588–593) | 74.4 (71.5–77.7) | 32.7 (32.6–33.4) | 124 (122–128) | 409 (407–411) | 156.5 (156.3–156.9) |
| Crystal `--release` (new port) | 335 (327–342) | 3755 (3719–3809) | 331 (329–337) | 6.75 (6.34–7.46) | 26.3 (25.1–27.5) | 108 (107–110) | 284 (283–284) | 185.6 (185.4–186.0) |
| Java (G1, new port) | 148 (144–150) | 15932 (15904–17025) | 421 (394–455) | 20.7 (20.0–21.6) | 25.7 (25.0–26.6) | 124 (115–136) | 226 (220–228) | 254.4 (253.1–254.8) |

**Table 2.** Avant against Valk, scenario by scenario (Table 1's runs)

| Scenario | Avant row | Avant, ms | Valk, ms | Avant ÷ Valk (medians) | Verdict (all runs) |
| --- | --- | --- | --- | --- | --- |
| 1 short-lived | Avant `--release` | 39.8 (38.4–39.9) | 40.8 (39.6–41.0) | 0.97 | ranges overlap |
| 1 short-lived | Avant `--release --cpu=v3` | 39.8 (38.3–39.9) | 40.8 (39.6–41.0) | 0.98 | ranges overlap |
| 2 stable collects | Avant `--release` | 5.12 (5.11–5.14) | 0.898 (0.799–0.908) | 5.70 | trails |
| 2 stable collects | Avant `--release --cpu=v3` | 5.12 (5.10–5.16) | 0.898 (0.799–0.908) | 5.70 | trails |
| 3 build chain | Avant `--release` | 182 (180–186) | 173 (170–175) | 1.06 | trails |
| 3 build chain | Avant `--release --cpu=v3` | 183 (180–186) | 173 (170–175) | 1.06 | trails |
| 4 free chain | Avant `--release` | 0.449 (0.438–0.643) | 84.0 (83.1–85.3) | 0.01 | leads |
| 4 free chain | Avant `--release --cpu=v3` | 0.449 (0.445–0.466) | 84.0 (83.1–85.3) | 0.01 | leads |
| 5 mutate links | Avant `--release` | 16.9 (16.9–16.9) | 21.1 (21.1–21.2) | 0.80 | leads |
| 5 mutate links | Avant `--release --cpu=v3` | 16.9 (16.9–16.9) | 21.1 (21.1–21.2) | 0.80 | leads |
| 6 churn + live | Avant `--release` | 34.4 (33.9–34.6) | 19.8 (19.1–20.8) | 1.74 | trails |
| 6 churn + live | Avant `--release --cpu=v3` | 34.5 (34.3–34.7) | 19.8 (19.1–20.8) | 1.75 | trails |
| 7 tree churn | Avant `--release` | 156 (155–158) | 117 (109–132) | 1.33 | trails |
| 7 tree churn | Avant `--release --cpu=v3` | 155 (154–156) | 117 (109–132) | 1.32 | trails |

Peak RSS beside these times (one process runs the seven scenarios, so it has one peak; Table 1's runs, medians): Avant `--release` 85.8 MB (÷ Valk 1.06); Avant `--release --cpu=v3` 85.8 MB (÷ Valk 1.06); Valk 81.1 MB. Per scenario: Table 10.

A verdict is `leads` when Avant's slowest run is faster than Valk's fastest, `trails` when Avant's fastest is slower than Valk's slowest, `ranges overlap` otherwise.

**Table 3.** The labelled variants: `time_us` of each scenario, in ms, median (minimum–maximum), and the process's peak RSS; each variant under the row it is read against

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | Peak RSS, MB (the process) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 39.8 (38.4–39.9) | 5.12 (5.11–5.14) | 182 (180–186) | 0.449 (0.438–0.643) | 16.9 (16.9–16.9) | 34.4 (33.9–34.6) | 156 (155–158) | 85.8 (85.8–85.9) |
| Avant `--release --cpu=baseline` | 38.5 (38.0–39.8) | 5.13 (5.10–5.18) | 183 (179–186) | 0.448 (0.437–0.458) | 16.9 (16.9–16.9) | 34.5 (33.8–34.7) | 153 (153–153) | 85.9 (85.8–85.9) |
| Avant, young collections (not complete collections) | 38.5 (38.0–39.8) | 5.07 (4.99–5.83) | 183 (180–186) | 5.83 (5.78–6.34) | 16.9 (16.8–17.1) | 26.3 (26.0–26.7) | 160 (160–163) | 104.4 (104.3–104.4) |
| Avant `--release --cpu=v3` | 39.8 (38.3–39.9) | 5.12 (5.10–5.16) | 183 (180–186) | 0.449 (0.445–0.466) | 16.9 (16.9–16.9) | 34.5 (34.3–34.7) | 155 (154–156) | 85.8 (85.8–85.8) |
| Avant `--cpu=v3`, young collections (not complete) | 38.4 (38.0–39.9) | 5.02 (4.96–5.61) | 183 (179–185) | 6.30 (5.78–6.64) | 16.8 (16.8–16.9) | 26.1 (25.8–26.5) | 159 (158–159) | 104.4 (104.3–104.4) |
| Avant with hints (`gc.hold`, `gc.collect()`) | 38.4 (38.2–38.5) | 5.13 (5.09–5.27) | 276 (270–299) | 2.41 (2.35–2.43) | 17.0 (16.9–17.3) | 40.9 (40.2–41.2) | 133 (133–136) | 127.8 (127.8–127.9) |
| Avant `--cpu=v3` with hints | 38.5 (37.9–39.8) | 5.11 (5.09–5.15) | 271 (268–280) | 2.42 (2.37–2.51) | 17.0 (17.0–17.2) | 41.0 (39.7–41.6) | 134 (132–163) | 127.8 (127.8–127.9) |
| Valk 0.8.5 `--release` | 40.8 (39.6–41.0) | 0.898 (0.799–0.908) | 173 (170–175) | 84.0 (83.1–85.3) | 21.1 (21.1–21.2) | 19.8 (19.1–20.8) | 117 (109–132) | 81.1 (80.9–81.2) |
| Valk `--release --no-regions` | 41.2 (41.0–41.5) | 0.835 (0.776–0.894) | 170 (165–172) | 85.1 (83.4–85.9) | 21.1 (21.1–21.1) | 19.9 (19.2–20.0) | 216 (213–216) | 81.1 (80.9–81.1) |
| Go | 369 (367–372) | 16804 (16705–16915) | 269 (262–286) | 18.2 (15.3–281) | 16.9 (16.8–17.0) | 325 (315–334) | 809 (728–857) | 43.5 (43.1–43.9) |
| Go, plain sink (no atomic store) | 340 (338–342) | 16739 (16547–16976) | 273 (269–285) | 278 (16.8–367) | 16.9 (16.8–16.9) | 320 (304–326) | 836 (762–866) | 44.1 (43.1–44.6) |
| Go, `FreeOSMemory` outside scenario 4's timer | 371 (369–373) | 16822 (16697–16867) | 276 (269–286) | 11.0 (9.16–278) | 16.9 (16.8–16.9) | 321 (316–337) | 803 (765–827) | 44.3 (43.2–44.8) |
| Go, `GOGC=off` (upstream's README variant) | 369 (358–376) | 16839 (16709–16870) | 183 (179–185) | 18.6 (16.6–21.3) | 16.9 (16.8–16.9) | 225 (220–228) | 742 (734–758) | 225.3 (224.3–225.9) |
| D (LDC `-O3 -release -boundscheck=off`) | 454 (447–457) | 5165 (5104–5169) | 462 (459–465) | 28.3 (27.6–28.9) | 23.7 (23.0–24.4) | 303 (302–339) | 793 (789–798) | 161.0 (160.9–161.2) |
| D, `GC.minimize` outside scenario 4's timer | 453 (450–460) | 5277 (5139–5360) | 461 (458–469) | 3.69 (3.62–3.79) | 23.6 (23.0–24.6) | 304 (302–308) | 795 (790–799) | 161.0 (160.9–161.2) |
| C# (.NET, tiered JIT) | 226 (223–230) | 9359 (9299–9419) | 592 (588–593) | 74.4 (71.5–77.7) | 32.7 (32.6–33.4) | 124 (122–128) | 409 (407–411) | 156.5 (156.3–156.9) |
| C#, `DOTNET_TieredCompilation=0` | 186 (183–187) | 9380 (9261–9606) | 593 (589–595) | 72.8 (69.6–75.2) | 18.2 (18.0–18.5) | 106 (104–106) | 363 (361–377) | 151.4 (151.0–151.6) |
| Java (G1, new port) | 148 (144–150) | 15932 (15904–17025) | 421 (394–455) | 20.7 (20.0–21.6) | 25.7 (25.0–26.6) | 124 (115–136) | 226 (220–228) | 254.4 (253.1–254.8) |
| Java, `-XX:+UseParallelGC` | 197 (191–203) | 13513 (13451–13604) | 35.0 (32.3–37.5) | 10.4 (10.2–11.2) | 25.1 (24.9–26.4) | 65.6 (60.7–67.2) | 275 (272–278) | 366.1 (365.6–366.5) |

**Table 4.** Whole process (the seven scenarios): wall time and peak RSS, median (minimum–maximum)

| Row | Runs | Wall, s (shell clock around the launch) | Elapsed, s (GNU time `%e`) | Peak RSS, MB (GNU time `%M`) |
| --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 7 | 0.513 (0.512–0.529) | 0.50 (0.49–0.50) | 85.8 (85.8–85.9) |
| Avant `--release --cpu=v3` | 7 | 0.513 (0.509–0.516) | 0.50 (0.49–0.50) | 85.8 (85.8–85.8) |
| Valk 0.8.5 `--release` | 7 | 0.545 (0.535–0.566) | 0.53 (0.52–0.55) | 81.1 (80.9–81.2) |
| Go | 7 | 18.960 (18.833–19.122) | 18.94 (18.81–19.10) | 43.5 (43.1–43.9) |
| D (LDC `-O3 -release -boundscheck=off`) | 7 | 7.419 (7.356–7.457) | 7.40 (7.34–7.44) | 161.0 (160.9–161.2) |
| C# (.NET, tiered JIT) | 7 | 11.069 (11.012–11.135) | 11.05 (10.99–11.12) | 156.5 (156.3–156.9) |
| Crystal `--release` (new port) | 7 | 5.028 (4.975–5.088) | 5.01 (4.96–5.07) | 185.6 (185.4–186.0) |
| Java (G1, new port) | 7 | 17.275 (17.161–18.331) | 17.26 (17.14–18.31) | 254.4 (253.1–254.8) |
| Avant `--release --cpu=baseline` | 7 | 0.511 (0.506–0.527) | 0.49 (0.49–0.50) | 85.9 (85.8–85.9) |
| Avant, young collections (not complete collections) | 7 | 0.555 (0.552–0.560) | 0.54 (0.53–0.54) | 104.4 (104.3–104.4) |
| Avant `--cpu=v3`, young collections (not complete) | 7 | 0.554 (0.545–0.557) | 0.54 (0.53–0.54) | 104.4 (104.3–104.4) |
| Avant with hints (`gc.hold`, `gc.collect()`) | 7 | 0.648 (0.635–0.664) | 0.63 (0.62–0.65) | 127.8 (127.8–127.9) |
| Avant `--cpu=v3` with hints | 7 | 0.642 (0.635–0.679) | 0.62 (0.62–0.66) | 127.8 (127.8–127.9) |
| Valk `--release --no-regions` | 7 | 0.642 (0.630–0.644) | 0.62 (0.61–0.63) | 81.1 (80.9–81.1) |
| Go, plain sink (no atomic store) | 7 | 18.965 (18.721–19.300) | 18.95 (18.70–19.28) | 44.1 (43.1–44.6) |
| Go, `FreeOSMemory` outside scenario 4's timer | 7 | 18.991 (18.840–19.218) | 18.97 (18.82–19.20) | 44.3 (43.2–44.8) |
| Go, `GOGC=off` (upstream's README variant) | 7 | 18.731 (18.606–18.766) | 18.71 (18.59–18.75) | 225.3 (224.3–225.9) |
| D, `GC.minimize` outside scenario 4's timer | 7 | 7.529 (7.396–7.603) | 7.51 (7.38–7.58) | 161.0 (160.9–161.2) |
| C#, `DOTNET_TieredCompilation=0` | 7 | 11.005 (10.872–11.225) | 10.99 (10.85–11.21) | 151.4 (151.0–151.6) |
| Java, `-XX:+UseParallelGC` | 7 | 14.411 (14.345–14.517) | 14.39 (14.33–14.50) | 366.1 (365.6–366.5) |

**Table 5.** Scenario-isolated builds, one scenario per process: `time_us`, in ms, median (minimum–maximum), and beside it the median peak RSS of the process that ran the scenario (its range in Table 6). Scenario 8 is added by Avant and is not part of Valk's suite

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | 8 reverse links (added) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 39.1 (38.3–40.4) · 3.4 MB | 1.20 (1.17–1.22) · 12.1 MB | 187 (184–189) · 85.4 MB | 0.476 (0.473–0.538) · 85.6 MB | 16.9 (16.8–16.9) · 5.9 MB | 26.8 (26.7–27.6) · 20.4 MB | 153 (152–153) · 6.6 MB | 23.2 (23.0–23.7) · 7.6 MB |
| Avant `--release --cpu=v3` | 40.3 (38.6–40.4) · 3.4 MB | 1.20 (1.17–1.24) · 12.1 MB | 184 (181–187) · 85.4 MB | 0.483 (0.474–0.493) · 85.6 MB | 16.9 (16.9–16.9) · 5.9 MB | 26.9 (26.6–27.2) · 20.4 MB | 153 (153–213) · 6.6 MB | 23.1 (23.0–23.2) · 7.6 MB |
| Valk 0.8.5 `--release` | 40.3 (38.7–40.7) · 4.1 MB | 1.04 (0.812–1.16) · 9.9 MB | 163 (158–168) · 80.9 MB | 84.8 (84.4–85.0) · 81.2 MB | 21.1 (21.1–21.2) · 5.2 MB | 20.9 (20.8–21.1) · 11.7 MB | 101 (100–122) · 2.9 MB | 315 (315–316) · 7.7 MB |
| Go | 373 (372–374) · 7.6 MB | 16758 (16681–16834) · 8.1 MB | 397 (387–403) · 43.1 MB | 103 (87.2–119) · 43.6 MB | 16.9 (16.8–17.0) · 4.6 MB | 461 (450–480) · 14.8 MB | 797 (666–832) · 8.2 MB | 21.6 (19.0–21.7) · 4.6 MB |
| D (LDC `-O3 -release -boundscheck=off`) | 454 (452–472) · 4.2 MB | 5210 (5117–5285) · 19.2 MB | 539 (535–546) · 159.2 MB | 30.4 (28.7–33.1) · 161.2 MB | 21.7 (21.4–23.4) · 9.7 MB | 336 (334–344) · 39.4 MB | 731 (725–737) · 8.4 MB | 26.7 (25.7–27.6) · 9.7 MB |
| C# (.NET, tiered JIT) | 222 (218–225) · 39.9 MB | 9129 (9090–9234) · 45.4 MB | 553 (548–559) · 151.1 MB | 37.6 (37.1–41.0) · 151.0 MB | 37.9 (37.5–38.3) · 43.7 MB | 150 (147–151) · 54.1 MB | 383 (378–389) · 38.6 MB | 70.3 (70.2–71.1) · 44.0 MB |
| Crystal `--release` (new port) | 336 (321–348) · 3.9 MB | 3735 (3711–4220) · 19.5 MB | 355 (351–363) · 168.4 MB | 7.49 (6.32–317) · 168.4 MB | 27.4 (24.6–32.4) · 9.7 MB | 178 (175–180) · 37.4 MB | 536 (509–557) · 4.5 MB | 29.2 (28.6–31.8) · 9.7 MB |
| Java (G1, new port) | 149 (146–151) · 176.9 MB | 15886 (15671–16170) · 100.7 MB | 102 (101–104) · 123.4 MB | 28.7 (27.8–29.1) · 156.7 MB | 25.7 (25.4–26.3) · 85.9 MB | 136 (134–139) · 115.9 MB | 349 (347–352) · 287.5 MB | 29.9 (29.3–34.6) · 85.9 MB |

**Table 6.** Scenario-isolated builds: peak RSS of the process that ran the scenario, MB, median (minimum–maximum). Scenario 4's process also ran scenario 3

| Row | 1 short-lived | 2 stable collects | 3 build chain | 4 free chain | 5 mutate links | 6 churn + live | 7 tree churn | 8 reverse links (added) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Avant `--release` (x86-64-v2, the compiler's default) | 3.4 (3.3–3.4) | 12.1 (12.1–12.2) | 85.4 (85.1–85.4) | 85.6 (85.6–85.7) | 5.9 (5.9–5.9) | 20.4 (20.4–20.4) | 6.6 (6.6–6.7) | 7.6 (7.6–7.7) |
| Avant `--release --cpu=v3` | 3.4 (3.3–3.4) | 12.1 (12.1–12.2) | 85.4 (85.3–85.4) | 85.6 (85.6–85.7) | 5.9 (5.8–5.9) | 20.4 (20.3–20.4) | 6.6 (6.4–6.7) | 7.6 (7.6–7.7) |
| Valk 0.8.5 `--release` | 4.1 (4.1–4.2) | 9.9 (9.9–9.9) | 80.9 (80.9–80.9) | 81.2 (81.1–81.2) | 5.2 (5.1–5.2) | 11.7 (11.7–11.7) | 2.9 (2.9–2.9) | 7.7 (7.6–7.7) |
| Go | 7.6 (7.1–7.9) | 8.1 (7.1–8.3) | 43.1 (42.6–43.6) | 43.6 (43.4–44.4) | 4.6 (4.4–4.6) | 14.8 (14.4–15.1) | 8.2 (7.7–8.8) | 4.6 (4.6–4.6) |
| D (LDC `-O3 -release -boundscheck=off`) | 4.2 (4.1–4.4) | 19.2 (19.1–19.2) | 159.2 (158.9–159.2) | 161.2 (161.0–161.2) | 9.7 (9.7–9.7) | 39.4 (39.1–39.4) | 8.4 (8.1–8.4) | 9.7 (9.6–9.7) |
| C# (.NET, tiered JIT) | 39.9 (39.7–40.2) | 45.4 (45.0–45.9) | 151.1 (150.9–151.3) | 151.0 (150.4–151.1) | 43.7 (43.5–43.9) | 54.1 (54.0–54.3) | 38.6 (38.1–38.9) | 44.0 (43.6–44.3) |
| Crystal `--release` (new port) | 3.9 (3.7–4.0) | 19.5 (19.1–19.7) | 168.4 (167.7–168.6) | 168.4 (167.9–168.6) | 9.7 (9.5–9.9) | 37.4 (36.7–37.5) | 4.5 (4.2–4.7) | 9.7 (9.4–10.0) |
| Java (G1, new port) | 176.9 (175.9–177.1) | 100.7 (100.5–101.4) | 123.4 (122.9–123.5) | 156.7 (155.7–156.7) | 85.9 (85.2–86.3) | 115.9 (115.4–116.2) | 287.5 (287.0–288.0) | 85.9 (85.3–86.2) |

**Table 10.** The scoreboard, isolated (Table 5's runs): Avant against Valk and Go, time and peak RSS per scenario

| Scenario | Avant, ms | Avant, MB | Valk, ms | Valk, MB | Go, ms | Go, MB | ÷ Valk, time | ÷ Valk, memory | against Valk (all runs) | ÷ Go, time | ÷ Go, memory | against Go (all runs) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 short-lived | 39.1 (38.3–40.4) | 3.4 (3.3–3.4) | 40.3 (38.7–40.7) | 4.1 (4.1–4.2) | 373 (372–374) | 7.6 (7.1–7.9) | 0.97 | 0.83 | ranges overlap | 0.10 | 0.45 | leads |
| 2 stable collects | 1.20 (1.17–1.22) | 12.1 (12.1–12.2) | 1.04 (0.812–1.16) | 9.9 (9.9–9.9) | 16758 (16681–16834) | 8.1 (7.1–8.3) | 1.16 | 1.23 | trails | 0.00 | 1.51 | leads |
| 3 build chain | 187 (184–189) | 85.4 (85.1–85.4) | 163 (158–168) | 80.9 (80.9–80.9) | 397 (387–403) | 43.1 (42.6–43.6) | 1.15 | 1.05 | trails | 0.47 | 1.98 | leads |
| 4 free chain | 0.476 (0.473–0.538) | 85.6 (85.6–85.7) | 84.8 (84.4–85.0) | 81.2 (81.1–81.2) | 103 (87.2–119) | 43.6 (43.4–44.4) | 0.01 | 1.05 | leads | 0.00 | 1.96 | leads |
| 5 mutate links | 16.9 (16.8–16.9) | 5.9 (5.9–5.9) | 21.1 (21.1–21.2) | 5.2 (5.1–5.2) | 16.9 (16.8–17.0) | 4.6 (4.4–4.6) | 0.80 | 1.14 | leads | 1.00 | 1.28 | ranges overlap |
| 6 churn + live | 26.8 (26.7–27.6) | 20.4 (20.4–20.4) | 20.9 (20.8–21.1) | 11.7 (11.7–11.7) | 461 (450–480) | 14.8 (14.4–15.1) | 1.28 | 1.75 | trails | 0.06 | 1.37 | leads |
| 7 tree churn | 153 (152–153) | 6.6 (6.6–6.7) | 101 (100–122) | 2.9 (2.9–2.9) | 797 (666–832) | 8.2 (7.7–8.8) | 1.52 | 2.27 | trails | 0.19 | 0.81 | leads |
| 8 reverse links (added) | 23.2 (23.0–23.7) | 7.6 (7.6–7.7) | 315 (315–316) | 7.7 (7.6–7.7) | 21.6 (19.0–21.7) | 4.6 (4.6–4.6) | 0.07 | 0.99 | leads | 1.07 | 1.66 | trails |

Avant is the `--release` row. Memory is the peak RSS of the process that ran the scenario (scenario 4's also ran 3). The verdicts are Table 2's, on time.
