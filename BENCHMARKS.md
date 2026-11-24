# Avant benchmarks

The full measurements behind the four tables of the README's [Benchmarks](README.md#benchmarks) section: how each number was taken, the per-task tables, where Avant loses, the peak memory read two ways, and the notes needed before quoting any of them.

## Contents

- [LangArena](#langarena)
- [Where Avant loses](#where-avant-loses)
- [HTTP and WebSocket servers](#http-and-websocket-servers)
- [Reading these numbers](#reading-these-numbers)

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
